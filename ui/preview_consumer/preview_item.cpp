#include <vulkan/vulkan.h>

#include "preview_item.hpp"

#include <QColor>
#include <QMetaObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QThread>
#include <QTimer>
#include <QQuickGraphicsConfiguration>
#include <QQuickView>
#include <QQuickWindow>
#include <QVBoxLayout>
#include <QtQml/qqml.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <optional>
#include <sys/eventfd.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fluxrec::ui {
namespace {

using ImageKey = std::pair<std::uint64_t, std::uint32_t>;

QString vkError(const char* operation, VkResult result) {
    return QStringLiteral("%1 failed with Vulkan result %2")
        .arg(QString::fromLatin1(operation)).arg(static_cast<int>(result));
}

bool hasName(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct ImportedImage {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer acquire_command = VK_NULL_HANDLE;
    VkCommandBuffer release_command = VK_NULL_HANDLE;
    VkFence release_fence = VK_NULL_HANDLE;
    VkSemaphore release_semaphore = VK_NULL_HANDLE;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    QSGTexture* texture = nullptr;
    std::shared_ptr<const PreviewPoolData> owner_pool;
    std::uint64_t generation = 0;
    std::uint32_t slot_index = 0;
    std::uint64_t sequence = 0;
    bool queue_owned_by_qt = false;
    bool acquire_pending = false;
    bool release_pending = false;
    bool ever_sampled = false;
};

class PreviewRenderState final : public std::enable_shared_from_this<PreviewRenderState> {
public:
    PreviewRenderState(QQuickWindow* window, PreviewChannel* channel,
                       PreviewItem* owner)
        : window_(window), channel_(channel), owner_(owner) {}

    ~PreviewRenderState() { destroy(); }

    [[nodiscard]] bool initialize(QString* error) {
        if (initialized_) return available_;
        initialized_ = true;
        if (!window_) return fail(error, QStringLiteral("Qt Quick window is unavailable."));

        QSGRendererInterface* renderer = window_->rendererInterface();
        if (!renderer || renderer->graphicsApi() != QSGRendererInterface::Vulkan) {
            return fail(error, QStringLiteral("Qt Quick did not initialize with Vulkan."));
        }
        auto resource = [renderer, this](QSGRendererInterface::Resource key) {
            return renderer->getResource(window_, key);
        };
        auto* device_pointer = static_cast<VkDevice*>(resource(QSGRendererInterface::DeviceResource));
        auto* physical_pointer = static_cast<VkPhysicalDevice*>(
            resource(QSGRendererInterface::PhysicalDeviceResource));
        auto* queue_pointer = static_cast<VkQueue*>(resource(QSGRendererInterface::CommandQueueResource));
        auto* family_pointer = static_cast<std::uint32_t*>(
            resource(QSGRendererInterface::GraphicsQueueFamilyIndexResource));
        if (!device_pointer || !physical_pointer || !queue_pointer || !family_pointer) {
            return fail(error, QStringLiteral("Qt Quick did not expose its Vulkan device and graphics queue."));
        }
        device_ = *device_pointer;
        physical_device_ = *physical_pointer;
        queue_ = *queue_pointer;
        queue_family_ = *family_pointer;
        if (device_ == VK_NULL_HANDLE || physical_device_ == VK_NULL_HANDLE ||
            queue_ == VK_NULL_HANDLE) {
            return fail(error, QStringLiteral("Qt Quick returned an invalid Vulkan handle."));
        }

        vkGetDeviceProcAddr_ = vkGetDeviceProcAddr;
        get_memory_fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr_(device_, "vkGetMemoryFdPropertiesKHR"));
        get_semaphore_fd_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr_(device_, "vkGetSemaphoreFdKHR"));
        import_semaphore_fd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            vkGetDeviceProcAddr_(device_, "vkImportSemaphoreFdKHR"));
        if (!get_memory_fd_properties_ || !get_semaphore_fd_ || !import_semaphore_fd_) {
            return fail(error, QStringLiteral("Qt Quick Vulkan device lacks DMA-BUF or sync-file support."));
        }

        std::uint32_t extension_count = 0;
        VkResult result = vkEnumerateDeviceExtensionProperties(
            physical_device_, nullptr, &extension_count, nullptr);
        if (result != VK_SUCCESS) return fail(error, vkError("vkEnumerateDeviceExtensionProperties", result));
        std::vector<VkExtensionProperties> extensions(extension_count);
        result = vkEnumerateDeviceExtensionProperties(
            physical_device_, nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS) return fail(error, vkError("vkEnumerateDeviceExtensionProperties", result));
        constexpr std::array required_extensions{
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        };
        for (const char* name : required_extensions) {
            if (!hasName(extensions, name)) {
                return fail(error, QStringLiteral("Qt Quick Vulkan device lacks %1.")
                                      .arg(QString::fromLatin1(name)));
            }
        }

        VkPhysicalDeviceProperties2 physical_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        };
        VkPhysicalDeviceIDProperties id_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        physical_properties.pNext = &id_properties;
        vkGetPhysicalDeviceProperties2(physical_device_, &physical_properties);
        std::copy(std::begin(id_properties.deviceUUID), std::end(id_properties.deviceUUID),
                  device_uuid_.begin());

        VkPhysicalDeviceExternalSemaphoreInfo external_semaphore_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkExternalSemaphoreProperties external_semaphore_properties{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        };
        vkGetPhysicalDeviceExternalSemaphoreProperties(
            physical_device_, &external_semaphore_info, &external_semaphore_properties);
        if ((external_semaphore_properties.externalSemaphoreFeatures &
             VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0 ||
            (external_semaphore_properties.externalSemaphoreFeatures &
             VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
            return fail(error, QStringLiteral("Qt Quick Vulkan device cannot import and export sync-file fences."));
        }

        VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family_,
        };
        result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
        if (result != VK_SUCCESS) return fail(error, vkError("vkCreateCommandPool", result));

        available_ = true;
        connect_render_callbacks();
        return true;
    }

    [[nodiscard]] const std::array<std::uint8_t, VK_UUID_SIZE>& device_uuid() const noexcept {
        return device_uuid_;
    }

    [[nodiscard]] bool matches(const PreviewPoolData& pool, QString* error) const {
        if (pool.description.device_uuid != device_uuid_) {
            if (error) *error = QStringLiteral("The recorder and Qt Quick use different Vulkan devices.");
            return false;
        }
        return true;
    }

    [[nodiscard]] ImportedImage* import_image(
        const std::shared_ptr<const PreviewPoolData>& pool,
        std::uint32_t slot_index, QString* error) {
        const ImageKey key{pool->description.generation, slot_index};
        auto found = images_.find(key);
        if (found != images_.end()) return found->second.get();
        if (slot_index >= pool->description.slot_count ||
            !pool->slot_data[slot_index].dmabuf_fd) {
            if (error) *error = QStringLiteral("The preview pool is missing a DMA-BUF slot.");
            return nullptr;
        }

        const auto& slot = pool->slot_data[slot_index];
        auto image = std::make_unique<ImportedImage>();
        image->device = device_;
        image->command_pool = command_pool_;
        image->owner_pool = pool;
        image->generation = pool->description.generation;
        image->slot_index = slot_index;
        auto fail_import = [&](const QString& message) -> ImportedImage* {
            if (error) *error = message;
            destroy_imported(*image);
            return nullptr;
        };

        VkPhysicalDeviceExternalImageFormatInfo external_format{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .pNext = &external_format,
            .drmFormatModifier = pool->description.modifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkPhysicalDeviceImageFormatInfo2 format_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &modifier_info,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .flags = 0,
        };
        VkExternalImageFormatProperties external_properties{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };
        VkImageFormatProperties2 format_properties{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &external_properties,
        };
        VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
            physical_device_, &format_info, &format_properties);
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkGetPhysicalDeviceImageFormatProperties2", result));
        }
        if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
            return fail_import(QStringLiteral("Qt Quick cannot import this preview DMA-BUF modifier."));
        }

        VkSubresourceLayout plane_layout{
            .offset = slot.offset,
            .size = slot.allocation_size - slot.offset,
            .rowPitch = slot.stride,
            .arrayPitch = 0,
            .depthPitch = 0,
        };
        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = pool->description.modifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &plane_layout,
        };
        VkExternalMemoryImageCreateInfo external_image{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &modifier_create,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external_image,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {pool->description.width, pool->description.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        result = vkCreateImage(device_, &image_info, nullptr, &image->image);
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkCreateImage(DRM modifier)", result));
        }

        VkMemoryRequirements2 memory_requirements{
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        };
        VkMemoryDedicatedRequirements dedicated_requirements{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        };
        memory_requirements.pNext = &dedicated_requirements;
        VkImageMemoryRequirementsInfo2 requirements_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = image->image,
        };
        vkGetImageMemoryRequirements2(device_, &requirements_info, &memory_requirements);
        if (memory_requirements.memoryRequirements.size > slot.allocation_size) {
            return fail_import(QStringLiteral("The preview DMA-BUF allocation is smaller than Vulkan requires."));
        }

        VkMemoryFdPropertiesKHR fd_properties{
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };
        const int borrowed_fd = slot.dmabuf_fd.get();
        result = get_memory_fd_properties_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                           borrowed_fd, &fd_properties);
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkGetMemoryFdPropertiesKHR", result));
        }
        const std::uint32_t compatible_types = fd_properties.memoryTypeBits &
            memory_requirements.memoryRequirements.memoryTypeBits;
        std::uint32_t memory_type = UINT32_MAX;
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((compatible_types & (1U << i)) == 0) continue;
            memory_type = i;
            if ((memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                break;
            }
        }
        if (memory_type == UINT32_MAX) {
            return fail_import(QStringLiteral("The preview DMA-BUF has no compatible Vulkan memory type."));
        }

        const int import_fd = ::fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0);
        if (import_fd < 0) {
            return fail_import(QStringLiteral("Could not duplicate a preview DMA-BUF descriptor."));
        }
        VkMemoryDedicatedAllocateInfo dedicated_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = (dedicated_requirements.requiresDedicatedAllocation ||
                      dedicated_requirements.prefersDedicatedAllocation) ? image->image : VK_NULL_HANDLE,
        };
        VkImportMemoryFdInfoKHR import_info{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = (dedicated_info.image != VK_NULL_HANDLE) ? &dedicated_info : nullptr,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = import_fd,
        };
        VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = memory_requirements.memoryRequirements.size,
            .memoryTypeIndex = memory_type,
        };
        result = vkAllocateMemory(device_, &allocate_info, nullptr, &image->memory);
        if (result != VK_SUCCESS) {
            (void)::close(import_fd);
            return fail_import(vkError("vkAllocateMemory(import DMA-BUF)", result));
        }
        // Successful import transfers the duplicate descriptor to Vulkan.
        result = vkBindImageMemory(device_, image->image, image->memory, 0);
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkBindImageMemory", result));
        }

        VkCommandBufferAllocateInfo command_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool_,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        std::array<VkCommandBuffer, 2> commands{};
        result = vkAllocateCommandBuffers(device_, &command_info, commands.data());
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkAllocateCommandBuffers", result));
        }
        image->acquire_command = commands[0];
        image->release_command = commands[1];
        VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device_, &fence_info, nullptr, &image->release_fence);
        if (result != VK_SUCCESS) {
            return fail_import(vkError("vkCreateFence", result));
        }

        image->texture = QNativeInterface::QSGVulkanTexture::fromNative(
            image->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, window_,
            QSize(static_cast<int>(pool->description.width),
                  static_cast<int>(pool->description.height)),
            QQuickWindow::TextureHasAlphaChannel);
        if (!image->texture) {
            return fail_import(QStringLiteral("Qt Quick could not wrap the imported Vulkan image."));
        }
        auto* result_image = image.get();
        images_.emplace(key, std::move(image));
        return result_image;
    }

    [[nodiscard]] bool can_acquire(ImportedImage* image) {
        if (!image || image->queue_owned_by_qt || image->acquire_pending) return false;
        if (image->release_pending) {
            const VkResult status = vkGetFenceStatus(device_, image->release_fence);
            if (status == VK_NOT_READY) return false;
            if (status != VK_SUCCESS) {
                report(vkError("vkGetFenceStatus", status));
                return false;
            }
            finish_release(image);
        }
        return true;
    }

    [[nodiscard]] bool select(ImportedImage* image,
                              const std::shared_ptr<PreviewFrameData>& frame) {
        if (image == current_image_ && frame && frame->sequence == current_sequence_) return true;
        if (image && !can_acquire(image)) return false;
        if (current_image_ && current_image_ != image && current_image_->queue_owned_by_qt) {
            schedule_release(current_image_);
        }
        current_image_ = image;
        current_frame_ = frame;
        current_sequence_ = frame ? frame->sequence : 0;
        if (current_image_) {
            current_image_->sequence = frame->sequence;
            current_image_->acquire_pending = true;
        }
        return true;
    }

    [[nodiscard]] QSGTexture* current_texture() const noexcept {
        return current_image_ ? current_image_->texture : nullptr;
    }

    [[nodiscard]] bool needs_retry() const noexcept {
        if (retry_needed_ || !pending_releases_.empty()) return true;
        return std::any_of(images_.begin(), images_.end(), [](const auto& entry) {
            return entry.second->release_pending;
        });
    }
    void clear_retry() noexcept { retry_needed_ = false; }

    void cleanup_retired(std::uint64_t active_generation) {
        for (auto it = images_.begin(); it != images_.end();) {
            ImportedImage* image = it->second.get();
            if (image == current_image_ ||
                std::find(pending_releases_.begin(), pending_releases_.end(), image) != pending_releases_.end() ||
                image->queue_owned_by_qt || image->acquire_pending) {
                ++it;
                continue;
            }
            if (image->release_pending) {
                const VkResult status = vkGetFenceStatus(device_, image->release_fence);
                if (status == VK_NOT_READY) {
                    ++it;
                    continue;
                }
                if (status != VK_SUCCESS) {
                    report(vkError("vkGetFenceStatus", status));
                    ++it;
                    continue;
                }
                finish_release(image);
            }
            if (image->generation == active_generation) {
                ++it;
                continue;
            }
            destroy_imported(*image);
            it = images_.erase(it);
        }
    }

    void clear_display() {
        if (current_image_ && current_image_->queue_owned_by_qt) schedule_release(current_image_);
        current_image_ = nullptr;
        current_frame_.reset();
        current_sequence_ = 0;
    }

    void before_rendering() {
        if (!available_ || !current_image_ || !current_image_->acquire_pending) return;
        ImportedImage* image = current_image_;
        if (!record_barrier(image, image->acquire_command, true)) {
            retry_needed_ = true;
            request_retry();
            return;
        }
        if (!current_frame_ || !current_frame_->acquire_fence_fd) {
            const QString reason = QStringLiteral("The preview frame has no acquire sync-file.");
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            retry_needed_ = true;
            request_retry();
            return;
        }
        VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkResult result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                            &image->acquire_semaphore);
        if (result != VK_SUCCESS) {
            const QString reason = vkError("vkCreateSemaphore(acquire sync-file)", result);
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            retry_needed_ = true;
            request_retry();
            return;
        }
        const int import_fd = ::fcntl(current_frame_->acquire_fence_fd.get(),
                                      F_DUPFD_CLOEXEC, 0);
        if (import_fd < 0) {
            vkDestroySemaphore(device_, image->acquire_semaphore, nullptr);
            image->acquire_semaphore = VK_NULL_HANDLE;
            const QString reason = QStringLiteral("Could not duplicate the preview acquire sync-file.");
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            retry_needed_ = true;
            request_retry();
            return;
        }
        VkImportSemaphoreFdInfoKHR import_info{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = image->acquire_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = import_fd,
        };
        result = import_semaphore_fd_(device_, &import_info);
        if (result != VK_SUCCESS) {
            (void)::close(import_fd);
            vkDestroySemaphore(device_, image->acquire_semaphore, nullptr);
            image->acquire_semaphore = VK_NULL_HANDLE;
            const QString reason = vkError("vkImportSemaphoreFdKHR(acquire sync-file)", result);
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            retry_needed_ = true;
            request_retry();
            return;
        }
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &image->acquire_semaphore,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &image->acquire_command,
        };
        result = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            vkDestroySemaphore(device_, image->acquire_semaphore, nullptr);
            image->acquire_semaphore = VK_NULL_HANDLE;
            const QString reason = vkError("vkQueueSubmit(preview acquire)", result);
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            retry_needed_ = true;
            request_retry();
            return;
        }
        image->queue_owned_by_qt = true;
        image->acquire_pending = false;
        image->ever_sampled = true;
        retry_needed_ = false;
    }

    void after_frame_end() {
        if (!available_ || pending_releases_.empty()) return;
        ImportedImage* image = pending_releases_.front();
        pending_releases_.pop_front();
        if (!record_barrier(image, image->release_command, false)) {
            schedule_release_front(image);
            retry_needed_ = true;
            request_retry();
            return;
        }

        VkExportSemaphoreCreateInfo export_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkSemaphoreCreateInfo semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export_info,
        };
        VkResult result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                            &image->release_semaphore);
        if (result != VK_SUCCESS) {
            report(vkError("vkCreateSemaphore(sync-file)", result));
            schedule_release_front(image);
            retry_needed_ = true;
            request_retry();
            return;
        }
        result = vkResetFences(device_, 1, &image->release_fence);
        if (result != VK_SUCCESS) {
            vkDestroySemaphore(device_, image->release_semaphore, nullptr);
            image->release_semaphore = VK_NULL_HANDLE;
            report(vkError("vkResetFences", result));
            schedule_release_front(image);
            retry_needed_ = true;
            request_retry();
            return;
        }
        VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &image->release_command,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &image->release_semaphore,
        };
        result = vkQueueSubmit(queue_, 1, &submit, image->release_fence);
        if (result != VK_SUCCESS) {
            vkDestroySemaphore(device_, image->release_semaphore, nullptr);
            image->release_semaphore = VK_NULL_HANDLE;
            report(vkError("vkQueueSubmit(preview release)", result));
            schedule_release_front(image);
            retry_needed_ = true;
            request_retry();
            return;
        }
        VkSemaphoreGetFdInfoKHR fd_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = image->release_semaphore,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        int fence_fd = -1;
        result = get_semaphore_fd_(device_, &fd_info, &fence_fd);
        if (result != VK_SUCCESS || fence_fd < 0) {
            image->release_pending = true;
            image->queue_owned_by_qt = false;
            retry_needed_ = false;
            if (result == VK_SUCCESS && fence_fd < 0) {
                fence_fd = ::eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
                if (fence_fd >= 0) {
                    const auto pool = image->owner_pool;
                    if (channel_) {
                        channel_->queue_release(image->generation, image->slot_index,
                                                pool->description.slot_count, image->sequence,
                                                preview::UniqueFd{fence_fd}, true);
                    } else {
                        (void)::close(fence_fd);
                    }
                    request_retry();
                    return;
                }
                const QString reason = QStringLiteral(
                    "Vulkan reported a completed preview release, but a completion descriptor could not be created.");
                report(reason);
                if (channel_) channel_->fail_preview(reason);
                request_retry();
                return;
            }
            if (fence_fd >= 0) (void)::close(fence_fd);
            const QString reason = vkError("vkGetSemaphoreFdKHR", result);
            report(reason);
            if (channel_) channel_->fail_preview(reason);
            request_retry();
            return;
        }
        image->release_pending = true;
        image->queue_owned_by_qt = false;
        const auto frame = image->sequence;
        const auto pool = image->owner_pool;
        if (channel_) {
            channel_->queue_release(image->generation, image->slot_index,
                                    pool->description.slot_count, frame,
                                    preview::UniqueFd{fence_fd}, true);
        } else {
            (void)::close(fence_fd);
        }
        retry_needed_ = false;
        request_retry();
    }

    void scene_graph_invalidated() {
        destroy();
    }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool needs_reinitialization() const noexcept {
        return !initialized_ || (!available_ && !terminal_initialization_failure_);
    }

private:
    void schedule_release(ImportedImage* image) {
        if (!image || std::find(pending_releases_.begin(), pending_releases_.end(), image) !=
                          pending_releases_.end()) {
            return;
        }
        pending_releases_.push_back(image);
    }

    void schedule_release_front(ImportedImage* image) {
        if (!image || std::find(pending_releases_.begin(), pending_releases_.end(), image) !=
                          pending_releases_.end()) {
            return;
        }
        pending_releases_.push_front(image);
    }

    void request_retry() {
        QPointer<PreviewItem> owner = owner_;
        if (!owner) return;
        const bool active = needs_retry();
        emit owner->retry_state_requested(active);
    }

    bool fail(QString* error, const QString& message) {
        available_ = false;
        terminal_initialization_failure_ = true;
        if (error) *error = message;
        return false;
    }

    void report(const QString& message) {
        QPointer<PreviewItem> owner = owner_;
        if (!owner) return;
        QMetaObject::invokeMethod(owner, [owner, message] {
            if (owner) emit owner->unavailable_report_requested(message);
        }, Qt::QueuedConnection);
    }

    void connect_render_callbacks() {
        std::weak_ptr<PreviewRenderState> weak = weak_from_this();
        QObject::connect(window_, &QQuickWindow::beforeRendering, window_, [weak] {
            if (auto state = weak.lock()) state->before_rendering();
        }, Qt::DirectConnection);
        QObject::connect(window_, &QQuickWindow::afterFrameEnd, window_, [weak] {
            if (auto state = weak.lock()) state->after_frame_end();
        }, Qt::DirectConnection);
        QObject::connect(window_, &QQuickWindow::sceneGraphInvalidated, window_, [weak] {
            if (auto state = weak.lock()) state->scene_graph_invalidated();
        }, Qt::DirectConnection);
    }

    [[nodiscard]] bool record_barrier(ImportedImage* image, VkCommandBuffer command,
                                      bool acquire) {
        VkResult result = vkResetCommandBuffer(command, 0);
        if (result != VK_SUCCESS) {
            report(vkError("vkResetCommandBuffer", result));
            return false;
        }
        VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS) {
            report(vkError("vkBeginCommandBuffer", result));
            return false;
        }
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = acquire ? VkAccessFlags{0} : VkAccessFlags{VK_ACCESS_SHADER_READ_BIT},
            .dstAccessMask = acquire ? VkAccessFlags{VK_ACCESS_SHADER_READ_BIT} : VkAccessFlags{0},
            .oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = acquire ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : queue_family_,
            .dstQueueFamilyIndex = acquire ? queue_family_ : VK_QUEUE_FAMILY_EXTERNAL,
            .image = image->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        vkCmdPipelineBarrier(command,
                             acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) {
            report(vkError("vkEndCommandBuffer", result));
            return false;
        }
        return true;
    }

    void destroy_imported(ImportedImage& image) {
        if (image.texture) {
            delete image.texture;
            image.texture = nullptr;
        }
        if (image.release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image.release_semaphore, nullptr);
            image.release_semaphore = VK_NULL_HANDLE;
        }
        if (image.acquire_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image.acquire_semaphore, nullptr);
            image.acquire_semaphore = VK_NULL_HANDLE;
        }
        if (image.release_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, image.release_fence, nullptr);
            image.release_fence = VK_NULL_HANDLE;
        }
        std::array<VkCommandBuffer, 2> commands{image.acquire_command, image.release_command};
        std::uint32_t command_count = 0;
        for (VkCommandBuffer command : commands) {
            if (command != VK_NULL_HANDLE) commands[command_count++] = command;
        }
        if (command_count > 0 && image.command_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, image.command_pool, command_count, commands.data());
        }
        image.acquire_command = VK_NULL_HANDLE;
        image.release_command = VK_NULL_HANDLE;
        if (image.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, image.image, nullptr);
            image.image = VK_NULL_HANDLE;
        }
        if (image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, image.memory, nullptr);
            image.memory = VK_NULL_HANDLE;
        }
    }

    void finish_release(ImportedImage* image) {
        if (image->acquire_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image->acquire_semaphore, nullptr);
            image->acquire_semaphore = VK_NULL_HANDLE;
        }
        if (image->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image->release_semaphore, nullptr);
            image->release_semaphore = VK_NULL_HANDLE;
        }
        image->release_pending = false;
        (void)vkResetCommandBuffer(image->acquire_command, 0);
        (void)vkResetCommandBuffer(image->release_command, 0);
    }

    void destroy() {
        if (!device_ || destroyed_) return;
        destroyed_ = true;
        // This runs only when the Quick scene graph is invalidated or destroyed,
        // away from the recorder's capture and encode path.
        const VkResult idle_result = vkDeviceWaitIdle(device_);
        if (idle_result == VK_SUCCESS && available_) {
            if (owner_) emit owner_->discard_pending_ticket_requested();
            if (current_image_ && current_image_->acquire_pending &&
                current_frame_ && channel_ && current_frame_->acquire_fence_fd) {
                const int release_fd = ::fcntl(current_frame_->acquire_fence_fd.get(),
                                               F_DUPFD_CLOEXEC, 0);
                if (release_fd >= 0) {
                    channel_->queue_release(current_frame_->pool->description.generation,
                                            current_frame_->slot_index,
                                            current_frame_->pool->description.slot_count,
                                            current_frame_->sequence,
                                            preview::UniqueFd{release_fd}, false);
                } else {
                    channel_->fail_preview(QStringLiteral(
                        "Could not duplicate a preview acquire fence during scene-graph shutdown."));
                }
            }
            if (current_image_ && current_image_->queue_owned_by_qt) {
                schedule_release(current_image_);
            }
            current_image_ = nullptr;
            current_frame_.reset();
            current_sequence_ = 0;
            while (!pending_releases_.empty()) {
                const std::size_t pending_count = pending_releases_.size();
                after_frame_end();
                if (pending_releases_.size() >= pending_count) break;
            }
            (void)vkDeviceWaitIdle(device_);
        }
        for (auto& [key, image] : images_) {
            (void)key;
            destroy_imported(*image);
        }
        images_.clear();
        current_image_ = nullptr;
        pending_releases_.clear();
        current_frame_.reset();
        current_sequence_ = 0;
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }
        available_ = false;
        terminal_initialization_failure_ = false;
    }

    QPointer<QQuickWindow> window_;
    QPointer<PreviewChannel> channel_;
    QPointer<PreviewItem> owner_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_ = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd_ = nullptr;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd_ = nullptr;
    std::array<std::uint8_t, VK_UUID_SIZE> device_uuid_{};
    std::map<ImageKey, std::unique_ptr<ImportedImage>> images_;
    ImportedImage* current_image_ = nullptr;
    std::deque<ImportedImage*> pending_releases_;
    std::shared_ptr<PreviewFrameData> current_frame_;
    std::uint64_t current_sequence_ = 0;
    bool initialized_ = false;
    bool available_ = false;
    bool terminal_initialization_failure_ = false;
    bool retry_needed_ = false;
    bool destroyed_ = false;
};

class PreviewNode final : public QSGSimpleTextureNode {
public:
    std::shared_ptr<PreviewRenderState> state;
    std::shared_ptr<PreviewFrameData> current_frame;
};

QString embeddedQml() {
    return QStringLiteral(R"qml(
        import QtQuick
        import Fluxrec.PreviewConsumer
        Item {
            id: root
            Accessible.role: Accessible.Pane
            Accessible.name: qsTr("Live capture preview")
            Accessible.description: previewItem.statusText

            Rectangle {
                id: viewport
                anchors.centerIn: parent
                width: Math.max(1, Math.min(root.width - 56, (root.height - 48) * 16 / 9))
                height: Math.max(1, Math.min(root.height - 48, (root.width - 56) * 9 / 16))
                color: "#121923"
                border.color: "#303C49"
                border.width: 1
            }
            PreviewItem {
                id: previewItem
                anchors.fill: viewport
                channel: fluxrecPreviewChannel
            }
            Rectangle {
                anchors.fill: viewport
                visible: !previewItem.hasFrame
                color: "#121923"
                border.color: "#303C49"
                border.width: 1
            }
            Text {
                id: previewStatus
                anchors.centerIn: viewport
                width: viewport.width - 48
                text: previewItem.statusText
                visible: !previewItem.hasFrame
                color: "#9AA9B9"
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.description: qsTr("GPU preview status. Pixel data is not copied to the CPU.")
            }
        }
    )qml");
}

void registerPreviewItemType() {
    static const int type_id = qmlRegisterType<PreviewItem>(
        "Fluxrec.PreviewConsumer", 1, 0, "PreviewItem");
    (void)type_id;
}

} // namespace

void configure_preview_quick_backend() {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
}

PreviewItem::PreviewItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    status_text_ = tr("Waiting for a recording session.");
    retry_timer_ = new QTimer(this);
    retry_timer_->setInterval(16);
    connect(retry_timer_, &QTimer::timeout, this, [this] { update(); });
    connect(this, &PreviewItem::retry_state_requested,
            this, &PreviewItem::set_retry_active, Qt::QueuedConnection);
    connect(this, &PreviewItem::discard_pending_ticket_requested, this, [this] {
        release_unconsumed_frame(std::exchange(pending_frame_, {}));
    }, Qt::DirectConnection);
    connect(this, &PreviewItem::unavailable_report_requested, this,
            [this](const QString& reason) { report_unavailable(reason); },
            Qt::QueuedConnection);
}

PreviewItem::~PreviewItem() = default;

void PreviewItem::set_channel(PreviewChannel* channel) {
    if (channel_ == channel) return;
    if (channel_) disconnect(channel_, nullptr, this, nullptr);
    channel_ = channel;
    if (channel_) {
        connect(channel_, &PreviewChannel::frame_available, this, [this] { update(); });
        connect(channel_, &PreviewChannel::pool_ready, this, [this] { update(); });
        connect(channel_, &PreviewChannel::state_changed, this,
                [this](PreviewChannel::State state) {
                    switch (state) {
                    case PreviewChannel::State::stopped:
                        set_presentation(false, tr("Preview stopped. Start a session to see the capture output."));
                        break;
                    case PreviewChannel::State::listening:
                        set_presentation(false, tr("Waiting for a recording session."));
                        break;
                    case PreviewChannel::State::connected:
                        set_presentation(false, tr("GPU preview connected. Waiting for the preview pool."));
                        break;
                    case PreviewChannel::State::ready:
                        set_presentation(false, tr("Waiting for the first GPU preview frame."));
                        break;
                    case PreviewChannel::State::failed:
                        set_presentation(false, tr("GPU preview is unavailable. Recording can continue."));
                        break;
                    }
                    update();
                });
        connect(channel_, &PreviewChannel::preview_error,
                this, [this](const QString& message) {
                    set_presentation(false, tr("GPU preview unavailable: %1").arg(message));
                    report_unavailable(message);
                });
        switch (channel_->state()) {
        case PreviewChannel::State::stopped:
            set_presentation(false, tr("Preview stopped. Start a session to see the capture output."));
            break;
        case PreviewChannel::State::listening:
            set_presentation(false, tr("Waiting for a recording session."));
            break;
        case PreviewChannel::State::connected:
            set_presentation(false, tr("GPU preview connected. Waiting for the preview pool."));
            break;
        case PreviewChannel::State::ready:
            set_presentation(false, tr("Waiting for the first GPU preview frame."));
            break;
        case PreviewChannel::State::failed:
            set_presentation(false, tr("GPU preview is unavailable. Recording can continue."));
            break;
        }
    } else {
        set_presentation(false, tr("The preview channel is unavailable."));
    }
    emit channel_changed();
    update();
}

QSGNode* PreviewItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    auto* node = static_cast<PreviewNode*>(old_node);
    if (!node) {
        node = new PreviewNode();
        node->setOwnsTexture(false);
        node->setFiltering(QSGTexture::Linear);
    }
    node->setRect(boundingRect());

    QQuickWindow* quick_window = window();
    if (!quick_window) {
        return nullptr;
    }
    if (!node->state || node->state->needs_reinitialization()) {
        node->state = std::make_shared<PreviewRenderState>(quick_window, channel_, this);
    }
    if (!node->state->initialized()) {
        QString reason;
        if (!node->state->initialize(&reason)) {
            report_unavailable(reason);
            if (channel_) channel_->fail_preview(reason);
            set_retry_active(false);
            return nullptr;
        }
    }
    auto state = node->state;
    if (!state->available()) {
        set_retry_active(false);
        return nullptr;
    }

    const auto channel_state = channel_ ? channel_->state() : PreviewChannel::State::stopped;
    const bool should_clear = !channel_ || channel_state == PreviewChannel::State::stopped ||
                              channel_state == PreviewChannel::State::failed;
    if (should_clear) {
        if (channel_) release_unconsumed_frame(channel_->take_latest_frame());
        release_unconsumed_frame(pending_frame_);
        pending_frame_.reset();
        if (node->current_frame) {
            state->clear_display();
            node->current_frame.reset();
        }
        if (channel_state == PreviewChannel::State::stopped) {
            set_presentation(false, tr("Preview stopped. Start a session to see the capture output."));
        }
        // QSGSimpleTextureNode::setTexture() requires a non-null texture.
        // Removing the node clears its non-owning texture reference safely.
        set_retry_active(false);
        return nullptr;
    }

    std::shared_ptr<PreviewFrameData> next_frame;
    if (channel_) next_frame = channel_->take_latest_frame();
    if (next_frame && pending_frame_) {
        // Keep the newest ready ticket. The older pending ticket has a signaled
        // acquire fence, so returning it without sampling is safe.
        release_unconsumed_frame(pending_frame_);
        pending_frame_.reset();
    }

    if (next_frame && next_frame->pool) {
        QString reason;
        if (!state->matches(*next_frame->pool, &reason)) {
            release_unconsumed_frame(next_frame);
            report_unavailable(reason);
            if (channel_) channel_->fail_preview(reason);
            state->clear_display();
            node->current_frame.reset();
            set_retry_active(false);
            return nullptr;
        }
    }

    if (next_frame && next_frame->pool) {
        QString reason;
        ImportedImage* imported = state->import_image(next_frame->pool,
                                                       next_frame->slot_index, &reason);
        if (!imported) {
            release_unconsumed_frame(next_frame);
            report_unavailable(reason);
            if (channel_) channel_->fail_preview(reason);
            state->clear_display();
            node->current_frame.reset();
            set_retry_active(false);
            return nullptr;
        } else if (!state->can_acquire(imported)) {
            // The returned release sync-file may still be completing on Qt's
            // queue. Keep this newest ticket and retry on the next UI tick.
            pending_frame_ = std::move(next_frame);
            if (QSGTexture* texture = state->current_texture()) node->setTexture(texture);
            set_retry_active(true);
            return node;
        } else {
            const bool switched = state->select(imported, next_frame);
            if (switched) {
                node->current_frame = std::move(next_frame);
                if (QSGTexture* texture = state->current_texture()) node->setTexture(texture);
                set_presentation(true, {});
            }
        }
    } else if (pending_frame_) {
        next_frame = std::exchange(pending_frame_, {});
        QString reason;
        ImportedImage* imported = next_frame && next_frame->pool
            ? state->import_image(next_frame->pool, next_frame->slot_index, &reason) : nullptr;
        if (imported && state->can_acquire(imported) && state->select(imported, next_frame)) {
            node->current_frame = std::move(next_frame);
            if (QSGTexture* texture = state->current_texture()) node->setTexture(texture);
            set_presentation(true, {});
        } else {
            pending_frame_ = std::move(next_frame);
            if (QSGTexture* texture = state->current_texture()) node->setTexture(texture);
            set_retry_active(true);
        }
    }

    if (node->current_frame && node->current_frame->pool) {
        const auto& description = node->current_frame->pool->description;
        const qreal source_aspect = static_cast<qreal>(description.width) /
                                    static_cast<qreal>(description.height);
        QRectF fitted = boundingRect();
        if (source_aspect > 0.0 && fitted.width() > 0.0 && fitted.height() > 0.0) {
            if (fitted.width() / fitted.height() > source_aspect) {
                const qreal width = fitted.height() * source_aspect;
                fitted.setLeft(fitted.center().x() - width / 2.0);
                fitted.setRight(fitted.center().x() + width / 2.0);
            } else {
                const qreal height = fitted.width() / source_aspect;
                fitted.setTop(fitted.center().y() - height / 2.0);
                fitted.setBottom(fitted.center().y() + height / 2.0);
            }
        }
        node->setRect(fitted);
    } else {
        node->setRect(boundingRect());
    }
    const auto active_generation = channel_ && channel_->pool()
        ? channel_->pool()->description.generation : 0;
    state->cleanup_retired(active_generation);
    set_retry_active(state->needs_retry());
    return node;
}

void PreviewItem::set_retry_active(bool active) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, active] { set_retry_active(active); },
                                  Qt::QueuedConnection);
        return;
    }
    if (active) {
        if (!retry_timer_->isActive()) retry_timer_->start();
    } else {
        retry_timer_->stop();
    }
}

void PreviewItem::report_unavailable(const QString& reason) {
    QPointer<PreviewItem> self(this);
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [self, reason] {
            if (self) {
                self->set_presentation(false,
                    self->tr("GPU preview unavailable: %1").arg(reason));
                emit self->preview_unavailable(reason);
            }
        }, Qt::QueuedConnection);
        return;
    }
    set_presentation(false, tr("GPU preview unavailable: %1").arg(reason));
    emit preview_unavailable(reason);
}

void PreviewItem::set_presentation(bool has_frame, const QString& status) {
    if (QThread::currentThread() != thread()) {
        QPointer<PreviewItem> self(this);
        QMetaObject::invokeMethod(this, [self, has_frame, status] {
            if (self) self->set_presentation(has_frame, status);
        }, Qt::QueuedConnection);
        return;
    }
    if (has_frame_ == has_frame && status_text_ == status) return;
    has_frame_ = has_frame;
    status_text_ = status;
    emit presentation_changed();
}

void PreviewItem::release_unconsumed_frame(const std::shared_ptr<PreviewFrameData>& frame) {
    if (!frame || !frame->pool || !channel_ || !frame->acquire_fence_fd) return;
    preview::UniqueFd release_fd{
        ::fcntl(frame->acquire_fence_fd.get(), F_DUPFD_CLOEXEC, 0)};
    if (!release_fd) {
        const QString reason = tr("Could not duplicate a completed preview acquire fence.");
        report_unavailable(reason);
        channel_->fail_preview(reason);
        return;
    }
    channel_->queue_release(frame->pool->description.generation, frame->slot_index,
                            frame->pool->description.slot_count, frame->sequence,
                            std::move(release_fd), false);
}

PreviewQuickWidget::PreviewQuickWidget(PreviewChannel* channel, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("gpuPreviewWidget"));
    setAccessibleName(tr("Live capture preview"));
    setAccessibleDescription(tr("GPU-resident preview of the configured capture output. No CPU pixel copy is used."));
    registerPreviewItemType();
    view_ = new QQuickView;
    QQuickGraphicsConfiguration graphics_configuration;
    graphics_configuration.setDeviceExtensions({
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    });
    // QQuickView creates its Vulkan device after this setting is applied. A
    // QQuickWidget creates its offscreen RHI in the base constructor, before a
    // subclass can request these device extensions.
    view_->setGraphicsConfiguration(graphics_configuration);
    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    view_->setColor(QColor(7, 11, 16));
    container_ = QWidget::createWindowContainer(view_, this);
    container_->setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(container_);

    connect(view_, &QQuickWindow::sceneGraphError, this,
            &PreviewQuickWidget::sceneGraphError);
    connect(view_, &QQuickView::statusChanged, this,
            &PreviewQuickWidget::statusChanged);

    view_->rootContext()->setContextProperty(QStringLiteral("fluxrecPreviewChannel"), channel);
    component_ = std::make_unique<QQmlComponent>(view_->engine());
    component_->setData(embeddedQml().toUtf8(), QUrl(QStringLiteral("inmemory:/preview.qml")));
    connect(component_.get(), &QQmlComponent::statusChanged, this,
            [this](QQmlComponent::Status status) {
                if (status == QQmlComponent::Ready) {
                    complete_component();
                } else if (status == QQmlComponent::Error) {
                    emit sceneGraphError(QQuickWindow::ContextNotAvailable,
                                         component_->errorString());
                }
            });
    if (component_->status() == QQmlComponent::Ready) {
        complete_component();
    } else if (component_->status() == QQmlComponent::Error) {
        emit sceneGraphError(QQuickWindow::ContextNotAvailable,
                             component_->errorString());
    }
}

PreviewQuickWidget::~PreviewQuickWidget() {
    // Destroy the view while the component backing its content is still alive.
    delete container_;
    container_ = nullptr;
    view_ = nullptr;
    item_ = nullptr;
    component_.reset();
}

QQuickView::Status PreviewQuickWidget::status() const noexcept {
    return view_ ? view_->status() : QQuickView::Null;
}

QObject* PreviewQuickWidget::rootObject() const noexcept {
    return view_ ? view_->rootObject() : nullptr;
}

QList<QQmlError> PreviewQuickWidget::errors() const {
    return view_ ? view_->errors() : QList<QQmlError>{};
}

void PreviewQuickWidget::complete_component() {
    if (!component_ || component_->status() != QQmlComponent::Ready ||
        (view_ && view_->rootObject())) return;
    QObject* root = component_->create(view_->rootContext());
    if (!root) {
        emit sceneGraphError(QQuickWindow::ContextNotAvailable,
                             component_->errorString());
        return;
    }
    view_->setContent(QUrl(QStringLiteral("inmemory:/preview.qml")), component_.get(), root);
    item_ = view_->rootObject() ? view_->rootObject()->findChild<PreviewItem*>() : nullptr;
}

} // namespace fluxrec::ui
