//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>

#include "decoders/processor/raw_processor.hpp"
#include "edit/operators/op_base.hpp"
#include "image/gpu_backend.hpp"

namespace alcedo {

enum class RawProcessBackend { ALCEDO, LIBRAW };

class RawDecodeOp : public OperatorBase<RawDecodeOp> {
 public:
  static constexpr PriorityLevel     priority_level_    = 0;
  static constexpr std::string_view  canonical_name_    = "RawDecode";
  static constexpr std::string_view  script_name_       = "raw_decode";
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Image_Loading;
  static constexpr OperatorType      operator_type_     = OperatorType::RAW_DECODE;

  RawParams                          params_;
  RawProcessBackend                  backend_ = RawProcessBackend::ALCEDO;
  /// Image-local RAW color/lens metadata persisted in operator params and used for
  /// decode + SetGlobalParams without a separate per-frame inject path.
  RawRuntimeColorContext             inherent_raw_context_;
  std::function<bool()>              cancel_requested_;

  RawDecodeOp() = delete;

  RawDecodeOp(const nlohmann::json& params);

  void SetInherentRawContext(const RawRuntimeColorContext& ctx) { inherent_raw_context_ = ctx; }
  void SetCancelRequested(std::function<bool()> cancel_requested) {
    cancel_requested_ = std::move(cancel_requested);
  }

  // The accelerator backend is a runtime property of the process (from the
  // user's backend setting), never part of the persisted operator params.
  // The pipeline executor pushes its resolved backend here before every
  // render; SetParams deliberately ignores backend keys so stored state can
  // never drive the decode.
  void SetRuntimeGpuBackend(GpuBackendKind backend);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;

  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};

}  // namespace alcedo
