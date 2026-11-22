//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: the VkInstance and VkDevice that the program creates and gives to Qt Quick, and
// the producer thread that writes the photo image on the pipeline queue. Experiment code: it does
// not merge.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vx2 {

void VkCheck(VkResult result, const char* what);

struct VulkanOptions {
  std::string device_filter;
  bool        validation       = false;
  bool        sync_validation  = false;
  bool        api_dump         = false;
  /// Second queue of the graphics family for the pipeline, when the family has one.
  bool        separate_queue   = true;
  /// The queue mutex of plan section 4.2 (shared queue only).
  bool        queue_mutex      = true;
  /// Enable only the features that the pipeline needs, not every supported core feature.
  bool        minimal_features = false;
  /// A second VkDevice on the same physical device for the pipeline. The photo images are shared
  /// with the Qt device through Win32 external memory (zero copy). Qt's vkDeviceWaitIdle then
  /// does not touch a pipeline queue.
  bool        split_device     = false;
  /// VX5: VK_EXT_memory_priority, VK_EXT_pageable_device_local_memory, VK_EXT_device_fault, when
  /// the device has them.
  bool        memory_priority  = false;
  bool        pageable         = false;
  bool        device_fault     = false;
};

class VulkanContext {
 public:
  explicit VulkanContext(const VulkanOptions& options);
  ~VulkanContext();
  VulkanContext(const VulkanContext&)            = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  VkInstance       instance        = VK_NULL_HANDLE;
  VkPhysicalDevice physical        = VK_NULL_HANDLE;
  VkDevice         device          = VK_NULL_HANDLE;
  std::uint32_t    family          = 0;
  std::uint32_t    qt_queue_index  = 0;
  std::uint32_t    pipe_queue_index = 0;
  VkQueue          pipeline_queue  = VK_NULL_HANDLE;
  /// The device of the pipeline: equal to device, or the second device in split mode.
  VkDevice         pipe_device     = VK_NULL_HANDLE;
  bool             split           = false;
  /// Qt's queue, used by the program only before Qt starts (initial layouts in split mode).
  VkQueue          qt_queue        = VK_NULL_HANDLE;
  bool             shared_queue    = true;
  bool             use_mutex       = false;
  std::string      device_name;
  std::uint8_t     luid[8]{};
  bool             luid_valid = false;
  PFN_vkCmdPushDescriptorSetKHR push_descriptor = nullptr;

  /// The lock of plan section 4.2 when the pipeline and Qt Quick share one queue.
  std::timed_mutex queue_mutex;
  /// VX3: the pipeline queue has more than one user thread (the producer and the load); this lock
  /// is the external synchronization of vkQueueSubmit on it when queue_mutex is not in use.
  std::mutex       pipeline_submit_mutex;
  std::vector<std::string> instance_extensions;

  /// VX5: the optional extensions that the devices enabled.
  bool             memory_priority_enabled = false;
  bool             pageable_enabled        = false;
  bool             device_fault_enabled    = false;

  /// VX5 restore: destroys the pipeline device (the caller destroyed its objects) and creates a
  /// new one with the same create info. Split mode only.
  void RecreatePipelineDevice();
  /// VX5 restore: the same for the display device (the caller destroyed Qt's objects on it).
  void RecreateDisplayDevice();

  auto MemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const -> std::uint32_t;
  void LogSurface(VkSurfaceKHR surface) const;
  void LogFormatFeatures() const;
  [[nodiscard]] auto ValidationReport() const -> std::string;
  [[nodiscard]] auto ValidationErrors() const -> std::uint32_t { return errors_.load(); }
  [[nodiscard]] auto ThreadingErrors() const -> std::uint32_t { return threading_.load(); }

  static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT        types,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void* user);

 private:
  void CreateInstance(const VulkanOptions& options);
  void PickDevice(const VulkanOptions& options);
  void CreateDevice(const VulkanOptions& options);
  auto MakeDevice() -> VkDevice;
  void AdoptPipelineDevice();

  VulkanOptions                   options_;
  VkDebugUtilsMessengerEXT        messenger_ = VK_NULL_HANDLE;
  std::atomic<std::uint32_t>      errors_{0};
  std::atomic<std::uint32_t>      warnings_{0};
  std::atomic<std::uint32_t>      threading_{0};
  mutable std::mutex              ids_mutex_;
  std::map<std::string, int>      ids_;
};

/// The image that the producer writes and Qt Quick samples.
struct ProducerFrame {
  int           slot       = -1;
  /// The image on the Qt device (the imported image in split mode).
  VkImage       image      = VK_NULL_HANDLE;
  std::uint64_t id         = 0;
  std::int64_t  request_ns = 0;
  std::int64_t  ready_ns   = 0;
};

/// A thread that writes RGBA32F images with the compute shader on the pipeline queue. The images
/// go to Qt Quick with zero copy; a ring of slots keeps an image unchanged until the frames that
/// sample it are complete.
class Producer {
 public:
  Producer(VulkanContext& context, const std::vector<float>& fixture, int width, int height,
           int work, const std::vector<std::uint32_t>& spirv, bool qt_layouts = true);
  ~Producer();
  Producer(const Producer&)            = delete;
  Producer& operator=(const Producer&) = delete;

  void Start(std::function<void()> on_ready);
  void Stop();
  /// Newest request wins. Thread safe.
  void Request(float gain, std::int64_t request_ns);
  /// Render thread, in synchronize of frame @p frame: the newest ready image, or nothing.
  auto TakeLatest(std::uint64_t frame) -> std::optional<ProducerFrame>;
  /// Render thread, after the end of each frame: the count of frames that Qt has submitted.
  void SetSubmittedFrames(std::uint64_t frames);
  [[nodiscard]] auto Split() const -> bool { return ctx_.split; }
  [[nodiscard]] auto Family() const -> std::uint32_t { return ctx_.family; }
  [[nodiscard]] auto SlotCount() const -> int { return static_cast<int>(slots_.size()); }
  [[nodiscard]] auto GpuTimesMs() -> std::vector<double>;
  [[nodiscard]] auto LockWaitsMs() -> std::vector<double>;
  /// Continuous: the thread writes images back to back, as a long render does, without requests.
  void SetContinuous(bool continuous) { continuous_ = continuous; }
  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  /// The thread stopped on a Vulkan error (VX3: device loss).
  [[nodiscard]] auto Failed() const -> bool { return failed_.load(); }
  /// VX5: the images of the Qt device. With qt_layouts = false the constructor leaves them in
  /// UNDEFINED, and the Qt frame sets SHADER_READ_ONLY_OPTIMAL (a restore while Qt runs).
  [[nodiscard]] auto QtImages() const -> std::vector<VkImage> {
    std::vector<VkImage> images;
    for (const auto& s : slots_) images.push_back(s.qt_image);
    return images;
  }

 private:
  enum class State { Free, Writing, Ready, Displayed, Retired };
  struct Slot {
    VkImage         image   = VK_NULL_HANDLE;
    VkDeviceMemory  memory  = VK_NULL_HANDLE;
    VkImageView     view    = VK_NULL_HANDLE;
    VkImage         qt_image  = VK_NULL_HANDLE;  // split mode: the import on the Qt device
    VkDeviceMemory  qt_memory = VK_NULL_HANDLE;
    VkBuffer        ubo     = VK_NULL_HANDLE;
    VkDeviceMemory  ubo_mem = VK_NULL_HANDLE;
    void*           ubo_map = nullptr;
    VkCommandBuffer cb      = VK_NULL_HANDLE;
    VkFence         fence   = VK_NULL_HANDLE;
    State           state   = State::Free;
    std::uint64_t   id      = 0;
    std::int64_t    request_ns = 0;
    std::int64_t    ready_ns   = 0;
    std::uint64_t   free_at    = 0;
  };

  void Run();
  auto FindFree() -> int;
  void Submit(VkCommandBuffer cb, VkFence fence);

  VulkanContext&           ctx_;
  int                      width_  = 0;
  int                      height_ = 0;
  int                      work_   = 0;
  VkBuffer                 source_     = VK_NULL_HANDLE;
  VkDeviceMemory           source_mem_ = VK_NULL_HANDLE;
  VkShaderModule           module_     = VK_NULL_HANDLE;
  VkDescriptorSetLayout    set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout         layout_     = VK_NULL_HANDLE;
  VkPipeline               pipeline_   = VK_NULL_HANDLE;
  VkCommandPool            pool_       = VK_NULL_HANDLE;
  VkQueryPool              queries_    = VK_NULL_HANDLE;
  double                   timestamp_ns_ = 1.0;
  std::vector<Slot>        slots_;

  std::mutex               mutex_;
  std::condition_variable  cv_;
  bool                     running_  = false;
  bool                     pending_  = false;
  float                    gain_     = 1.0f;
  std::int64_t             pending_ns_ = 0;
  std::uint64_t            next_id_  = 1;
  std::uint64_t            submitted_frames_ = 0;
  std::vector<double>      gpu_ms_;
  std::vector<double>      lock_wait_ms_;
  bool                     continuous_ = false;
  std::atomic<bool>        failed_{false};
  std::function<void()>    on_ready_;
  std::thread              thread_;
};

}  // namespace vx2
