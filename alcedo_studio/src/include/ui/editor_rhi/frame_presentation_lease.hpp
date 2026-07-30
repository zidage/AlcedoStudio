//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::editor_rhi {

// Backend-neutral presentation lease protocol used by FramePresentationBroker
// (Phase 2+) and exercised by EditorRhiHarness / production EditorViewportItem.
//
// Lifecycle:
//   1. Consumer (render thread) publishes WritableTargetLease objects for a generation.
//   2. Producer (pipeline worker) acquires a matching lease, fills the writable resource,
//      and submits CompletedFrameLease after GPU work is complete.
//   3. Consumer imports into QRhi, samples, then dual-sided release:
//      producer_done && renderer_done => destroy native object.
//
// Metal uses the same immutable lease boundary; the shared-texture path is
// qualified by the Phase 9 macOS harness.

enum class LeasePixelFormat : std::uint8_t {
  Rgba32f = 0,
};

enum class LeaseNativeHandleKind : std::uint8_t {
  None = 0,
  D3D11Texture2D,   // ID3D11Texture2D*  (presentation handle)
  OpenGLTexture2D,  // GLuint texture name
  MetalTexture,     // MTLTexture* (id) as opaque pointer
};

// Writable resource handed to the producer. Distinct from the presentation
// native_handle and from any synchronization primitive.
enum class LeaseWritableResourceKind : std::uint8_t {
  None = 0,
  CudaArray,     // cudaArray_t as opaque pointer
  OpenClImage,   // cl_mem image
  MetalTexture,  // same as presentation handle for Metal
};

// The broker keeps these layer identities independent from the edit-viewer
// implementation so a future Metal adapter can use the same protocol.
enum class LeaseFrameLayer : std::uint8_t {
  InteractivePrimary = 0,
  QualityBase,
  DetailPatch,
};

// Mirrors alcedo::FramePresentationMode without pulling edit-viewer headers into
// the contracts library. Convert at the IFrameSink / renderer boundary.
enum class LeasePresentationMode : std::uint8_t {
  FullFrame = 0,
  RoiFrame  = 1,
};

struct LeaseDimensions {
  int                width  = 0;
  int                height = 0;

  [[nodiscard]] auto valid() const -> bool { return width > 0 && height > 0; }
  [[nodiscard]] auto operator==(const LeaseDimensions& other) const -> bool = default;
};

// Immutable identity for one presentation target generation.
// image_identity is the durable DB image id; image_generation is a monotonic
// session counter that advances on every open/switch (including A→B→A).
struct TargetGeneration {
  std::uint64_t target_generation = 0;
  std::uint64_t image_generation  = 0;
  std::uint64_t layer_generation  = 0;
  std::uint64_t image_identity    = 0;
};

// Producer request for a writable target. The render thread creates or reuses
// targets that match layer + dimensions + generation.
struct WritableTargetRequest {
  LeaseFrameLayer    layer = LeaseFrameLayer::InteractivePrimary;
  LeaseDimensions    dimensions{};
  std::uint64_t      layer_generation = 0;
  std::uint64_t      image_generation = 0;
  std::uint64_t      image_identity   = 0;

  [[nodiscard]] auto valid() const -> bool { return dimensions.valid() && image_generation != 0; }
};

// Dual-sided lifetime token. Native resources are destroyed only when both
// sides have marked complete and the shared_ptr is released by the adapter.
struct LeaseLifetimeToken {
  std::atomic<bool> producer_complete{false};
  std::atomic<bool> renderer_complete{false};
  std::atomic<bool> cancel_requested{false};

  void MarkProducerComplete() { producer_complete.store(true, std::memory_order_release); }
  void MarkRendererComplete() { renderer_complete.store(true, std::memory_order_release); }
  void RequestCancel() { cancel_requested.store(true, std::memory_order_release); }

  [[nodiscard]] auto cancelled() const -> bool {
    return cancel_requested.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto can_destroy() const -> bool {
    return producer_complete.load(std::memory_order_acquire) &&
           renderer_complete.load(std::memory_order_acquire);
  }
};

// Published by the render thread for the producer to write into.
struct WritableTargetLease {
  EditorBackend                       backend       = EditorBackend::Cuda;
  LeaseNativeHandleKind               handle_kind   = LeaseNativeHandleKind::None;
  LeaseWritableResourceKind           writable_kind = LeaseWritableResourceKind::None;
  LeasePixelFormat                    pixel_format  = LeasePixelFormat::Rgba32f;
  LeaseDimensions                     dimensions{};
  TargetGeneration                    generation{};
  LeaseFrameLayer                     layer             = LeaseFrameLayer::InteractivePrimary;
  // Presentation-side native handle (D3D11 texture / GL name / Metal texture).
  std::uintptr_t                      native_handle     = 0;
  // Producer-writable resource. CUDA: cudaArray_t. OpenCL: cl_mem image.
  // Never overloaded as a sync object.
  std::uintptr_t                      writable_resource = 0;
  // Optional external semaphore / fence for producer->consumer sync.
  std::uintptr_t                      sync_object       = 0;
  std::uint64_t                       sync_value        = 0;
  // Keeps the native object alive until dual-sided release.
  std::shared_ptr<LeaseLifetimeToken> lifetime_token{};

  [[nodiscard]] auto                  valid() const -> bool {
    return dimensions.valid() && native_handle != 0 && lifetime_token != nullptr &&
           handle_kind != LeaseNativeHandleKind::None &&
           writable_kind != LeaseWritableResourceKind::None && writable_resource != 0;
  }
};

// Submitted by the producer after filling the target and waiting for GPU write
// completion. producer_complete must be true only after the write is ordered.
struct CompletedFrameLease {
  WritableTargetLease   target{};
  TargetGeneration      generation{};
  LeaseFrameLayer       layer                   = LeaseFrameLayer::InteractivePrimary;
  std::uint64_t         preview_generation      = 0;
  std::uint64_t         detail_serial           = 0;
  std::uint64_t         presentation_request_id = 0;
  float                 roi_x                   = 0.0f;
  float                 roi_y                   = 0.0f;
  float                 roi_width               = 1.0f;
  float                 roi_height              = 1.0f;
  LeasePresentationMode presentation_mode       = LeasePresentationMode::FullFrame;
  bool                  producer_complete       = false;

  [[nodiscard]] auto    valid() const -> bool {
    return target.valid() && producer_complete &&
           generation.target_generation == target.generation.target_generation &&
           generation.image_generation == target.generation.image_generation &&
           generation.image_identity == target.generation.image_identity &&
           (target.generation.layer_generation == 0 ||
            generation.layer_generation == target.generation.layer_generation) &&
           layer == target.layer;
  }
};

// Dual-sided release bookkeeping snapshot (diagnostic / adapter use).
struct LeaseReleaseState {
  std::uint64_t                       target_generation = 0;
  bool                                producer_complete = false;
  bool                                renderer_complete = false;
  std::shared_ptr<LeaseLifetimeToken> lifetime_token{};

  [[nodiscard]] auto                  can_destroy() const -> bool {
    return producer_complete && renderer_complete && lifetime_token != nullptr;
  }
};

// Metal-specific payload carried inside lifetime_token / native_handle.
// Defined now so Windows and macOS share one lease schema.
struct MetalSharedTextureLeasePayload {
  // Opaque MTLTexture* retained by lifetime_token.
  std::uintptr_t mtl_texture      = 0;
  // Optional MTLSharedEvent* / value for cross-queue sync.
  std::uintptr_t mtl_shared_event = 0;
  std::uint64_t  mtl_signal_value = 0;
  int            width            = 0;
  int            height           = 0;
  // MTLPixelFormat as integer.
  int            mtl_pixel_format = 0;
};

[[nodiscard]] inline auto LeaseHandleKindForBackend(EditorBackend backend)
    -> LeaseNativeHandleKind {
  switch (backend) {
    case EditorBackend::Cuda:
      return LeaseNativeHandleKind::D3D11Texture2D;
    case EditorBackend::OpenCl:
      return LeaseNativeHandleKind::OpenGLTexture2D;
    case EditorBackend::Metal:
      return LeaseNativeHandleKind::MetalTexture;
  }
  return LeaseNativeHandleKind::None;
}

[[nodiscard]] inline auto LeaseWritableKindForBackend(EditorBackend backend)
    -> LeaseWritableResourceKind {
  switch (backend) {
    case EditorBackend::Cuda:
      return LeaseWritableResourceKind::CudaArray;
    case EditorBackend::OpenCl:
      return LeaseWritableResourceKind::OpenClImage;
    case EditorBackend::Metal:
      return LeaseWritableResourceKind::MetalTexture;
  }
  return LeaseWritableResourceKind::None;
}

[[nodiscard]] inline auto ToString(LeaseNativeHandleKind kind) -> const char* {
  switch (kind) {
    case LeaseNativeHandleKind::None:
      return "none";
    case LeaseNativeHandleKind::D3D11Texture2D:
      return "d3d11_texture2d";
    case LeaseNativeHandleKind::OpenGLTexture2D:
      return "opengl_texture2d";
    case LeaseNativeHandleKind::MetalTexture:
      return "metal_texture";
  }
  return "unknown";
}

[[nodiscard]] inline auto ToString(LeaseWritableResourceKind kind) -> const char* {
  switch (kind) {
    case LeaseWritableResourceKind::None:
      return "none";
    case LeaseWritableResourceKind::CudaArray:
      return "cuda_array";
    case LeaseWritableResourceKind::OpenClImage:
      return "opencl_image";
    case LeaseWritableResourceKind::MetalTexture:
      return "metal_texture";
  }
  return "unknown";
}

[[nodiscard]] inline auto ToString(LeaseFrameLayer layer) -> const char* {
  switch (layer) {
    case LeaseFrameLayer::InteractivePrimary:
      return "interactive_primary";
    case LeaseFrameLayer::QualityBase:
      return "quality_base";
    case LeaseFrameLayer::DetailPatch:
      return "detail_patch";
  }
  return "unknown";
}

// Structured diagnostic string without filesystem paths or pixel data.
[[nodiscard]] auto DescribeLease(const WritableTargetLease& lease) -> std::string;

}  // namespace alcedo::editor_rhi
