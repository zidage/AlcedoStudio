//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX1 experiment: a minimum Vulkan 1.3 compute program. One device, one compute queue, device-local
// buffers with device addresses, RGBA32F storage images, push descriptors, timestamp queries.
// Experiment code: it does not merge.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vx1 {

inline void VkCheck(VkResult result, const char* what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string("Vulkan: ") + what + " failed, VkResult " +
                             std::to_string(static_cast<int>(result)));
  }
}

inline std::vector<std::uint32_t> ReadSpirv(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("SPIR-V file not found: " + path);
  const auto size = static_cast<std::size_t>(file.tellg());
  if (size == 0 || size % 4 != 0) throw std::runtime_error("SPIR-V file has a bad size: " + path);
  std::vector<std::uint32_t> words(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
  return words;
}

struct VkBuffer_ {
  VkBuffer        buffer  = VK_NULL_HANDLE;
  VkDeviceMemory  memory  = VK_NULL_HANDLE;
  VkDeviceAddress address = 0;
  VkDeviceSize    size    = 0;
  void*           mapped  = nullptr;  // host-visible staging buffers only
};

struct VkImage_ {
  VkImage        image  = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView    view   = VK_NULL_HANDLE;
  std::uint32_t  width  = 0;
  std::uint32_t  height = 0;
};

struct VkKernel {
  std::string           name;
  VkShaderModule        module          = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout      = VK_NULL_HANDLE;
  VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
  VkPipeline            pipeline        = VK_NULL_HANDLE;
  std::uint32_t         push_size       = 0;
  std::uint32_t         image_bindings  = 0;
};

class VulkanCompute {
 public:
  explicit VulkanCompute(const std::string& device_filter, bool validation) {
    CreateInstance(validation);
    PickDevice(device_filter);
    CreateDevice();
  }

  ~VulkanCompute() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    for (auto& k : kernels_) {
      vkDestroyPipeline(device_, k.pipeline, nullptr);
      vkDestroyPipelineLayout(device_, k.pipeline_layout, nullptr);
      vkDestroyDescriptorSetLayout(device_, k.set_layout, nullptr);
      vkDestroyShaderModule(device_, k.module, nullptr);
    }
    for (auto& b : buffers_) Destroy(b);
    for (auto& i : images_) {
      vkDestroyImageView(device_, i.view, nullptr);
      vkDestroyImage(device_, i.image, nullptr);
      vkFreeMemory(device_, i.memory, nullptr);
    }
    if (query_pool_) vkDestroyQueryPool(device_, query_pool_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (messenger_) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy) destroy(instance_, messenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
  }

  VulkanCompute(const VulkanCompute&)            = delete;
  VulkanCompute& operator=(const VulkanCompute&) = delete;

  [[nodiscard]] const std::string& DeviceName() const { return device_name_; }
  [[nodiscard]] std::string        DriverInfo() const { return driver_info_; }
  [[nodiscard]] bool               ValidationEnabled() const { return validation_enabled_; }
  [[nodiscard]] std::uint32_t      ValidationErrors() const { return validation_errors_; }
  [[nodiscard]] std::uint32_t      ValidationWarnings() const { return validation_warnings_; }

  /// A device-local buffer with a device address. @p data (optional) is uploaded.
  VkBuffer_ CreateBuffer(VkDeviceSize size, const void* data = nullptr) {
    auto buffer = MakeBuffer(size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    buffers_.push_back(buffer);
    if (data != nullptr) Upload(buffers_.back(), data, size);
    return buffers_.back();
  }

  void Upload(const VkBuffer_& dst, const void* data, VkDeviceSize size) {
    auto staging = MakeStaging(size);
    std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));
    Submit([&](VkCommandBuffer cb) {
      const VkBufferCopy copy{0, 0, size};
      vkCmdCopyBuffer(cb, staging.buffer, dst.buffer, 1, &copy);
    });
    Destroy(staging);
  }

  void Download(const VkBuffer_& src, void* data, VkDeviceSize size) {
    auto staging = MakeStaging(size);
    Submit([&](VkCommandBuffer cb) {
      const VkBufferCopy copy{0, 0, size};
      vkCmdCopyBuffer(cb, src.buffer, staging.buffer, 1, &copy);
    });
    std::memcpy(data, staging.mapped, static_cast<std::size_t>(size));
    Destroy(staging);
  }

  /// An RGBA32F storage image in VK_IMAGE_LAYOUT_GENERAL. @p rgba (optional) is uploaded.
  VkImage_ CreateImage(std::uint32_t width, std::uint32_t height,
                              const float* rgba = nullptr) {
    VkImage_          image{};
    image.width  = width;
    image.height = height;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
    info.extent        = {width, height, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkCheck(vkCreateImage(device_, &info, nullptr, &image.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkCheck(vkAllocateMemory(device_, &alloc, nullptr, &image.memory), "vkAllocateMemory (image)");
    VkCheck(vkBindImageMemory(device_, image.image, image.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image            = image.image;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = VK_FORMAT_R32G32B32A32_SFLOAT;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCheck(vkCreateImageView(device_, &view, nullptr, &image.view), "vkCreateImageView");

    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4 * sizeof(float);
    VkBuffer_          staging{};
    if (rgba != nullptr) {
      staging = MakeStaging(bytes);
      std::memcpy(staging.mapped, rgba, static_cast<std::size_t>(bytes));
    }
    Submit([&](VkCommandBuffer cb) {
      Transition(cb, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 rgba ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
      if (rgba != nullptr) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {width, height, 1};
        vkCmdCopyBufferToImage(cb, staging.buffer, image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
      }
    });
    if (rgba != nullptr) Destroy(staging);
    images_.push_back(image);
    return images_.back();
  }

  std::vector<float> DownloadImage(const VkImage_& image) {
    const VkDeviceSize bytes = VkDeviceSize(image.width) * image.height * 4 * sizeof(float);
    auto               staging = MakeStaging(bytes);
    Submit([&](VkCommandBuffer cb) {
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent      = {image.width, image.height, 1};
      vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
    });
    std::vector<float> out(static_cast<std::size_t>(bytes / sizeof(float)));
    std::memcpy(out.data(), staging.mapped, static_cast<std::size_t>(bytes));
    Destroy(staging);
    return out;
  }

  /// A compute pipeline of @p entry in @p spirv_path. Set 0 has @p image_bindings storage images
  /// (push descriptors). The push constant range has @p push_size bytes.
  std::size_t CreateKernel(const std::string& spirv_path, const std::string& entry,
                           std::uint32_t push_size, std::uint32_t image_bindings) {
    // The record is in kernels_ before the first create call, so the destructor releases the
    // objects of a kernel whose pipeline creation fails.
    kernels_.emplace_back();
    VkKernel& kernel      = kernels_.back();
    kernel.name           = entry;
    kernel.push_size      = push_size;
    kernel.image_bindings = image_bindings;
    const auto               words = ReadSpirv(spirv_path);
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = words.size() * 4;
    module.pCode    = words.data();
    VkCheck(vkCreateShaderModule(device_, &module, nullptr, &kernel.module),
            "vkCreateShaderModule");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (std::uint32_t b = 0; b < image_bindings; ++b) {
      bindings.push_back({b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                          nullptr});
    }
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set.bindingCount = static_cast<std::uint32_t>(bindings.size());
    set.pBindings    = bindings.data();
    VkCheck(vkCreateDescriptorSetLayout(device_, &set, nullptr, &kernel.set_layout),
            "vkCreateDescriptorSetLayout");
    const VkPushConstantRange  range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount         = 1;
    layout.pSetLayouts            = &kernel.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges    = &range;
    VkCheck(vkCreatePipelineLayout(device_, &layout, nullptr, &kernel.pipeline_layout),
            "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage        = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = kernel.module;
    pipeline.stage.pName  = entry.c_str();
    pipeline.layout       = kernel.pipeline_layout;
    VkCheck(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                                     &kernel.pipeline),
            ("vkCreateComputePipelines (" + entry + ")").c_str());
    return kernels_.size() - 1;
  }

  /// Runs @p kernel once over (@p width, @p height) in work groups of 16 x 16, and returns the GPU
  /// time in milliseconds.
  double Dispatch(std::size_t kernel_index, const void* push, const std::vector<VkImage_>& images,
                  std::uint32_t width, std::uint32_t height) {
    const VkKernel& kernel = kernels_.at(kernel_index);
    Submit([&](VkCommandBuffer cb) {
      vkCmdResetQueryPool(cb, query_pool_, 0, 2);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pipeline);
      std::vector<VkDescriptorImageInfo> infos(images.size());
      std::vector<VkWriteDescriptorSet>  writes(images.size());
      for (std::size_t i = 0; i < images.size(); ++i) {
        infos[i]                  = {VK_NULL_HANDLE, images[i].view, VK_IMAGE_LAYOUT_GENERAL};
        writes[i]                 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstBinding      = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &infos[i];
      }
      if (!writes.empty()) {
        push_descriptor_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pipeline_layout, 0,
                         static_cast<std::uint32_t>(writes.size()), writes.data());
      }
      vkCmdPushConstants(cb, kernel.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         kernel.push_size, push);
      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, query_pool_, 0);
      vkCmdDispatch(cb, (width + 15) / 16, (height + 15) / 16, 1);
      vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, query_pool_, 1);
      VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
      barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.memoryBarrierCount = 1;
      dep.pMemoryBarriers    = &barrier;
      vkCmdPipelineBarrier2(cb, &dep);
    });
    std::uint64_t stamps[2]{};
    VkCheck(vkGetQueryPoolResults(device_, query_pool_, 0, 2, sizeof(stamps), stamps,
                                  sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults");
    return double(stamps[1] - stamps[0]) * timestamp_period_ns_ * 1.0e-6;
  }

 private:
  static VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* user) {
    auto* self = static_cast<VulkanCompute*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++self->validation_errors_;
      std::cerr << "[validation error] " << data->pMessage << "\n";
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
      ++self->validation_warnings_;
      std::cerr << "[validation warning] " << data->pMessage << "\n";
    }
    return VK_FALSE;
  }

  void CreateInstance(bool validation) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    const char* kValidation = "VK_LAYER_KHRONOS_validation";
    validation_enabled_     = validation && std::any_of(layers.begin(), layers.end(), [&](auto& l) {
                            return std::strcmp(l.layerName, kValidation) == 0;
                          });
    if (validation && !validation_enabled_) {
      throw std::runtime_error("Vulkan: the validation layer is not installed");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "alcedo-vx1";
    app.apiVersion       = VK_API_VERSION_1_3;
    std::vector<const char*> extensions;
    std::vector<const char*> enabled_layers;
    if (validation_enabled_) {
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
      enabled_layers.push_back(kValidation);
    }
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo        = &app;
    info.enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount       = static_cast<std::uint32_t>(enabled_layers.size());
    info.ppEnabledLayerNames     = enabled_layers.data();
    VkCheck(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    if (validation_enabled_) {
      VkDebugUtilsMessengerCreateInfoEXT messenger{
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      messenger.pfnUserCallback = &VulkanCompute::OnMessage;
      messenger.pUserData       = this;
      auto create               = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
      VkCheck(create(instance_, &messenger, nullptr, &messenger_), "vkCreateDebugUtilsMessenger");
    }
  }

  void PickDevice(const std::string& filter) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    std::string seen;
    for (auto device : devices) {
      VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2      props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props.pNext = &driver;
      vkGetPhysicalDeviceProperties2(device, &props);
      const std::string name = props.properties.deviceName;
      seen += "'" + name + "' ";
      if (name.find(filter) == std::string::npos) continue;
      if (props.properties.apiVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan: device '" + name + "' has no Vulkan 1.3");
      }
      physical_          = device;
      device_name_       = name;
      driver_info_       = std::string(driver.driverName) + " " + driver.driverInfo;
      timestamp_period_ns_ = props.properties.limits.timestampPeriod;
      return;
    }
    throw std::runtime_error("Vulkan: no device matches '" + filter + "'. Devices: " + seen);
  }

  void CreateDevice() {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
    family_ = UINT32_MAX;
    for (std::uint32_t i = 0; i < count; ++i) {
      if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && families[i].timestampValidBits > 0) {
        family_ = i;
        break;
      }
    }
    if (family_ == UINT32_MAX) throw std::runtime_error("Vulkan: no compute queue with timestamps");

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext               = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.scalarBlockLayout   = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext                   = &f12;
    features.features.shaderInt64    = VK_TRUE;
    features.features.shaderStorageImageWriteWithoutFormat = VK_FALSE;

    const float             priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family_;
    queue.queueCount       = 1;
    queue.pQueuePriorities = &priority;
    const char*        extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext                   = &features;
    info.queueCreateInfoCount    = 1;
    info.pQueueCreateInfos       = &queue;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = extensions;
    VkCheck(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    push_descriptor_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR"));
    if (!push_descriptor_) throw std::runtime_error("Vulkan: no vkCmdPushDescriptorSetKHR");

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family_;
    VkCheck(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool        = pool_;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCheck(vkAllocateCommandBuffers(device_, &alloc, &cb_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = 2;
    VkCheck(vkCreateQueryPool(device_, &query, nullptr, &query_pool_), "vkCreateQueryPool");
  }

  std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("Vulkan: no memory type with the required properties");
  }

  VkBuffer_ MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                       bool address) {
    VkBuffer_          buffer{};
    buffer.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size  = size;
    info.usage = usage;
    VkCheck(vkCreateBuffer(device_, &info, nullptr, &buffer.buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &req);
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext           = address ? &flags_info : nullptr;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, flags);
    VkCheck(vkAllocateMemory(device_, &alloc, nullptr, &buffer.memory),
            ("vkAllocateMemory (" + std::to_string(size) + " bytes)").c_str());
    VkCheck(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
    if (address) {
      VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      addr.buffer    = buffer.buffer;
      buffer.address = vkGetBufferDeviceAddress(device_, &addr);
    }
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      VkCheck(vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped), "vkMapMemory");
    }
    return buffer;
  }

  VkBuffer_ MakeStaging(VkDeviceSize size) {
    return MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      false);
  }

  void Destroy(VkBuffer_& buffer) {
    if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
    vkDestroyBuffer(device_, buffer.buffer, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
  }

  static void Transition(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout           = from;
    barrier.newLayout           = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cb, &dep);
  }

  void Submit(const std::function<void(VkCommandBuffer)>& record) {
    VkCheck(vkResetCommandBuffer(cb_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(cb_, &begin), "vkBeginCommandBuffer");
    record(cb_);
    VkCheck(vkEndCommandBuffer(cb_), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb_info.commandBuffer = cb_;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &cb_info;
    VkCheck(vkQueueSubmit2(queue_, 1, &submit, fence_), "vkQueueSubmit2");
    VkCheck(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    VkCheck(vkResetFences(device_, 1, &fence_), "vkResetFences");
  }

  VkInstance                   instance_        = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT     messenger_       = VK_NULL_HANDLE;
  VkPhysicalDevice             physical_        = VK_NULL_HANDLE;
  VkDevice                     device_          = VK_NULL_HANDLE;
  VkQueue                      queue_           = VK_NULL_HANDLE;
  std::uint32_t                family_          = 0;
  VkCommandPool                pool_            = VK_NULL_HANDLE;
  VkCommandBuffer              cb_              = VK_NULL_HANDLE;
  VkFence                      fence_           = VK_NULL_HANDLE;
  VkQueryPool                  query_pool_      = VK_NULL_HANDLE;
  PFN_vkCmdPushDescriptorSetKHR push_descriptor_ = nullptr;
  double                       timestamp_period_ns_ = 1.0;
  std::string                  device_name_;
  std::string                  driver_info_;
  bool                         validation_enabled_  = false;
  std::uint32_t                validation_errors_   = 0;
  std::uint32_t                validation_warnings_ = 0;
  std::vector<VkBuffer_>       buffers_;
  std::vector<VkImage_>        images_;
  std::vector<VkKernel>        kernels_;
};

}  // namespace vx1
