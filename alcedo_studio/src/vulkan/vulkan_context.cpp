//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_context.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <filesystem>
#endif

#include "utils/logging/log.hpp"
#include "vulkan/vulkan_device_selection.hpp"
#include "vulkan/vulkan_requirements.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo::vulkan {
namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

#if defined(__APPLE__)
/// The Vulkan loader files that macOS can have outside the dlopen search path: the loader of an
/// application bundle (Contents/Frameworks) and the Homebrew loader (Apple silicon, then Intel).
/// dlopen of a bare file name searches neither folder, so volkInitialize does not find them.
auto MacVulkanLoaderCandidates() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> candidates;
  uint32_t                           size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string executable(size, '\0');
  if (size > 0 && _NSGetExecutablePath(executable.data(), &size) == 0) {
    executable.resize(std::char_traits<char>::length(executable.c_str()));
    candidates.push_back(std::filesystem::path(executable).parent_path() / ".." / "Frameworks" /
                         "libvulkan.1.dylib");
  }
  candidates.emplace_back("/opt/homebrew/lib/libvulkan.1.dylib");
  candidates.emplace_back("/usr/local/lib/libvulkan.1.dylib");
  return candidates;
}
#endif

/// Loads the Vulkan loader into volk. Throws with the names that were tried when no loader loads.
void InitializeVulkanLoader() {
  const VkResult result = volkInitialize();
  if (result == VK_SUCCESS) {
    return;
  }
#if defined(__APPLE__)
  std::string tried = "the dlopen search path";
  for (const auto& candidate : MacVulkanLoaderCandidates()) {
    tried += ", " + candidate.string();
    // The library stays loaded for the lifetime of the process, as the loader that volkInitialize
    // loads does until volkFinalize.
    void* library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
      continue;
    }
    auto* get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
      dlclose(library);
      continue;
    }
    volkInitializeCustom(get_instance_proc_addr);
    return;
  }
  throw std::runtime_error("Vulkan: no Vulkan loader was found (tried " + tried + ").");
#else
  ThrowIfVulkanError(result, "volkInitialize (load the Vulkan loader)");
#endif
}

auto IsInstanceExtensionAvailable(const char* extension_name) -> bool {
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
  extensions.resize(count);
  return std::any_of(extensions.begin(), extensions.end(),
                     [extension_name](const VkExtensionProperties& extension) {
                       return std::string_view(extension.extensionName) == extension_name;
                     });
}

std::atomic<uint64_t> g_validation_error_count{0};

/// The slot of the process context and its lock.
struct ContextSlot {
  std::mutex                     mutex;
  std::unique_ptr<VulkanContext> context;
};

/// The slot is never destroyed: a context that the process does not destroy with
/// `VulkanContext::Destroy` stays until the process ends, and the driver releases it. A static
/// destructor would make Vulkan calls and unload the Vulkan loader while Windows holds the loader
/// lock of the DLL unload.
auto Slot() -> ContextSlot& {
  static auto* slot = new ContextSlot();
  return *slot;
}

auto ReadEnvironmentVariable(const char* name) -> std::string {
#if defined(_WIN32)
  char*       value  = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

auto IsInstanceLayerInstalled(const char* layer_name) -> bool {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  layers.resize(count);
  return std::any_of(layers.begin(), layers.end(), [layer_name](const VkLayerProperties& layer) {
    return std::string_view(layer.layerName) == layer_name;
  });
}

VKAPI_ATTR auto VKAPI_CALL OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                               VkDebugUtilsMessageTypeFlagsEXT             types,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data,
                                               void* /*user_data*/) -> VkBool32 {
  const char* message       = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "";
  // GENERAL messages come from the loader, for example a registered implicit layer whose JSON
  // file is missing. They describe the computer, not the use of the API, so they are not counted.
  const bool  is_validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
  const char* source        = is_validation ? "Vulkan validation: " : "Vulkan loader: ";
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 && is_validation) {
    g_validation_error_count.fetch_add(1, std::memory_order_relaxed);
    ALCEDO_LOG_ERROR(Gpu) << source << message;
  } else if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) != 0) {
    ALCEDO_LOG_WARN(Gpu) << source << message;
  }
  return VK_FALSE;
}

auto ValidationMessengerCreateInfo() -> VkDebugUtilsMessengerCreateInfoEXT {
  VkDebugUtilsMessengerCreateInfoEXT info{};
  info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = &OnValidationMessage;
  return info;
}

}  // namespace

auto VulkanInitializationOptions::FromEnvironment() -> VulkanInitializationOptions {
  VulkanInitializationOptions options;
  options.device_name_filter = ReadEnvironmentVariable("ALCEDO_VULKAN_DEVICE");
#ifdef NDEBUG
  options.enable_validation_layer = false;
#else
  options.enable_validation_layer = true;
#endif
  return options;
}

auto VulkanContext::CreateUnlocked(const VulkanInitializationOptions& options)
    -> std::unique_ptr<VulkanContext> {
  // The destructor releases a partly created context when a step throws.
  std::unique_ptr<VulkanContext> context(new VulkanContext());
  context->CreateInstance(options);
  context->SelectPhysicalDevice(options);
  context->CreatePipelineDevice();
  context->CreatePipelineAllocator();
  context->CreateKernelObjects(options);

  ALCEDO_LOG_INFO(Gpu) << "Vulkan: device " << context->capabilities_.name << ", Vulkan "
                       << FormatVulkanApiVersion(context->capabilities_.api_version)
                       << ", queue family " << context->pipeline_queue_.family_index
                       << ", validation " << (context->ValidationLayerEnabled() ? "on" : "off");
  return context;
}

auto VulkanContext::Create(const VulkanInitializationOptions& options) -> VulkanContext& {
  auto&           slot = Slot();
  std::lock_guard lock(slot.mutex);
  if (slot.context) {
    throw std::logic_error("VulkanContext::Create: the context of the process exists.");
  }
  slot.context = CreateUnlocked(options);
  return *slot.context;
}

auto VulkanContext::InstanceOrCreate(const VulkanInitializationOptions& options) -> VulkanContext& {
  auto&           slot = Slot();
  std::lock_guard lock(slot.mutex);
  if (!slot.context) {
    slot.context = CreateUnlocked(options);
  }
  return *slot.context;
}

auto VulkanContext::Instance() -> VulkanContext& {
  auto&           slot = Slot();
  std::lock_guard lock(slot.mutex);
  if (!slot.context) {
    throw std::logic_error("VulkanContext::Instance: VulkanContext::Create has not run.");
  }
  return *slot.context;
}

auto VulkanContext::IsCreated() -> bool {
  auto&           slot = Slot();
  std::lock_guard lock(slot.mutex);
  return slot.context != nullptr;
}

void VulkanContext::Destroy() {
  std::unique_ptr<VulkanContext> context;
  {
    auto&           slot = Slot();
    std::lock_guard lock(slot.mutex);
    context = std::move(slot.context);
  }
  // The destructor waits for the device and releases the objects outside the lock.
}

auto VulkanContext::ValidationErrorCount() -> uint64_t {
  return g_validation_error_count.load(std::memory_order_relaxed);
}

VulkanContext::~VulkanContext() { Release(); }

void VulkanContext::CreateInstance(const VulkanInitializationOptions& options) {
  InitializeVulkanLoader();
  loader_initialized_           = true;

  const uint32_t loader_version = volkGetInstanceVersion();
  if (loader_version < kRequiredVulkanApiVersion) {
    throw std::runtime_error("Vulkan: the Vulkan loader has version " +
                             FormatVulkanApiVersion(loader_version) + "; Vulkan " +
                             FormatVulkanApiVersion(kRequiredVulkanApiVersion) + " is required.");
  }

  const bool enable_validation =
      options.enable_validation_layer && IsInstanceLayerInstalled(kValidationLayerName);
  if (options.enable_validation_layer && !enable_validation) {
    ALCEDO_LOG_INFO(Gpu) << "Vulkan: " << kValidationLayerName
                         << " is not installed; the instance has no validation layer.";
  }

  VkApplicationInfo application{};
  application.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "Alcedo Studio";
  application.pEngineName      = "Alcedo";
  application.apiVersion       = kRequiredVulkanApiVersion;

  std::vector<const char*>           layers;
  std::vector<const char*>           extensions;
  // Chained into the instance create info, the messenger also reports the messages of
  // vkCreateInstance and vkDestroyInstance.
  VkDebugUtilsMessengerCreateInfoEXT messenger_info = ValidationMessengerCreateInfo();
  VkInstanceCreateInfo               create_info{};
  create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;
  if (enable_validation) {
    layers.push_back(kValidationLayerName);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    create_info.pNext = &messenger_info;
  }
  // A loader with portability drivers (MoltenVK on macOS) lists them only for an instance that
  // enables the portability enumeration.
  if (IsInstanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
#if defined(__APPLE__)
  // MoltenVK compiles each kernel with the Metal fast math option by default. The kernels need the
  // float rules of the OpenCL programs (decision B9): with fast math the RCD result of a Bayer RAW
  // differed from the strict result by up to 0.47, and without it the result was equal to strict
  // Metal bit for bit (phase VK6 record). 0 is MVK_CONFIG_FAST_MATH_NEVER.
  const uint32_t    fast_math_never = 0;
  VkLayerSettingEXT fast_math{};
  fast_math.pLayerName   = "MoltenVK";
  fast_math.pSettingName = "MVK_CONFIG_FAST_MATH_ENABLED";
  fast_math.type         = VK_LAYER_SETTING_TYPE_UINT32_EXT;
  fast_math.valueCount   = 1;
  fast_math.pValues      = &fast_math_never;
  VkLayerSettingsCreateInfoEXT layer_settings{};
  layer_settings.sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
  layer_settings.settingCount = 1;
  layer_settings.pSettings    = &fast_math;
  if (IsInstanceExtensionAvailable(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
    extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    layer_settings.pNext = create_info.pNext;
    create_info.pNext    = &layer_settings;
  }
#endif
  create_info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
  create_info.ppEnabledLayerNames     = layers.data();
  create_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
  create_info.ppEnabledExtensionNames = extensions.data();

  ThrowIfVulkanError(vkCreateInstance(&create_info, nullptr, &instance_), "vkCreateInstance");
  volkLoadInstanceOnly(instance_);

  if (enable_validation) {
    ThrowIfVulkanError(
        vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &debug_messenger_),
        "vkCreateDebugUtilsMessengerEXT");
  }
}

void VulkanContext::SelectPhysicalDevice(const VulkanInitializationOptions& options) {
  uint32_t count = 0;
  ThrowIfVulkanError(vkEnumeratePhysicalDevices(instance_, &count, nullptr),
                     "vkEnumeratePhysicalDevices");
  std::vector<VkPhysicalDevice> physical_devices(count);
  ThrowIfVulkanError(vkEnumeratePhysicalDevices(instance_, &count, physical_devices.data()),
                     "vkEnumeratePhysicalDevices");
  physical_devices.resize(count);

  std::vector<VulkanDeviceCapabilities> devices;
  devices.reserve(physical_devices.size());
  for (const VkPhysicalDevice physical_device : physical_devices) {
    devices.push_back(ReadVulkanDeviceCapabilities(physical_device));
  }

  const std::size_t selected = SelectVulkanDevice(devices, options.device_name_filter);
  physical_device_           = physical_devices[selected];
  capabilities_              = std::move(devices[selected]);
}

void VulkanContext::CreatePipelineDevice() {
  const auto family = FindPipelineQueueFamily(capabilities_);
  if (!family.has_value()) {
    // SelectVulkanDevice accepts only devices with this family.
    throw std::logic_error("VulkanContext: the selected device has no pipeline queue family.");
  }

  pipeline_device_extensions_.assign(std::begin(kRequiredDeviceExtensions),
                                     std::end(kRequiredDeviceExtensions));
  const bool enable_memory_priority = capabilities_.features.memory_priority;
  if (enable_memory_priority) {
    pipeline_device_extensions_.emplace_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
  }
  // The specification requires the portability subset to be enabled on a device that lists it
  // (MoltenVK).
  // (MoltenVK). The name macro is in the beta extension header, so the name is written here.
  constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";
  if (capabilities_.HasExtension(kPortabilitySubsetExtensionName)) {
    pipeline_device_extensions_.emplace_back(kPortabilitySubsetExtensionName);
  }
  std::sort(pipeline_device_extensions_.begin(), pipeline_device_extensions_.end());
  std::vector<const char*> extension_names;
  extension_names.reserve(pipeline_device_extensions_.size());
  for (const auto& name : pipeline_device_extensions_) {
    extension_names.push_back(name.c_str());
  }

  const float             queue_priority = 1.0F;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = *family;
  queue_info.queueCount       = 1;
  queue_info.pQueuePriorities = &queue_priority;

  VkPhysicalDeviceMemoryPriorityFeaturesEXT memory_priority{};
  memory_priority.sType          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
  memory_priority.memoryPriority = VK_TRUE;
  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features13.synchronization2 = VK_TRUE;
  if (enable_memory_priority) {
    features13.pNext = &memory_priority;
  }
  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features12.pNext               = &features13;
  features12.bufferDeviceAddress = VK_TRUE;
  features12.scalarBlockLayout   = VK_TRUE;
  VkPhysicalDeviceFeatures2 features{};
  features.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext                = &features12;
  features.features.shaderInt64 = VK_TRUE;

  VkDeviceCreateInfo create_info{};
  create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  create_info.pNext                   = &features;
  create_info.queueCreateInfoCount    = 1;
  create_info.pQueueCreateInfos       = &queue_info;
  create_info.enabledExtensionCount   = static_cast<uint32_t>(extension_names.size());
  create_info.ppEnabledExtensionNames = extension_names.data();

  VkDevice device                     = VK_NULL_HANDLE;
  ThrowIfVulkanError(vkCreateDevice(physical_device_, &create_info, nullptr, &device),
                     "vkCreateDevice (" + capabilities_.name + ")");
  volkLoadDeviceTable(&pipeline_device_functions_, device);

  pipeline_queue_.device       = device;
  pipeline_queue_.family_index = *family;
  pipeline_queue_.queue_index  = 0;
  pipeline_device_functions_.vkGetDeviceQueue(device, *family, 0, &pipeline_queue_.queue);
}

void VulkanContext::CreatePipelineAllocator() {
  VmaAllocatorCreateInfo create_info{};
  create_info.flags =
      VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  if (capabilities_.features.memory_priority) {
    create_info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
  }
  create_info.physicalDevice   = physical_device_;
  create_info.device           = pipeline_queue_.device;
  create_info.instance         = instance_;
  create_info.vulkanApiVersion = kRequiredVulkanApiVersion;

  VmaVulkanFunctions functions{};
  ThrowIfVulkanError(vmaImportVulkanFunctionsFromVolk(&create_info, &functions),
                     "vmaImportVulkanFunctionsFromVolk");
  create_info.pVulkanFunctions = &functions;
  ThrowIfVulkanError(vmaCreateAllocator(&create_info, &pipeline_allocator_), "vmaCreateAllocator");
}

void VulkanContext::CreateKernelObjects(const VulkanInitializationOptions& options) {
  shader_modules_ = std::make_unique<VulkanShaderModuleRegistry>(
      pipeline_queue_.device, pipeline_device_functions_,
      options.kernel_folders.value_or(VulkanKernelFolderCandidatesOfThisProcess()));

  VulkanComputePipelineCacheDevice device;
  device.device                       = pipeline_queue_.device;
  device.functions                    = &pipeline_device_functions_;
  device.identity.vendor_id           = capabilities_.vendor_id;
  device.identity.device_id           = capabilities_.device_id;
  device.identity.pipeline_cache_uuid = capabilities_.pipeline_cache_uuid;
  device.max_push_constant_bytes      = capabilities_.limits.maxPushConstantsSize;
  compute_pipelines_ = std::make_unique<VulkanComputePipelineCache>(device, *shader_modules_,
                                                                    options.pipeline_cache_file);
}

void VulkanContext::SubmitToPipelineQueue(const VkSubmitInfo2& submit, VkFence fence) {
  std::lock_guard lock(pipeline_queue_mutex_);
  ThrowIfVulkanError(
      pipeline_device_functions_.vkQueueSubmit2(pipeline_queue_.queue, 1, &submit, fence),
      "vkQueueSubmit2");
}

void VulkanContext::Release() noexcept {
  if (pipeline_queue_.device != VK_NULL_HANDLE) {
    // A failure here (device lost) leaves nothing to wait for; the objects are destroyed anyway.
    (void)pipeline_device_functions_.vkDeviceWaitIdle(pipeline_queue_.device);
  }
  if (compute_pipelines_) {
    try {
      compute_pipelines_->StoreToDisk();
    } catch (const std::exception& error) {
      // The next process starts with an empty pipeline cache; no result is lost.
      ALCEDO_LOG_WARN(Gpu) << "Vulkan: the pipeline cache data is not stored: " << error.what();
    }
    compute_pipelines_.reset();
  }
  shader_modules_.reset();
  if (pipeline_allocator_ != VK_NULL_HANDLE) {
    vmaDestroyAllocator(pipeline_allocator_);
    pipeline_allocator_ = VK_NULL_HANDLE;
  }
  if (pipeline_queue_.device != VK_NULL_HANDLE) {
    pipeline_device_functions_.vkDestroyDevice(pipeline_queue_.device, nullptr);
    pipeline_queue_ = {};
  }
  if (debug_messenger_ != VK_NULL_HANDLE) {
    vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
    debug_messenger_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  if (loader_initialized_) {
    volkFinalize();
    loader_initialized_ = false;
  }
}

}  // namespace alcedo::vulkan
