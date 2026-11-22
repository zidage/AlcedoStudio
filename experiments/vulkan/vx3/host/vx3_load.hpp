//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: the 45 megapixel load on the pipeline VkDevice of the VX2 context. A render is a
// list of dispatches (ops). A plan divides the ops into submissions: all in one, one for each op,
// or submissions of a GPU time budget, where an op is divided into bands of rows with
// vkCmdDispatchBase. Experiment code: it does not merge.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "vx2_vulkan.hpp"

namespace vx3 {

using vx2::VulkanContext;

struct Buffer {
  VkBuffer        buffer  = VK_NULL_HANDLE;
  VkDeviceMemory  memory  = VK_NULL_HANDLE;
  VkDeviceAddress address = 0;
  VkDeviceSize    size    = 0;
};

struct Image {
  VkImage        image  = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView    view   = VK_NULL_HANDLE;
};

/// One dispatch of a render. The grid is in threads (16 x 16 groups).
struct Op {
  std::string                  name;
  /// point: one pixel in, one out. neighbourhood: a window of taps. small: a plane of the LLF
  /// pyramid (2048 pixels long edge). spin: the timeout kernel.
  std::string                  cls;
  int                          kernel = -1;
  std::array<std::uint8_t, 128> push{};
  std::uint32_t                push_size = 0;
  VkImageView                  image0 = VK_NULL_HANDLE;
  VkImageView                  image1 = VK_NULL_HANDLE;
  std::uint32_t                width  = 0;
  std::uint32_t                height = 0;
  double                       ms     = 0;  // calibrated GPU time of the whole op
};

struct Band {
  int           op   = 0;
  std::uint32_t row0 = 0;  // thread rows, multiples of 16
  std::uint32_t row1 = 0;
};

struct Chunk {
  std::vector<Band> bands;
  double            estimate_ms = 0;
};

struct ChunkRecord {
  double       gpu_ms      = 0;  // timestamps at the start and the end of the command buffer
  std::int64_t submit_ns   = 0;
  std::int64_t complete_ns = 0;  // the fence wait returned
};

struct RunRecord {
  std::vector<ChunkRecord> chunks;
  std::int64_t             begin_ns = 0;
  std::int64_t             end_ns   = 0;
  bool                     device_lost = false;
  std::string              error;
};

class Gpu {
 public:
  Gpu(VulkanContext& ctx, const std::string& spv_dir);
  ~Gpu();
  Gpu(const Gpu&)            = delete;
  Gpu& operator=(const Gpu&) = delete;

  auto Kernel(const std::string& module, const std::string& entry) -> int;
  auto CreateBuffer(VkDeviceSize size) -> Buffer;
  auto CreateImage(std::uint32_t width, std::uint32_t height) -> Image;
  void Upload(const Buffer& dst, const void* data, std::size_t bytes);
  void Destroy(Buffer& buffer);
  void Destroy(Image& image);

  /// GPU time of each op of @p ops (one command buffer, a timestamp after each op). Best of @p runs.
  void Calibrate(std::vector<Op>& ops, int runs);
  /// Runs @p chunks with at most @p inflight submissions on the queue. Stops at the first error.
  auto Execute(const std::vector<Op>& ops, const std::vector<Chunk>& chunks, int inflight,
               std::int64_t wait_limit_ns) -> RunRecord;
  /// One command buffer of all @p ops, submitted and waited for (setup work).
  void RunOnce(const std::vector<Op>& ops);

  [[nodiscard]] auto Context() -> VulkanContext& { return ctx_; }
  [[nodiscard]] auto BytesAllocated() const -> std::uint64_t { return bytes_; }

 private:
  struct Slot {
    VkCommandBuffer cb    = VK_NULL_HANDLE;
    VkFence         fence = VK_NULL_HANDLE;
  };
  void Record(VkCommandBuffer cb, const std::vector<Op>& ops, const Chunk& chunk,
              std::uint32_t query0);
  auto SubmitLocked(VkCommandBuffer cb, VkFence fence) -> VkResult;
  void SetupLayout();

  VulkanContext&               ctx_;
  VkDevice                     dev_ = VK_NULL_HANDLE;
  std::string                  spv_dir_;
  VkDescriptorSetLayout        set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout             layout_     = VK_NULL_HANDLE;
  std::vector<VkShaderModule>  modules_;
  std::vector<std::string>     module_names_;
  std::vector<VkPipeline>      pipelines_;
  std::vector<std::string>     kernel_names_;
  VkCommandPool                pool_    = VK_NULL_HANDLE;
  VkQueryPool                  queries_ = VK_NULL_HANDLE;
  std::uint32_t                query_count_ = 0;
  std::vector<Slot>            slots_;
  double                       timestamp_ns_ = 1.0;
  std::uint64_t                bytes_ = 0;
};

/// Divides @p ops into submissions. mode: whole | op | budget (with @p budget_ms).
auto Plan(const std::vector<Op>& ops, const std::string& mode, double budget_ms)
    -> std::vector<Chunk>;

/// The resources and the ops of one 45 megapixel render: Local Tone (LLF) in the grade, then the
/// film print stages FP1, FP4 and FP5, FP6 with separable blurs of @p blur_radius.
struct Render {
  std::uint32_t      width = 0, height = 0;
  Image              scene_a, scene_b;
  Buffer             q, a, mid;
  Buffer             film_data, film_values, film_print, blur_weights;
  std::vector<Buffer> planes;
  std::vector<Op>    init;   // writes the scene (not part of a render)
  std::vector<Op>    ops;
  int                llf_levels = 0, llf_samples = 0;
  std::uint32_t      llf_width = 0, llf_height = 0;

  void Build(Gpu& gpu, std::uint32_t w, std::uint32_t h, int blur_radius, int blur_pairs);
  void Release(Gpu& gpu);
};

/// The ops of the timeout test. form: wide (many groups, each short), long (one group for the whole
/// time), many (@p count dispatches in one submission).
struct Spin {
  Buffer          out;
  std::vector<Op> ops;
  void Build(Gpu& gpu, const std::string& form, double target_ms, int count);
  void Release(Gpu& gpu);
};

}  // namespace vx3
