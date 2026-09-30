#include "capture/capture.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using fluxrec::capture::CaptureRequirements;
using fluxrec::capture::CapturedFrame;
using fluxrec::capture::DmabufImage;
using fluxrec::capture::UniqueFd;
using fluxrec::capture::WaylandCapture;

constexpr int kDefaultCaptureWaitMs = 3000;
constexpr int kFenceWaitMs = 2000;
constexpr unsigned int kDefaultPoolSize = 8U;
constexpr unsigned int kMaximumPoolSize = 16U;
constexpr int kMaximumDurationMs = 5000;
constexpr std::uint32_t kMaximumFrames = 1800U;

struct Options {
    std::string output_name = "DP-1";
    std::string output_file;
    int capture_wait_ms = kDefaultCaptureWaitMs;
    int duration_ms = 0;
    std::uint32_t max_frames = 1U;
    unsigned int pool_slots = kDefaultPoolSize;
    bool gl_owned_control = false;
    bool gl_copy_from_import = false;
    bool cuda_gl_only = false;
};

void print_usage(const char* executable) {
    std::fprintf(stderr,
        "usage: %s [--output NAME] [--capture-wait-ms N] [--duration-ms N] "
        "[--max-frames N] [--pool-slots N] [--gl-owned-control|--gl-copy-from-import] [--cuda-gl-only] [--output-file PATH]\n"
        "Default mode captures and encodes one real DMA-BUF frame. "
        "--duration-ms enables a bounded H.264 run (maximum 5 seconds / 1800 frames). "
        "--gl-owned-control allocates a GL_RGBA8 texture with null pixel data and is single-frame only. "
        "--gl-copy-from-import copies each DMA-BUF texture to a GL-owned NVENC texture. "
        "--cuda-gl-only captures one frame and tests CUDA registration/map/array metadata without NVENC.\n",
        executable);
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    bool max_frames_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help") {
            print_usage(argv[0]);
            return std::nullopt;
        }
        if (arg == "--gl-owned-control") {
            options.gl_owned_control = true;
        } else if (arg == "--gl-copy-from-import") {
            options.gl_copy_from_import = true;
        } else if (arg == "--cuda-gl-only") {
            options.cuda_gl_only = true;
        } else if (i + 1 < argc && (arg == "--output" || arg == "--capture-wait-ms" ||
            arg == "--duration-ms" || arg == "--max-frames" ||
            arg == "--pool-slots" || arg == "--output-file")) {
            const std::string_view value{argv[++i]};
            try {
                if (arg == "--output") options.output_name = value;
                else if (arg == "--output-file") options.output_file = value;
                else if (arg == "--capture-wait-ms") options.capture_wait_ms = std::stoi(std::string(value));
                else if (arg == "--duration-ms") options.duration_ms = std::stoi(std::string(value));
                else if (arg == "--max-frames") {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<std::uint32_t>::max()) throw std::out_of_range("max frames");
                    options.max_frames = static_cast<std::uint32_t>(parsed);
                    max_frames_set = true;
                } else {
                    const unsigned long parsed = std::stoul(std::string(value));
                    if (parsed > std::numeric_limits<unsigned int>::max()) throw std::out_of_range("pool slots");
                    options.pool_slots = static_cast<unsigned int>(parsed);
                }
            } catch (...) {
                std::fprintf(stderr, "invalid value for %.*s\n", static_cast<int>(arg.size()), arg.data());
                return std::nullopt;
            }
        } else {
            print_usage(argv[0]);
            return std::nullopt;
        }
    }
    if (options.output_file.empty() && !options.cuda_gl_only) {
        std::fprintf(stderr, "--output-file is required\n");
        return std::nullopt;
    }
    if (options.cuda_gl_only && (!options.output_file.empty() || options.gl_owned_control ||
        options.gl_copy_from_import || options.duration_ms != 0 || max_frames_set)) {
        std::fprintf(stderr, "--cuda-gl-only is a single imported-texture test; do not combine it with encode/output options\n");
        return std::nullopt;
    }
    if (options.capture_wait_ms < 1 || options.capture_wait_ms > 10000) {
        std::fprintf(stderr, "--capture-wait-ms must be between 1 and 10000\n");
        return std::nullopt;
    }
    if (options.duration_ms != 0 &&
        (options.duration_ms < 100 || options.duration_ms > kMaximumDurationMs)) {
        std::fprintf(stderr, "--duration-ms must be between 100 and %d\n", kMaximumDurationMs);
        return std::nullopt;
    }
    if (options.duration_ms != 0 && !max_frames_set) options.max_frames = kMaximumFrames;
    if (options.max_frames == 0U || options.max_frames > kMaximumFrames) {
        std::fprintf(stderr, "--max-frames must be between 1 and %u\n", kMaximumFrames);
        return std::nullopt;
    }
    if (options.pool_slots < 3U || options.pool_slots > kMaximumPoolSize) {
        std::fprintf(stderr, "--pool-slots must be between 3 and %u\n", kMaximumPoolSize);
        return std::nullopt;
    }
    if (options.duration_ms == 0 && max_frames_set && options.max_frames != 1U) {
        std::fprintf(stderr, "--max-frames greater than 1 requires --duration-ms\n");
        return std::nullopt;
    }
    if (options.gl_owned_control && options.duration_ms != 0) {
        std::fprintf(stderr, "--gl-owned-control is a one-frame control and cannot be combined with --duration-ms\n");
        return std::nullopt;
    }
    if (options.gl_owned_control && options.gl_copy_from_import) {
        std::fprintf(stderr, "--gl-owned-control and --gl-copy-from-import are mutually exclusive\n");
        return std::nullopt;
    }
    return options;
}

class DynamicLibrary {
public:
    explicit DynamicLibrary(const char* name) : handle_(::dlopen(name, RTLD_NOW | RTLD_LOCAL)) {}
    ~DynamicLibrary() { if (handle_) ::dlclose(handle_); }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] void* symbol(const char* name) const noexcept {
        return handle_ ? ::dlsym(handle_, name) : nullptr;
    }
private:
    void* handle_ = nullptr;
};

template<class T>
T load_symbol(const DynamicLibrary& library, const char* name) {
    void* raw = library.symbol(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

struct PoolSlot {
    std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)> bo{nullptr, gbm_bo_destroy};
    UniqueFd plane_fd;
    DmabufImage image;
};

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

std::optional<PoolSlot> make_pool_slot(gbm_device* device,
                                       const CaptureRequirements& requirements,
                                       std::uint32_t fourcc,
                                       std::uint64_t modifier) {
    PoolSlot slot;
    gbm_bo* raw = gbm_bo_create_with_modifiers2(device, requirements.width,
        requirements.height, fourcc, &modifier, 1U, GBM_BO_USE_RENDERING);
    if (!raw) return std::nullopt;
    slot.bo.reset(raw);
    if (gbm_bo_get_modifier(raw) != modifier || gbm_bo_get_format(raw) != fourcc ||
        gbm_bo_get_plane_count(raw) != 1) return std::nullopt;
    slot.plane_fd.reset(gbm_bo_get_fd_for_plane(raw, 0));
    if (!slot.plane_fd) return std::nullopt;
    slot.image.width = requirements.width;
    slot.image.height = requirements.height;
    slot.image.fourcc = fourcc;
    slot.image.modifier = modifier;
    slot.image.planes.push_back({slot.plane_fd.get(), gbm_bo_get_offset(raw, 0),
                                 gbm_bo_get_stride_for_plane(raw, 0)});
    return slot;
}

bool allocate_pool(gbm_device* device, const CaptureRequirements& requirements,
                   std::vector<PoolSlot>& slots, std::uint32_t& selected_fourcc,
                   std::uint64_t& selected_modifier, unsigned int requested_slots) {
    const std::array<std::uint32_t, 2> preferred{DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
    for (const std::uint32_t wanted : preferred) {
        for (const auto& pair : requirements.dmabuf_formats) {
            if (pair.fourcc != wanted) continue;
            std::vector<PoolSlot> candidate;
            candidate.reserve(requested_slots);
            for (unsigned int i = 0; i < requested_slots; ++i) {
                auto slot = make_pool_slot(device, requirements, pair.fourcc, pair.modifier);
                if (!slot) break;
                candidate.push_back(std::move(*slot));
            }
            if (candidate.size() != requested_slots) continue;
            slots = std::move(candidate);
            selected_fourcc = pair.fourcc;
            selected_modifier = pair.modifier;
            return true;
        }
    }
    return false;
}

std::string fourcc_string(std::uint32_t value) {
    std::string result(4, ' ');
    for (unsigned int i = 0; i < 4U; ++i) result[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
    return result;
}

bool contains_extension(const char* extensions, std::string_view wanted) {
    if (!extensions) return false;
    std::string_view all{extensions};
    std::size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && all[at] == ' ') ++at;
        const std::size_t end = all.find(' ', at);
        const auto item = all.substr(at, end == std::string_view::npos ? all.size() - at : end - at);
        if (item == wanted) return true;
        if (end == std::string_view::npos) break;
        at = end + 1U;
    }
    return false;
}

template<class T>
T egl_proc(const char* name) {
    const auto raw = eglGetProcAddress(name);
    T result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

struct EglApi {
    PFNEGLQUERYDEVICESEXTPROC query_devices = nullptr;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_dmabuf_formats = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dmabuf_modifiers = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
};

struct GlSlot {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0U; // Imported source, or GL-owned input when not copying.
    GLuint copy_texture = 0U;
    GLuint copy_framebuffer = 0U;
    NV_ENC_INPUT_RESOURCE_OPENGL_TEX nvenc_texture{};
    NV_ENC_REGISTERED_PTR registered = nullptr;
};

struct OpenGlState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLint major = 0;
    EGLint minor = 0;
    bool initialized = false;
    EglApi api;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d = nullptr;
    GLuint copy_program = 0U;
    GLuint copy_vertex_array = 0U;
    GLint copy_sampler_location = -1;
    std::vector<GlSlot> slots;

    ~OpenGlState() {
        if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE) {
            (void)eglMakeCurrent(display, surface, surface, context);
            for (const auto& slot : slots) {
                if (slot.copy_framebuffer != 0U) glDeleteFramebuffers(1, &slot.copy_framebuffer);
                if (slot.copy_texture != 0U) glDeleteTextures(1, &slot.copy_texture);
                if (slot.texture != 0U) glDeleteTextures(1, &slot.texture);
            }
            if (copy_vertex_array != 0U) glDeleteVertexArrays(1, &copy_vertex_array);
            if (copy_program != 0U) glDeleteProgram(copy_program);
            (void)glFinish();
        }
        if (display != EGL_NO_DISPLAY && api.destroy_image) {
            for (const auto& slot : slots) {
                if (slot.image != EGL_NO_IMAGE_KHR) (void)api.destroy_image(display, slot.image);
            }
        }
        if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT) (void)eglDestroyContext(display, context);
        if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE) (void)eglDestroySurface(display, surface);
        if (display != EGL_NO_DISPLAY && initialized) (void)eglTerminate(display);
    }
};

std::expected<GLuint, std::string> compile_gl_shader(GLenum type, const char* source,
                                                     std::string_view label) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0U) return std::unexpected("glCreateShader failed for " + std::string(label));
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    GLint log_size = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_size);
    std::string log(static_cast<std::size_t>(std::max(1, log_size)), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &written, log.data());
    glDeleteShader(shader);
    return std::unexpected("GL " + std::string(label) + " shader compile failed: " + log);
}

std::expected<void, std::string> initialize_copy_pipeline(OpenGlState& gl) {
    static constexpr char vertex_source[] = R"glsl(#version 330 core
void main() {
    vec2 position;
    if (gl_VertexID == 0) position = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) position = vec2(3.0, -1.0);
    else position = vec2(-1.0, 3.0);
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";
    static constexpr char fragment_source[] = R"glsl(#version 330 core
uniform sampler2D source_image;
layout(location = 0) out vec4 output_color;
void main() {
    ivec2 source_pixel = ivec2(gl_FragCoord.xy);
    output_color = texelFetch(source_image, source_pixel, 0);
}
)glsl";
    auto vertex = compile_gl_shader(GL_VERTEX_SHADER, vertex_source, "copy vertex");
    if (!vertex) return std::unexpected(vertex.error());
    auto fragment = compile_gl_shader(GL_FRAGMENT_SHADER, fragment_source, "copy fragment");
    if (!fragment) {
        glDeleteShader(*vertex);
        return std::unexpected(fragment.error());
    }
    gl.copy_program = glCreateProgram();
    if (gl.copy_program == 0U) {
        glDeleteShader(*vertex);
        glDeleteShader(*fragment);
        return std::unexpected("glCreateProgram failed for DMA-BUF copy shader");
    }
    glAttachShader(gl.copy_program, *vertex);
    glAttachShader(gl.copy_program, *fragment);
    glLinkProgram(gl.copy_program);
    glDeleteShader(*vertex);
    glDeleteShader(*fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(gl.copy_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint log_size = 0;
        glGetProgramiv(gl.copy_program, GL_INFO_LOG_LENGTH, &log_size);
        std::string log(static_cast<std::size_t>(std::max(1, log_size)), '\0');
        GLsizei written = 0;
        glGetProgramInfoLog(gl.copy_program, static_cast<GLsizei>(log.size()), &written, log.data());
        return std::unexpected("DMA-BUF copy shader link failed: " + log);
    }
    gl.copy_sampler_location = glGetUniformLocation(gl.copy_program, "source_image");
    if (gl.copy_sampler_location < 0) return std::unexpected("copy shader has no source_image sampler uniform");
    glGenVertexArrays(1, &gl.copy_vertex_array);
    if (gl.copy_vertex_array == 0U || glGetError() != GL_NO_ERROR) {
        return std::unexpected("glGenVertexArrays failed for DMA-BUF copy shader");
    }
    std::printf("stage=gl_copy_pipeline passed shader=GLSL_330 texelFetch=same_coordinate output=RGBA8\n");
    return {};
}

std::expected<std::unique_ptr<OpenGlState>, std::string> initialize_opengl(
    std::string_view render_node, const DmabufImage& sample, bool enable_copy_pipeline) {
    auto gl = std::make_unique<OpenGlState>();
    gl->api.query_devices = egl_proc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
    gl->api.query_device_string = egl_proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
    gl->api.get_platform_display = egl_proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
    gl->api.query_dmabuf_formats = egl_proc<PFNEGLQUERYDMABUFFORMATSEXTPROC>("eglQueryDmaBufFormatsEXT");
    gl->api.query_dmabuf_modifiers = egl_proc<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>("eglQueryDmaBufModifiersEXT");
    gl->api.create_image = egl_proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
    gl->api.destroy_image = egl_proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
    if (!gl->api.query_devices || !gl->api.query_device_string || !gl->api.get_platform_display ||
        !gl->api.query_dmabuf_formats || !gl->api.query_dmabuf_modifiers ||
        !gl->api.create_image || !gl->api.destroy_image) {
        return std::unexpected("required EGL device/modifier/image entry point is absent");
    }

    EGLint device_count = 0;
    if (gl->api.query_devices(0, nullptr, &device_count) != EGL_TRUE || device_count <= 0) {
        return std::unexpected("eglQueryDevicesEXT returned no EGL devices");
    }
    std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(device_count));
    if (gl->api.query_devices(device_count, devices.data(), &device_count) != EGL_TRUE) {
        return std::unexpected("eglQueryDevicesEXT failed to enumerate EGL devices");
    }
    for (const EGLDeviceEXT device : devices) {
        const char* path = gl->api.query_device_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (path && render_node == path) { gl->device = device; break; }
    }
    if (gl->device == EGL_NO_DEVICE_EXT) return std::unexpected("no EGL device matched capture render node");

    gl->display = gl->api.get_platform_display(EGL_PLATFORM_DEVICE_EXT, gl->device, nullptr);
    if (gl->display == EGL_NO_DISPLAY) return std::unexpected("eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT) failed");
    if (eglInitialize(gl->display, &gl->major, &gl->minor) != EGL_TRUE) {
        return std::unexpected("eglInitialize on capture EGL device failed");
    }
    gl->initialized = true;
    const char* display_extensions = eglQueryString(gl->display, EGL_EXTENSIONS);
    if (!contains_extension(display_extensions, "EGL_EXT_image_dma_buf_import") ||
        !contains_extension(display_extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
        return std::unexpected("capture EGL display lacks DMA-BUF import/modifier extensions");
    }
    std::printf("stage=egl_device passed render_node=%.*s egl=%d.%d\n",
        static_cast<int>(render_node.size()), render_node.data(), gl->major, gl->minor);

    EGLint format_count = 0;
    if (gl->api.query_dmabuf_formats(gl->display, 0, nullptr, &format_count) != EGL_TRUE || format_count < 0) {
        return std::unexpected("eglQueryDmaBufFormatsEXT(count) failed");
    }
    std::vector<EGLint> formats(static_cast<std::size_t>(format_count));
    if (gl->api.query_dmabuf_formats(gl->display, format_count, formats.data(), &format_count) != EGL_TRUE ||
        format_count > static_cast<EGLint>(formats.size())) {
        return std::unexpected("eglQueryDmaBufFormatsEXT(list) failed or exceeded capacity");
    }
    if (std::find(formats.begin(), formats.end(), static_cast<EGLint>(sample.fourcc)) == formats.end()) {
        return std::unexpected("EGL does not advertise captured DRM fourcc");
    }
    EGLint modifier_count = 0;
    if (gl->api.query_dmabuf_modifiers(gl->display, static_cast<EGLint>(sample.fourcc),
            0, nullptr, nullptr, &modifier_count) != EGL_TRUE || modifier_count < 0) {
        return std::unexpected("eglQueryDmaBufModifiersEXT(count) failed");
    }
    std::vector<EGLuint64KHR> modifiers(static_cast<std::size_t>(modifier_count));
    std::vector<EGLBoolean> external_only(static_cast<std::size_t>(modifier_count));
    if (gl->api.query_dmabuf_modifiers(gl->display, static_cast<EGLint>(sample.fourcc),
            modifier_count, modifiers.data(), external_only.data(), &modifier_count) != EGL_TRUE ||
        modifier_count > static_cast<EGLint>(modifiers.size())) {
        return std::unexpected("eglQueryDmaBufModifiersEXT(list) failed or exceeded capacity");
    }
    const auto modifier = std::find(modifiers.begin(), modifiers.begin() + modifier_count, sample.modifier);
    if (modifier == modifiers.begin() + modifier_count) {
        return std::unexpected("EGL does not advertise the capture modifier for its fourcc");
    }
    const auto modifier_index = static_cast<std::size_t>(modifier - modifiers.begin());
    if (external_only[modifier_index] == EGL_TRUE) {
        return std::unexpected("capture modifier is external-only; NVENC accepts only GL_TEXTURE_2D/RECTANGLE");
    }
    std::printf("stage=egl_modifier passed fourcc=0x%08x modifier=0x%016llx external_only=no\n",
        sample.fourcc, static_cast<unsigned long long>(sample.modifier));

    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) return std::unexpected("eglBindAPI(EGL_OPENGL_API) failed");
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (eglChooseConfig(gl->display, config_attrs, &gl->config, 1, &config_count) != EGL_TRUE || config_count != 1) {
        return std::unexpected("eglChooseConfig did not find a desktop OpenGL pbuffer config");
    }
    const EGLint pbuffer_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    gl->surface = eglCreatePbufferSurface(gl->display, gl->config, pbuffer_attrs);
    if (gl->surface == EGL_NO_SURFACE) return std::unexpected("eglCreatePbufferSurface failed");

    const std::array<std::array<EGLint, 5>, 3> context_candidates{{
        {{EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_NONE}},
        {{EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE}},
        {{EGL_NONE, 0, EGL_NONE, 0, EGL_NONE}},
    }};
    for (const auto& attrs : context_candidates) {
        gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT, attrs.data());
        if (gl->context != EGL_NO_CONTEXT) break;
        (void)eglGetError();
    }
    if (gl->context == EGL_NO_CONTEXT) return std::unexpected("eglCreateContext failed for OpenGL 4.5, 3.3, and default contexts");
    if (eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context) != EGL_TRUE) {
        return std::unexpected("eglMakeCurrent for NVENC OpenGL context failed");
    }
    gl->image_target_texture_2d = egl_proc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
    if (!gl->image_target_texture_2d) return std::unexpected("glEGLImageTargetTexture2DOES entry point is absent");
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!vendor || !renderer || !version) return std::unexpected("OpenGL context returned empty vendor/renderer/version");
    std::printf("stage=opengl_context passed vendor=%s renderer=%s version=%s\n", vendor, renderer, version);
    if (enable_copy_pipeline) {
        auto copy_pipeline = initialize_copy_pipeline(*gl);
        if (!copy_pipeline) return std::unexpected("GL copy pipeline: " + copy_pipeline.error());
    }
    return gl;
}

std::expected<EGLImageKHR, std::string> import_egl_image(OpenGlState& gl,
                                                         const DmabufImage& image,
                                                         int fd) {
    if (image.planes.size() != 1U) return std::unexpected("probe requires one-plane AR24/XR24 capture buffers");
    const auto& plane = image.planes[0];
    if (image.width > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        plane.offset > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
        plane.stride > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max())) {
        return std::unexpected("DMA-BUF plane metadata exceeds EGLint range");
    }
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(image.width),
        EGL_HEIGHT, static_cast<EGLint>(image.height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(image.fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(image.modifier & 0xffffffffULL),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>((image.modifier >> 32U) & 0xffffffffULL),
        EGL_NONE,
    };
    const EGLImageKHR result = gl.api.create_image(gl.display, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (result == EGL_NO_IMAGE_KHR) {
        const EGLint error = eglGetError();
        return std::unexpected("eglCreateImageKHR failed EGL error=" + std::to_string(error));
    }
    std::printf("stage=egl_import passed width=%u height=%u fourcc=0x%08x pitch=%u offset=%u modifier=0x%016llx\n",
        image.width, image.height, image.fourcc, plane.stride, plane.offset,
        static_cast<unsigned long long>(image.modifier));
    return result;
}

bool wait_for_dma_buf_read_fence(int dma_buf_fd) {
    dma_buf_export_sync_file export_info{};
    export_info.flags = DMA_BUF_SYNC_READ;
    export_info.fd = -1;
    if (::ioctl(dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_info) < 0) {
        std::fprintf(stderr, "stage=fence_export failed errno=%d (%s)\n", errno, std::strerror(errno));
        return false;
    }
    UniqueFd sync_fd{export_info.fd};
    if (!sync_fd) return false;
    pollfd descriptor{.fd = sync_fd.get(), .events = POLLIN, .revents = 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFenceWaitMs);
    int result = 0;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { result = 0; break; }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(1, remaining));
        result = ::poll(&descriptor, 1, wait_ms);
        if (result < 0 && errno == EINTR) continue;
        break;
    }
    if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
        std::fprintf(stderr, "stage=fence_wait failed result=%d errno=%d (%s)\n", result, errno, std::strerror(errno));
        return false;
    }
    return true;
}

bool report_gl_texture_state(const char* stage, GLuint texture,
                             std::uint32_t expected_width, std::uint32_t expected_height) {
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint width = 0;
    GLint height = 0;
    GLint internal_format = 0;
    GLint min_filter = 0;
    GLint mag_filter = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
    const GLenum error = glGetError();
    std::printf("stage=%s texture=%u target=GL_TEXTURE_2D dimensions=%dx%d internal_format=0x%x min_filter=0x%x mag_filter=0x%x wrap_s=0x%x wrap_t=0x%x gl_error=0x%x\n",
        stage, texture, width, height, internal_format, min_filter, mag_filter, wrap_s, wrap_t, error);
    return error == GL_NO_ERROR && width == static_cast<GLint>(expected_width) &&
        height == static_cast<GLint>(expected_height);
}

std::expected<void, std::string> bind_slot_texture(OpenGlState& gl, GlSlot& slot,
                                                   CapturedFrame& frame,
                                                   const DmabufImage& expected,
                                                   std::uint32_t expected_fourcc,
                                                   std::uint64_t expected_modifier,
                                                   bool gl_owned_control,
                                                   bool gl_copy_from_import) {
    const DmabufImage& source = frame.lease.image();
    if (source.width != expected.width || source.height != expected.height ||
        source.fourcc != expected_fourcc || source.modifier != expected_modifier ||
        source.planes.size() != 1U) {
        return std::unexpected("leased DMA-BUF metadata differs from selected exact-modifier pool");
    }
    glGenTextures(1, &slot.texture);
    if (slot.texture == 0U) return std::unexpected("glGenTextures returned zero");
    glBindTexture(GL_TEXTURE_2D, slot.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum setup_error = glGetError();
    if (setup_error != GL_NO_ERROR) {
        return std::unexpected("GL texture setup failed GL error=" + std::to_string(setup_error));
    }
    if (gl_owned_control) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
            static_cast<GLsizei>(source.width), static_cast<GLsizei>(source.height),
            0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        const GLenum allocation_error = glGetError();
        if (allocation_error != GL_NO_ERROR) {
            return std::unexpected("glTexImage2D(GL_RGBA8, null pixels) failed GL error=" +
                                   std::to_string(allocation_error));
        }
    } else {
        auto duplicated = frame.lease.duplicate_plane_fd(0U);
        if (!duplicated) return std::unexpected("DMA-BUF duplicate: " + duplicated.error().message);
        if (!wait_for_dma_buf_read_fence(duplicated->get())) {
            return std::unexpected("producer DMA-BUF read fence did not signal");
        }
        auto image = import_egl_image(gl, source, duplicated->get());
        if (!image) return std::unexpected(image.error());
        slot.image = *image;
        gl.image_target_texture_2d(GL_TEXTURE_2D, slot.image);
        const GLenum import_error = glGetError();
        if (import_error != GL_NO_ERROR) {
            return std::unexpected("glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) failed GL error=" +
                                   std::to_string(import_error));
        }
    }
    if (!report_gl_texture_state(gl_owned_control ? "gl_owned_texture" : "gl_dmabuf_source_texture",
                                 slot.texture, source.width, source.height)) {
        return std::unexpected("GL texture query did not report the expected dimensions");
    }
    GLuint nvenc_texture = slot.texture;
    if (gl_copy_from_import) {
        glGenTextures(1, &slot.copy_texture);
        if (slot.copy_texture == 0U) return std::unexpected("glGenTextures returned zero for copy output");
        glBindTexture(GL_TEXTURE_2D, slot.copy_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
            static_cast<GLsizei>(source.width), static_cast<GLsizei>(source.height),
            0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        const GLenum allocation_error = glGetError();
        if (allocation_error != GL_NO_ERROR) {
            return std::unexpected("glTexImage2D(GL_RGBA8, null copy target) failed GL error=" +
                                   std::to_string(allocation_error));
        }
        glGenFramebuffers(1, &slot.copy_framebuffer);
        if (slot.copy_framebuffer == 0U) return std::unexpected("glGenFramebuffers returned zero for copy output");
        glBindFramebuffer(GL_FRAMEBUFFER, slot.copy_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, slot.copy_texture, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum framebuffer_error = glGetError();
        glBindFramebuffer(GL_FRAMEBUFFER, 0U);
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE || framebuffer_error != GL_NO_ERROR) {
            return std::unexpected("copy output FBO incomplete status=" +
                std::to_string(framebuffer_status) + " GL error=" + std::to_string(framebuffer_error));
        }
        if (!report_gl_texture_state("gl_copy_output_texture", slot.copy_texture,
                                     source.width, source.height)) {
            return std::unexpected("GL copy output texture query failed");
        }
        nvenc_texture = slot.copy_texture;
    }
    slot.nvenc_texture.texture = nvenc_texture;
    slot.nvenc_texture.target = GL_TEXTURE_2D;
    return {};
}

// Small dynamically loaded CUDA Driver API surface for the isolated
// --cuda-gl-only experiment. Keep the ABI declarations local so the probe
// builds without a CUDA toolkit dependency.
using CUresult = int;
using CUdevice = int;
struct CUctx_st;
struct CUarray_st;
struct CUgraphicsResource_st;
using CUcontext = CUctx_st*;
using CUarray = CUarray_st*;
using CUgraphicsResource = CUgraphicsResource_st*;
using CUstream = void*;
enum CudaArrayFormat : int { kCudaArrayUnsignedInt8 = 0x01 };
constexpr unsigned int kCudaGraphicsRegisterFlagsReadOnly = 0x01U;
struct CudaArray3DDescriptor {
    std::size_t width;
    std::size_t height;
    std::size_t depth;
    CudaArrayFormat format;
    unsigned int channels;
    unsigned int flags;
};
static_assert(sizeof(CudaArray3DDescriptor) == 40U);
static_assert(offsetof(CudaArray3DDescriptor, channels) == sizeof(std::size_t) * 3U + sizeof(int));

struct CudaGlApi {
    DynamicLibrary library{"libcuda.so.1"};
    CUresult (*init)(unsigned int) = nullptr;
    CUresult (*gl_get_devices)(unsigned int*, CUdevice*, unsigned int, int) = nullptr;
    CUresult (*context_create)(CUcontext*, unsigned int, CUdevice) = nullptr;
    CUresult (*context_destroy)(CUcontext) = nullptr;
    CUresult (*register_image)(CUgraphicsResource*, GLuint, GLenum, unsigned int) = nullptr;
    CUresult (*map_resources)(unsigned int, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*get_mapped_array)(CUarray*, CUgraphicsResource, unsigned int, unsigned int) = nullptr;
    CUresult (*array_get_descriptor)(CudaArray3DDescriptor*, CUarray) = nullptr;
    CUresult (*unmap_resources)(unsigned int, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*unregister_resource)(CUgraphicsResource) = nullptr;
    CUresult (*get_error_name)(CUresult, const char**) = nullptr;
    CUresult (*get_error_string)(CUresult, const char**) = nullptr;

    CudaGlApi() {
        init = load_symbol<decltype(init)>(library, "cuInit");
        gl_get_devices = load_symbol<decltype(gl_get_devices)>(library, "cuGLGetDevices_v2");
        context_create = load_symbol<decltype(context_create)>(library, "cuCtxCreate_v2");
        context_destroy = load_symbol<decltype(context_destroy)>(library, "cuCtxDestroy_v2");
        register_image = load_symbol<decltype(register_image)>(library, "cuGraphicsGLRegisterImage");
        map_resources = load_symbol<decltype(map_resources)>(library, "cuGraphicsMapResources");
        get_mapped_array = load_symbol<decltype(get_mapped_array)>(library, "cuGraphicsSubResourceGetMappedArray");
        array_get_descriptor = load_symbol<decltype(array_get_descriptor)>(library, "cuArray3DGetDescriptor_v2");
        unmap_resources = load_symbol<decltype(unmap_resources)>(library, "cuGraphicsUnmapResources");
        unregister_resource = load_symbol<decltype(unregister_resource)>(library, "cuGraphicsUnregisterResource");
        get_error_name = load_symbol<decltype(get_error_name)>(library, "cuGetErrorName");
        get_error_string = load_symbol<decltype(get_error_string)>(library, "cuGetErrorString");
    }

    [[nodiscard]] bool available() const noexcept {
        return library && init && gl_get_devices && context_create && context_destroy &&
            register_image && map_resources && get_mapped_array && array_get_descriptor &&
            unmap_resources && unregister_resource && get_error_name && get_error_string;
    }

    [[nodiscard]] std::string describe(CUresult code) const {
        const char* name = nullptr;
        const char* message = nullptr;
        if (get_error_name) (void)get_error_name(code, &name);
        if (get_error_string) (void)get_error_string(code, &message);
        std::string result = "code=" + std::to_string(code);
        if (name) result += " name=" + std::string(name);
        if (message) result += " message=\"" + std::string(message) + "\"";
        return result;
    }
};

struct CudaGlState {
    CudaGlApi api;
    CUcontext context = nullptr;
    CUgraphicsResource resource = nullptr;
    bool mapped = false;

    [[nodiscard]] bool cleanup_resource() {
        if (resource && mapped) {
            const CUresult code = api.unmap_resources(1U, &resource, nullptr);
            if (code != 0) {
                std::fprintf(stderr, "stage=cuda_gl_unmap failed %s\n", api.describe(code).c_str());
                return false;
            }
            mapped = false;
            std::printf("stage=cuda_gl_unmap passed\n");
        }
        if (resource) {
            const CUresult code = api.unregister_resource(resource);
            if (code != 0) {
                std::fprintf(stderr, "stage=cuda_gl_unregister failed %s\n", api.describe(code).c_str());
                return false;
            }
            resource = nullptr;
            std::printf("stage=cuda_gl_unregister passed\n");
        }
        return true;
    }

    [[nodiscard]] bool destroy_context() {
        if (!context) return true;
        const CUresult code = api.context_destroy(context);
        if (code != 0) {
            std::fprintf(stderr, "stage=cuda_gl_context_destroy failed %s\n", api.describe(code).c_str());
            return false;
        }
        context = nullptr;
        return true;
    }

    ~CudaGlState() {
        if (!cleanup_resource()) std::fprintf(stderr, "stage=cuda_gl_cleanup failed during teardown\n");
        if (!destroy_context()) std::fprintf(stderr, "stage=cuda_gl_context_cleanup failed during teardown\n");
    }
};

enum class CudaGlProbeResult { passed, failed };

[[noreturn]] void cuda_gl_fail_stop(const char* reason) {
    std::fprintf(stderr, "stage=cuda_gl_fatal_cleanup %s; capture lease retained until process exit\n", reason);
    std::fflush(stderr);
    ::_exit(EXIT_FAILURE);
}

bool cuda_gl_init(CudaGlState& state) {
    if (!state.api.available()) {
        std::fprintf(stderr, "stage=cuda_gl_setup failed: libcuda.so.1 lacks required Driver API symbols\n");
        return false;
    }
    CUresult code = state.api.init(0U);
    if (code != 0) {
        std::fprintf(stderr, "stage=cuda_gl_init failed %s\n", state.api.describe(code).c_str());
        return false;
    }
    CUdevice device = 0;
    unsigned int count = 0U;
    constexpr int kCuGlDeviceListAll = 1;
    code = state.api.gl_get_devices(&count, &device, 1U, kCuGlDeviceListAll);
    if (code != 0 || count != 1U) {
        std::fprintf(stderr, "stage=cuda_gl_device failed result=%s associated_device_count=%u\n",
            state.api.describe(code).c_str(), count);
        return false;
    }
    std::printf("stage=cuda_gl_device passed device=%d associated_devices=%u\n", device, count);
    code = state.api.context_create(&state.context, 0U, device);
    if (code != 0 || !state.context) {
        std::fprintf(stderr, "stage=cuda_gl_context_create failed %s\n", state.api.describe(code).c_str());
        return false;
    }
    std::printf("stage=cuda_gl_context_create passed device=%d\n", device);
    return true;
}

bool cuda_gl_probe_texture(CudaGlState& state, GLuint texture, unsigned int width,
                          unsigned int height, const char* label) {
    CUresult code = state.api.register_image(&state.resource, texture, GL_TEXTURE_2D,
        kCudaGraphicsRegisterFlagsReadOnly);
    if (code != 0 || !state.resource) {
        std::fprintf(stderr, "stage=%s_register failed %s texture=%u target=GL_TEXTURE_2D flags=CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY\n",
            label, state.api.describe(code).c_str(), texture);
        return false;
    }
    std::printf("stage=%s_register passed texture=%u target=GL_TEXTURE_2D flags=CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY\n",
        label, texture);

    code = state.api.map_resources(1U, &state.resource, nullptr);
    if (code != 0) {
        std::fprintf(stderr, "stage=%s_map failed %s\n", label, state.api.describe(code).c_str());
        return false;
    }
    state.mapped = true;
    CUarray array = nullptr;
    code = state.api.get_mapped_array(&array, state.resource, 0U, 0U);
    if (code != 0 || !array) {
        std::fprintf(stderr, "stage=%s_get_mapped_array failed %s\n", label, state.api.describe(code).c_str());
        return false;
    }
    CudaArray3DDescriptor descriptor{};
    code = state.api.array_get_descriptor(&descriptor, array);
    if (code != 0) {
        std::fprintf(stderr, "stage=%s_array_descriptor failed %s\n", label, state.api.describe(code).c_str());
        return false;
    }
    const bool valid = descriptor.width == width && descriptor.height == height &&
        descriptor.depth == 0U && descriptor.format == kCudaArrayUnsignedInt8 &&
        descriptor.channels == 4U;
    std::printf("stage=%s_array_descriptor width=%zu height=%zu depth=%zu format=0x%x channels=%u flags=0x%x expected=UINT8x4 result=%s\n",
        label, descriptor.width, descriptor.height, descriptor.depth,
        static_cast<unsigned int>(descriptor.format), descriptor.channels, descriptor.flags,
        valid ? "pass" : "fail");
    return valid;
}

bool clear_gl_errors(const char* label) {
    for (unsigned int i = 0U; i < 32U; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            if (i != 0U) std::printf("stage=%s_gl_errors cleared=%u\n", label, i);
            return true;
        }
        std::fprintf(stderr, "stage=%s_gl_errors inherited=0x%x\n", label, error);
    }
    std::fprintf(stderr, "stage=%s_gl_errors failed: GL error queue did not drain\n", label);
    return false;
}

bool check_gl_call(const char* label) {
    bool passed = true;
    for (unsigned int i = 0U; i < 32U; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        std::fprintf(stderr, "stage=cuda_gl_control_call call=%s error=0x%x\n", label, error);
        passed = false;
        if (i == 31U) std::fprintf(stderr, "stage=cuda_gl_control_call call=%s error_queue_not_drained\n", label);
    }
    if (passed) std::printf("stage=cuda_gl_control_call call=%s gl_error=0x0\n", label);
    return passed;
}

bool verify_current_gl_context(OpenGlState& gl, const char* label) {
    const bool errors_drained = clear_gl_errors(label);
    const EGLDisplay current_display = eglGetCurrentDisplay();
    const EGLContext current_context = eglGetCurrentContext();
    const EGLSurface current_draw = eglGetCurrentSurface(EGL_DRAW);
    const bool current = current_display == gl.display && current_context == gl.context &&
        current_draw == gl.surface;
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const bool gl_query_ok = check_gl_call("verify_current_context_glGetString");
    std::printf("stage=%s_current_gl_context display_match=%s context_match=%s draw_surface_match=%s vendor=%s renderer=%s version=%s\n",
        label, current_display == gl.display ? "yes" : "no",
        current_context == gl.context ? "yes" : "no", current_draw == gl.surface ? "yes" : "no",
        vendor ? vendor : "null", renderer ? renderer : "null", version ? version : "null");
    return current && errors_drained && gl_query_ok && vendor && renderer && version;
}

bool cuda_gl_owned_control(CudaGlState& state, OpenGlState& gl, unsigned int width,
                           unsigned int height, GLint internal_format, const char* label) {
    if (!verify_current_gl_context(gl, label)) {
        std::fprintf(stderr, "stage=%s_control failed: EGL GL context is not current\n", label);
        return false;
    }
    GLuint texture = 0U;
    glGenTextures(1, &texture);
    if (!check_gl_call("glGenTextures") || !texture) {
        std::fprintf(stderr, "stage=%s_control_texture failed glGenTextures returned %u\n", label, texture);
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    if (!check_gl_call("glBindTexture")) goto gl_failure;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    if (!check_gl_call("glTexParameteri_MIN_FILTER")) goto gl_failure;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!check_gl_call("glTexParameteri_MAG_FILTER")) goto gl_failure;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    if (!check_gl_call("glTexParameteri_WRAP_S")) goto gl_failure;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!check_gl_call("glTexParameteri_WRAP_T")) goto gl_failure;
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, static_cast<GLsizei>(width),
        static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (!check_gl_call("glTexImage2D")) goto gl_failure;
    {
        GLint actual_width = 0, actual_height = 0, actual_internal = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_width);
        const bool width_query_ok = check_gl_call("glGetTexLevelParameteriv_WIDTH");
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_height);
        const bool height_query_ok = check_gl_call("glGetTexLevelParameteriv_HEIGHT");
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &actual_internal);
        const bool format_query_ok = check_gl_call("glGetTexLevelParameteriv_INTERNAL_FORMAT");
        std::printf("stage=%s_control_texture texture=%u target=GL_TEXTURE_2D dimensions=%dx%d internal_format=0x%x requested_internal_format=0x%x\n",
            label, texture, actual_width, actual_height, actual_internal, internal_format);
        if (!width_query_ok || !height_query_ok || !format_query_ok ||
            actual_width != static_cast<GLint>(width) || actual_height != static_cast<GLint>(height)) {
            goto gl_failure;
        }
    }
    {
        const bool control_ok = cuda_gl_probe_texture(state, texture, width, height, label);
        if (!state.cleanup_resource()) cuda_gl_fail_stop("could not clean up GL-owned CUDA control resource");
        glDeleteTextures(1, &texture);
        if (!check_gl_call("glDeleteTextures_control")) cuda_gl_fail_stop("could not delete CUDA control texture");
        std::printf("stage=%s_control result=%s storage_internal_format=0x%x pixel_data=null pixel_readback=no\n",
            label, control_ok ? "pass" : "fail", internal_format);
        return control_ok;
    }
gl_failure:
    glDeleteTextures(1, &texture);
    if (!check_gl_call("glDeleteTextures_after_control_setup_failure")) {
        cuda_gl_fail_stop("could not delete partially initialized CUDA control texture");
    }
    return false;
}

void destroy_cuda_only_import(OpenGlState& gl, GlSlot& slot, CapturedFrame& frame) {
    // CUDA has already unmapped/unregistered the imported image. Finish any
    // prior GL work, then detach both GL references before returning the
    // capture slot to Wayland.
    if (eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context) != EGL_TRUE) {
        cuda_gl_fail_stop("could not restore GL context before imported EGLImage teardown");
    }
    (void)clear_gl_errors("before_imported_gl_teardown");
    glFinish();
    if (!check_gl_call("glFinish_before_imported_eglimage_teardown")) {
        cuda_gl_fail_stop("glFinish failed before imported EGLImage teardown");
    }
    if (slot.texture != 0U) {
        glDeleteTextures(1, &slot.texture);
        slot.texture = 0U;
    }
    if (slot.image != EGL_NO_IMAGE_KHR) {
        if (gl.api.destroy_image(gl.display, slot.image) != EGL_TRUE) {
            cuda_gl_fail_stop("eglDestroyImageKHR failed before source lease release");
        }
        slot.image = EGL_NO_IMAGE_KHR;
    }
    if (!check_gl_call("glDeleteTextures_imported")) cuda_gl_fail_stop("GL texture deletion failed before source lease release");
    frame.lease.release_after_gpu();
}

int run_cuda_gl_only(OpenGlState& gl, CapturedFrame& frame, GlSlot& slot,
                     unsigned int width, unsigned int height) {
    CudaGlState cuda;
    if (!cuda_gl_init(cuda)) {
        if (!cuda.destroy_context()) cuda_gl_fail_stop("could not destroy CUDA context after setup failure");
        destroy_cuda_only_import(gl, slot, frame);
        return 1;
    }
    if (!verify_current_gl_context(gl, "after_cuda_context_create")) {
        if (!cuda.destroy_context()) cuda_gl_fail_stop("could not destroy CUDA context after GL-context check failed");
        destroy_cuda_only_import(gl, slot, frame);
        return 1;
    }
    // Run both owned controls before touching the imported texture. This makes
    // their GL setup errors independent of CUDA's response to imported storage.
    const bool rgba8_control_ok = cuda_gl_owned_control(cuda, gl, width, height,
        GL_RGBA8, "cuda_gl_owned_rgba8");
    const bool unsized_rgba_control_ok = cuda_gl_owned_control(cuda, gl, width, height,
        GL_RGBA, "cuda_gl_owned_unsized_rgba");
    const bool imported_ok = cuda_gl_probe_texture(cuda, slot.texture, width, height, "cuda_gl_imported");
    if (!cuda.cleanup_resource()) cuda_gl_fail_stop("could not clean up imported CUDA resource");
    if (!cuda.destroy_context()) cuda_gl_fail_stop("could not destroy CUDA context while source lease is held");
    const std::uint64_t sequence = frame.sequence;
    destroy_cuda_only_import(gl, slot, frame);
    if (imported_ok) {
        std::printf("probe_result=cuda_gl_imported_array_passed source_sequence=%llu dimensions=%ux%u pixel_readback=no nvenc=no\n",
            static_cast<unsigned long long>(sequence), width, height);
        return 0;
    }
    std::printf("probe_result=cuda_gl_imported_failed rgba8_control=%s unsized_rgba_control=%s source_sequence=%llu pixel_readback=no nvenc=no\n",
        rgba8_control_ok ? "passed" : "failed", unsized_rgba_control_ok ? "passed" : "failed",
        static_cast<unsigned long long>(sequence));
    return (rgba8_control_ok || unsized_rgba_control_ok) ? 2 : 1;
}

bool copy_imported_texture_to_owned(OpenGlState& gl, const GlSlot& slot,
                                    unsigned int width, unsigned int height) {
    if (gl.copy_program == 0U || gl.copy_vertex_array == 0U ||
        slot.texture == 0U || slot.copy_texture == 0U || slot.copy_framebuffer == 0U ||
        slot.texture == slot.copy_texture) {
        std::fprintf(stderr, "stage=gl_gpu_copy failed: copy pipeline or distinct source/output is missing\n");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, slot.copy_framebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(gl.copy_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, slot.texture);
    glUniform1i(gl.copy_sampler_location, 0);
    glBindVertexArray(gl.copy_vertex_array);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const GLenum draw_error = glGetError();
    // Keep the DMA-BUF lease until the GPU has finished sampling the imported
    // texture and writing the GL-owned NVENC input texture.
    glFinish();
    const GLenum finish_error = glGetError();
    glBindVertexArray(0U);
    glUseProgram(0U);
    glBindFramebuffer(GL_FRAMEBUFFER, 0U);
    const GLenum cleanup_error = glGetError();
    if (draw_error != GL_NO_ERROR || finish_error != GL_NO_ERROR || cleanup_error != GL_NO_ERROR) {
        std::fprintf(stderr,
            "stage=gl_gpu_copy failed draw_error=0x%x finish_error=0x%x cleanup_error=0x%x dimensions=%ux%u\n",
            draw_error, finish_error, cleanup_error, width, height);
        if (finish_error != GL_NO_ERROR) {
            std::fprintf(stderr, "stage=gl_gpu_copy completion_unconfirmed; terminating with source lease retained\n");
            std::fflush(stderr);
            ::_exit(EXIT_FAILURE);
        }
        return false;
    }
    return true;
}

struct NvencState {
    DynamicLibrary library{"libnvidia-encode.so.1"};
    NV_ENCODE_API_FUNCTION_LIST functions{};
    void* encoder = nullptr;
    std::vector<NV_ENC_REGISTERED_PTR> registered;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;

    ~NvencState() {
        if (!encoder) return;
        if (mapped && functions.nvEncUnmapInputResource) (void)functions.nvEncUnmapInputResource(encoder, mapped);
        if (functions.nvEncUnregisterResource) {
            for (const auto resource : registered) if (resource) (void)functions.nvEncUnregisterResource(encoder, resource);
        }
        if (bitstream && functions.nvEncDestroyBitstreamBuffer) (void)functions.nvEncDestroyBitstreamBuffer(encoder, bitstream);
        if (functions.nvEncDestroyEncoder) (void)functions.nvEncDestroyEncoder(encoder);
    }
};

std::expected<std::unique_ptr<NvencState>, std::string> initialize_nvenc(
    unsigned int width, unsigned int height) {
    auto state = std::make_unique<NvencState>();
    if (!state->library) return std::unexpected("dlopen(libnvidia-encode.so.1) failed");
    using CreateInstanceFn = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto create_instance = load_symbol<CreateInstanceFn>(state->library, "NvEncodeAPICreateInstance");
    if (!create_instance) return std::unexpected("NvEncodeAPICreateInstance is absent");
    state->functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create_instance(&state->functions);
    if (status != NV_ENC_SUCCESS) return std::unexpected("NvEncodeAPICreateInstance status=" + std::to_string(status));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params{};
    open_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.deviceType = NV_ENC_DEVICE_TYPE_OPENGL;
    open_params.device = nullptr;
    open_params.apiVersion = NVENCAPI_VERSION;
    status = state->functions.nvEncOpenEncodeSessionEx(&open_params, &state->encoder);
    if (status != NV_ENC_SUCCESS || !state->encoder) {
        return std::unexpected("nvEncOpenEncodeSessionEx(OPENGL, NULL) status=" + std::to_string(status));
    }

    uint32_t count = 0;
    status = state->functions.nvEncGetInputFormatCount(state->encoder, NV_ENC_CODEC_H264_GUID, &count);
    if (status != NV_ENC_SUCCESS || count == 0U || count > 256U) return std::unexpected("NvEncGetInputFormatCount(H.264) failed/invalid");
    std::vector<NV_ENC_BUFFER_FORMAT> formats(count);
    uint32_t returned = 0;
    status = state->functions.nvEncGetInputFormats(state->encoder, NV_ENC_CODEC_H264_GUID,
        formats.data(), count, &returned);
    if (status != NV_ENC_SUCCESS || returned > count) return std::unexpected("NvEncGetInputFormats(H.264) failed");
    bool has_argb = false;
    std::printf("stage=nvenc_input_formats passed H264 formats=");
    for (uint32_t i = 0; i < returned; ++i) {
        std::printf("%s%u", i == 0U ? "" : ",", static_cast<unsigned int>(formats[i]));
        has_argb |= formats[i] == NV_ENC_BUFFER_FORMAT_ARGB;
    }
    std::putchar('\n');
    if (!has_argb) return std::unexpected("NVENC H.264 does not report ARGB input support");

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = state->functions.nvEncGetEncodePresetConfigEx(state->encoder,
        NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (status != NV_ENC_SUCCESS) return std::unexpected("NvEncGetEncodePresetConfigEx(P1/ULL) failed");
    preset.presetCfg.frameIntervalP = 1U;
    NV_ENC_INITIALIZE_PARAMS initialize{};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
    initialize.encodeWidth = width;
    initialize.encodeHeight = height;
    initialize.darWidth = width;
    initialize.darHeight = height;
    initialize.frameRateNum = 360U;
    initialize.frameRateDen = 1U;
    initialize.enableEncodeAsync = 0U;
    initialize.enablePTD = 1U;
    initialize.encodeConfig = &preset.presetCfg;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    status = state->functions.nvEncInitializeEncoder(state->encoder, &initialize);
    if (status != NV_ENC_SUCCESS) return std::unexpected("nvEncInitializeEncoder(H.264 P1 ULL) status=" + std::to_string(status));
    std::printf("stage=nvenc_initialize passed device=OPENGL codec=H264 input=ARGB preset=P1 tuning=ULL dimensions=%ux%u\n", width, height);
    return state;
}

bool register_texture(NvencState& nvenc, GlSlot& slot, unsigned int width, unsigned int height) {
    NV_ENC_REGISTER_RESOURCE registration{};
    registration.version = NV_ENC_REGISTER_RESOURCE_VER;
    registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX;
    registration.width = width;
    registration.height = height;
    registration.pitch = width * 4U;
    registration.resourceToRegister = &slot.nvenc_texture;
    registration.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    registration.bufferUsage = NV_ENC_INPUT_IMAGE;
    const NVENCSTATUS status = nvenc.functions.nvEncRegisterResource(nvenc.encoder, &registration);
    if (status != NV_ENC_SUCCESS) {
        const char* last_error = nvenc.functions.nvEncGetLastErrorString
            ? nvenc.functions.nvEncGetLastErrorString(nvenc.encoder) : nullptr;
        const char* status_name = status == NV_ENC_ERR_RESOURCE_REGISTER_FAILED
            ? "NV_ENC_ERR_RESOURCE_REGISTER_FAILED" : "unknown_NVENC_status";
        std::fprintf(stderr,
            "stage=nvenc_register_opengl_texture failed status=%u status_name=%s "
            "last_error=%s resource_type=OPENGL_TEX registered_texture=%u source_texture=%u target=0x%x "
            "dimensions=%ux%u pitch=%u buffer_format=ARGB buffer_usage=INPUT_IMAGE\n",
            status, status_name, last_error ? last_error : "unavailable",
            slot.nvenc_texture.texture, slot.texture, slot.nvenc_texture.target,
            width, height, registration.pitch);
        return false;
    }
    slot.registered = registration.registeredResource;
    nvenc.registered.push_back(slot.registered);
    std::printf("stage=nvenc_register_opengl_texture passed registered_texture=%u source_texture=%u target=0x%x pitch=%u format=ARGB\n",
        slot.nvenc_texture.texture, slot.texture, slot.nvenc_texture.target, registration.pitch);
    return true;
}

bool create_bitstream(NvencState& nvenc) {
    NV_ENC_CREATE_BITSTREAM_BUFFER create{};
    create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    const NVENCSTATUS status = nvenc.functions.nvEncCreateBitstreamBuffer(nvenc.encoder, &create);
    if (status != NV_ENC_SUCCESS || !create.bitstreamBuffer) {
        std::fprintf(stderr, "stage=nvenc_output_buffer failed status=%u\n", status);
        return false;
    }
    nvenc.bitstream = create.bitstreamBuffer;
    return true;
}

bool map_texture(NvencState& nvenc, const GlSlot& slot) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = slot.registered;
    const NVENCSTATUS status = nvenc.functions.nvEncMapInputResource(nvenc.encoder, &map);
    if (status != NV_ENC_SUCCESS || !map.mappedResource) {
        std::fprintf(stderr, "stage=nvenc_map_opengl_texture failed status=%u\n", status);
        return false;
    }
    nvenc.mapped = map.mappedResource;
    if (map.mappedBufferFmt != NV_ENC_BUFFER_FORMAT_ARGB) {
        std::fprintf(stderr, "stage=nvenc_map_opengl_texture failed mapped_format=%u\n", static_cast<unsigned int>(map.mappedBufferFmt));
        return false;
    }
    return true;
}

bool unmap_texture(NvencState& nvenc) {
    if (!nvenc.mapped) return true;
    const NVENCSTATUS status = nvenc.functions.nvEncUnmapInputResource(nvenc.encoder, nvenc.mapped);
    if (status != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "stage=nvenc_unmap_opengl_texture failed status=%u\n", status);
        return false;
    }
    nvenc.mapped = nullptr;
    return true;
}

bool write_all(int fd, const void* source, std::size_t length, const char* stage) {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t remaining = length;
    while (remaining != 0U) {
        const ssize_t written = ::write(fd, bytes, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            std::fprintf(stderr, "stage=%s failed errno=%d (%s)\n", stage, errno, std::strerror(errno));
            return false;
        }
        bytes += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

class OutputFiles {
public:
    explicit OutputFiles(const std::string& path) : path_(path), csv_path_(path + ".frames.csv") {
        csv_.reset(::open(csv_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!csv_) { error_ = "could not exclusively create frame CSV: " + std::string(std::strerror(errno)); return; }
        created_csv_ = true;
        stream_.reset(::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!stream_) { error_ = "could not exclusively create H.264 stream: " + std::string(std::strerror(errno)); return; }
        created_stream_ = true;
        constexpr std::string_view header = "encode_index,source_sequence,has_presentation_time,presentation_time_ns,slot_index,nvenc_output_timestamp,bytes,picture_type\n";
        if (!write_all(csv_.get(), header.data(), header.size(), "csv_header")) error_ = "could not write CSV header";
    }
    ~OutputFiles() {
        if (!complete_) {
            if (created_stream_) (void)::unlink(path_.c_str());
            if (created_csv_) (void)::unlink(csv_path_.c_str());
        }
    }
    [[nodiscard]] bool good() const noexcept { return stream_ && csv_ && error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] int stream_fd() const noexcept { return stream_.get(); }
    [[nodiscard]] int csv_fd() const noexcept { return csv_.get(); }
    void mark_complete() noexcept { complete_ = true; }
    bool flush() {
        if (::fsync(stream_.get()) != 0 || ::fsync(csv_.get()) != 0) {
            error_ = std::string("fsync failed: ") + std::strerror(errno);
            return false;
        }
        return true;
    }
private:
    std::string path_;
    std::string csv_path_;
    UniqueFd stream_;
    UniqueFd csv_;
    std::string error_;
    bool complete_ = false;
    bool created_stream_ = false;
    bool created_csv_ = false;
};

bool write_csv_row(OutputFiles& output, std::uint64_t index, const CapturedFrame& frame,
                   std::uint32_t bytes, std::uint64_t output_timestamp, std::uint32_t picture_type) {
    char line[256]{};
    const int length = std::snprintf(line, sizeof(line), "%llu,%llu,%u,%llu,%zu,%llu,%u,%u\n",
        static_cast<unsigned long long>(index), static_cast<unsigned long long>(frame.sequence),
        frame.has_presentation_time ? 1U : 0U,
        static_cast<unsigned long long>(frame.presentation_time_ns), frame.lease.slot_index(),
        static_cast<unsigned long long>(output_timestamp), bytes, picture_type);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(line) &&
        write_all(output.csv_fd(), line, static_cast<std::size_t>(length), "csv_write");
}

struct CaptureStopGuard {
    WaylandCapture& capture;
    bool active = false;
    void stop_and_drain() noexcept {
        if (!active) return;
        capture.stop();
        while (auto queued = capture.try_pop_frame()) queued->lease.release_after_gpu();
        active = false;
    }
    ~CaptureStopGuard() {
        stop_and_drain();
    }
};

std::expected<std::uint32_t, std::string> encode_texture(
    NvencState& nvenc, const GlSlot& slot, unsigned int width, unsigned int height,
    std::uint64_t sequence, OutputFiles& output, std::uint64_t& timestamp,
    std::uint32_t& picture_type) {
    if (!nvenc.mapped || !nvenc.bitstream) return std::unexpected("NVENC texture or bitstream is not mapped/created");
    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = width;
    picture.inputHeight = height;
    picture.inputPitch = width * 4U;
    picture.inputTimeStamp = sequence;
    picture.inputDuration = 1U;
    picture.inputBuffer = nvenc.mapped;
    picture.outputBitstream = nvenc.bitstream;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    NVENCSTATUS status = nvenc.functions.nvEncEncodePicture(nvenc.encoder, &picture);
    if (status != NV_ENC_SUCCESS) {
        // Some non-success statuses can mean that NVENC accepted input but has
        // not produced its bitstream yet. Keep the current lease and all GPU
        // registrations alive until process teardown rather than unwinding it.
        std::fprintf(stderr, "stage=nvenc_encode failed status=%u; terminating with input lease retained\n", status);
        std::fflush(stderr);
        ::_exit(EXIT_FAILURE);
    }
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = nvenc.bitstream;
    status = nvenc.functions.nvEncLockBitstream(nvenc.encoder, &lock);
    if (status != NV_ENC_SUCCESS) {
        // The encode call succeeded, so it may still be reading the source.
        // An unconfirmed completion must not unwind the Wayland lease.
        std::fprintf(stderr, "stage=nvenc_lock_bitstream failed status=%u; terminating with input lease retained\n", status);
        std::fflush(stderr);
        ::_exit(EXIT_FAILURE);
    }
    if (!lock.bitstreamBufferPtr || lock.bitstreamSizeInBytes == 0U) {
        (void)nvenc.functions.nvEncUnlockBitstream(nvenc.encoder, nvenc.bitstream);
        std::fprintf(stderr, "stage=nvenc_lock_bitstream returned an empty successful lock; terminating with input lease retained\n");
        std::fflush(stderr);
        ::_exit(EXIT_FAILURE);
    }
    const bool wrote = write_all(output.stream_fd(), lock.bitstreamBufferPtr,
        lock.bitstreamSizeInBytes, "h264_write");
    timestamp = lock.outputTimeStamp;
    picture_type = static_cast<std::uint32_t>(lock.pictureType);
    const std::uint32_t size = lock.bitstreamSizeInBytes;
    const NVENCSTATUS unlock = nvenc.functions.nvEncUnlockBitstream(nvenc.encoder, nvenc.bitstream);
    if (unlock != NV_ENC_SUCCESS) return std::unexpected("nvEncUnlockBitstream status=" + std::to_string(unlock));
    if (!wrote) return std::unexpected("writing encoded bitstream failed");
    (void)slot;
    return size;
}

void report_capture_error(WaylandCapture& capture, const char* stage) {
    const auto error = capture.runtime_error();
    const auto stats = capture.stats();
    std::fprintf(stderr,
        "stage=%s failed error=%s requests=%llu completed=%llu admitted=%llu dropped=%llu failures=%llu "
        "cancelled=%llu free=%zu capturing=%zu leased=%zu queue=%zu\n",
        stage, error ? error->message.c_str() : "capture wait timed out",
        static_cast<unsigned long long>(stats.requests), static_cast<unsigned long long>(stats.completed),
        static_cast<unsigned long long>(stats.admitted), static_cast<unsigned long long>(stats.dropped),
        static_cast<unsigned long long>(stats.failures), static_cast<unsigned long long>(stats.cancelled_at_stop),
        stats.pool_free_slots, stats.pool_capturing_slots, stats.pool_leased_slots,
        stats.frame_queue_occupancy);
}

} // namespace

int main(int argc, char** argv) {
    (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto parsed = parse_options(argc, argv);
    if (!parsed) return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    const Options& options = *parsed;

    fluxrec::capture::CaptureConfig capture_config;
    capture_config.output_name = options.output_name;
    capture_config.frame_queue_capacity = options.pool_slots;
    auto capture_result = WaylandCapture::connect(capture_config);
    if (!capture_result) {
        std::fprintf(stderr, "stage=capture_connect failed: %s\n", capture_result.error().message.c_str());
        return 1;
    }
    auto capture = std::move(*capture_result);
    const CaptureRequirements& requirements = capture.requirements();
    if (!requirements.dmabuf_device) {
        std::fprintf(stderr, "stage=capture_negotiation failed: no DMA-BUF device\n");
        return 1;
    }
    const auto render_node = render_node_for_device(*requirements.dmabuf_device);
    if (!render_node) {
        std::fprintf(stderr, "stage=capture_negotiation failed: DMA-BUF device is not in /dev/dri\n");
        return 1;
    }
    UniqueFd render_fd{::open(render_node->c_str(), O_RDWR | O_CLOEXEC)};
    if (!render_fd) {
        std::fprintf(stderr, "stage=gbm_open failed path=%s error=%s\n", render_node->c_str(), std::strerror(errno));
        return 1;
    }
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> gbm{gbm_create_device(render_fd.get()), gbm_device_destroy};
    if (!gbm) { std::fprintf(stderr, "stage=gbm_device failed path=%s\n", render_node->c_str()); return 1; }

    std::vector<PoolSlot> pool;
    std::uint32_t selected_fourcc = 0;
    std::uint64_t selected_modifier = 0;
    if (!allocate_pool(gbm.get(), requirements, pool, selected_fourcc, selected_modifier, options.pool_slots)) {
        std::fprintf(stderr, "stage=gbm_pool failed: could not allocate %u exact-modifier slots\n", options.pool_slots);
        return 1;
    }
    std::vector<DmabufImage> images;
    images.reserve(pool.size());
    for (const auto& slot : pool) images.push_back(slot.image);
    auto set_pool = capture.set_buffer_pool(images);
    if (!set_pool) { std::fprintf(stderr, "stage=capture_pool failed: %s\n", set_pool.error().message.c_str()); return 1; }
    std::printf("stage=capture_pool passed output=%s dimensions=%ux%u render_node=%s format=%s modifier=0x%016llx slots=%zu\n",
        options.output_name.c_str(), requirements.width, requirements.height, render_node->c_str(),
        fourcc_string(selected_fourcc).c_str(), static_cast<unsigned long long>(selected_modifier), pool.size());

    // This optional is declared before OpenGlState so that on early errors the
    // imported GL/EGL objects are destroyed before the capture lease releases.
    std::optional<CapturedFrame> cuda_only_frame;
    // Declaration order is intentional: NvencState is destroyed before GL/EGL,
    // and OpenGlState deletes textures/images before GBM buffers are destroyed.
    auto gl_result = initialize_opengl(*render_node, pool.front().image,
                                       options.gl_copy_from_import);
    if (!gl_result) { std::fprintf(stderr, "stage=opengl_setup failed: %s\n", gl_result.error().c_str()); return 1; }
    auto gl = std::move(*gl_result);
    gl->slots.resize(pool.size());

    if (options.cuda_gl_only) {
        auto started = capture.start();
        if (!started) {
            std::fprintf(stderr, "stage=capture_start failed: %s\n", started.error().message.c_str());
            return 1;
        }
        CaptureStopGuard stop{capture, true};
        cuda_only_frame = capture.wait_pop_frame(std::chrono::milliseconds(options.capture_wait_ms));
        if (!cuda_only_frame || !cuda_only_frame->lease) {
            report_capture_error(capture, "cuda_gl_capture");
            stop.stop_and_drain();
            return 1;
        }
        stop.stop_and_drain();
        const std::size_t slot_index = cuda_only_frame->lease.slot_index();
        if (slot_index >= gl->slots.size()) {
            std::fprintf(stderr, "stage=cuda_gl_frame_slot failed slot=%zu\n", slot_index);
            return 1;
        }
        GlSlot& slot = gl->slots[slot_index];
        auto bound = bind_slot_texture(*gl, slot, *cuda_only_frame, pool[slot_index].image,
            selected_fourcc, selected_modifier, false, false);
        if (!bound) {
            std::fprintf(stderr, "stage=cuda_gl_import failed: %s\n", bound.error().c_str());
            return 1;
        }
        return run_cuda_gl_only(*gl, *cuda_only_frame, slot, requirements.width, requirements.height);
    }

    auto nvenc_result = initialize_nvenc(requirements.width, requirements.height);
    if (!nvenc_result) { std::fprintf(stderr, "stage=nvenc_setup failed: %s\n", nvenc_result.error().c_str()); return 1; }
    auto nvenc = std::move(*nvenc_result);

    OutputFiles output{options.output_file};
    if (!output.good()) { std::fprintf(stderr, "stage=output_create failed: %s\n", output.error().c_str()); return 1; }
    if (!create_bitstream(*nvenc)) return 1;
    auto started = capture.start();
    if (!started) { std::fprintf(stderr, "stage=capture_start failed: %s\n", started.error().message.c_str()); return 1; }
    CaptureStopGuard stop{capture, true};

    std::uint64_t encoded_count = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t gaps = 0;
    std::uint64_t timestamp_mismatches = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t gpu_gl_copy_count = 0;
    std::size_t queue_high_water = 0;
    std::size_t busy_high_water = 0;
    auto encode_one = [&](CapturedFrame& frame) -> bool {
        const std::size_t slot_index = frame.lease.slot_index();
        if (slot_index >= gl->slots.size()) { std::fprintf(stderr, "stage=frame_slot failed slot=%zu\n", slot_index); return false; }
        GlSlot& slot = gl->slots[slot_index];
        if (slot.texture == 0U) {
            auto bound = bind_slot_texture(*gl, slot, frame, pool[slot_index].image,
                selected_fourcc, selected_modifier, options.gl_owned_control,
                options.gl_copy_from_import);
            if (!bound) {
                const char* stage = options.gl_owned_control ? "owned_texture" :
                    (options.gl_copy_from_import ? "copy_source_setup" : "dmabuf_texture");
                std::fprintf(stderr, "stage=gl_%s failed: %s\n", stage, bound.error().c_str());
                return false;
            }
            if (options.gl_copy_from_import) {
                if (!copy_imported_texture_to_owned(*gl, slot, requirements.width, requirements.height)) return false;
                ++gpu_gl_copy_count;
            }
            if (!register_texture(*nvenc, slot, requirements.width, requirements.height)) return false;
        } else {
            auto fd = frame.lease.duplicate_plane_fd(0U);
            if (!fd || !wait_for_dma_buf_read_fence(fd->get())) {
                std::fprintf(stderr, "stage=fence_wait failed while reusing texture slot=%zu\n", slot_index);
                return false;
            }
            if (options.gl_copy_from_import) {
                if (!copy_imported_texture_to_owned(*gl, slot, requirements.width, requirements.height)) return false;
                ++gpu_gl_copy_count;
            }
        }
        if (!map_texture(*nvenc, slot)) return false;
        std::uint64_t timestamp = 0;
        std::uint32_t picture_type = 0;
        auto result = encode_texture(*nvenc, slot, requirements.width, requirements.height,
            frame.sequence, output, timestamp, picture_type);
        if (!result) {
            (void)unmap_texture(*nvenc);
            std::fprintf(stderr, "stage=nvenc_encode failed: %s\n", result.error().c_str());
            return false;
        }
        // NVENC bitstream lock/unlock has completed before unmap and lease release.
        if (!unmap_texture(*nvenc)) return false;
        if (!write_csv_row(output, encoded_count + 1U, frame, *result, timestamp, picture_type)) return false;
        if (timestamp != frame.sequence) ++timestamp_mismatches;
        if (last_sequence != 0U && frame.sequence > last_sequence + 1U) gaps += frame.sequence - last_sequence - 1U;
        if (first_sequence == 0U) first_sequence = frame.sequence;
        last_sequence = frame.sequence;
        ++encoded_count;
        total_bytes += *result;
        frame.lease.release_after_gpu();
        const auto stats = capture.stats();
        queue_high_water = std::max(queue_high_water, stats.frame_queue_occupancy);
        busy_high_water = std::max(busy_high_water, stats.pool_busy_slots);
        return true;
    };

    // First frame also pays all importer, GL, NVENC-registration and encoder setup costs.
    auto first = capture.wait_pop_frame(std::chrono::milliseconds(options.capture_wait_ms));
    if (!first || !first->lease) { report_capture_error(capture, "first_capture"); return 1; }
    if (!encode_one(*first)) return 1;
    if (options.duration_ms == 0) {
        stop.stop_and_drain();
        if (!output.flush()) { std::fprintf(stderr, "stage=output_flush failed: %s\n", output.error().c_str()); return 1; }
        output.mark_complete();
        std::printf("probe_result=one_frame_encoded passed encoded=%llu source_sequence=%llu bytes=%llu texture_input=NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX gl_owned_control=%s gl_copy_from_import=%s gpu_gl_copy_count=%llu source_pixels_used=%s\n",
            static_cast<unsigned long long>(encoded_count), static_cast<unsigned long long>(first_sequence),
            static_cast<unsigned long long>(total_bytes), options.gl_owned_control ? "yes" : "no",
            options.gl_copy_from_import ? "yes" : "no",
            static_cast<unsigned long long>(gpu_gl_copy_count), options.gl_owned_control ? "no" : "yes");
        return 0;
    }

    // Drain queued pre-measurement frames with explicit lease release.
    while (auto stale = capture.try_pop_frame()) stale->lease.release_after_gpu();
    const auto measurement_start = std::chrono::steady_clock::now();
    while (encoded_count < options.max_frames &&
           std::chrono::steady_clock::now() - measurement_start < std::chrono::milliseconds(options.duration_ms)) {
        const auto elapsed = std::chrono::steady_clock::now() - measurement_start;
        const auto remaining = std::chrono::milliseconds(options.duration_ms) -
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        const int wait_ms = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, options.capture_wait_ms));
        auto frame = capture.wait_pop_frame(std::chrono::milliseconds(wait_ms));
        if (!frame || !frame->lease) {
            if (std::chrono::steady_clock::now() - measurement_start >= std::chrono::milliseconds(options.duration_ms)) break;
            report_capture_error(capture, "measurement_capture");
            return 1;
        }
        if (std::chrono::steady_clock::now() - measurement_start >= std::chrono::milliseconds(options.duration_ms)) {
            frame->lease.release_after_gpu();
            break;
        }
        if (!encode_one(*frame)) return 1;
    }
    const auto measurement_end = std::chrono::steady_clock::now();
    stop.stop_and_drain();
    const auto stats = capture.stats();
    if (encoded_count < 2U) { std::fprintf(stderr, "stage=measurement failed: fewer than two frames encoded\n"); return 1; }
    if (!output.flush()) { std::fprintf(stderr, "stage=output_flush failed: %s\n", output.error().c_str()); return 1; }
    output.mark_complete();
    const double elapsed_s = std::chrono::duration<double>(measurement_end - measurement_start).count();
    const double fps = elapsed_s > 0.0 ? static_cast<double>(encoded_count - 1U) / elapsed_s : 0.0;
    std::printf("probe_result=multi_frame_encoded passed duration_requested_ms=%d encoded=%llu measurement_fps=%.3f bytes=%llu source_sequence_first=%llu source_sequence_last=%llu source_sequence_gaps=%llu timestamp_mismatches=%llu\n",
        options.duration_ms, static_cast<unsigned long long>(encoded_count), fps,
        static_cast<unsigned long long>(total_bytes), static_cast<unsigned long long>(first_sequence),
        static_cast<unsigned long long>(last_sequence), static_cast<unsigned long long>(gaps),
        static_cast<unsigned long long>(timestamp_mismatches));
    std::printf("capture_stats completed=%llu admitted=%llu dropped=%llu failures=%llu cancelled=%llu queue_high_water=%zu busy_high_water=%zu starvation_periods=%llu\n",
        static_cast<unsigned long long>(stats.completed), static_cast<unsigned long long>(stats.admitted),
        static_cast<unsigned long long>(stats.dropped), static_cast<unsigned long long>(stats.failures),
        static_cast<unsigned long long>(stats.cancelled_at_stop), queue_high_water, busy_high_water,
        static_cast<unsigned long long>(stats.pool_starvation_periods));
    std::printf("resource_strategy=cached_opengl_texture_per_pool_slot lease_release=after_bitstream_unlock_and_unmap stream=%s frame_csv=%s\n",
        options.output_file.c_str(), (options.output_file + ".frames.csv").c_str());
    if (options.gl_copy_from_import) {
        std::printf("gpu_copy_route=GLSL_texelFetch_to_RGBA8_FBO copies_completed=%llu synchronization=glFinish output_texture=GL-owned NVENC texture_input=OPENGL_TEX\n",
            static_cast<unsigned long long>(gpu_gl_copy_count));
    }
    return 0;
}
