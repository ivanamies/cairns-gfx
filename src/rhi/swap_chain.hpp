// rhi/swap_chain.hpp
//
// Backend swapchain + render targets (MSAA color, depth) + render pass +
// framebuffers. Owns everything needed to begin a frame's render pass and
// present. Device/instance/queue/surface are created by the app and passed in.

#pragma once

#include "util/define.hpp"
#include "util/log.hpp"
#include "rhi/swap_resolve_target.hpp"

#if CAIRNS_VULKAN

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

namespace cairns::rhi {

// Shell-provided getter for current window pixel dims; used during resize
// (RecreateSwapChain / chooseSwapExtent). cairns_app fills this with a
// SDL_GetWindowSizeInPixels wrapper. A SwapChain is windowed by definition;
// render-to-texture (cairns_serve) never constructs one.
using WindowSizeFn = void (*)(void* user, int* w, int* h);

struct SwapChain {
    // Injected by Init (owned by the app).
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    WindowSizeFn window_size_fn_ = nullptr;
    void* window_size_user_ = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    bool dump_swapchain = false;

    // Owned.
    VkSwapchainKHR swapChain = VK_NULL_HANDLE;
    std::vector<VkImage> swapChainImages;
    std::vector<VkImageView> swapChainImageViews;
    VkFormat swapChainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapChainExtent{};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkImage colorImage = VK_NULL_HANDLE;
    VkDeviceMemory colorImageMemory = VK_NULL_HANDLE;
    VkImageView colorImageView = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapChainFramebuffers;

    bool Init(VkDevice dev, VkPhysicalDevice phys, VkSurfaceKHR surf,
              WindowSizeFn size_fn, void* size_user,
              VkCommandPool pool, VkQueue queue,
              VkSampleCountFlagBits samples, bool dump) {
        device = dev;
        physicalDevice = phys;
        surface = surf;
        window_size_fn_ = size_fn;
        window_size_user_ = size_user;
        commandPool = pool;
        graphicsQueue = queue;
        msaaSamples = samples;
        dump_swapchain = dump;
        if (!createSwapChain()) return false;
        if (!createImageViews()) return false;
        if (!createRenderPass()) return false;
        if (!createColorResources()) return false;
        if (!createDepthResources()) return false;
        if (!createFramebuffers()) return false;
        return true;
    }

    uint32_t Width() const { return swapChainExtent.width; }
    uint32_t Height() const { return swapChainExtent.height; }

    // Per-frame swap-target acquire. Caller passes the returned value to
    // Frames::Begin/End and RenderGraph::Execute. The actual
    // vkAcquireNextImageKHR still happens inside Frames::Begin (needs the
    // per-frame image-available semaphore); this just publishes the SwapChain
    // pointer the rest of the pipeline binds against.
    SwapResolveTarget AcquireForFrame() {
        SwapResolveTarget t;
        t.width = swapChainExtent.width;
        t.height = swapChainExtent.height;
        t.swap_chain = this;
        return t;
    }

    void RecreateSwapChain() {
        int width = 0;
        int height = 0;
        if (window_size_fn_) {
            window_size_fn_(window_size_user_, &width, &height);
            while (width == 0 || height == 0) {
                window_size_fn_(window_size_user_, &width, &height);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        vkDeviceWaitIdle(device);
        cleanupSwapChain();
        createSwapChain();
        createColorResources();
        createImageViews();
        createDepthResources();
        createFramebuffers();
    }

    void Cleanup() {
        cleanupSwapChain();
        if (renderPass) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
    }

    // Neutral teardown entry (matches Metal SwapChain::Deinit). Device must still
    // be alive (call before ResourceManager::Deinit destroys it).
    void Deinit() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        Cleanup();
    }

private:
    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsAndComputeFamily;
        std::optional<uint32_t> presentFamily;
        bool isComplete() {
            return graphicsAndComputeFamily.has_value() && presentFamily.has_value();
        }
    };

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice dev) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());
        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                indices.graphicsAndComputeFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) {
                break;
            }
            ++i;
        }
        return indices;
    }

    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice dev) {
        SwapChainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &details.capabilities);
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, details.formats.data());
        }
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }
        return availableFormats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_FIFO_KHR) {
                return availablePresentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            int width = 0;
            int height = 0;
            if (window_size_fn_) {
                window_size_fn_(window_size_user_, &width, &height);
            }
            VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height)};
            actualExtent.width = std::clamp(actualExtent.width,
                                            capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height,
                                             capabilities.minImageExtent.height,
                                             capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }

    bool findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t& out) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                out = i;
                return true;
            }
        }
        return false;
    }

    bool findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                             VkFormatFeatureFlags features, VkFormat& out) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (tiling == VK_IMAGE_TILING_LINEAR &&
                (props.linearTilingFeatures & features) == features) {
                out = format;
                return true;
            } else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
                       (props.optimalTilingFeatures & features) == features) {
                out = format;
                return true;
            }
        }
        return false;
    }

    bool findDepthFormat(VkFormat& out) {
        return findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, out);
    }

    bool hasStencilComponent(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        vkEndCommandBuffer(commandBuffer);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    bool createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                     VkSampleCountFlagBits numSamples, VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = numSamples;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.flags = 0;
        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, image, &memRequirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        if (!findMemoryType(memRequirements.memoryTypeBits, properties,
                            allocInfo.memoryTypeIndex)) {
            return false;
        }
        if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            return false;
        }
        vkBindImageMemory(device, image, imageMemory, 0);
        return true;
    }

    bool createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags,
                         uint32_t mipLevels, VkImageView& out) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &out) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout,
                               VkImageLayout newLayout, uint32_t mipLevels) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (hasStencilComponent(format)) {
                barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        } else {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        } else {
            return false;
        }
        vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        endSingleTimeCommands(commandBuffer);
        return true;
    }

    bool createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);
        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

        // Android Vulkan pre-rotation: currentTransform may be ROTATE_90/180/270.
        // If we set preTransform=currentTransform we claim we pre-rotated the
        // content -- but we didn't, so the OS doesn't rotate at present and
        // display ends up rotated 90deg. Fix: request preTransform=IDENTITY
        // when IDENTITY is supported; WSI handles the compositor rotation.
        VkSurfaceTransformFlagBitsKHR preTransform =
            swapChainSupport.capabilities.currentTransform;
        const bool is_non_identity =
            preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        const bool identity_supported =
            (swapChainSupport.capabilities.supportedTransforms &
             VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0;
        if (is_non_identity && identity_supported) {
            preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        }

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (dump_swapchain) {
            createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = {indices.graphicsAndComputeFamily.value(),
                                         indices.presentFamily.value()};
        if (indices.graphicsAndComputeFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }
        createInfo.preTransform = preTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            return false;
        }
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());
        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
        return true;
    }

    bool createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); ++i) {
            if (!createImageView(swapChainImages[i], swapChainImageFormat,
                                 VK_IMAGE_ASPECT_COLOR_BIT, 1, swapChainImageViews[i])) {
                return false;
            }
        }
        return true;
    }

    bool createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = msaaSamples;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // DONT_CARE: the MSAA samples are throwaway -- the single-sample
        // resolve attachment is the real output. STORE here would force the
        // tiler to write the entire NxMSAA buffer out to DRAM every frame
        // (~37 MB/frame at 2268x1080 4xMSAA on Adreno), with the resolve as
        // a separate read+write pass on top. With DONT_CARE the resolve
        // happens in-tile and the MSAA samples never leave the tile.
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription colorAttachmentResolve{};
        colorAttachmentResolve.format = swapChainImageFormat;
        colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentResolveRef{};
        colorAttachmentResolveRef.attachment = 2;
        colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        VkFormat depthFormat;
        if (!findDepthFormat(depthFormat)) return false;
        depthAttachment.format = depthFormat;
        depthAttachment.samples = msaaSamples;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pResolveAttachments = &colorAttachmentResolveRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 3> attachments = {colorAttachment, depthAttachment,
                                                              colorAttachmentResolve};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    // Try LAZILY_ALLOCATED|DEVICE_LOCAL first (tile-resident memory on
    // mobile/tilers -- never backs DRAM when paired with DONT_CARE storeOps).
    // Falls back to DEVICE_LOCAL on devices that don't expose lazy memory
    // (desktop GPUs typically don't; the fallback costs nothing there since
    // they're not tilers).
    bool createImageLazyOrDeviceLocal(uint32_t width, uint32_t height,
                                      VkSampleCountFlagBits samples,
                                      VkFormat format, VkImageUsageFlags usage,
                                      VkImage& out_image,
                                      VkDeviceMemory& out_memory) {
        if (createImage(width, height, 1, samples, format, VK_IMAGE_TILING_OPTIMAL,
                        usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                        VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        out_image, out_memory)) {
            return true;
        }
        return createImage(width, height, 1, samples, format, VK_IMAGE_TILING_OPTIMAL,
                           usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           out_image, out_memory);
    }

    bool createColorResources() {
        VkFormat colorFormat = swapChainImageFormat;
        if (!createImageLazyOrDeviceLocal(
                swapChainExtent.width, swapChainExtent.height, msaaSamples,
                colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                colorImage, colorImageMemory)) {
            return false;
        }
        if (!createImageView(colorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, colorImageView)) {
            return false;
        }
        return true;
    }

    bool createDepthResources() {
        VkFormat depthFormat;
        if (!findDepthFormat(depthFormat)) return false;
        if (!createImageLazyOrDeviceLocal(
                swapChainExtent.width, swapChainExtent.height, msaaSamples,
                depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                depthImage, depthImageMemory)) {
            return false;
        }
        if (!createImageView(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, depthImageView)) {
            return false;
        }
        if (!transitionImageLayout(depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1)) {
            return false;
        }
        return true;
    }

    bool createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());
        for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
            std::array<VkImageView, 3> attachments = {colorImageView, depthImageView,
                                                      swapChainImageViews[i]};
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i])) {
                return false;
            }
        }
        return true;
    }

    void cleanupSwapChain() {
        vkDestroyImageView(device, colorImageView, nullptr);
        vkDestroyImage(device, colorImage, nullptr);
        vkFreeMemory(device, colorImageMemory, nullptr);
        vkDestroyImageView(device, depthImageView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthImageMemory, nullptr);
        for (size_t i = 0; i < swapChainFramebuffers.size(); ++i) {
            vkDestroyFramebuffer(device, swapChainFramebuffers[i], nullptr);
        }
        for (size_t i = 0; i < swapChainImageViews.size(); ++i) {
            vkDestroyImageView(device, swapChainImageViews[i], nullptr);
        }
        vkDestroySwapchainKHR(device, swapChain, nullptr);
    }
};

}  // namespace cairns::rhi

#elif CAIRNS_METAL

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "util/size.hpp"

namespace cairns::rhi {

struct SwapChain {
    SwapChain() {}

    // Layer is pre-resolved by the SDL shell via SDL_Metal_CreateView +
    // SDL_Metal_GetLayer and ownership stays with the shell (it also calls
    // SDL_Metal_DestroyView). Headless host passes nullptr and never invokes
    // this code path (SwapChain::Init is skipped).
    bool Init(MTL::Device* device, CA::MetalLayer* layer) {
        if (!layer) {
            return false;
        }
        metalLayer_ = layer;
        metalLayer_->setDevice(device);
        metalLayer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        // Drawables must be blittable: Frames::End copies from the drawable for the
        // swapchain dump, which Metal forbids on a framebufferOnly layer.
        metalLayer_->setFramebufferOnly(false);
        size_ = cairns::Size(metalLayer_->drawableSize().width,
                             metalLayer_->drawableSize().height);
        return true;
    }

    void Deinit() {}

    bool NextDrawable() {
        if (!metalLayer_) {
            return false;
        }
        metalDrawable_ = metalLayer_->nextDrawable();
        return metalDrawable_ != nullptr;
    }

    CA::MetalDrawable* GetDrawable() const { return metalDrawable_; }

    // Per-frame swap-target acquire. Pulls the next drawable and returns a
    // SwapResolveTarget describing the resolve texture + the drawable to
    // present at Frames::End. The render-to-texture (cairns_serve) path does
    // not construct a SwapChain at all -- it builds its SwapResolveTarget
    // directly via MakeSwapResolveTargetFromTexture in
    // rhi/swap_resolve_target.hpp.
    SwapResolveTarget AcquireForFrame() {
        NextDrawable();
        SwapResolveTarget t;
        t.width = size_.width;
        t.height = size_.height;
        t.drawable = metalDrawable_;
        t.texture = metalDrawable_ ? metalDrawable_->texture() : nullptr;
        return t;
    }

    void SetDrawableSize(uint32_t width, uint32_t height) {
        size_ = cairns::Size(width, height);
        if (metalLayer_) {
            metalLayer_->setDrawableSize(CGSizeMake(width, height));
        }
    }

    Size GetDrawableSize() const { return size_; }
    uint32_t Width() const { return size_.width; }
    uint32_t Height() const { return size_.height; }

    MTL::PixelFormat GetPixelFormat() const {
        return metalLayer_ ? metalLayer_->pixelFormat()
                           : MTL::PixelFormatBGRA8Unorm;
    }

private:
    Size size_ = cairns::kInvalidSize;
    CA::MetalDrawable* metalDrawable_ = nullptr;
    CA::MetalLayer* metalLayer_ = nullptr;
};

// resolve_override: when non-null, used as the MSAA-resolve texture
// instead of swap_chain.GetDrawable()->texture(). Headless mode passes
// the engine's final_target_ texture so the swap pass writes into it
// directly instead of a swapchain drawable.
inline bool InitRenderPassDescriptor(MTL::RenderPassDescriptor*& renderPassDescriptor,
                                     MTL::Texture* msaa, MTL::Texture* depth,
                                     SwapChain& swap_chain,
                                     MTL::Texture* resolve_override = nullptr) {
    renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
        renderPassDescriptor->colorAttachments()->object(0);
    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment =
        renderPassDescriptor->depthAttachment();
    colorAttachment->setTexture(msaa);
    MTL::Texture* resolve_tex = resolve_override
        ? resolve_override
        : (swap_chain.GetDrawable() ? swap_chain.GetDrawable()->texture()
                                    : nullptr);
    colorAttachment->setResolveTexture(resolve_tex);
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setClearColor(MTL::ClearColor(41.0f / 255.0f, 42.0f / 255.0f,
                                                   48.0f / 255.0f, 1.0));
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);
    depthAttachment->setTexture(depth);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);
    return true;
}

inline bool UpdateRenderPassDescriptor(MTL::RenderPassDescriptor* render_pass_desc,
                                       MTL::Texture* msaa, MTL::Texture* depth,
                                       SwapChain& swap_chain,
                                       MTL::Texture* resolve_override = nullptr) {
    render_pass_desc->colorAttachments()->object(0)->setTexture(msaa);
    MTL::Texture* resolve_tex = resolve_override
        ? resolve_override
        : (swap_chain.GetDrawable() ? swap_chain.GetDrawable()->texture()
                                    : nullptr);
    render_pass_desc->colorAttachments()->object(0)->setResolveTexture(resolve_tex);
    render_pass_desc->depthAttachment()->setTexture(depth);
    return true;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN / CAIRNS_METAL
