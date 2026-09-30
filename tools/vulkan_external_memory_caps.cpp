#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// The INVALID sentinel is DRM_FORMAT_MOD_INVALID from drm_fourcc.h. Keep the
// standalone probe independent of libdrm headers so it only needs Vulkan.
constexpr std::uint64_t kDrmFormatModInvalid = (1ULL << 56U) - 1ULL;
constexpr std::array<std::uint64_t, 13> kCaptureModifiers{
    kDrmFormatModInvalid,
    0x0300000000606010ULL,
    0x0300000000606011ULL,
    0x0300000000606012ULL,
    0x0300000000606013ULL,
    0x0300000000606014ULL,
    0x0300000000606015ULL,
    0x0300000000e08010ULL,
    0x0300000000e08011ULL,
    0x0300000000e08012ULL,
    0x0300000000e08013ULL,
    0x0300000000e08014ULL,
    0x0300000000e08015ULL,
};

bool has_extension(const std::vector<VkExtensionProperties>& extensions,
                   const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

const char* result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    default: return "other VkResult";
    }
}

bool check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "%s failed: %s (%d)\n", operation,
                 result_name(result), static_cast<int>(result));
    return false;
}

struct HandleQuery {
    VkResult result = VK_ERROR_UNKNOWN;
    VkExternalMemoryProperties properties{};
};

HandleQuery query_handle(VkPhysicalDevice physical_device,
                         VkFormat format,
                         std::uint64_t modifier,
                         VkExternalMemoryHandleTypeFlagBits handle_type,
                         const char* handle_name) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{};
    modifier_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier_info.drmFormatModifier = modifier;
    modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkPhysicalDeviceExternalImageFormatInfo external_info{};
    external_info.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleType = handle_type;

    VkPhysicalDeviceImageFormatInfo2 image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    image_info.pNext = &external_info;
    image_info.format = format;
    image_info.type = VK_IMAGE_TYPE_2D;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    VkExternalImageFormatProperties external_properties{};
    external_properties.sType =
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 format_properties{};
    format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    format_properties.pNext = &external_properties;

    HandleQuery query;
    query.result = vkGetPhysicalDeviceImageFormatProperties2(
        physical_device, &image_info, &format_properties);
    const auto& memory = external_properties.externalMemoryProperties;
    if (query.result == VK_SUCCESS) {
        query.properties = memory;
        std::printf(
            "  handle=%s result=%s(%d) features=0x%x "
            "importable=%d exportable=%d dedicated_only=%d "
            "compatible=0x%x export_from_import=0x%x\n",
            handle_name, result_name(query.result), static_cast<int>(query.result),
            memory.externalMemoryFeatures,
            (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
            (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0,
            (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0,
            memory.compatibleHandleTypes, memory.exportFromImportedHandleTypes);
    } else {
        std::printf("  handle=%s result=%s(%d)\n", handle_name,
                    result_name(query.result), static_cast<int>(query.result));
    }
    return query;
}

void query_format(VkPhysicalDevice physical_device, VkFormat format,
                  const char* format_name) {
    std::printf("format=%s type=VK_IMAGE_TYPE_2D "
                "tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT "
                "usage=VK_IMAGE_USAGE_SAMPLED_BIT "
                "sharing=VK_SHARING_MODE_EXCLUSIVE\n",
                format_name);
    for (const auto modifier : kCaptureModifiers) {
        const bool invalid = modifier == kDrmFormatModInvalid;
        std::printf("modifier=%s0x%016llx capture_advertisement=AR24/XR24\n",
                    invalid ? "DRM_FORMAT_MOD_INVALID/" : "",
                    static_cast<unsigned long long>(modifier));
        const auto dma_buf = query_handle(
            physical_device, format, modifier,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "DMA_BUF");
        const auto opaque_fd = query_handle(
            physical_device, format, modifier,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD");
        const bool can_export_opaque_fd_from_dma_buf =
            opaque_fd.result == VK_SUCCESS &&
            (opaque_fd.properties.exportFromImportedHandleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
        const VkExternalMemoryHandleTypeFlags compatible_both =
            dma_buf.result == VK_SUCCESS && opaque_fd.result == VK_SUCCESS
                ? dma_buf.properties.compatibleHandleTypes &
                      opaque_fd.properties.compatibleHandleTypes
                : 0;
        std::printf(
            "  dma_buf_import_to_opaque_fd_export=%s "
            "compatible_handle_intersection=0x%x\n",
            can_export_opaque_fd_from_dma_buf ? "yes" : "no",
            compatible_both);
    }
}

} // namespace

int main() {
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Fluxrec external-memory capability query";
    application_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (!check(vkCreateInstance(&instance_info, nullptr, &instance),
               "vkCreateInstance")) {
        return 1;
    }

    std::uint32_t physical_device_count = 0;
    if (!check(vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr),
               "vkEnumeratePhysicalDevices(count)")) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (!check(vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                          physical_devices.data()),
               "vkEnumeratePhysicalDevices(list)")) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    bool matched_device = false;
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        if (std::strstr(properties.deviceName, "NVIDIA GeForce RTX 3090 Ti") == nullptr) {
            continue;
        }
        matched_device = true;
        std::printf("device=%s api=%u.%u.%u\n",
                    properties.deviceName,
                    VK_VERSION_MAJOR(properties.apiVersion),
                    VK_VERSION_MINOR(properties.apiVersion),
                    VK_VERSION_PATCH(properties.apiVersion));

        std::uint32_t extension_count = 0;
        if (!check(vkEnumerateDeviceExtensionProperties(
                       physical_device, nullptr, &extension_count, nullptr),
                   "vkEnumerateDeviceExtensionProperties(count)")) {
            continue;
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (!check(vkEnumerateDeviceExtensionProperties(
                       physical_device, nullptr, &extension_count, extensions.data()),
                   "vkEnumerateDeviceExtensionProperties(list)")) {
            continue;
        }
        constexpr std::array required_extensions{
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        };
        for (const char* extension : required_extensions) {
            std::printf("extension[%s]=%s\n", extension,
                        has_extension(extensions, extension) ? "yes" : "no");
        }

        query_format(physical_device, VK_FORMAT_B8G8R8A8_UNORM,
                     "VK_FORMAT_B8G8R8A8_UNORM");
        query_format(physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                     "VK_FORMAT_R8G8B8A8_UNORM");
    }

    vkDestroyInstance(instance, nullptr);
    if (!matched_device) {
        std::fprintf(stderr, "RTX 3090 Ti Vulkan physical device was not found\n");
        return 2;
    }
    return 0;
}
