//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <device_types.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "edit/operators/GPU_kernels/param.cuh"

namespace alcedo {
namespace CUDA {

template <typename T>
class HasApplyOps {
  template <typename U>
  static auto test(int)
      -> decltype(std::declval<U>().template ApplyOps<0>(nullptr,
                                                         std::declval<GPUOperatorParams&>()),
                  std::true_type());
  template <typename U>
  static std::false_type test(...);

 public:
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
class HasCustomDispatch {
  template <typename U>
  static auto test(int)
      -> decltype(std::declval<U>().Dispatch(std::declval<float4*>(), std::declval<float4*>(), 0,
                                             0, 0, std::declval<GPUOperatorParams&>(), dim3{},
                                             dim3{}, nullptr),
                  std::true_type());
  template <typename U>
  static std::false_type test(...);

 public:
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
class HasReleaseResources {
  template <typename U>
  static auto test(int) -> decltype(std::declval<U>().ReleaseResources(), std::true_type());
  template <typename U>
  static std::false_type test(...);

 public:
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename Op>
__global__ void GenericPointKernel(Op op, float4* __restrict src, float4* __restrict dst, int width,
                                   int height, size_t pitch_elems, GPUOperatorParams params) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;

  if (x < width && y < height) {
    const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);

    float4       v      = (src != nullptr) ? src[offset] : dst[offset];

    op.template ApplyOps<0>(&v, params);

    dst[offset] = v;
  }
}

template <typename Op>
__global__ void GenericNeighborKernel(Op  op, const float4* __restrict src, float4* __restrict dst,
                                      int width, int height, size_t pitch_elems,
                                      GPUOperatorParams params) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;

  if (x < width && y < height) {
    // NeighborOp interface: operator()(x, y, src, dst, w, h, pitch, params)
    op(x, y, src, dst, width, height, pitch_elems, params);
  }
}

// Minimal tuple-like chain that is trivially device-callable; avoids std::get, which can be
// host-only in some libstdc++ builds used by nvcc.
template <typename... Ts>
struct OpList;

template <>
struct OpList<> {
  OpList() = default;
  __device__ __forceinline__ void Apply(float4*, GPUOperatorParams&) {}
};

template <typename Head, typename... Tail>
struct OpList<Head, Tail...> {
  Head            head_;
  OpList<Tail...> tail_;

  OpList() = default;
  __host__ __device__ explicit OpList(Head h, Tail... t)
      : head_(std::move(h)), tail_(std::move(t)...) {}

  __device__ __forceinline__ void Apply(float4* p, GPUOperatorParams& params) {
    head_(p, params);
    tail_.Apply(p, params);
  }
};

template <typename... Ops>
struct GPU_PointChain {
  OpList<Ops...> ops_;

  GPU_PointChain(Ops... ops) : ops_(std::move(ops)...) {}

  template <size_t I = 0>
  __device__ __forceinline__ void ApplyOps(float4* p, GPUOperatorParams& params) {
    ops_.Apply(p, params);
  }
};

template <typename... Stages>
class GPU_StaticKernelStream {
  std::tuple<Stages...> stages_;

 public:
  GPU_StaticKernelStream(Stages... stages) : stages_(std::move(stages)...) {}

  template <size_t I = 0>
  inline void ReleaseResources() {
    if constexpr (I < sizeof...(Stages)) {
      auto& stage = std::get<I>(stages_);
      using StageType = std::decay_t<decltype(stage)>;
      if constexpr (HasReleaseResources<StageType>::value) {
        stage.ReleaseResources();
      }
      ReleaseResources<I + 1>();
    }
  }

  template <size_t I = 0>
  inline void Dispatch(float4* src, float4* dst, int width, int height, size_t pitch_elems,
                       GPUOperatorParams& params, dim3 grid, dim3 block, cudaStream_t stream) {
    if constexpr (I < sizeof...(Stages)) {
      auto& stage     = std::get<I>(stages_);
      using StageType = std::decay_t<decltype(stage)>;

      if constexpr (HasCustomDispatch<StageType>::value) {
        stage.Dispatch(src, dst, width, height, pitch_elems, params, grid, block, stream);
      } else if constexpr (HasApplyOps<StageType>::value) {
        // PointOp
        GenericPointKernel<<<grid, block, 0, stream>>>(stage, src, dst, width, height, pitch_elems,
                                                       params);
      } else {
        // NeighborOp
        GenericNeighborKernel<<<grid, block, 0, stream>>>(stage, src, dst, width, height,
                                                          pitch_elems, params);
      }

      {
        const auto launch_err = cudaGetLastError();
        if (launch_err != cudaSuccess) {
          std::cout << "CUDA kernel launch failed at stage " << I << ": "
                    << cudaGetErrorString(launch_err) << std::endl;
          throw std::runtime_error(std::string("CUDA kernel launch failed at stage ") +
                                   std::to_string(I) + ": " + cudaGetErrorString(launch_err));
        }
      }

      std::swap(src, dst);  // Ping-pong buffers
      Dispatch<I + 1>(src, dst, width, height, pitch_elems, params, grid, block, stream);
    }
  }

  float4* Process(float4* d_in, float4* d_temp, int width, int height, size_t pitch_elems,
                  GPUOperatorParams& params, cudaStream_t stream = 0, bool sync = true) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    Dispatch<0>(d_in, d_temp, width, height, pitch_elems, params, grid, block, stream);

    if (sync) {
      const auto sync_err = cudaStreamSynchronize(stream);
      if (sync_err != cudaSuccess) {
        throw std::runtime_error(std::string("CUDA stream sync failed: ") +
                                 cudaGetErrorString(sync_err));
      }
    }

    return (sizeof...(Stages) % 2 == 0) ? d_in : d_temp;
  }
};
};  // namespace CUDA
};  // namespace alcedo
