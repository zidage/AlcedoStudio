//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_device_runtime_api.h>
#include <cuda_runtime_api.h>
#include <opencv2/core/hal/interface.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <opencv2/core/cuda.hpp>
#include <stdexcept>
#include <string>

#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/op_base.hpp"
#include "edit/scope/detail/scope_cuda_shared.cuh"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "kernel_stream_gpu.cuh"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace CUDA {
template <typename KernelStreamT>
class GPU_KernelLauncher {
 private:
  std::shared_ptr<ImageBuffer>               input_img_;

  float4*                                    work_buffer_    = nullptr;
  float4*                                    temp_buffer_    = nullptr;
  size_t                                     allocated_size_ = 0;

  std::shared_ptr<ImageBuffer>               output_img_;

  KernelStreamT                              kernel_stream_;

  cudaStream_t                               stream_ = nullptr;

  GPUOperatorParams                          params_;
  ViewerDisplayConfig                        display_config_{};

  IFrameSink*                                frame_sink_ = nullptr;
  FrameCompletionSubmission                  bound_frame_submission_{};
  // Monotonic present sequence for FinalDisplayFrameView::frame_id only.
  size_t                                     total_frames_rendered_ = 0;

 public:
  GPU_KernelLauncher(std::shared_ptr<ImageBuffer> input_img, KernelStreamT kernel_stream)
      : input_img_(input_img), kernel_stream_(kernel_stream) {
    const auto stream_err = cudaStreamCreate(&stream_);
    if (stream_err != cudaSuccess) {
      throw std::runtime_error(std::string("cudaStreamCreate failed: ") +
                               cudaGetErrorString(stream_err));
    }
  }

  void ReleaseScratchBuffers() {
    if (stream_) {
      (void)cudaStreamSynchronize(stream_);
    }
    if (work_buffer_) {
      cudaFree(work_buffer_);
      work_buffer_ = nullptr;
    }
    if (temp_buffer_) {
      cudaFree(temp_buffer_);
      temp_buffer_ = nullptr;
    }
    allocated_size_ = 0;
  }

  void ReleaseResources() {
    ReleaseScratchBuffers();
    kernel_stream_.ReleaseResources();
    params_.to_ws_lut_.Reset();
    params_.lmt_lut_.Reset();
    params_.to_output_lut_.Reset();
    params_.to_output_params_.Reset();
  }

  [[nodiscard]] auto GetAllocatedScratchBytes() const -> size_t { return allocated_size_; }

  ~GPU_KernelLauncher() {
    ReleaseResources();
    if (stream_) {
      cudaStreamDestroy(stream_);
      stream_ = nullptr;
    }
  }

  void SetInputImage(std::shared_ptr<ImageBuffer> input_img) {
    input_img_ = input_img;
    if (!input_img_) {
      throw std::runtime_error("GPU_KernelLauncher: input image is null.");
    }
    if (input_img_ && !input_img_->gpu_data_valid_ && input_img_->cpu_data_valid_) {
      input_img_->SyncToGPU();
    }
    cv::cuda::GpuMat gpu_mat = input_img_->GetCUDAImage();
    if (gpu_mat.type() != CV_32FC4) {
      throw std::runtime_error(
          std::string("GPU_KernelLauncher: expected input type CV_32FC4, got type ") +
          std::to_string(gpu_mat.type()));
    }

    size_t width       = gpu_mat.cols;
    size_t height      = gpu_mat.rows;
    size_t needed_size = width * height * sizeof(float4);

    if (needed_size > allocated_size_) {
      if (work_buffer_) {
        cudaFree(work_buffer_);
        work_buffer_ = nullptr;
      }
      if (temp_buffer_) {
        cudaFree(temp_buffer_);
        temp_buffer_ = nullptr;
      }

      const auto work_alloc_err = cudaMalloc((void**)&work_buffer_, needed_size);
      if (work_alloc_err != cudaSuccess) {
        throw std::runtime_error(std::string("cudaMalloc (work_buffer_) failed: ") +
                                 cudaGetErrorString(work_alloc_err));
      }
      const auto temp_alloc_err = cudaMalloc((void**)&temp_buffer_, needed_size);
      if (temp_alloc_err != cudaSuccess) {
        cudaFree(work_buffer_);
        work_buffer_ = nullptr;
        throw std::runtime_error(std::string("cudaMalloc (temp_buffer_) failed: ") +
                                 cudaGetErrorString(temp_alloc_err));
      }
      allocated_size_ = needed_size;

      {
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        if (cudaMemGetInfo(&free_bytes, &total_bytes) == cudaSuccess) {
          const size_t used_bytes = total_bytes - free_bytes;
          std::cout << "[VRAM] GPU_KernelLauncher scratch allocated (" << (needed_size >> 20)
                    << " MB each, 2 buffers): free=" << (free_bytes >> 20)
                    << " MB / total=" << (total_bytes >> 20)
                    << " MB (used=" << (used_bytes >> 20) << " MB)\n";
        }
      }
    }
  }

  void SetOutputImage(std::shared_ptr<ImageBuffer> output_img) { output_img_ = output_img; }

  void SetParams(OperatorParams& cpu_params) {
    params_         = GPUParamsConverter::ConvertFromCPU(cpu_params, params_);
    display_config_ = ViewerDisplayConfig{cpu_params.to_output_params_.encoding_space_,
                                          cpu_params.to_output_params_.eotf_};
  }

  void SetFrameSink(IFrameSink* frame_sink) { frame_sink_ = frame_sink; }

  void SetBoundFrameSubmission(const FrameCompletionSubmission& submission) {
    bound_frame_submission_ = submission;
  }

  void Execute() {
    if (!input_img_ || !work_buffer_) {
      throw std::runtime_error("Input image not set or work buffer not allocated.");
    }

    if (!stream_) {
      throw std::runtime_error("CUDA stream not initialized.");
    }

    cv::cuda::GpuMat gpu_mat = input_img_->GetCUDAImage();
    if (gpu_mat.type() != CV_32FC4) {
      throw std::runtime_error(
          std::string("GPU_KernelLauncher: expected execution input type CV_32FC4, got type ") +
          std::to_string(gpu_mat.type()));
    }
    const size_t      width        = static_cast<size_t>(gpu_mat.cols);
    const size_t      height       = static_cast<size_t>(gpu_mat.rows);
    GPUOperatorParams frame_params = params_;

    if (frame_sink_) {
      frame_sink_->EnsureSize(static_cast<int>(width), static_cast<int>(height));
    }

    {
      const auto copy_err = cudaMemcpy2DAsync(
          work_buffer_, width * sizeof(float4), gpu_mat.ptr<float4>(), gpu_mat.step,
          width * sizeof(float4), height, cudaMemcpyDeviceToDevice, stream_);
      if (copy_err != cudaSuccess) {
        throw std::runtime_error(std::string("cudaMemcpy2D (input->work) failed: ") +
                                 cudaGetErrorString(copy_err));
      }
    }

    float4* result_ptr = kernel_stream_.Process(work_buffer_, temp_buffer_, static_cast<int>(width),
                                                static_cast<int>(height),
                                                static_cast<size_t>(width), frame_params, stream_,
                                                /*sync=*/false);
    // Synchronize once later, right before presenting.

    if (frame_sink_) {
      const FrameWriteMapping mapping = frame_sink_->MapResourceForWrite();
      if (mapping) {
        if (mapping.pixel_format != FramePixelFormat::RGBA32F ||
            mapping.memory_domain != FrameMemoryDomain::CudaDevice) {
          frame_sink_->UnmapResource();
          throw std::runtime_error("GPU frame sink does not expose a CUDA RGBA32F mapping.");
        }

        const size_t row_bytes    = width * sizeof(float4);
        cudaError_t  out_copy_err = cudaSuccess;
        if (mapping.target_type == FrameWriteTargetType::LinearBuffer) {
          auto* mapped_ptr = static_cast<float4*>(mapping.data);
          out_copy_err =
              cudaMemcpy2DAsync(mapped_ptr, mapping.row_bytes, result_ptr, row_bytes, row_bytes,
                                height, cudaMemcpyDeviceToDevice, stream_);
        } else if (mapping.target_type == FrameWriteTargetType::CudaArray) {
          auto mapped_array = reinterpret_cast<cudaArray_t>(mapping.image_array);
          out_copy_err =
              cudaMemcpy2DToArrayAsync(mapped_array, 0, 0, result_ptr, row_bytes, row_bytes, height,
                                       cudaMemcpyDeviceToDevice, stream_);
        } else {
          frame_sink_->UnmapResource();
          throw std::runtime_error("Unsupported GPU frame sink target type.");
        }
        if (out_copy_err == cudaSuccess && mapping.cuda_signal_semaphore &&
            mapping.cuda_signal_value != 0) {
          cudaExternalSemaphoreSignalParams signal_params{};
          signal_params.params.fence.value = mapping.cuda_signal_value;
          cudaExternalSemaphore_t signal_semaphore =
              reinterpret_cast<cudaExternalSemaphore_t>(mapping.cuda_signal_semaphore);
          out_copy_err =
              cudaSignalExternalSemaphoresAsync(&signal_semaphore, &signal_params, 1, stream_);
        }
        if (out_copy_err != cudaSuccess) {
          frame_sink_->UnmapResource();
          throw std::runtime_error(std::string("cudaMemcpyAsync (work->frame) failed: ") +
                                   cudaGetErrorString(out_copy_err));
        }

        auto final_image           = std::make_shared<scope::cuda_detail::CudaLinearImageResource>();
        final_image->device_ptr    = result_ptr;
        final_image->row_bytes     = row_bytes;
        final_image->width         = static_cast<int>(width);
        final_image->height        = static_cast<int>(height);
        final_image->format        = FramePixelFormat::RGBA32F;
        final_image->owns_memory   = false;
        final_image->native_object = mapping.native_object;

        auto ready_signal    = std::make_shared<scope::cuda_detail::CudaStreamSignalResource>();
        ready_signal->stream = stream_;

        frame_sink_->SubmitFinalDisplayFrame(FinalDisplayFrameView{
            SharedGpuImageHandle{GpuBackend::Cuda, std::move(final_image), static_cast<int>(width),
                                 static_cast<int>(height), row_bytes, FramePixelFormat::RGBA32F},
            static_cast<int>(width), static_cast<int>(height), FramePixelFormat::RGBA32F,
            display_config_, AnalysisDomain::DisplayEncoded,
            GpuSignalHandle{GpuBackend::Cuda, std::move(ready_signal)},
            total_frames_rendered_ + 1});

        const auto sync_err = cudaStreamSynchronize(stream_);
        if (sync_err != cudaSuccess) {
          frame_sink_->UnmapResource();
          throw std::runtime_error(std::string("cudaStreamSynchronize (present) failed: ") +
                                   cudaGetErrorString(sync_err));
        }

        frame_sink_->UnmapResource();
        frame_sink_->NotifyFrameReady(bound_frame_submission_);
        ++total_frames_rendered_;
      }
    }

    if (output_img_) {
      output_img_->InitGPUData(width, height, CV_32FC4);
      cv::cuda::GpuMat output_gpu_mat = output_img_->GetCUDAImage();
      {
        const auto out_copy_err = cudaMemcpy2DAsync(
            output_gpu_mat.ptr<float4>(), output_gpu_mat.step, result_ptr, width * sizeof(float4),
            width * sizeof(float4), height, cudaMemcpyDeviceToDevice, stream_);
        if (out_copy_err != cudaSuccess) {
          throw std::runtime_error(std::string("cudaMemcpy2DAsync (work->output) failed: ") +
                                   cudaGetErrorString(out_copy_err));
        }
      }
      {
        const auto sync_err = cudaStreamSynchronize(stream_);
        if (sync_err != cudaSuccess) {
          throw std::runtime_error(std::string("cudaStreamSynchronize (output copy) failed: ") +
                                   cudaGetErrorString(sync_err));
        }
      }
      output_img_->SetGPUDataValid(true);
    }
  }
};
}  // namespace CUDA
};  // namespace alcedo
