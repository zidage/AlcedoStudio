//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_gpu_timestamp_pool.hpp"

#include "cuda/cuda_check.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kInitialSlotCount = 16;
constexpr std::size_t kInvalidIndex     = static_cast<std::size_t>(-1);

auto ElapsedNs(cudaEvent_t start, cudaEvent_t stop, bool& failed) -> std::int64_t {
  float milliseconds = 0.0f;
  const auto error   = ::cudaEventElapsedTime(&milliseconds, start, stop);
  if (error != cudaSuccess) {
    failed = true;
    return 0;
  }
  return static_cast<std::int64_t>(static_cast<double>(milliseconds) * 1.0e6);
}

}  // namespace

CudaGpuTimestampPool::CudaGpuTimestampPool() { Grow(kInitialSlotCount); }

CudaGpuTimestampPool::~CudaGpuTimestampPool() {
  for (auto& slot : slots_) {
    if (slot.start != nullptr) {
      ::cudaEventDestroy(slot.start);
      slot.start = nullptr;
    }
    if (slot.stop != nullptr) {
      ::cudaEventDestroy(slot.stop);
      slot.stop = nullptr;
    }
  }
}

void CudaGpuTimestampPool::Grow(const std::size_t extra) {
  slots_.reserve(slots_.size() + extra);
  for (std::size_t i = 0; i < extra; ++i) {
    Slot slot;
    cuda::CheckCuda(::cudaEventCreate(&slot.start), "CudaGpuTimestampPool::cudaEventCreate start");
    cuda::CheckCuda(::cudaEventCreate(&slot.stop), "CudaGpuTimestampPool::cudaEventCreate stop");
    slots_.push_back(slot);
  }
}

auto CudaGpuTimestampPool::AcquireFreeSlot() -> std::size_t {
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    if (!slots_[index].in_flight && !slots_[index].open) {
      return index;
    }
  }
  const auto index = slots_.size();
  Grow(slots_.empty() ? kInitialSlotCount : slots_.size());
  return index;
}

void CudaGpuTimestampPool::Recycle(Slot& slot) noexcept {
  slot.request_id    = 0;
  slot.submission_id = 0;
  slot.pass_index    = 0;
  slot.sub_index     = 0;
  slot.is_sub        = false;
  slot.open          = false;
  slot.recorded      = false;
  slot.in_flight     = false;
}

void CudaGpuTimestampPool::Begin(cudaStream_t stream, const std::uint64_t submission_id) {
  if (stream == nullptr || !diag::PreviewPerformanceEnabled()) {
    return;
  }
  const auto target = diag::PreviewPerformance::CurrentGpuSampleTarget();
  if (!target.valid) {
    return;
  }
  const auto index = AcquireFreeSlot();
  auto&      slot  = slots_[index];
  slot.request_id    = target.request_id;
  slot.submission_id = submission_id;
  slot.pass_index    = target.pass_index;
  slot.sub_index     = target.sub_index;
  slot.is_sub        = target.is_sub;
  slot.open          = true;
  slot.recorded      = false;
  slot.in_flight     = true;
  cuda::CheckCuda(::cudaEventRecord(slot.start, stream), "CudaGpuTimestampPool::Begin");
  open_stack_.push_back(index);
}

void CudaGpuTimestampPool::End(cudaStream_t stream) {
  if (stream == nullptr || open_stack_.empty()) {
    return;
  }
  const auto index = open_stack_.back();
  open_stack_.pop_back();
  if (index >= slots_.size()) {
    return;
  }
  auto& slot = slots_[index];
  if (!slot.open) {
    return;
  }
  cuda::CheckCuda(::cudaEventRecord(slot.stop, stream), "CudaGpuTimestampPool::End");
  slot.open     = false;
  slot.recorded = true;
}

void CudaGpuTimestampPool::ResolveReady() {
  for (auto& slot : slots_) {
    if (!slot.in_flight || !slot.recorded || slot.open) {
      continue;
    }
    const auto query = ::cudaEventQuery(slot.stop);
    if (query == cudaErrorNotReady) {
      continue;
    }
    const auto request_id = slot.request_id;
    const auto pass_index = slot.pass_index;
    const auto is_sub     = slot.is_sub;
    const auto sub_index  = slot.sub_index;
    if (query != cudaSuccess) {
      diag::PreviewPerformance::NoteGpuDuration(request_id, pass_index, is_sub, sub_index, 0,
                                                diag::PreviewGpuTimeStatus::Failed);
      Recycle(slot);
      continue;
    }
    bool failed = false;
    const auto gpu_ns = ElapsedNs(slot.start, slot.stop, failed);
    diag::PreviewPerformance::NoteGpuDuration(
        request_id, pass_index, is_sub, sub_index, gpu_ns,
        failed ? diag::PreviewGpuTimeStatus::Failed : diag::PreviewGpuTimeStatus::Available);
    Recycle(slot);
  }
}

void CudaGpuTimestampPool::DiscardAll() {
  open_stack_.clear();
  for (auto& slot : slots_) {
    Recycle(slot);
  }
}

auto CudaGpuTimestampPool::InFlightCount() const -> std::size_t {
  std::size_t count = 0;
  for (const auto& slot : slots_) {
    if (slot.in_flight) {
      ++count;
    }
  }
  return count;
}

auto CudaGpuTimestampPool::SlotCount() const -> std::size_t { return slots_.size(); }

}  // namespace alcedo
