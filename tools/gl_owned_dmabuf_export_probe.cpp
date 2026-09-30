#include "capture/capture.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using fluxrec::capture::CaptureConfig;
using fluxrec::capture::CaptureRequirements;
using fluxrec::capture::UniqueFd;
using fluxrec::capture::WaylandCapture;

template<class T>
T egl_proc(const char* name) {
    const auto raw = eglGetProcAddress(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

bool has_extension(const char* extensions, std::string_view wanted) {
    if (!extensions) return false;
    std::string_view all{extensions};
    std::size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && all[at] == ' ') ++at;
        const auto end = all.find(' ', at);
        const auto item = all.substr(at, end == std::string_view::npos ? all.size() - at : end - at);
        if (item == wanted) return true;
        if (end == std::string_view::npos) break;
        at = end + 1U;
    }
    return false;
}

std::string fourcc_string(std::uint32_t value) {
    std::string result(4, ' ');
    for (unsigned int i = 0; i < 4U; ++i) {
        result[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
    }
    return result;
}

std::optional<std::string> render_node_for_device(std::uint64_t expected_device) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error)) {
        struct stat metadata {};
        if (::stat(entry.path().c_str(), &metadata) == 0 && S_ISCHR(metadata.st_mode) &&
            static_cast<std::uint64_t>(metadata.st_rdev) == expected_device) {
            return entry.path().string();
        }
    }
    return std::nullopt;
}

struct EglGlState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLint egl_major = 0;
    EGLint egl_minor = 0;
    GLuint texture = 0U;
    bool initialized = false;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;

    ~EglGlState() {
        if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE) {
            (void)eglMakeCurrent(display, surface, surface, context);
            if (texture != 0U) glDeleteTextures(1, &texture);
            (void)glFinish();
            (void)eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (display != EGL_NO_DISPLAY && image != EGL_NO_IMAGE_KHR && destroy_image) {
            (void)destroy_image(display, image);
        }
        if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT) {
            (void)eglDestroyContext(display, context);
        }
        if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) {
            (void)eglDestroySurface(display, surface);
        }
        if (display != EGL_NO_DISPLAY && initialized) (void)eglTerminate(display);
    }
};

struct Options {
    std::string output = "DP-1";
    bool bgra8 = false;
    bool rgb8 = false;
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help") {
            std::printf("usage: %s [--output NAME] [--internal-format rgba8|bgra8|rgb8]\n", argv[0]);
            return std::nullopt;
        }
        if (arg == "--output" && i + 1 < argc) {
            options.output = argv[++i];
            continue;
        }
        if (arg == "--internal-format" && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            if (value == "rgba8") {
                options.bgra8 = false;
                options.rgb8 = false;
            } else if (value == "bgra8") {
                options.bgra8 = true;
                options.rgb8 = false;
            } else if (value == "rgb8") {
                options.bgra8 = false;
                options.rgb8 = true;
            }
            else {
                std::fprintf(stderr, "--internal-format must be rgba8, bgra8, or rgb8\n");
                return std::nullopt;
            }
            continue;
        }
        std::fprintf(stderr, "unknown or incomplete argument: %.*s\n",
            static_cast<int>(arg.size()), arg.data());
        return std::nullopt;
    }
    return options;
}

bool gl_has_extension(std::string_view wanted) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    if (glGetError() != GL_NO_ERROR || count < 0) return false;
    for (GLint i = 0; i < count; ++i) {
        const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,
            static_cast<GLuint>(i)));
        if (extension && wanted == extension) return true;
    }
    return false;
}

std::expected<std::unique_ptr<EglGlState>, std::string> initialize_egl_gl(
    std::string_view wanted_render_node) {
    auto state = std::make_unique<EglGlState>();
    const auto query_devices = egl_proc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
    const auto query_device_string = egl_proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
    const auto get_platform_display = egl_proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
    state->create_image = egl_proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    state->destroy_image = egl_proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    const auto export_query = egl_proc<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>("eglExportDMABUFImageQueryMESA");
    const auto export_image = egl_proc<PFNEGLEXPORTDMABUFIMAGEMESAPROC>("eglExportDMABUFImageMESA");
    if (!query_devices || !query_device_string || !get_platform_display || !state->create_image ||
        !state->destroy_image || !export_query || !export_image) {
        return std::unexpected("required EGL device, image, or DMA-BUF export entry point is absent");
    }

    EGLint device_count = 0;
    if (query_devices(0, nullptr, &device_count) != EGL_TRUE || device_count <= 0) {
        return std::unexpected("eglQueryDevicesEXT found no EGL devices");
    }
    std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(device_count));
    if (query_devices(device_count, devices.data(), &device_count) != EGL_TRUE ||
        device_count > static_cast<EGLint>(devices.size())) {
        return std::unexpected("eglQueryDevicesEXT failed or exceeded its result capacity");
    }
    for (EGLint i = 0; i < device_count; ++i) {
        const char* path = query_device_string(devices[static_cast<std::size_t>(i)], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (path && wanted_render_node == path) {
            state->device = devices[static_cast<std::size_t>(i)];
            break;
        }
    }
    if (state->device == EGL_NO_DEVICE_EXT) {
        return std::unexpected("no EGL device matches capture render node " + std::string(wanted_render_node));
    }

    state->display = get_platform_display(EGL_PLATFORM_DEVICE_EXT, state->device, nullptr);
    if (state->display == EGL_NO_DISPLAY) {
        return std::unexpected("eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT) failed");
    }
    if (eglInitialize(state->display, &state->egl_major, &state->egl_minor) != EGL_TRUE) {
        return std::unexpected("eglInitialize on capture EGL device failed");
    }
    state->initialized = true;
    const char* extensions = eglQueryString(state->display, EGL_EXTENSIONS);
    for (const std::string_view extension : {"EGL_MESA_image_dma_buf_export", "EGL_KHR_image_base",
                                              "EGL_KHR_gl_texture_2D_image"}) {
        if (!has_extension(extensions, extension)) {
            return std::unexpected("EGL display does not advertise " + std::string(extension));
        }
    }

    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        return std::unexpected("eglBindAPI(EGL_OPENGL_API) failed");
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (eglChooseConfig(state->display, config_attributes, &state->config, 1, &config_count) != EGL_TRUE ||
        config_count != 1) {
        return std::unexpected("eglChooseConfig found no desktop OpenGL pbuffer config");
    }
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    state->surface = eglCreatePbufferSurface(state->display, state->config, pbuffer_attributes);
    if (state->surface == EGL_NO_SURFACE) return std::unexpected("eglCreatePbufferSurface failed");

    const std::array<std::array<EGLint, 5>, 3> context_candidates{{
        {{EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_NONE}},
        {{EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE}},
        {{EGL_NONE, 0, EGL_NONE, 0, EGL_NONE}},
    }};
    for (const auto& attributes : context_candidates) {
        state->context = eglCreateContext(state->display, state->config, EGL_NO_CONTEXT, attributes.data());
        if (state->context != EGL_NO_CONTEXT) break;
        (void)eglGetError();
    }
    if (state->context == EGL_NO_CONTEXT) return std::unexpected("eglCreateContext failed");
    if (eglMakeCurrent(state->display, state->surface, state->surface, state->context) != EGL_TRUE) {
        return std::unexpected("eglMakeCurrent failed");
    }
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!vendor || !renderer || !version) return std::unexpected("OpenGL context identity query failed");
    std::printf("stage=context passed render_node=%.*s egl=%d.%d vendor=%s renderer=%s version=%s\n",
        static_cast<int>(wanted_render_node.size()), wanted_render_node.data(),
        state->egl_major, state->egl_minor, vendor, renderer, version);
    return state;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (!options) return argc > 1 && std::string_view{argv[1]} == "--help" ? 0 : 2;

    CaptureConfig capture_config;
    capture_config.output_name = options->output;
    auto capture_result = WaylandCapture::connect(std::move(capture_config));
    if (!capture_result) {
        std::fprintf(stderr, "stage=capture_constraints failed: %s\n", capture_result.error().message.c_str());
        return 1;
    }
    auto capture = std::move(*capture_result);
    const CaptureRequirements requirements = capture.requirements();
    if (!requirements.dmabuf_device) {
        std::fprintf(stderr, "capture protocol did not provide a DMA-BUF device identity\n");
        return 1;
    }
    const auto render_node = render_node_for_device(*requirements.dmabuf_device);
    if (!render_node) {
        std::fprintf(stderr, "capture DMA-BUF device %llu has no local DRM render node\n",
            static_cast<unsigned long long>(*requirements.dmabuf_device));
        return 1;
    }
    std::printf("stage=capture_constraints passed output=%s dimensions=%ux%u format_modifier_pairs=%zu capture_started=no\n",
        options->output.c_str(), requirements.width, requirements.height, requirements.dmabuf_formats.size());
    std::vector<std::uint32_t> advertised_fourccs;
    advertised_fourccs.reserve(requirements.dmabuf_formats.size());
    for (const auto& pair : requirements.dmabuf_formats) advertised_fourccs.push_back(pair.fourcc);
    std::sort(advertised_fourccs.begin(), advertised_fourccs.end());
    advertised_fourccs.erase(std::unique(advertised_fourccs.begin(), advertised_fourccs.end()), advertised_fourccs.end());
    for (const std::uint32_t value : advertised_fourccs) {
        std::printf("stage=capture_fourcc value=0x%08x text=\"%s\"\n",
            value, fourcc_string(value).c_str());
    }
    for (const auto& pair : requirements.dmabuf_formats) {
        std::printf("stage=capture_tuple fourcc=0x%08x text=\"%s\" modifier=0x%016llx\n",
            pair.fourcc, fourcc_string(pair.fourcc).c_str(),
            static_cast<unsigned long long>(pair.modifier));
    }
    capture.stop();

    auto graphics_result = initialize_egl_gl(*render_node);
    if (!graphics_result) {
        std::fprintf(stderr, "stage=egl_gl_init failed: %s\n", graphics_result.error().c_str());
        return 1;
    }
    auto graphics = std::move(*graphics_result);

    const GLenum requested_internal_format = options->bgra8 ? GL_BGRA8_EXT :
        (options->rgb8 ? GL_RGB8 : GL_RGBA8);
    const GLenum external_format = options->bgra8 ? GL_BGRA_EXT :
        (options->rgb8 ? GL_RGB : GL_RGBA);
    if (options->bgra8) {
        const bool extension_present = gl_has_extension("GL_EXT_texture_format_BGRA8888");
        std::printf("stage=gl_extension name=GL_EXT_texture_format_BGRA8888 present=%s\n",
            extension_present ? "yes" : "no");
        if (!extension_present) {
            std::fprintf(stderr, "BGRA8 probe stopped: required GL_EXT_texture_format_BGRA8888 is not advertised\n");
            return 4;
        }
    }

    glGenTextures(1, &graphics->texture);
    if (graphics->texture == 0U) {
        std::fprintf(stderr, "glGenTextures returned zero\n");
        return 1;
    }
    glBindTexture(GL_TEXTURE_2D, graphics->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (requirements.width == 0U || requirements.height == 0U ||
        requirements.width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) ||
        requirements.height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        std::fprintf(stderr, "capture dimensions exceed positive GLsizei range\n");
        return 1;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(requested_internal_format),
        static_cast<GLsizei>(requirements.width), static_cast<GLsizei>(requirements.height),
        0, external_format, GL_UNSIGNED_BYTE, nullptr);
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        std::fprintf(stderr, "glTexImage2D(GL_RGBA8, null pixels) failed GL error=0x%x\n", gl_error);
        return 1;
    }
    (void)glFinish();
    GLint texture_width = 0;
    GLint texture_height = 0;
    GLint queried_internal_format = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texture_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &queried_internal_format);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR || texture_width != static_cast<GLint>(requirements.width) ||
        texture_height != static_cast<GLint>(requirements.height) ||
        queried_internal_format != static_cast<GLint>(requested_internal_format)) {
        std::fprintf(stderr, "GL texture validation failed dimensions=%dx%d internal_format=0x%x error=0x%x\n",
            texture_width, texture_height, queried_internal_format, gl_error);
        return 1;
    }
    const char* internal_format_name = options->bgra8 ? "GL_BGRA8_EXT" :
        (options->rgb8 ? "GL_RGB8" : "GL_RGBA8");
    std::printf("stage=gl_owned_texture passed texture=%u dimensions=%dx%d internal_format=%s pixel_data=none\n",
        graphics->texture, texture_width, texture_height, internal_format_name);

    const EGLint image_attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
    graphics->image = graphics->create_image(graphics->display, graphics->context,
        EGL_GL_TEXTURE_2D_KHR, reinterpret_cast<EGLClientBuffer>(static_cast<std::uintptr_t>(graphics->texture)),
        image_attributes);
    if (graphics->image == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr, "eglCreateImageKHR(GL texture) failed EGL error=0x%x\n", eglGetError());
        return 1;
    }

    const auto export_query = egl_proc<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>("eglExportDMABUFImageQueryMESA");
    const auto export_image = egl_proc<PFNEGLEXPORTDMABUFIMAGEMESAPROC>("eglExportDMABUFImageMESA");
    EGLint fourcc = 0;
    EGLint plane_count = 0;
    EGLuint64KHR modifier = 0;
    if (export_query(graphics->display, graphics->image, &fourcc, &plane_count, &modifier) != EGL_TRUE) {
        std::fprintf(stderr, "eglExportDMABUFImageQueryMESA failed EGL error=0x%x\n", eglGetError());
        return 1;
    }
    std::printf("stage=export_query passed fourcc=0x%08x fourcc_text=\"%s\" planes=%d modifier=0x%016llx modifier_valid=%s\n",
        static_cast<std::uint32_t>(fourcc), fourcc_string(static_cast<std::uint32_t>(fourcc)).c_str(),
        plane_count, static_cast<unsigned long long>(modifier),
        modifier == DRM_FORMAT_MOD_INVALID ? "no" : "yes");

    const bool fourcc_advertised = std::any_of(requirements.dmabuf_formats.begin(),
        requirements.dmabuf_formats.end(), [fourcc](const auto& pair) {
            return pair.fourcc == static_cast<std::uint32_t>(fourcc);
        });
    const bool exact_tuple_advertised = std::any_of(requirements.dmabuf_formats.begin(),
        requirements.dmabuf_formats.end(), [fourcc, modifier](const auto& pair) {
            return pair.fourcc == static_cast<std::uint32_t>(fourcc) &&
                pair.modifier == static_cast<std::uint64_t>(modifier);
        });
    std::printf("stage=capture_format_compare fourcc_advertised=%s exact_fourcc_modifier_advertised=%s\n",
        fourcc_advertised ? "yes" : "no", exact_tuple_advertised ? "yes" : "no");

    if (plane_count <= 0 || plane_count > 4) {
        std::fprintf(stderr, "EGL reported unsupported plane count %d (expected 1..4); export not attempted\n", plane_count);
        return 1;
    }
    std::array<EGLint, 4> fds{-1, -1, -1, -1};
    std::array<EGLint, 4> strides{};
    std::array<EGLint, 4> offsets{};
    if (export_image(graphics->display, graphics->image, fds.data(), strides.data(), offsets.data()) != EGL_TRUE) {
        std::fprintf(stderr, "eglExportDMABUFImageMESA failed EGL error=0x%x\n", eglGetError());
        for (const EGLint fd : fds) if (fd >= 0) (void)::close(fd);
        return 1;
    }
    std::array<UniqueFd, 4> owned_fds;
    for (std::size_t i = 0; i < static_cast<std::size_t>(plane_count); ++i) {
        owned_fds[i].reset(fds[i]);
    }
    for (int i = 0; i < plane_count; ++i) {
        if (!owned_fds[static_cast<std::size_t>(i)]) {
            std::fprintf(stderr, "EGL export returned invalid fd for plane %d\n", i);
            return 1;
        }
        std::printf("stage=export_plane index=%d fd=%d stride=%d offset=%d\n",
            i, owned_fds[static_cast<std::size_t>(i)].get(), strides[static_cast<std::size_t>(i)],
            offsets[static_cast<std::size_t>(i)]);
    }
    std::printf("stage=complete capture_started=no nvenc_initialized=no cpu_pixel_access=no\n");
    return exact_tuple_advertised ? 0 : 3;
}
