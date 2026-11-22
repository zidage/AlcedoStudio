//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: Vulkan context and producer. Experiment code: it does not merge.
#include "vx2_vulkan.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "vx2_common.hpp"

namespace vx2 {

void VkCheck(VkResult result, const char* what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string("Vulkan: ") + what + " failed, VkResult " +
                             std::to_string(static_cast<int>(result)));
  }
}

namespace {

auto Lower(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

auto HasLayer(const char* name) -> bool {
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const auto& l : layers) {
    if (std::strcmp(l.layerName, name) == 0) return true;
  }
  return false;
}

auto InstanceExtensions(const char* layer) -> std::vector<std::string> {
  std::uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
  std::vector<VkExtensionProperties> props(count);
  vkEnumerateInstanceExtensionProperties(layer, &count, props.data());
  std::vector<std::string> names;
  for (const auto& p : props) names.emplace_back(p.extensionName);
  return names;
}

auto Contains(const std::vector<std::string>& names, const char* name) -> bool {
  return std::find(names.begin(), names.end(), name) != names.end();
}

const char* FormatName(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "other";
  }
}

const char* ColorSpaceName(VkColorSpaceKHR space) {
  switch (space) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "EXTENDED_SRGB_LINEAR";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "DISPLAY_P3_NONLINEAR";
    case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return "EXTENDED_SRGB_NONLINEAR";
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return "BT2020_LINEAR";
    case VK_COLOR_SPACE_PASS_THROUGH_EXT: return "PASS_THROUGH";
    default: return "other";
  }
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::Message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  auto*             self = static_cast<VulkanContext*>(user);
  const std::string id   = data->pMessageIdName ? data->pMessageIdName : "(no id)";
  const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  if (error) {
    self->errors_++;
  } else {
    self->warnings_++;
  }
  if (id.find("Threading") != std::string::npos) self->threading_++;
  int seen = 0;
  {
    std::lock_guard<std::mutex> lock(self->ids_mutex_);
    seen = ++self->ids_[std::string(error ? "E " : "W ") + id];
  }
  if (seen <= 3) {
    Log("VALIDATION %s [%s] %s", error ? "ERROR" : "WARNING", id.c_str(),
        data->pMessage ? data->pMessage : "");
  }
  return VK_FALSE;
}

VulkanContext::VulkanContext(const VulkanOptions& options) {
  CreateInstance(options);
  PickDevice(options);
  CreateDevice(options);
}

VulkanContext::~VulkanContext() {
  if (split && pipe_device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(pipe_device);
    vkDestroyDevice(pipe_device, nullptr);
  }
  if (device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
  }
  if (messenger_ != VK_NULL_HANDLE) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance, messenger_, nullptr);
  }
  if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
}

void VulkanContext::CreateInstance(const VulkanOptions& options) {
  const auto available = InstanceExtensions(nullptr);
  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  for (const char* optional : {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                               VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
                               VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME}) {
    if (Contains(available, optional)) extensions.push_back(optional);
  }
  std::vector<const char*> layers;
  const bool validation = options.validation && HasLayer("VK_LAYER_KHRONOS_validation");
  if (options.validation && !validation) {
    throw std::runtime_error("VK_LAYER_KHRONOS_validation is not installed");
  }
  if (validation) layers.push_back("VK_LAYER_KHRONOS_validation");
  if (options.api_dump) {
    if (!HasLayer("VK_LAYER_LUNARG_api_dump")) {
      throw std::runtime_error("VK_LAYER_LUNARG_api_dump is not installed");
    }
    layers.push_back("VK_LAYER_LUNARG_api_dump");
  }

  // Thread safety is on by default in the validation layer; the settings make it explicit.
  const VkBool32 on = VK_TRUE, off = VK_FALSE;
  const VkBool32 sync = options.sync_validation ? on : off;
  std::vector<VkLayerSettingEXT> settings = {
      {"VK_LAYER_KHRONOS_validation", "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on},
      {"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &sync},
  };
  VkLayerSettingsCreateInfoEXT layer_settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
  layer_settings.settingCount = static_cast<std::uint32_t>(settings.size());
  layer_settings.pSettings    = settings.data();
  bool use_settings           = false;
  if (validation && Contains(InstanceExtensions("VK_LAYER_KHRONOS_validation"),
                             VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
    extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    use_settings = true;
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "alcedo_vx2";
  app.apiVersion       = VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pNext                   = use_settings ? &layer_settings : nullptr;
  info.pApplicationInfo        = &app;
  info.enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  info.enabledLayerCount       = static_cast<std::uint32_t>(layers.size());
  info.ppEnabledLayerNames     = layers.data();
  VkCheck(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
  std::string ext_text;
  for (auto* e : extensions) {
    ext_text += std::string(e) + " ";
    instance_extensions.emplace_back(e);
  }
  Log("INSTANCE extensions: %s", ext_text.c_str());
  Log("INSTANCE validation=%d thread_safety=%s sync_validation=%d", validation ? 1 : 0,
      use_settings ? "on (layer setting)" : "layer default", options.sync_validation ? 1 : 0);

  if (Contains(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    m.pfnUserCallback = &VulkanContext::Message;
    m.pUserData       = this;
    VkCheck(create(instance, &m, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
  }

  // The loader reports the implicit layers that it loaded for this instance.
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> all(count);
  vkEnumerateInstanceLayerProperties(&count, all.data());
  std::string names;
  for (const auto& l : all) names += std::string(l.layerName) + " ";
  Log("INSTANCE available layers: %s", names.c_str());
}

void VulkanContext::PickDevice(const VulkanOptions& options) {
  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  const std::string filter = Lower(options.device_filter);
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2  p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(d, &p);
    const std::string name = p.properties.deviceName;
    Log("DEVICE candidate '%s' type=%d api=%u.%u.%u luid=%08x%08x", name.c_str(),
        static_cast<int>(p.properties.deviceType), VK_API_VERSION_MAJOR(p.properties.apiVersion),
        VK_API_VERSION_MINOR(p.properties.apiVersion), VK_API_VERSION_PATCH(p.properties.apiVersion),
        id.deviceLUIDValid ? *reinterpret_cast<const std::uint32_t*>(id.deviceLUID + 4) : 0u,
        id.deviceLUIDValid ? *reinterpret_cast<const std::uint32_t*>(id.deviceLUID) : 0u);
    const bool match = filter.empty()
                           ? p.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                           : Lower(name).find(filter) != std::string::npos;
    if (match && physical == VK_NULL_HANDLE) {
      physical    = d;
      device_name = name;
      if (id.deviceLUIDValid) {
        std::memcpy(luid, id.deviceLUID, 8);
        luid_valid = true;
      }
    }
  }
  if (physical == VK_NULL_HANDLE) {
    throw std::runtime_error("no Vulkan device matches '" + options.device_filter + "'");
  }
}

void VulkanContext::CreateDevice(const VulkanOptions& options) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  bool found = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto flags = families[i].queueFlags;
    if ((flags & VK_QUEUE_GRAPHICS_BIT) && (flags & VK_QUEUE_COMPUTE_BIT) &&
        vkGetPhysicalDeviceWin32PresentationSupportKHR(physical, i)) {
      family = i;
      found  = true;
      break;
    }
  }
  if (!found) throw std::runtime_error("no graphics and compute queue family with presentation");
  const std::uint32_t queues = families[family].queueCount;
  if (options.separate_queue && queues < 2) {
    throw std::runtime_error("the graphics family of '" + device_name +
                             "' has one queue; run with --queue=shared");
  }
  split            = options.split_device;
  shared_queue     = !split && !(options.separate_queue && queues >= 2);
  qt_queue_index   = 0;
  pipe_queue_index = shared_queue || split ? 0 : 1;
  use_mutex        = shared_queue && options.queue_mutex;
  Log("QUEUE family=%u count=%u qt=(%u,%u) pipeline=(%u,%u)%s shared=%d mutex=%d", family, queues,
      family, qt_queue_index, family, pipe_queue_index, split ? " on the second VkDevice" : "",
      shared_queue ? 1 : 0, use_mutex ? 1 : 0);

  // Qt Quick at priority 1, the pipeline at 0.5.
  const float priorities[2] = {1.0f, 0.5f};
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = family;
  q.queueCount       = shared_queue || split ? 1 : 2;
  q.pQueuePriorities = priorities;

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
  VkPhysicalDeviceFeatures2        supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
  vkGetPhysicalDeviceFeatures2(physical, &supported);

  VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
  VkPhysicalDeviceFeatures2        enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
  if (!options.minimal_features) {
    enabled.features                   = supported.features;
    // Robust access changes the speed of every kernel; the product does not need it.
    enabled.features.robustBufferAccess = VK_FALSE;
  }
  e12.timelineSemaphore   = f12.timelineSemaphore;
  e12.bufferDeviceAddress = f12.bufferDeviceAddress;
  e12.scalarBlockLayout   = f12.scalarBlockLayout;
  e12.shaderFloat16       = f12.shaderFloat16;
  e13.synchronization2    = f13.synchronization2;
  e13.dynamicRendering    = f13.dynamicRendering;
  e13.maintenance4        = f13.maintenance4;

  std::uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> props(ext_count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, props.data());
  std::vector<std::string> available;
  for (const auto& p : props) available.emplace_back(p.extensionName);
  std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                         VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  for (const char* optional : {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VK_EXT_HDR_METADATA_EXTENSION_NAME}) {
    if (Contains(available, optional)) extensions.push_back(optional);
  }
  if (split) {
    if (!Contains(available, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
      throw std::runtime_error("VK_KHR_external_memory_win32 is absent");
    }
    extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
  }
  std::string ext_text;
  for (auto* e : extensions) ext_text += std::string(e) + " ";
  Log("DEVICE '%s' extensions: %s features=%s", device_name.c_str(), ext_text.c_str(),
      options.minimal_features ? "minimal" : "all supported core except robustBufferAccess");

  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
  info.queueCreateInfoCount    = 1;
  info.pQueueCreateInfos       = &q;
  info.enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size());
  info.ppEnabledExtensionNames = extensions.data();
  VkCheck(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");
  vkGetDeviceQueue(device, family, 0, &qt_queue);
  pipe_device = device;
  if (split) {
    // The same create info: one queue of the graphics family, on a VkDevice of its own.
    VkCheck(vkCreateDevice(physical, &info, nullptr, &pipe_device), "vkCreateDevice (pipeline)");
  }
  vkGetDeviceQueue(pipe_device, family, pipe_queue_index, &pipeline_queue);
  push_descriptor = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
      vkGetDeviceProcAddr(pipe_device, "vkCmdPushDescriptorSetKHR"));
  if (push_descriptor == nullptr) throw std::runtime_error("vkCmdPushDescriptorSetKHR is absent");
}

auto VulkanContext::MemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const
    -> std::uint32_t {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physical, &props);
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  throw std::runtime_error("no memory type with the requested properties");
}

void VulkanContext::LogFormatFeatures() const {
  VkFormatProperties p{};
  vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32A32_SFLOAT, &p);
  const auto f = p.optimalTilingFeatures;
  Log("FORMAT R32G32B32A32_SFLOAT optimal: sampled=%d linear_filter=%d storage=%d",
      (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : 0,
      (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1 : 0,
      (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? 1 : 0);
}

void VulkanContext::LogSurface(VkSurfaceKHR surface) const {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
  std::ostringstream text;
  for (const auto& f : formats) {
    text << FormatName(f.format) << "(" << static_cast<int>(f.format) << ")/"
         << ColorSpaceName(f.colorSpace) << " ";
  }
  Log("SURFACE formats of '%s': %s", device_name.c_str(), text.str().c_str());
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());
  std::ostringstream mode_text;
  for (auto m : modes) mode_text << static_cast<int>(m) << " ";
  Log("SURFACE present modes: %s(0 immediate, 1 mailbox, 2 fifo, 3 fifo relaxed)",
      mode_text.str().c_str());

  std::uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(d, &p);
    VkBool32 support = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(d, 0, surface, &support);
    Log("SURFACE support of '%s' (family 0): %d", p.deviceName, support ? 1 : 0);
  }
}

auto VulkanContext::ValidationReport() const -> std::string {
  std::ostringstream text;
  text << "validation_errors=" << errors_.load() << " validation_warnings=" << warnings_.load()
       << " threading_errors=" << threading_.load();
  std::lock_guard<std::mutex> lock(ids_mutex_);
  for (const auto& [id, n] : ids_) text << "\nVALIDATION_ID " << id << " count=" << n;
  return text.str();
}

// ---------------------------------------------------------------------------------------------

Producer::Producer(VulkanContext& context, const std::vector<float>& fixture, int width,
                   int height, int work, const std::vector<std::uint32_t>& spirv)
    : ctx_(context), width_(width), height_(height), work_(work) {
  VkDevice dev = ctx_.pipe_device;

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(ctx_.physical, &props);
  timestamp_ns_ = props.limits.timestampPeriod;

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = ctx_.family;
  VkCheck(vkCreateCommandPool(dev, &pool, nullptr, &pool_), "vkCreateCommandPool");

  // Source buffer, device local, filled through a host-visible staging buffer.
  const VkDeviceSize bytes = VkDeviceSize(fixture.size()) * sizeof(float);
  auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                         VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size  = size;
    b.usage = usage;
    VkCheck(vkCreateBuffer(dev, &b, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize  = req.size;
    a.memoryTypeIndex = ctx_.MemoryType(req.memoryTypeBits, flags);
    VkCheck(vkAllocateMemory(dev, &a, nullptr, &memory), "vkAllocateMemory");
    VkCheck(vkBindBufferMemory(dev, buffer, memory, 0), "vkBindBufferMemory");
  };
  make_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, source_, source_mem_);
  VkBuffer       staging     = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
              staging_mem);
  void* map = nullptr;
  VkCheck(vkMapMemory(dev, staging_mem, 0, bytes, 0, &map), "vkMapMemory");
  std::memcpy(map, fixture.data(), static_cast<std::size_t>(bytes));
  vkUnmapMemory(dev, staging_mem);

  // Pipeline: one push descriptor set (source buffer, target image, parameters).
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = spirv.size() * 4;
  sm.pCode    = spirv.data();
  VkCheck(vkCreateShaderModule(dev, &sm, nullptr, &module_), "vkCreateShaderModule");
  const VkDescriptorSetLayoutBinding bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  dsl.bindingCount = 3;
  dsl.pBindings    = bindings;
  VkCheck(vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts    = &set_layout_;
  VkCheck(vkCreatePipelineLayout(dev, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cp.stage  = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_COMPUTE_BIT, module_, "main", nullptr};
  cp.layout = layout_;
  VkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline_),
          "vkCreateComputePipelines");

  VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qp.queryType  = VK_QUERY_TYPE_TIMESTAMP;
  qp.queryCount = 2 * 8;
  VkCheck(vkCreateQueryPool(dev, &qp, nullptr, &queries_), "vkCreateQueryPool");

  // Ring of images. A retired image waits for 3 submitted Qt frames (2 frames in flight + 1).
  slots_.resize(5);
  for (auto& s : slots_) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent        = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    constexpr auto kHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = kHandle;
    if (ctx_.split) ii.pNext = &external;
    VkCheck(vkCreateImage(dev, &ii, nullptr, &s.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, s.image, &req);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = s.image;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
    export_info.handleTypes = kHandle;
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.pNext           = ctx_.split ? &export_info : nullptr;
    a.allocationSize  = req.size;
    a.memoryTypeIndex = ctx_.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkCheck(vkAllocateMemory(dev, &a, nullptr, &s.memory), "vkAllocateMemory (image)");
    VkCheck(vkBindImageMemory(dev, s.image, s.memory, 0), "vkBindImageMemory");
    if (ctx_.split) {
      // The same memory as an image of the Qt device: a Win32 handle, imported, then closed.
      auto get_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
          vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR"));
      VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
      gi.memory     = s.memory;
      gi.handleType = kHandle;
      HANDLE handle = nullptr;
      VkCheck(get_handle(dev, &gi, &handle), "vkGetMemoryWin32HandleKHR");
      VkCheck(vkCreateImage(ctx_.device, &ii, nullptr, &s.qt_image), "vkCreateImage (Qt device)");
      VkMemoryRequirements qreq{};
      vkGetImageMemoryRequirements(ctx_.device, s.qt_image, &qreq);
      VkMemoryDedicatedAllocateInfo qded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      qded.image = s.qt_image;
      VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                              &qded};
      import.handleType = kHandle;
      import.handle     = handle;
      VkMemoryAllocateInfo qa{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
      qa.allocationSize  = qreq.size;
      qa.memoryTypeIndex = ctx_.MemoryType(qreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkCheck(vkAllocateMemory(ctx_.device, &qa, nullptr, &s.qt_memory), "vkAllocateMemory (import)");
      VkCheck(vkBindImageMemory(ctx_.device, s.qt_image, s.qt_memory, 0), "vkBindImageMemory (import)");
      CloseHandle(handle);
    } else {
      s.qt_image = s.image;
    }
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image            = s.image;
    v.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    v.format           = VK_FORMAT_R32G32B32A32_SFLOAT;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCheck(vkCreateImageView(dev, &v, nullptr, &s.view), "vkCreateImageView");
    make_buffer(32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.ubo,
                s.ubo_mem);
    VkCheck(vkMapMemory(dev, s.ubo_mem, 0, 32, 0, &s.ubo_map), "vkMapMemory (ubo)");
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool        = pool_;
    cba.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCheck(vkAllocateCommandBuffers(dev, &cba, &s.cb), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(dev, &fi, nullptr, &s.fence), "vkCreateFence");
  }

  // Upload of the source; the images go to SHADER_READ_ONLY_OPTIMAL so that Qt can wrap them.
  VkCommandBuffer cb = slots_[0].cb;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkCheck(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");
  const VkBufferCopy copy{0, 0, bytes};
  vkCmdCopyBuffer(cb, staging, source_, 1, &copy);
  std::vector<VkImageMemoryBarrier> barriers;
  for (auto& s : slots_) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = s.image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers.push_back(b);
  }
  VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  bb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
  bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer              = source_;
  bb.size                = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       nullptr, 1, &bb, static_cast<std::uint32_t>(barriers.size()),
                       barriers.data());
  VkCheck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
  Submit(cb, slots_[0].fence);
  VkCheck(vkWaitForFences(dev, 1, &slots_[0].fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  vkResetFences(dev, 1, &slots_[0].fence);
  vkDestroyBuffer(dev, staging, nullptr);
  vkFreeMemory(dev, staging_mem, nullptr);

  if (ctx_.split) {
    // The imported images get SHADER_READ_ONLY_OPTIMAL on the Qt device, on Qt's queue, before
    // Qt starts. The layout of an image is a state of each VkImage, so each device sets its own.
    VkCommandPoolCreateInfo qpool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    qpool.queueFamilyIndex = ctx_.family;
    VkCommandPool qt_pool = VK_NULL_HANDLE;
    VkCheck(vkCreateCommandPool(ctx_.device, &qpool, nullptr, &qt_pool), "vkCreateCommandPool (Qt)");
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool        = qt_pool;
    cba.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer qcb = VK_NULL_HANDLE;
    VkCheck(vkAllocateCommandBuffers(ctx_.device, &cba, &qcb), "vkAllocateCommandBuffers (Qt)");
    VkCheck(vkBeginCommandBuffer(qcb, &begin), "vkBeginCommandBuffer (Qt)");
    for (std::size_t i = 0; i < slots_.size(); ++i) barriers[i].image = slots_[i].qt_image;
    vkCmdPipelineBarrier(qcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()),
                         barriers.data());
    VkCheck(vkEndCommandBuffer(qcb), "vkEndCommandBuffer (Qt)");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &qcb;
    VkCheck(vkQueueSubmit(ctx_.qt_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (Qt, layouts)");
    VkCheck(vkQueueWaitIdle(ctx_.qt_queue), "vkQueueWaitIdle (Qt, layouts)");
    vkDestroyCommandPool(ctx_.device, qt_pool, nullptr);
  }
}

Producer::~Producer() {
  Stop();
  VkDevice dev = ctx_.pipe_device;
  for (auto& s : slots_) {
    vkWaitForFences(dev, 1, &s.fence, VK_TRUE, 1'000'000'000ull);
    vkDestroyFence(dev, s.fence, nullptr);
    vkDestroyBuffer(dev, s.ubo, nullptr);
    vkFreeMemory(dev, s.ubo_mem, nullptr);
    vkDestroyImageView(dev, s.view, nullptr);
    vkDestroyImage(dev, s.image, nullptr);
    vkFreeMemory(dev, s.memory, nullptr);
    if (ctx_.split) {
      vkDestroyImage(ctx_.device, s.qt_image, nullptr);
      vkFreeMemory(ctx_.device, s.qt_memory, nullptr);
    }
  }
  vkDestroyQueryPool(dev, queries_, nullptr);
  vkDestroyPipeline(dev, pipeline_, nullptr);
  vkDestroyPipelineLayout(dev, layout_, nullptr);
  vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
  vkDestroyShaderModule(dev, module_, nullptr);
  vkDestroyCommandPool(dev, pool_, nullptr);
  vkDestroyBuffer(dev, source_, nullptr);
  vkFreeMemory(dev, source_mem_, nullptr);
}

void Producer::Submit(VkCommandBuffer cb, VkFence fence) {
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  if (ctx_.use_mutex) {
    // A wait above 2 s means that the Qt render thread holds the lock outside a frame.
    const std::int64_t t0 = NowNs();
    while (!ctx_.queue_mutex.try_lock_for(std::chrono::seconds(2))) {
      Log("ERROR queue mutex: the pipeline waits for more than 2 s");
    }
    std::lock_guard<std::timed_mutex> lock(ctx_.queue_mutex, std::adopt_lock);
    {
      std::lock_guard<std::mutex> stats(mutex_);
      lock_wait_ms_.push_back((NowNs() - t0) * 1e-6);
    }
    VkCheck(vkQueueSubmit(ctx_.pipeline_queue, 1, &si, fence), "vkQueueSubmit (pipeline)");
  } else {
    VkCheck(vkQueueSubmit(ctx_.pipeline_queue, 1, &si, fence), "vkQueueSubmit (pipeline)");
  }
}

void Producer::Start(std::function<void()> on_ready) {
  on_ready_ = std::move(on_ready);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }
  thread_ = std::thread([this] { Run(); });
}

void Producer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Producer::Request(float gain, std::int64_t request_ns) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gain_       = gain;
    pending_ns_ = request_ns;
    pending_    = true;
  }
  cv_.notify_all();
}

auto Producer::FindFree() -> int {
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    auto& s = slots_[i];
    if (s.state == State::Retired && submitted_frames_ >= s.free_at) s.state = State::Free;
    if (s.state == State::Free) return i;
  }
  return -1;
}

auto Producer::TakeLatest(std::uint64_t frame) -> std::optional<ProducerFrame> {
  std::lock_guard<std::mutex> lock(mutex_);
  int newest = -1;
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (slots_[i].state == State::Ready && (newest < 0 || slots_[i].id > slots_[newest].id)) {
      newest = i;
    }
  }
  if (newest < 0) return std::nullopt;
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    auto& s = slots_[i];
    if (i != newest && s.state == State::Ready) s.state = State::Free;  // never sampled
    if (s.state == State::Displayed) {
      // Last sampled in frame (frame - 1); complete when Qt has begun frame (frame + 1).
      s.state   = State::Retired;
      s.free_at = frame + 3;
    }
  }
  auto& n = slots_[newest];
  n.state = State::Displayed;
  cv_.notify_all();
  return ProducerFrame{newest, n.qt_image, n.id, n.request_ns, n.ready_ns};
}

void Producer::SetSubmittedFrames(std::uint64_t frames) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    submitted_frames_ = frames;
  }
  cv_.notify_all();
}

auto Producer::GpuTimesMs() -> std::vector<double> {
  std::lock_guard<std::mutex> lock(mutex_);
  return gpu_ms_;
}

auto Producer::LockWaitsMs() -> std::vector<double> {
  std::lock_guard<std::mutex> lock(mutex_);
  return lock_wait_ms_;
}

void Producer::Run() {
  VkDevice dev = ctx_.pipe_device;
  int      query_slot = 0;
  while (true) {
    int          slot = -1;
    float        gain = 1.0f;
    std::int64_t request_ns = 0;
    std::uint64_t id = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] {
        if (!running_) return true;
        if (!pending_ && !continuous_) return false;
        slot = FindFree();
        return slot >= 0;
      });
      if (!running_) return;
      pending_   = false;
      gain       = gain_;
      request_ns = pending_ns_;
      id         = next_id_++;
      slots_[slot].state = State::Writing;
    }
    auto& s = slots_[slot];
    const std::int32_t params[4] = {width_, height_, work_, 0};
    const float        grade[4]  = {gain, 0, 0, 0};
    std::memcpy(s.ubo_map, params, 16);
    std::memcpy(static_cast<char*>(s.ubo_map) + 16, grade, 16);

    VkCheck(vkResetCommandBuffer(s.cb, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(s.cb, &begin), "vkBeginCommandBuffer");
    const std::uint32_t q0 = static_cast<std::uint32_t>(query_slot * 2);
    vkCmdResetQueryPool(s.cb, queries_, q0, 2);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = s.image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, q0);
    vkCmdBindPipeline(s.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    VkDescriptorBufferInfo src{source_, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo  dst{VK_NULL_HANDLE, s.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo ubo{s.ubo, 0, 32};
    VkWriteDescriptorSet   writes[3]{};
    for (int i = 0; i < 3; ++i) {
      writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstBinding      = static_cast<std::uint32_t>(i);
      writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo    = &src;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo     = &dst;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo    = &ubo;
    ctx_.push_descriptor(s.cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 3, writes);
    vkCmdDispatch(s.cb, static_cast<std::uint32_t>((width_ + 15) / 16),
                  static_cast<std::uint32_t>((height_ + 15) / 16), 1);
    vkCmdWriteTimestamp(s.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, q0 + 1);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (ctx_.split) {
      // Release to the external owner (the Qt device acquires it in the frame that samples it).
      b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
      b.dstAccessMask       = 0;
      b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = ctx_.family;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkCheck(vkEndCommandBuffer(s.cb), "vkEndCommandBuffer");
    Submit(s.cb, s.fence);
    VkCheck(vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (pipeline)");
    vkResetFences(dev, 1, &s.fence);
    std::uint64_t stamps[2]{};
    const bool timed = vkGetQueryPoolResults(dev, queries_, q0, 2, sizeof(stamps), stamps,
                                             sizeof(std::uint64_t),
                                             VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    query_slot = (query_slot + 1) % 8;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      s.state      = State::Ready;
      s.id         = id;
      s.request_ns = request_ns;
      s.ready_ns   = NowNs();
      if (timed) gpu_ms_.push_back(static_cast<double>(stamps[1] - stamps[0]) * timestamp_ns_ * 1e-6);
    }
    if (on_ready_) on_ready_();
  }
}

}  // namespace vx2
