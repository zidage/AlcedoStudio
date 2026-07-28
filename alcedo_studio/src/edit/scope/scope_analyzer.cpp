//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/scope/scope_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda_runtime_api.h>

#include "edit/scope/detail/scope_cuda_shared.cuh"
#endif

#ifdef HAVE_METAL
#include "edit/scope/detail/scope_metal_shared.hpp"
#endif

#ifdef HAVE_OPENCL
#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "opencl/opencl_context.hpp"
#endif

namespace alcedo {
namespace {

class NullScopeAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView&, const ScopeRequest&) override {}

  auto GetLatestOutput() -> ScopeOutputSet override { return {}; }

  void ResizeResources(const ScopeRequest&) override {}

  void ReleaseResources() override {}
};

class CompositeScopeAnalyzer final : public IScopeAnalyzer {
 public:
  explicit CompositeScopeAnalyzer(std::vector<std::shared_ptr<IScopeAnalyzer>> analyzers)
      : analyzers_(std::move(analyzers)) {}

  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) override {
    for (const auto& analyzer : analyzers_) {
      if (analyzer) {
        analyzer->SubmitFrame(frame, request);
      }
    }
  }

  auto GetLatestOutput() -> ScopeOutputSet override {
    ScopeOutputSet latest;
    for (const auto& analyzer : analyzers_) {
      if (!analyzer) {
        continue;
      }
      ScopeOutputSet output = analyzer->GetLatestOutput();
      if (output.generation > latest.generation) {
        latest = std::move(output);
      }
    }
    return latest;
  }

  void ResizeResources(const ScopeRequest& request) override {
    for (const auto& analyzer : analyzers_) {
      if (analyzer) {
        analyzer->ResizeResources(request);
      }
    }
  }

  void ReleaseResources() override {
    for (const auto& analyzer : analyzers_) {
      if (analyzer) {
        analyzer->ReleaseResources();
      }
    }
  }

 private:
  std::vector<std::shared_ptr<IScopeAnalyzer>> analyzers_;
};

constexpr float kHistogramClipWarningRatio = 0.02f;

auto            HistogramTailBins(int bins) -> int {
  if (bins <= 0) {
    return 0;
  }
  return std::clamp(bins / 64, 1, 4);
}

auto AverageTailRatio(const std::vector<uint32_t>& counts, int bins, int tail_bins,
                      bool highlight_tail) -> float {
  if (bins <= 0 || tail_bins <= 0 || counts.size() < static_cast<size_t>(bins * 3)) {
    return 0.0f;
  }

  float max_ratio = 0.0f;
  for (int channel = 0; channel < 3; ++channel) {
    const size_t channel_offset = static_cast<size_t>(channel) * static_cast<size_t>(bins);
    uint64_t     total_count    = 0U;
    uint64_t     tail_count     = 0U;
    for (int bin = 0; bin < bins; ++bin) {
      const uint32_t count = counts[channel_offset + static_cast<size_t>(bin)];
      total_count += static_cast<uint64_t>(count);
      const bool in_tail = highlight_tail ? (bin >= bins - tail_bins) : (bin < tail_bins);
      if (in_tail) {
        tail_count += static_cast<uint64_t>(count);
      }
    }
    if (total_count > 0U) {
      max_ratio =
          std::max(max_ratio, static_cast<float>(tail_count) / static_cast<float>(total_count));
    }
  }

  return max_ratio;
}

auto NormalizeHistogramToUnitRange(const std::vector<uint32_t>& counts, int bins)
    -> ScopeHistogramRenderData {
  ScopeHistogramRenderData data;
  if (bins <= 0 || counts.size() < static_cast<size_t>(bins * 3)) {
    return data;
  }

  data.bins                   = bins;
  data.clip_tail_bins         = HistogramTailBins(bins);
  data.shadow_clip_ratio      = AverageTailRatio(counts, bins, data.clip_tail_bins, false);
  data.highlight_clip_ratio   = AverageTailRatio(counts, bins, data.clip_tail_bins, true);
  data.shadow_clip_warning    = data.shadow_clip_ratio >= kHistogramClipWarningRatio;
  data.highlight_clip_warning = data.highlight_clip_ratio >= kHistogramClipWarningRatio;

  uint32_t denom_count        = 0U;
  for (int channel = 0; channel < 3; ++channel) {
    const size_t channel_offset = static_cast<size_t>(channel) * static_cast<size_t>(bins);
    for (int bin = 0; bin < bins; ++bin) {
      const bool skip_shadow    = data.shadow_clip_warning && bin < data.clip_tail_bins;
      const bool skip_highlight = data.highlight_clip_warning && bin >= bins - data.clip_tail_bins;
      if (skip_shadow || skip_highlight) {
        continue;
      }
      denom_count = std::max(denom_count, counts[channel_offset + static_cast<size_t>(bin)]);
    }
  }

  if (denom_count == 0U) {
    const auto max_it =
        std::max_element(counts.begin(), counts.begin() + static_cast<size_t>(bins * 3));
    if (max_it != counts.end()) {
      denom_count = *max_it;
    }
  }

  const float denom = denom_count > 0U ? static_cast<float>(denom_count) : 1.0f;

  data.rgb.resize(static_cast<size_t>(bins * 3), 0.0f);
  for (size_t i = 0; i < data.rgb.size(); ++i) {
    data.rgb[i] = std::clamp(static_cast<float>(counts[i]) / denom, 0.0f, 1.0f);
  }
  data.valid = true;
  return data;
}

auto NormalizeWaveformToUnitRange(const std::vector<float>& rgba, int width, int height)
    -> ScopeWaveformRenderData {
  ScopeWaveformRenderData data;
  if (width <= 0 || height <= 0 ||
      rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
    return data;
  }

  float max_value = 0.0f;
  for (float value : rgba) {
    max_value = std::max(max_value, value);
  }
  if (max_value <= std::numeric_limits<float>::epsilon()) {
    max_value = 1.0f;
  }

  data.width  = width;
  data.height = height;
  data.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, 0.0f);
  for (size_t i = 0; i < data.rgba.size(); ++i) {
    data.rgba[i] = std::clamp(rgba[i] / max_value, 0.0f, 1.0f);
  }
  data.valid = true;
  return data;
}

auto NormalizeWaveformCountsToUnitRange(const std::vector<uint32_t>& rgba, int width, int height)
    -> ScopeWaveformRenderData {
  if (width <= 0 || height <= 0 ||
      rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
    return {};
  }

  std::vector<float> waveform(rgba.size(), 0.0f);
  for (size_t i = 0; i < rgba.size(); ++i) {
    waveform[i] = static_cast<float>(rgba[i]);
  }
  return NormalizeWaveformToUnitRange(waveform, width, height);
}

}  // namespace

#ifdef HAVE_CUDA
auto CreateCudaScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer>;
#endif

#ifndef HAVE_METAL
auto CreateMetalScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  return std::make_shared<NullScopeAnalyzer>();
}
#endif

#ifndef HAVE_OPENCL
auto CreateOpenClScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  return std::make_shared<NullScopeAnalyzer>();
}
#endif

auto CreateDefaultScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  std::vector<std::shared_ptr<IScopeAnalyzer>> analyzers;
#ifdef HAVE_CUDA
  analyzers.push_back(CreateCudaScopeAnalyzer());
#endif
#ifdef HAVE_OPENCL
  analyzers.push_back(CreateOpenClScopeAnalyzer());
#endif
#ifdef HAVE_METAL
  analyzers.push_back(CreateMetalScopeAnalyzer());
#endif
  if (analyzers.empty()) {
    return std::make_shared<NullScopeAnalyzer>();
  }
  if (analyzers.size() == 1U) {
    return analyzers.front();
  }
  return std::make_shared<CompositeScopeAnalyzer>(std::move(analyzers));
}

auto ReadScopeRenderSnapshot(const ScopeOutputSet& output) -> ScopeRenderSnapshot {
  ScopeRenderSnapshot snapshot;
  snapshot.generation         = output.generation;
  snapshot.image_identity     = output.image_identity;
  snapshot.image_generation   = output.image_generation;
  snapshot.display_generation = output.display_generation;

#ifdef HAVE_CUDA
  if (output.histogram_valid && output.histogram_buffer &&
      output.histogram_buffer.backend == GpuBackend::Cuda) {
    const auto* resource = static_cast<const scope::cuda_detail::CudaDeviceBufferResource*>(
        output.histogram_buffer.resource.get());
    if (resource && resource->device_ptr && output.histogram_bins > 0) {
      std::vector<uint32_t> counts(static_cast<size_t>(output.histogram_bins) * 3U, 0U);
      const size_t          expected_bytes = counts.size() * sizeof(uint32_t);
      if (resource->size_bytes >= expected_bytes &&
          cudaMemcpy(counts.data(), resource->device_ptr, expected_bytes, cudaMemcpyDeviceToHost) ==
              cudaSuccess) {
        snapshot.histogram = NormalizeHistogramToUnitRange(counts, output.histogram_bins);
      }
    }
  }

  if (output.waveform_valid && output.waveform_image &&
      output.waveform_image.backend == GpuBackend::Cuda) {
    const auto* resource = static_cast<const scope::cuda_detail::CudaLinearImageResource*>(
        output.waveform_image.resource.get());
    if (resource && resource->device_ptr && output.waveform_width > 0 &&
        output.waveform_height > 0) {
      std::vector<float> rgba(static_cast<size_t>(output.waveform_width) *
                                  static_cast<size_t>(output.waveform_height) * 4U,
                              0.0f);
      const size_t row_bytes = static_cast<size_t>(output.waveform_width) * sizeof(float) * 4U;
      const auto   copy_status =
          cudaMemcpy2D(rgba.data(), row_bytes, resource->device_ptr, resource->row_bytes, row_bytes,
                       static_cast<size_t>(output.waveform_height), cudaMemcpyDeviceToHost);
      if (copy_status == cudaSuccess) {
        snapshot.waveform =
            NormalizeWaveformToUnitRange(rgba, output.waveform_width, output.waveform_height);
      }
    }
  }
#endif

#ifdef HAVE_OPENCL
  if (output.histogram_valid && output.histogram_buffer &&
      output.histogram_buffer.backend == GpuBackend::OpenCL) {
    const auto* resource = static_cast<const scope::opencl_detail::OpenClBufferResource*>(
        output.histogram_buffer.resource.get());
    if (resource && resource->buffer && output.histogram_bins > 0 &&
        OpenClContext::Instance().IsInitialized()) {
      std::vector<uint32_t> counts(static_cast<size_t>(output.histogram_bins) * 3U, 0U);
      const size_t          expected_bytes = counts.size() * sizeof(uint32_t);
      if (resource->size_bytes >= expected_bytes &&
          clEnqueueReadBuffer(OpenClContext::Instance().Queue(), resource->buffer, CL_TRUE, 0,
                              expected_bytes, counts.data(), 0, nullptr, nullptr) == CL_SUCCESS) {
        snapshot.histogram = NormalizeHistogramToUnitRange(counts, output.histogram_bins);
      }
    }
  }

  if (output.waveform_valid && output.waveform_image &&
      output.waveform_image.backend == GpuBackend::OpenCL) {
    const auto* resource = static_cast<const scope::opencl_detail::OpenClLinearImageResource*>(
        output.waveform_image.resource.get());
    if (resource && resource->buffer && output.waveform_width > 0 && output.waveform_height > 0 &&
        OpenClContext::Instance().IsInitialized()) {
      std::vector<uint32_t> rgba(static_cast<size_t>(output.waveform_width) *
                                     static_cast<size_t>(output.waveform_height) * 4U,
                                 0U);
      const size_t row_bytes = static_cast<size_t>(output.waveform_width) * sizeof(uint32_t) * 4U;
      if (resource->row_bytes == row_bytes) {
        if (clEnqueueReadBuffer(OpenClContext::Instance().Queue(), resource->buffer, CL_TRUE, 0,
                                rgba.size() * sizeof(uint32_t), rgba.data(), 0, nullptr,
                                nullptr) == CL_SUCCESS) {
          snapshot.waveform = NormalizeWaveformCountsToUnitRange(rgba, output.waveform_width,
                                                                 output.waveform_height);
        }
      } else if (resource->row_bytes >= row_bytes) {
        bool read_ok = true;
        for (int y = 0; y < output.waveform_height; ++y) {
          const cl_int err =
              clEnqueueReadBuffer(OpenClContext::Instance().Queue(), resource->buffer, CL_TRUE,
                                  static_cast<size_t>(y) * resource->row_bytes, row_bytes,
                                  rgba.data() + static_cast<size_t>(y) *
                                                    static_cast<size_t>(output.waveform_width) * 4U,
                                  0, nullptr, nullptr);
          if (err != CL_SUCCESS) {
            read_ok = false;
            break;
          }
        }
        if (read_ok) {
          snapshot.waveform = NormalizeWaveformCountsToUnitRange(rgba, output.waveform_width,
                                                                 output.waveform_height);
        }
      }
    }
  }
#endif

#ifdef HAVE_METAL
  if (output.histogram_valid && output.histogram_buffer &&
      output.histogram_buffer.backend == GpuBackend::Metal) {
    const auto* resource = static_cast<const scope::metal_detail::MetalBufferResource*>(
        output.histogram_buffer.resource.get());
    if (resource && resource->buffer && output.histogram_bins > 0) {
      std::vector<uint32_t> counts(static_cast<size_t>(output.histogram_bins) * 3U, 0U);
      const size_t          expected_bytes = counts.size() * sizeof(uint32_t);
      if (resource->size_bytes >= expected_bytes) {
        std::memcpy(counts.data(), resource->buffer->contents(), expected_bytes);
        snapshot.histogram = NormalizeHistogramToUnitRange(counts, output.histogram_bins);
      }
    }
  }

  if (output.waveform_valid && output.waveform_image &&
      output.waveform_image.backend == GpuBackend::Metal) {
    const auto* resource = static_cast<const scope::metal_detail::MetalLinearImageResource*>(
        output.waveform_image.resource.get());
    if (resource && resource->buffer && output.waveform_width > 0 && output.waveform_height > 0 &&
        resource->storage == scope::metal_detail::MetalLinearImageStorage::UInt32RGBA) {
      std::vector<uint32_t> rgba(static_cast<size_t>(output.waveform_width) *
                                     static_cast<size_t>(output.waveform_height) * 4U,
                                 0U);
      const size_t row_bytes = static_cast<size_t>(output.waveform_width) * sizeof(uint32_t) * 4U;
      const auto*  src_bytes = static_cast<const std::byte*>(resource->buffer->contents());
      for (int y = 0; y < output.waveform_height; ++y) {
        std::memcpy(
            rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(output.waveform_width) * 4U,
            src_bytes + static_cast<size_t>(y) * resource->row_bytes, row_bytes);
      }
      snapshot.waveform =
          NormalizeWaveformCountsToUnitRange(rgba, output.waveform_width, output.waveform_height);
    }
  }
#endif

#if !defined(HAVE_CUDA) && !defined(HAVE_METAL) && !defined(HAVE_OPENCL)
  (void)output;
#endif

  return snapshot;
}

}  // namespace alcedo
