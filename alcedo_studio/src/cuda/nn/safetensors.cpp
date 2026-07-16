//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/safetensors.hpp"

#include <stdexcept>
#include <string>

namespace alcedo::cuda::nn {
namespace {

[[nodiscard]] auto Fail(const std::string& msg) -> std::runtime_error {
  return std::runtime_error(msg);
}

}  // namespace

auto UploadToDevice(const SafetensorsTensor& tensor, cudaStream_t stream) -> DeviceBufferF32 {
  if (tensor.dtype != SafetensorsTensor::Dtype::F32) {
    throw Fail("UploadToDevice: only F32 supported");
  }
  if (tensor.data.size() != tensor.numel()) {
    throw Fail("UploadToDevice: tensor '" + tensor.name +
               "' data length does not match shape product");
  }
  DeviceBufferF32 buf(tensor.data.size());
  if (!tensor.data.empty()) {
    buf.Upload(tensor.data, stream);
  }
  return buf;
}

void UploadTo(DeviceBufferF32& dst, const SafetensorsTensor& tensor, cudaStream_t stream) {
  if (tensor.dtype != SafetensorsTensor::Dtype::F32) {
    throw Fail("UploadTo: only F32 supported");
  }
  if (tensor.data.size() != tensor.numel()) {
    throw Fail("UploadTo: tensor '" + tensor.name + "' data length does not match shape product");
  }
  if (dst.size() != tensor.data.size()) {
    throw Fail("UploadTo: DeviceBuffer size " + std::to_string(dst.size()) +
               " does not match tensor numel " + std::to_string(tensor.data.size()) + " for '" +
               tensor.name + "'");
  }
  if (!tensor.data.empty()) {
    dst.Upload(tensor.data, stream);
  }
}

}  // namespace alcedo::cuda::nn
