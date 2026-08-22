//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct ScalarFloatPayload {
  float value = 0.0f;
};

/**
 * @brief Single-float adjustment Model parameterized by Traits.
 *
 * Traits provide TypeId, JSON key, default, min, max, and Dirty enum.
 */
template <class Traits>
class ScalarOperatorModel final
    : public OperatorModelBase<ScalarOperatorModel<Traits>, typename Traits::Payload,
                               typename Traits::Dirty> {
 public:
  using Payload = typename Traits::Payload;
  using Dirty   = typename Traits::Dirty;

  ScalarOperatorModel() { this->payload_.value = Traits::kDefault; }

  static auto TypeId() -> const OperatorTypeId& { return Traits::TypeId(); }

  [[nodiscard]] auto IsDefault() const -> bool override {
    return this->Read([](const Payload& payload) { return payload.value == Traits::kDefault; });
  }

  /**
   * @brief Set the scalar parameter, clamped to Traits min/max, and mark dirty.
   */
  void SetValue(float value) {
    const float clamped = std::clamp(value, Traits::kMin, Traits::kMax);
    this->Mutate(Dirty::Value, [clamped](Payload& payload) { payload.value = clamped; });
  }

  [[nodiscard]] auto Value() const -> float {
    return this->Read([](const Payload& payload) { return payload.value; });
  }

  [[nodiscard]] auto ToJson() const -> nlohmann::json override {
    nlohmann::json json;
    json[std::string{Traits::kJsonKey}] = Value();
    return json;
  }

  void LoadJson(const nlohmann::json& json) override {
    float value = Traits::kDefault;
    if (json.contains(Traits::kJsonKey) && json[Traits::kJsonKey].is_number()) {
      value = json[Traits::kJsonKey].template get<float>();
    }
    SetValue(value);
  }
};

struct ExposureTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Exposure(); }
  static constexpr std::string_view kJsonKey    = "exposure_ev";
  static constexpr std::string_view kDisplayName = "Exposure";
  static constexpr std::string_view kInstanceSuffix = "exposure";
  static constexpr float            kDefault     = 0.0f;
  static constexpr float            kMin         = -16.0f;
  static constexpr float            kMax         = 16.0f;
};

struct ContrastTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Contrast(); }
  static constexpr std::string_view kJsonKey        = "contrast";
  static constexpr std::string_view kDisplayName    = "Contrast";
  static constexpr std::string_view kInstanceSuffix = "contrast";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct WhiteTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::White(); }
  static constexpr std::string_view kJsonKey        = "white";
  static constexpr std::string_view kDisplayName    = "White";
  static constexpr std::string_view kInstanceSuffix = "white";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct BlackTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Black(); }
  static constexpr std::string_view kJsonKey        = "black";
  static constexpr std::string_view kDisplayName    = "Black";
  static constexpr std::string_view kInstanceSuffix = "black";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct ShadowsTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Shadows(); }
  static constexpr std::string_view kJsonKey        = "shadows";
  static constexpr std::string_view kDisplayName    = "Shadows";
  static constexpr std::string_view kInstanceSuffix = "shadows";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct HighlightsTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Highlights(); }
  static constexpr std::string_view kJsonKey        = "highlights";
  static constexpr std::string_view kDisplayName    = "Highlights";
  static constexpr std::string_view kInstanceSuffix = "highlights";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct SaturationTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Saturation(); }
  static constexpr std::string_view kJsonKey        = "saturation";
  static constexpr std::string_view kDisplayName    = "Saturation";
  static constexpr std::string_view kInstanceSuffix = "saturation";
  static constexpr float            kDefault        = 1.0f;
  static constexpr float            kMin            = 0.0f;
  static constexpr float            kMax            = 4.0f;
};

struct VibranceTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Vibrance(); }
  static constexpr std::string_view kJsonKey        = "vibrance";
  static constexpr std::string_view kDisplayName    = "Vibrance";
  static constexpr std::string_view kInstanceSuffix = "vibrance";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct ClarityTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Clarity(); }
  static constexpr std::string_view kJsonKey        = "clarity";
  static constexpr std::string_view kDisplayName    = "Clarity";
  static constexpr std::string_view kInstanceSuffix = "clarity";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

struct HalationTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Halation(); }
  static constexpr std::string_view kJsonKey        = "strength";
  static constexpr std::string_view kDisplayName    = "Halation";
  static constexpr std::string_view kInstanceSuffix = "halation";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = 0.0f;
  static constexpr float            kMax            = 1.0f;
};

struct FilmGrainTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::FilmGrain(); }
  static constexpr std::string_view kJsonKey        = "strength";
  static constexpr std::string_view kDisplayName    = "Film Grain";
  static constexpr std::string_view kInstanceSuffix = "film_grain";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = 0.0f;
  static constexpr float            kMax            = 1.0f;
};

struct TintTraits {
  using Payload = ScalarFloatPayload;
  enum class Dirty : std::uint32_t { None = 0, Value = 1U << 0, All = Value };
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Tint(); }
  static constexpr std::string_view kJsonKey        = "tint";
  static constexpr std::string_view kDisplayName    = "Tint";
  static constexpr std::string_view kInstanceSuffix = "tint";
  static constexpr float            kDefault        = 0.0f;
  static constexpr float            kMin            = -100.0f;
  static constexpr float            kMax            = 100.0f;
};

using ExposureModel   = ScalarOperatorModel<ExposureTraits>;
using ContrastModel   = ScalarOperatorModel<ContrastTraits>;
using WhiteModel      = ScalarOperatorModel<WhiteTraits>;
using BlackModel      = ScalarOperatorModel<BlackTraits>;
using ShadowsModel    = ScalarOperatorModel<ShadowsTraits>;
using HighlightsModel = ScalarOperatorModel<HighlightsTraits>;
using SaturationModel = ScalarOperatorModel<SaturationTraits>;
using VibranceModel   = ScalarOperatorModel<VibranceTraits>;
using ClarityModel    = ScalarOperatorModel<ClarityTraits>;
using HalationModel   = ScalarOperatorModel<HalationTraits>;
using FilmGrainModel  = ScalarOperatorModel<FilmGrainTraits>;
using TintModel       = ScalarOperatorModel<TintTraits>;

}  // namespace alcedo
