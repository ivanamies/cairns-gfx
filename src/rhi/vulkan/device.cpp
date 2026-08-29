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

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

#include "rhi/device.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "util/alloc_tags.hpp"
#include "util/print_allocator.hpp"

namespace cairns::rhi {

namespace {

const std::vector<const char*,
                  cairns::print_allocator<const char*,
                                          cairns::tags::VkDeviceValidationLayers>>
    kValidationLayers = {"VK_LAYER_KHRONOS_validation"};
const std::vector<const char*,
                  cairns::print_allocator<const char*,
                                          cairns::tags::VkDeviceExtensionsConst>>
    kDeviceExtensions = {
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
    std::vector<VkLayerProperties,
                cairns::print_allocator<VkLayerProperties,
                                        cairns::tags::VkDeviceLayerProps>>
        available(count);
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
    std::vector<VkQueueFamilyProperties,
                cairns::print_allocator<VkQueueFamilyProperties,
                                        cairns::tags::VkDeviceQueueFamilies>>
        families(count);
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
    std::vector<VkExtensionProperties,
                cairns::print_allocator<VkExtensionProperties,
                                        cairns::tags::VkDeviceExtProps>>
        available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());  // INIT ONLY
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

// Headless variant: no surface, so no present queue, no swapchain. Picks any
// device with a graphics+compute queue family + samplerAnisotropy.
QueueFamilies find_queue_families_headless(VkPhysicalDevice device) {
    QueueFamilies indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties,
                cairns::print_allocator<VkQueueFamilyProperties,
                                        cairns::tags::VkDeviceQueueFamilies>>
        families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            indices.graphics_compute = i;
            indices.present = i;  // unused in headless but keeps complete() happy.
            break;
        }
    }
    return indices;
}

bool is_device_suitable_headless(VkPhysicalDevice device) {
    QueueFamilies indices = find_queue_families_headless(device);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    return indices.complete() && features.samplerAnisotropy;
}

}  // namespace

Device::~Device() { Deinit(); }

bool Device::Init(const InitConfig& cfg) {
    if (inited_) {
        return true;
    }

    // Surface-creation callback is required when not surfaceless. In
    // surfaceless mode there's no swapchain extension, no present queue.
    if (!cfg.surfaceless && !cfg.plat.vk_create_surface) {
        return false;
    }

#ifdef NDEBUG
    plat.validation_enabled_ = false;
#else
    plat.validation_enabled_ = true;
#endif
    if (plat.validation_enabled_ && !check_validation_layer_support()) {
        plat.validation_enabled_ = false;
    }

    {  // instance
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "cairns";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "cairns";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_2;

        std::vector<const char*,
                    cairns::print_allocator<const char*,
                                            cairns::tags::VkDeviceInstanceExts>>
            extensions(
                cfg.plat.vk_instance_extensions,
                cfg.plat.vk_instance_extensions + cfg.plat.vk_instance_extension_count);
        if (plat.validation_enabled_) {
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
        if (plat.validation_enabled_) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
            populate_debug_ci(dbg);
            ci.pNext = &dbg;
        }
        if (vkCreateInstance(&ci, nullptr, &plat.instance_) != VK_SUCCESS) {
            return false;
        }
    }

    if (plat.validation_enabled_) {  // debug messenger
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        populate_debug_ci(ci);
        create_debug_messenger(plat.instance_, &ci, &plat.debug_messenger_);
    }

    if (!cfg.surfaceless) {
        if (!cfg.plat.vk_create_surface(cfg.plat.vk_create_surface_user, plat.instance_,
                                    &plat.surface_)) {
            return false;
        }
    }

    {  // physical device
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(plat.instance_, &count, nullptr);
        if (count == 0) {
            return false;
        }
        std::vector<VkPhysicalDevice,
                    cairns::print_allocator<VkPhysicalDevice,
                                            cairns::tags::VkDevicePhysicalDevices>>
            devices(count);
        vkEnumeratePhysicalDevices(plat.instance_, &count, devices.data());
        for (VkPhysicalDevice d : devices) {
            const bool suitable = cfg.surfaceless
                ? is_device_suitable_headless(d)
                : is_device_suitable(d, plat.surface_);
            if (suitable) {
                plat.physical_ = d;
                plat.msaa_samples_ = max_usable_sample_count(d);
                break;
            }
        }
        if (plat.physical_ == VK_NULL_HANDLE) {
            return false;
        }
    }

    QueueFamilies indices = cfg.surfaceless
        ? find_queue_families_headless(plat.physical_)
        : find_queue_families(plat.physical_, plat.surface_);

    {  // logical device + queues
        std::set<uint32_t> unique = {indices.graphics_compute.value(),  // INIT ONLY
                                     indices.present.value()};
        std::vector<VkDeviceQueueCreateInfo,
                    cairns::print_allocator<VkDeviceQueueCreateInfo,
                                            cairns::tags::VkDeviceQueueCreateInfos>>
            queue_cis;
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
            vkGetPhysicalDeviceFeatures2(plat.physical_, &f2);
            plat.host_query_reset_ = (probe.hostQueryReset == VK_TRUE);
        }
        if (plat.host_query_reset_) {
            vk12.hostQueryReset = VK_TRUE;
        }
        // Resolved after vkCreateDevice below; see post-device-create block.
        {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(plat.physical_, &pp);
            plat.timestamp_period_ns_ = pp.limits.timestampPeriod;
        }

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.features.samplerAnisotropy = VK_TRUE;
        // #206 MRT: per-attachment blend state (color attachment 0 alpha-
        // blends, R32U id attachment has blendEnable=false). Without
        // independentBlend, all attachments must share the same blend state.
        features2.features.independentBlend = VK_TRUE;
        features2.pNext = &vk12;

        // Headless device skips VK_KHR_SWAPCHAIN (the only required one
        // besides portability_subset on Apple); portability_subset stays.
        std::vector<const char*,
                    cairns::print_allocator<const char*,
                                            cairns::tags::VkDeviceDeviceExts>>
            device_exts;
        for (const char* e : kDeviceExtensions) {
            if (cfg.surfaceless &&
                std::strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                continue;
            }
            device_exts.push_back(e);
        }

        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queue_cis.size());
        ci.pQueueCreateInfos = queue_cis.data();
        ci.pNext = &features2;
        ci.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
        ci.ppEnabledExtensionNames = device_exts.data();
        if (plat.validation_enabled_) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
        }
        if (vkCreateDevice(plat.physical_, &ci, nullptr, &plat.device_) !=
            VK_SUCCESS) {
            return false;
        }
        vkGetDeviceQueue(plat.device_, indices.graphics_compute.value(), 0,
                         &plat.graphics_queue_);
        vkGetDeviceQueue(plat.device_, indices.present.value(), 0,
                         &plat.present_queue_);
        vkGetDeviceQueue(plat.device_, indices.graphics_compute.value(), 0,
                         &plat.compute_queue_);
        plat.queue_family_index_ = indices.graphics_compute.value();
        if (plat.host_query_reset_) {
            plat.vk_reset_query_pool_ = reinterpret_cast<PFN_vkResetQueryPool>(
                vkGetDeviceProcAddr(plat.device_, "vkResetQueryPool"));
            if (plat.vk_reset_query_pool_ == nullptr) {
                plat.vk_reset_query_pool_ = reinterpret_cast<PFN_vkResetQueryPool>(
                    vkGetDeviceProcAddr(plat.device_, "vkResetQueryPoolEXT"));
            }
            if (plat.vk_reset_query_pool_ == nullptr) {
                plat.host_query_reset_ = false;
            }
        }
    }

    {  // command pool
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = indices.graphics_compute.value();
        if (vkCreateCommandPool(plat.device_, &ci, nullptr,
                                &plat.command_pool_) != VK_SUCCESS) {
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
    if (plat.device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(plat.device_);
    }
    if (plat.command_pool_) {
        vkDestroyCommandPool(plat.device_, plat.command_pool_, nullptr);
    }
    if (plat.device_) {
        vkDestroyDevice(plat.device_, nullptr);
    }
    if (plat.validation_enabled_ && plat.debug_messenger_) {
        destroy_debug_messenger(plat.instance_, plat.debug_messenger_);
    }
    if (plat.surface_) {
        vkDestroySurfaceKHR(plat.instance_, plat.surface_, nullptr);
    }
    if (plat.instance_) {
        vkDestroyInstance(plat.instance_, nullptr);
    }
    inited_ = false;
}

bool Device::InitSwapChain(SwapChain& sc, const InitConfig& cfg) {
    return sc.plat.Init(plat.device_, plat.physical_, plat.surface_,
                   cfg.plat.vk_window_size, cfg.plat.vk_window_size_user,
                   plat.command_pool_, plat.graphics_queue_, plat.msaa_samples_,
                   true);
}

void Device::WaitIdle() {
    if (plat.device_) {
        vkDeviceWaitIdle(plat.device_);
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
