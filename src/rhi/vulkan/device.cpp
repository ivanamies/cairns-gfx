// rhi/vulkan/device.cpp
//
// Vulkan platform device lifetime. The instance/surface/physical+logical
// device/queues/command-pool creation + the device-suitability helpers moved
// here out of resource_manager.cpp (Phase 0e). ResourceManager mirrors these
// handle values during InitDevice; Device owns their teardown.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstring>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

#include "rhi/device.hpp"
#include "rhi/swap_chain.hpp"

namespace cairns::rhi {

namespace {

const std::vector<const char*> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};
const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#if CAIRNS_APPLE
    VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
#endif
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::cerr << "validation layer: " << data->pMessage << std::endl;
    return VK_FALSE;
}

VkResult create_debug_messenger(VkInstance instance,
                                const VkDebugUtilsMessengerCreateInfoEXT* ci,
                                VkDebugUtilsMessengerEXT* out) {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    return fn ? fn(instance, ci, nullptr, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT m) {
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) {
        fn(instance, m, nullptr);
    }
}

void populate_debug_ci(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
}

bool check_validation_layer_support() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const char* name : kValidationLayers) {
        bool found = false;
        for (const auto& props : available) {
            if (strcmp(name, props.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

struct QueueFamilies {
    std::optional<uint32_t> graphics_compute;
    std::optional<uint32_t> present;
    bool complete() const {
        return graphics_compute.has_value() && present.has_value();
    }
};

QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            indices.graphics_compute = i;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
        if (present) {
            indices.present = i;
        }
        if (indices.complete()) {
            break;
        }
    }
    return indices;
}

bool check_device_extension_support(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

bool device_swapchain_adequate(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    uint32_t present_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_count, nullptr);
    return format_count != 0 && present_count != 0;
}

VkSampleCountFlagBits max_usable_sample_count(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                props.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
    if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
    if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
    if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
    if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
    if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }
    return VK_SAMPLE_COUNT_1_BIT;
}

bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies indices = find_queue_families(device, surface);
    bool extensions_ok = check_device_extension_support(device);
    bool swapchain_ok = extensions_ok && device_swapchain_adequate(device, surface);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    return indices.complete() && extensions_ok && swapchain_ok &&
           features.samplerAnisotropy;
}

}  // namespace

Device::~Device() { Deinit(); }

bool Device::Init(SDL_Window* window) {
    if (inited_) {
        return true;
    }

    validation_enabled_ = false;
    if (validation_enabled_ && !check_validation_layer_support()) {
        validation_enabled_ = false;
    }

    {  // instance
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "cairns";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "cairns";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_2;

        uint32_t sdl_count = 0;
        const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_count);
        if (validation_enabled_) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
#if CAIRNS_APPLE
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
#if CAIRNS_APPLE
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
        ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        VkDebugUtilsMessengerCreateInfoEXT dbg{};
        if (validation_enabled_) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
            populate_debug_ci(dbg);
            ci.pNext = &dbg;
        }
        if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
            return false;
        }
    }

    if (validation_enabled_) {  // debug messenger
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        populate_debug_ci(ci);
        create_debug_messenger(instance_, &ci, &debug_messenger_);
    }

    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr,
                                  &surface_)) {
        return false;
    }

    {  // physical device
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) {
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice d : devices) {
            if (is_device_suitable(d, surface_)) {
                physical_ = d;
                msaa_samples_ = max_usable_sample_count(d);
                break;
            }
        }
        if (physical_ == VK_NULL_HANDLE) {
            return false;
        }
    }

    QueueFamilies indices = find_queue_families(physical_, surface_);

    {  // logical device + queues
        std::set<uint32_t> unique = {indices.graphics_compute.value(),
                                     indices.present.value()};
        std::vector<VkDeviceQueueCreateInfo> queue_cis;
        const float priority = 1.0f;
        for (uint32_t fam : unique) {
            VkDeviceQueueCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = fam;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            queue_cis.push_back(qci);
        }

        VkPhysicalDeviceVulkan12Features vk12{};
        vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vk12.runtimeDescriptorArray = VK_TRUE;
        vk12.descriptorBindingPartiallyBound = VK_TRUE;
        vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vk12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        vk12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        vk12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        vk12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        vk12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        // Probe + enable hostQueryReset so Frames::Begin can vkResetQueryPool
        // host-side and avoid the cross-queue cmd-reset race.
        {
            VkPhysicalDeviceVulkan12Features probe{};
            probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &probe;
            vkGetPhysicalDeviceFeatures2(physical_, &f2);
            host_query_reset_ = (probe.hostQueryReset == VK_TRUE);
        }
        if (host_query_reset_) {
            vk12.hostQueryReset = VK_TRUE;
        }
        // Resolved after vkCreateDevice below; see post-device-create block.
        {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(physical_, &pp);
            timestamp_period_ns_ = pp.limits.timestampPeriod;
        }

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.features.samplerAnisotropy = VK_TRUE;
        features2.pNext = &vk12;

        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queue_cis.size());
        ci.pQueueCreateInfos = queue_cis.data();
        ci.pNext = &features2;
        ci.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
        ci.ppEnabledExtensionNames = kDeviceExtensions.data();
        if (validation_enabled_) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
        }
        if (vkCreateDevice(physical_, &ci, nullptr, &device_) !=
            VK_SUCCESS) {
            return false;
        }
        vkGetDeviceQueue(device_, indices.graphics_compute.value(), 0,
                         &graphics_queue_);
        vkGetDeviceQueue(device_, indices.present.value(), 0,
                         &present_queue_);
        vkGetDeviceQueue(device_, indices.graphics_compute.value(), 0,
                         &compute_queue_);
        queue_family_index_ = indices.graphics_compute.value();
        if (host_query_reset_) {
            vk_reset_query_pool_ = reinterpret_cast<PFN_vkResetQueryPool>(
                vkGetDeviceProcAddr(device_, "vkResetQueryPool"));
            if (vk_reset_query_pool_ == nullptr) {
                vk_reset_query_pool_ = reinterpret_cast<PFN_vkResetQueryPool>(
                    vkGetDeviceProcAddr(device_, "vkResetQueryPoolEXT"));
            }
            if (vk_reset_query_pool_ == nullptr) {
                host_query_reset_ = false;
            }
        }
    }

    {  // command pool
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = indices.graphics_compute.value();
        if (vkCreateCommandPool(device_, &ci, nullptr,
                                &command_pool_) != VK_SUCCESS) {
            return false;
        }
    }

    inited_ = true;
    return true;
}

void Device::Deinit() {
    if (!inited_) {
        return;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
    }
    if (validation_enabled_ && debug_messenger_) {
        destroy_debug_messenger(instance_, debug_messenger_);
    }
    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }
    inited_ = false;
}

bool Device::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(device_, physical_, surface_, window,
                   command_pool_, graphics_queue_, msaa_samples_,
                   true);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
