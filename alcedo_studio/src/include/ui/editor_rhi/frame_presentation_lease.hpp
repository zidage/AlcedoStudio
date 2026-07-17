//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::editor_rhi {

// Backend-neutral presentation lease protocol used by FramePresentationBroker
// (Phase 2+) and exercised in reduced form by EditorRhiHarness (Phase 0).
//
// Lifecycle:
//   1. Consumer (render thread) publishes a WritableTargetLease for a generation.
//   2. Producer (pipeline worker) fills the native object and submits CompletedFrameLease.
//   3. Consumer imports into QRhi, samples once, then dual-sided release:
//      producer_done && renderer_done => destroy native object.
//
// Metal: the contract is defined here so later phases share one boundary. Phase 0
// does not claim Metal feasibility; implementation and qualification are Phase 8.

enum class LeasePixelFormat : std::uint8_t {
  Rgba32f = 0,
};

enum class LeaseNativeHandleKind : std::uint8_t {
  None = 0,
  D3D11Texture2D,     // ID3D11Texture2D*
  OpenGLTexture2D,    // GLuint texture name
  MetalTexture,       // MTLTexture* (id) as opaque pointer
};

// The broker keeps these layer identities independent from the edit-viewer
// implementation so a future Metal adapter can use the same protocol.
enum class LeaseFrameLayer : std::uint8_t {
  InteractivePrimary = 0,
  QualityBase,
  DetailPatch,
};

struct LeaseDimensions {
  int width  = 0;
  int height = 0;

  [[nodiscard]] auto valid() const -> bool { return width > 0 && height > 0; }
};

// Immutable identity for one presentation target generation.
struct TargetGeneration {
  std::uint64_t target_generation = 0;
  std::uint64_t image_generation  = 0;
  std::uint64_t layer_generation  = 0;
};

// Published by the render thread for the producer to write into.
struct WritableTargetLease {
  EditorBackend         backend       = EditorBackend::Cuda;
  LeaseNativeHandleKind handle_kind   = LeaseNativeHandleKind::None;
  LeasePixelFormat      pixel_format  = LeasePixelFormat::Rgba32f;
  LeaseDimensions       dimensions{};
  TargetGeneration      generation{};
  LeaseFrameLayer       layer         = LeaseFrameLayer::InteractivePrimary;
  std::uintptr_t        native_handle = 0;
  // Optional external semaphore / fence for producer->consumer sync.
  std::uintptr_t        sync_object   = 0;
  std::uint64_t         sync_value    = 0;
  // Keeps the native object alive until dual-sided release.
  std::shared_ptr<const void> lifetime_token{};

  [[nodiscard]] auto valid() const -> bool {
    return dimensions.valid() && native_handle != 0 && lifetime_token != nullptr &&
           handle_kind != LeaseNativeHandleKind::None;
  }
};

// Submitted by the producer after filling the target.
struct CompletedFrameLease {
  WritableTargetLease   target{};
  TargetGeneration      generation{};
  LeaseFrameLayer       layer              = LeaseFrameLayer::InteractivePrimary;
  std::uint64_t         preview_generation = 0;
  std::uint64_t         detail_serial      = 0;
  float                 roi_x              = 0.0f;
  float                 roi_y              = 0.0f;
  float                 roi_width          = 1.0f;
  float                 roi_height         = 1.0f;
  bool                  producer_complete = false;

  [[nodiscard]] auto valid() const -> bool {
    return target.valid() && producer_complete &&
           generation.target_generation == target.generation.target_generation &&
           generation.image_generation == target.generation.image_generation &&
           (target.generation.layer_generation == 0 ||
            generation.layer_generation == target.generation.layer_generation) &&
           layer == target.layer;
  }
};

// Dual-sided release bookkeeping. Destroy native resources only when both sides
// have completed for the same lifetime token and target generation.
struct LeaseReleaseState {
  std::uint64_t target_generation   = 0;
  bool          producer_complete   = false;
  bool          renderer_complete   = false;
  std::shared_ptr<const void> lifetime_token{};

  [[nodiscard]] auto can_destroy() const -> bool {
    return producer_complete && renderer_complete && lifetime_token != nullptr;
  }
};

// Metal-specific payload carried inside lifetime_token / native_handle for Phase 8.
// Defined now so Windows and macOS share one lease schema.
struct MetalSharedTextureLeasePayload {
  // Opaque MTLTexture* retained by lifetime_token.
  std::uintptr_t mtl_texture = 0;
  // Optional MTLSharedEvent* / value for cross-queue sync.
  std::uintptr_t mtl_shared_event = 0;
  std::uint64_t  mtl_signal_value = 0;
  int            width            = 0;
  int            height           = 0;
  // MTLPixelFormat as integer; Phase 8 pins the exact enum value.
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

// Structured diagnostic string without filesystem paths or pixel data.
[[nodiscard]] auto DescribeLease(const WritableTargetLease& lease) -> std::string;

}  // namespace alcedo::editor_rhi
