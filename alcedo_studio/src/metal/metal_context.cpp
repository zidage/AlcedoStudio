//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL
#include "metal/metal_context.hpp"

#include <stdexcept>

namespace alcedo {
namespace {

MTL::Device*       g_pending_device       = nullptr;
MTL::CommandQueue* g_pending_queue        = nullptr;
bool               g_instance_constructed = false;

}  // namespace

void MetalContext::BindPresentationDevice(MTL::Device* device, MTL::CommandQueue* queue) {
  if (device == nullptr) {
    throw std::runtime_error("MetalContext: presentation device is null.");
  }
  if (g_instance_constructed) {
    auto& instance = Instance();
    if (instance.Device() != device) {
      throw std::runtime_error(
          "MetalContext: a different Metal device is already in use; workspace, "
          "MetalImage, and present must share the presentation device.");
    }
    if (queue != nullptr && instance.Queue() != queue) {
      throw std::runtime_error("MetalContext: a different Metal command queue is already in use.");
    }
    return;
  }
  if (g_pending_device != nullptr && g_pending_device != device) {
    throw std::runtime_error(
        "MetalContext: presentation device was already bound to a different MTLDevice.");
  }
  if (g_pending_device != device) {
    device->retain();
    if (g_pending_device != nullptr) {
      g_pending_device->release();
    }
    g_pending_device = device;
  }
  if (queue != nullptr && g_pending_queue != queue) {
    queue->retain();
    if (g_pending_queue != nullptr) {
      g_pending_queue->release();
    }
    g_pending_queue = queue;
  }
}

MetalContext::MetalContext() {
  g_instance_constructed = true;
  if (g_pending_device != nullptr) {
    device_          = g_pending_device;
    g_pending_device = nullptr;
    if (g_pending_queue != nullptr) {
      queue_          = g_pending_queue;
      g_pending_queue = nullptr;
    } else {
      queue_ = device_->newCommandQueue();
    }
  } else {
    device_ = MTL::CreateSystemDefaultDevice();
    if (device_ == nullptr) {
      throw std::runtime_error("[FATAL] MetalContext: Failed to create Metal device.");
    }
    queue_ = device_->newCommandQueue();
  }
  if (queue_ == nullptr) {
    throw std::runtime_error("[FATAL] MetalContext: Failed to create Metal command queue.");
  }
}

MetalContext::~MetalContext() {
  if (queue_) {
    queue_->release();
    queue_ = nullptr;
  }
  if (device_) {
    device_->release();
    device_ = nullptr;
  }
}

auto MetalContext::Instance() -> MetalContext& {
  static MetalContext instance;
  return instance;
}

auto MetalContext::Device() const -> MTL::Device* { return device_; }

auto MetalContext::Queue() const -> MTL::CommandQueue* { return queue_; }
}  // namespace alcedo
#endif
