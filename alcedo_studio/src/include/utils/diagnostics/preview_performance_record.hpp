//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alcedo::diag {

enum class PreviewPerformanceMode : std::uint8_t { Off = 0, Summary = 1, Detail = 2 };

enum class PreviewCpuStage : std::uint8_t {
  Apply = 0,
  Invalidation,
  PlanKey,
  PlanLookup,
  PlanCompile,
  Allocation,
  Encode,
  Submit,
  Wait,
};

enum class PreviewPassKind : std::uint8_t {
  UploadRaw         = 0,
  UploadRgb         = 1,
  Linearize         = 2,
  CfaClamp          = 3,
  Demosaic          = 4,
  HighlightRecover  = 5,
  InverseCamMulPack = 6,
  Lens              = 7,
  GeometryResample  = 8,
  CameraToAp1       = 9,
  MaskEvaluate      = 10,
  PrimaryColorGrade = 12,
  Drt               = 13,
  MaskUnion         = 14,
};

enum class PreviewSubStageKind : std::uint8_t {
  Upload = 0,
  Linearize,
  CfaClamp,
  Demosaic,
  HighlightRecover,
  InverseCamMulPack,
  Lens,
  Pointwise,
  Neighborhood,
  LlfExtract,
  LlfPyramid,
  LlfRemap,
  LlfSelect,
  LlfCollapse,
  LlfApply,
  LlfSampleCanonical,
  Mix,
  DngWarp,
};

enum class PreviewExecutionState : std::uint8_t {
  Executed = 0,
  Skipped,
  Aliased,
  Disabled,
};

enum class PreviewTerminalOutcome : std::uint8_t {
  Presented = 0,
  Coalesced,
  Cancelled,
  Failed,
  Stale,
  Dropped,
};

enum class PreviewGpuTimeStatus : std::uint8_t {
  Unavailable = 0,
  Available   = 1,
  Failed      = 2,
};

enum class PreviewFrameRole : std::uint8_t {
  InteractivePrimary = 0,
  QualityBase,
  DetailPatch,
};

enum class PreviewQuality : std::uint8_t { Interactive = 0, Quality, Detail };

enum class PreviewDecodeRes : std::uint8_t { Full = 0, Half, Quarter, Eighth };

enum class PreviewCfaKind : std::uint8_t { Bayer = 0, XTrans, DirectRgb };

enum class PreviewDemosaicMethod : std::uint8_t { Legacy = 0, NeuralEngine };

enum class PreviewDevelopLayout : std::uint8_t { Unknown = 0, FullFrame, Tiled, UploadRgb };

/**
 * @brief Decode-path identity for one Develop encode. Times are unreadable without this.
 *
 * Copied scalars only. No document, parameter body, or pixel buffer.
 */
struct PreviewDevelopDecodeParams {
  PreviewDecodeRes       decode_res            = PreviewDecodeRes::Full;
  PreviewCfaKind         cfa                   = PreviewCfaKind::Bayer;
  PreviewDemosaicMethod  demosaic              = PreviewDemosaicMethod::Legacy;
  bool                   highlights_reconstruct = false;
  std::uint8_t           downsample_passes     = 0;
  std::uint32_t          host_width            = 0;
  std::uint32_t          host_height           = 0;
  std::uint32_t          develop_width         = 0;
  std::uint32_t          develop_height        = 0;
  std::uint32_t          full_ref_width        = 0;
  std::uint32_t          full_ref_height       = 0;
  bool                   upload_rgb            = false;
  PreviewDevelopLayout   layout                = PreviewDevelopLayout::Unknown;
};

/**
 * @brief Aggregated GPU pool and device totals for one request. Not per-allocation.
 */
struct PreviewResourceSnapshot {
  std::size_t   texture_used_bytes       = 0;
  std::size_t   texture_leased_bytes     = 0;
  std::size_t   texture_unleased_bytes   = 0;
  std::size_t   texture_entry_count      = 0;
  std::uint64_t texture_allocation_count = 0;
  std::size_t   texture_peak_used_bytes  = 0;
  std::size_t   transient_used_bytes     = 0;
  std::size_t   transient_capacity_bytes = 0;
  std::size_t   published_image_count    = 0;
  std::size_t   write_image_count        = 0;
  std::size_t   value_bytes               = 0;
  std::size_t   value_count               = 0;
  std::size_t   scene_work_member_count   = 0;
  std::size_t   scene_work_used_bytes     = 0;
  std::size_t   scene_work_peak_used_bytes = 0;
  std::uint64_t scene_work_allocation_count = 0;
  std::size_t   device_used_bytes        = 0;
  std::size_t   device_free_bytes        = 0;
  std::size_t   device_total_bytes       = 0;
  bool          device_memory_valid      = false;
};

struct PreviewSubStageRecord {
  PreviewSubStageKind    kind  = PreviewSubStageKind::Pointwise;
  PreviewExecutionState  state = PreviewExecutionState::Executed;
  std::int64_t           cpu_ns = 0;
  std::int64_t           gpu_ns = 0;
  PreviewGpuTimeStatus   gpu_status = PreviewGpuTimeStatus::Unavailable;
};

struct PreviewPassRecord {
  std::string                 owner;
  std::string                 mask_id;
  PreviewPassKind             kind    = PreviewPassKind::UploadRaw;
  std::uint32_t               ordinal = 0;
  PreviewExecutionState       state   = PreviewExecutionState::Executed;
  std::int64_t                cpu_ns  = 0;
  std::int64_t                gpu_ns  = 0;
  PreviewGpuTimeStatus        gpu_status = PreviewGpuTimeStatus::Unavailable;
  std::vector<PreviewSubStageRecord> sub_stages;
};

struct PreviewCpuStageTimes {
  std::int64_t apply_ns        = 0;
  std::int64_t invalidation_ns = 0;
  std::int64_t plan_key_ns     = 0;
  std::int64_t plan_lookup_ns  = 0;
  std::int64_t plan_compile_ns = 0;
  std::int64_t allocation_ns   = 0;
  std::int64_t encode_ns       = 0;
  std::int64_t submit_ns       = 0;
  std::int64_t wait_ns         = 0;
};

/**
 * @brief One assembled request after present or a terminal outcome.
 *
 * GPU durations come from backend timestamp slots resolved after submission
 * completion. Incomplete records (lost diagnostic events) must not enter E2E
 * quantiles.
 */
struct PreviewRequestRecord {
  std::uint64_t              request_id         = 0;
  std::uint64_t              input_sequence_id  = 0;
  PreviewFrameRole           frame_role         = PreviewFrameRole::InteractivePrimary;
  PreviewQuality             quality            = PreviewQuality::Interactive;
  std::string                reason;
  bool                       has_user_input     = false;
  bool                       incomplete         = false;
  std::int64_t               first_accepted_ns  = 0;
  std::int64_t               latest_accepted_ns = 0;
  std::int64_t               qml_first_write_ns = 0;
  std::int64_t               qml_latest_write_ns = 0;
  std::int64_t               submit_ns          = 0;
  std::int64_t               startable_ns       = 0;
  std::int64_t               extra_schedule_wait_ns = 0;
  std::int64_t               scheduled_ns       = 0;
  std::int64_t               worker_start_ns    = 0;
  std::int64_t               sink_submit_ns     = 0;
  std::int64_t               producer_ready_ns  = 0;
  std::int64_t               present_wake_ns    = 0;
  std::int64_t               gui_update_ns      = 0;
  std::int64_t               consume_begin_ns   = 0;
  std::int64_t               displayed_ns       = 0;
  std::int64_t               imported_ns        = 0;
  std::int64_t               frame_swapped_ns   = 0;
  std::int64_t               frame_end_ns       = 0;
  std::uint64_t              qt_frame           = 0;
  std::uint32_t              render_width       = 0;
  std::uint32_t              render_height      = 0;
  PreviewCpuStageTimes       cpu{};
  std::vector<PreviewPassRecord> passes;
  bool                       has_develop_decode = false;
  PreviewDevelopDecodeParams develop{};
  bool                       has_resources      = false;
  PreviewResourceSnapshot    resources{};
  PreviewTerminalOutcome     outcome            = PreviewTerminalOutcome::Presented;
  std::string                terminal_reason;
  std::int64_t               gpu_ns             = 0;
  PreviewGpuTimeStatus       gpu_status         = PreviewGpuTimeStatus::Unavailable;
};

/**
 * @brief Open preview pass or sub-stage that a backend timestamp slot should bind to.
 */
struct PreviewGpuSampleTarget {
  std::uint64_t request_id = 0;
  std::uint8_t  pass_index = 0;
  std::uint8_t  sub_index  = 0;
  bool          is_sub     = false;
  bool          valid      = false;
};

[[nodiscard]] inline auto PreviewPassKindName(PreviewPassKind kind) -> const char* {
  switch (kind) {
    case PreviewPassKind::UploadRaw:
      return "UploadRaw";
    case PreviewPassKind::UploadRgb:
      return "UploadRgb";
    case PreviewPassKind::Linearize:
      return "Linearize";
    case PreviewPassKind::CfaClamp:
      return "CfaClamp";
    case PreviewPassKind::Demosaic:
      return "Demosaic";
    case PreviewPassKind::HighlightRecover:
      return "HighlightRecover";
    case PreviewPassKind::InverseCamMulPack:
      return "InverseCamMulPack";
    case PreviewPassKind::Lens:
      return "Lens";
    case PreviewPassKind::GeometryResample:
      return "GeometryResample";
    case PreviewPassKind::CameraToAp1:
      return "CameraToAp1";
    case PreviewPassKind::MaskEvaluate:
      return "MaskEvaluate";
    case PreviewPassKind::PrimaryColorGrade:
      return "PrimaryColorGrade";
    case PreviewPassKind::Drt:
      return "Drt";
    case PreviewPassKind::MaskUnion:
      return "MaskUnion";
  }
  return "Unknown";
}

[[nodiscard]] inline auto PreviewSubStageKindName(PreviewSubStageKind kind) -> const char* {
  switch (kind) {
    case PreviewSubStageKind::Upload:
      return "Upload";
    case PreviewSubStageKind::Linearize:
      return "Linearize";
    case PreviewSubStageKind::CfaClamp:
      return "CfaClamp";
    case PreviewSubStageKind::Demosaic:
      return "Demosaic";
    case PreviewSubStageKind::HighlightRecover:
      return "HighlightRecover";
    case PreviewSubStageKind::InverseCamMulPack:
      return "InverseCamMulPack";
    case PreviewSubStageKind::Lens:
      return "Lens";
    case PreviewSubStageKind::Pointwise:
      return "pointwise";
    case PreviewSubStageKind::Neighborhood:
      return "neighborhood";
    case PreviewSubStageKind::LlfExtract:
      return "llf_extract";
    case PreviewSubStageKind::LlfPyramid:
      return "llf_pyramid";
    case PreviewSubStageKind::LlfRemap:
      return "llf_remap";
    case PreviewSubStageKind::LlfSelect:
      return "llf_select";
    case PreviewSubStageKind::LlfCollapse:
      return "llf_collapse";
    case PreviewSubStageKind::LlfApply:
      return "llf_apply";
    case PreviewSubStageKind::LlfSampleCanonical:
      return "llf_sample_canonical";
    case PreviewSubStageKind::Mix:
      return "mix";
    case PreviewSubStageKind::DngWarp:
      return "dng_warp";
  }
  return "unknown";
}

[[nodiscard]] inline auto PreviewGpuTimeStatusName(PreviewGpuTimeStatus status) -> const char* {
  switch (status) {
    case PreviewGpuTimeStatus::Unavailable:
      return "unavailable";
    case PreviewGpuTimeStatus::Available:
      return "available";
    case PreviewGpuTimeStatus::Failed:
      return "failed";
  }
  return "unavailable";
}

[[nodiscard]] inline auto PreviewExecutionStateName(PreviewExecutionState state) -> const char* {
  switch (state) {
    case PreviewExecutionState::Executed:
      return "executed";
    case PreviewExecutionState::Skipped:
      return "skipped";
    case PreviewExecutionState::Aliased:
      return "aliased";
    case PreviewExecutionState::Disabled:
      return "disabled";
  }
  return "unknown";
}

[[nodiscard]] inline auto PreviewTerminalOutcomeName(PreviewTerminalOutcome outcome)
    -> const char* {
  switch (outcome) {
    case PreviewTerminalOutcome::Presented:
      return "presented";
    case PreviewTerminalOutcome::Coalesced:
      return "coalesced";
    case PreviewTerminalOutcome::Cancelled:
      return "cancelled";
    case PreviewTerminalOutcome::Failed:
      return "failed";
    case PreviewTerminalOutcome::Stale:
      return "stale";
    case PreviewTerminalOutcome::Dropped:
      return "dropped";
  }
  return "unknown";
}

[[nodiscard]] inline auto PreviewDecodeResName(PreviewDecodeRes value) -> const char* {
  switch (value) {
    case PreviewDecodeRes::Full:
      return "FULL";
    case PreviewDecodeRes::Half:
      return "HALF";
    case PreviewDecodeRes::Quarter:
      return "QUARTER";
    case PreviewDecodeRes::Eighth:
      return "EIGHTH";
  }
  return "FULL";
}

[[nodiscard]] inline auto PreviewCfaKindName(PreviewCfaKind value) -> const char* {
  switch (value) {
    case PreviewCfaKind::Bayer:
      return "Bayer";
    case PreviewCfaKind::XTrans:
      return "XTrans";
    case PreviewCfaKind::DirectRgb:
      return "DirectRgb";
  }
  return "Bayer";
}

[[nodiscard]] inline auto PreviewDemosaicMethodName(PreviewDemosaicMethod value) -> const char* {
  switch (value) {
    case PreviewDemosaicMethod::Legacy:
      return "legacy";
    case PreviewDemosaicMethod::NeuralEngine:
      return "neural_engine";
  }
  return "legacy";
}

[[nodiscard]] inline auto PreviewDevelopLayoutName(PreviewDevelopLayout value) -> const char* {
  switch (value) {
    case PreviewDevelopLayout::Unknown:
      return "unknown";
    case PreviewDevelopLayout::FullFrame:
      return "FullFrame";
    case PreviewDevelopLayout::Tiled:
      return "Tiled";
    case PreviewDevelopLayout::UploadRgb:
      return "UploadRgb";
  }
  return "unknown";
}

[[nodiscard]] inline auto PreviewFrameRoleName(PreviewFrameRole value) -> const char* {
  switch (value) {
    case PreviewFrameRole::InteractivePrimary:
      return "InteractivePrimary";
    case PreviewFrameRole::QualityBase:
      return "QualityBase";
    case PreviewFrameRole::DetailPatch:
      return "DetailPatch";
  }
  return "InteractivePrimary";
}

[[nodiscard]] inline auto PreviewQualityName(PreviewQuality value) -> const char* {
  switch (value) {
    case PreviewQuality::Interactive:
      return "Interactive";
    case PreviewQuality::Quality:
      return "Quality";
    case PreviewQuality::Detail:
      return "Detail";
  }
  return "Interactive";
}

}  // namespace alcedo::diag
