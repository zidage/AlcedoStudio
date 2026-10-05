//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LMT composite table bake against identity LUTs, an OpenColorIO 2.5.1 conversion LUT and the R2
// raster conversion (lut_color_encoding_plan.md, phase L3). Only test targets link OpenColorIO.

#include "edit/runtime/lut_bake.hpp"

#include <OpenColorIO/OpenColorIO.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "aces2_ocio_reference.hpp"
#include "color/color_encoding_catalog.hpp"
#include "color/color_encoding_math.h"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/raster_develop_params.hpp"
#include "edit/runtime/raster_linearize_math.h"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

namespace OCIO = OCIO_NAMESPACE;
using color::ColorEncoding;
using color::ColorReferral;

constexpr std::size_t kEdge = kLmtCompositeEdge;

auto                  Encoding(const char* id) -> const ColorEncoding& {
  const auto* encoding = color::FindColorEncoding(id);
  if (encoding == nullptr) {
    throw std::runtime_error(std::string("lut_bake_test: unknown encoding ") + id);
  }
  return *encoding;
}

/// Packed table of @p edge whose node (r, g, b) holds @p value((r, g, b) / (edge - 1)).
template <class Value>
auto MakeTable(std::uint32_t edge, Value value) -> PackedGradeLut {
  PackedGradeLut table;
  table.edge = edge;
  table.rgba.resize(static_cast<std::size_t>(edge) * edge * edge * 4 * sizeof(float));
  auto*       out   = reinterpret_cast<float*>(table.rgba.data());
  const float scale = 1.0f / static_cast<float>(edge - 1);
  for (std::uint32_t b = 0; b < edge; ++b) {
    for (std::uint32_t g = 0; g < edge; ++g) {
      for (std::uint32_t r = 0; r < edge; ++r) {
        const LutRgb v = value(LutRgb{static_cast<float>(r) * scale, static_cast<float>(g) * scale,
                                      static_cast<float>(b) * scale});
        float*       voxel = out + ((static_cast<std::size_t>(b) * edge + g) * edge + r) * 4;
        voxel[0]           = v[0];
        voxel[1]           = v[1];
        voxel[2]           = v[2];
        voxel[3]           = 1.0f;
      }
    }
  }
  return table;
}

/// 2^3 identity: trilinear sampling reproduces every code value in [0, 1] exactly.
auto IdentityTable() -> PackedGradeLut {
  return MakeTable(2, [](const LutRgb& c) { return c; });
}

auto Voxel(const PackedGradeLut& table, std::size_t r, std::size_t g, std::size_t b) -> LutRgb {
  const auto  edge = static_cast<std::size_t>(table.edge);
  const auto* v =
      reinterpret_cast<const float*>(table.rgba.data()) + ((b * edge + g) * edge + r) * 4;
  return {v[0], v[1], v[2]};
}

auto CompositeNode(const std::vector<std::byte>& composite, std::size_t r, std::size_t g,
                   std::size_t b) -> LutRgb {
  const auto* v =
      reinterpret_cast<const float*>(composite.data()) + ((b * kEdge + g) * kEdge + r) * 4;
  return {v[0], v[1], v[2]};
}

auto NodeCode(std::size_t r, std::size_t g, std::size_t b) -> LutRgb {
  const float step = 1.0f / static_cast<float>(kEdge - 1);
  return {static_cast<float>(r) * step, static_cast<float>(g) * step, static_cast<float>(b) * step};
}

auto MaxAbsDifference(const LutRgb& a, const LutRgb& b) -> float {
  return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

auto InsideUnitCube(const LutRgb& code) -> bool {
  return std::all_of(code.begin(), code.end(), [](float v) { return v >= 0.0f && v <= 1.0f; });
}

auto Span(const LutRgb& v) -> float {
  return std::max({v[0], v[1], v[2]}) - std::min({v[0], v[1], v[2]});
}

auto Multiply(const color::Matrix33d& m, const LutRgb& v) -> LutRgb {
  return {static_cast<float>(m[0] * v[0] + m[1] * v[1] + m[2] * v[2]),
          static_cast<float>(m[3] * v[0] + m[4] * v[1] + m[5] * v[2]),
          static_cast<float>(m[6] * v[0] + m[7] * v[1] + m[8] * v[2])};
}

auto DecodeAcescc(const LutRgb& acescc) -> LutRgb {
  return {CeAcesccDecode(acescc[0]), CeAcesccDecode(acescc[1]), CeAcesccDecode(acescc[2])};
}

/// True when the transfer function of scene encoding @p encoding returns every channel of
/// @p linear through encode then decode. Apple Log clips below its R0, and the published F-Log
/// constants leave a 1e-4 code step at the break, so a narrow linear band below 0.00089 decodes
/// on the other side of it. Neither is a property of the bake.
auto CurveReturnsLinear(const ColorEncoding& encoding, const LutRgb& linear) -> bool {
  const int tf = static_cast<int>(encoding.transfer_);
  return std::all_of(linear.begin(), linear.end(), [tf](float x) {
    return std::abs(CeDecode(tf, CeEncode(tf, x)) - x) <= 1.0e-6f + 1.0e-5f * std::abs(x);
  });
}

/// Channel span (ACEScc) up to which the float arithmetic holds a scene identity within 1e-4:
/// above it the float curve round trip of the brightest channel (about 1e-6 relative) is mixed
/// into the darkest channel by the gamut matrix. 0.625 ACEScc is about 11 stops.
constexpr float kSceneFloatSpan       = 0.625f;
/// Worst scene identity error over all represented nodes, the float limit at a span of 17.5
/// stops (RED Log3G10 / REDWideGamutRGB: 4.9e-3 debug, 5.0e-3 release build).
constexpr float kSceneFloatLimitError = 6.0e-3f;

TEST(LutBakeTest, IdentityLutIsNoOpForEveryEqualSceneEncodingPair) {
  const auto identity = IdentityTable();
  for (const auto& encoding : color::ColorEncodings()) {
    if (encoding.referral_ != ColorReferral::SceneReferred) continue;
    SCOPED_TRACE(std::string(encoding.id_));
    const LmtEncodingConversion conversion(encoding, encoding);
    const auto                  composite = BakeLmtCompositeTable(identity, encoding, encoding);
    const auto                  to_encoding =
        color::GamutConversionMatrix(color::ColorGamutId::Ap1, encoding.gamut_);
    std::size_t represented = 0, within_span = 0;
    float       worst_within_span = 0.0f, worst = 0.0f;
    for (std::size_t b = 0; b < kEdge; ++b) {
      for (std::size_t g = 0; g < kEdge; ++g) {
        for (std::size_t r = 0; r < kEdge; ++r) {
          const LutRgb node = NodeCode(r, g, b);
          // Outside the LUT domain the LUT clamps, as every 3D LUT does: no identity there.
          if (!InsideUnitCube(conversion.AcesccToLutInput(node))) continue;
          if (!CurveReturnsLinear(encoding, Multiply(to_encoding, DecodeAcescc(node)))) continue;
          ++represented;
          const float error = MaxAbsDifference(CompositeNode(composite, r, g, b), node);
          worst             = std::max(worst, error);
          if (Span(node) <= kSceneFloatSpan) {
            ++within_span;
            worst_within_span = std::max(worst_within_span, error);
          }
        }
      }
    }
    std::cout << "[scene identity " << encoding.id_ << "] represented " << represented << ", worst "
              << worst << "; span <= " << kSceneFloatSpan << ": " << within_span << ", worst "
              << worst_within_span << '\n';
    // Every scene encoding represents at least the ACEScc range up to 0.5 (about 4.6 stops
    // above middle grey) inside its unit cube.
    EXPECT_GT(within_span, kEdge * kEdge * kEdge / 8);
    EXPECT_LE(worst_within_span, 1.0e-4f);
    EXPECT_LE(worst, kSceneFloatLimitError);
  }
}

/// Identity tolerance of one AP1 channel @p x of a display pair, in ACEScc: 1e-4, or the R2
/// accuracy of the inverse against OCIO (absolute 1e-5 linear below 1e-2, raster plan 5.7) where
/// that is wider. Below linear 1e-2 an ACEScc step of 1e-4 is less than 1e-5 linear, so the
/// identity cannot be tighter than the inverse that produces it.
auto DisplayIdentityTolerance(float x) -> float {
  if (x >= 1.0e-2f) return 1.0e-4f;
  return std::max(1.0e-4f, CeAcesccEncode(x + 1.0e-5f) - CeAcesccEncode(x));
}

/// Largest per-channel identity error of @p actual against @p node, divided by the channel
/// tolerance plus @p allowance.
auto DisplayIdentityRatio(const LutRgb& actual, const LutRgb& node, const LutRgb& ap1,
                          float allowance) -> float {
  float worst = 0.0f;
  for (std::size_t ch = 0; ch < 3; ++ch) {
    worst = std::max(
        worst, std::abs(actual[ch] - node[ch]) / (DisplayIdentityTolerance(ap1[ch]) + allowance));
  }
  return worst;
}

/// Scene error at one AP1 point of OCIO's own forward/inverse pair, in ACEScc, with the R2 clamp.
auto OcioSceneRoundTripError(const aces2_ocio_reference::Aces2Case& c, const LutRgb& ap1,
                             float ap1_max) -> float {
  const auto ap0 = Multiply(
      color::GamutConversionMatrix(color::ColorGamutId::Ap1, color::ColorGamutId::Ap0), ap1);
  const auto back = aces2_ocio_reference::OcioInverse(
      c, aces2_ocio_reference::OcioForward(c, {ap0[0], ap0[1], ap0[2]}));
  const auto back_ap1 =
      Multiply(color::GamutConversionMatrix(color::ColorGamutId::Ap0, color::ColorGamutId::Ap1),
               {back[0], back[1], back[2]});
  float worst = 0.0f;
  for (std::size_t ch = 0; ch < 3; ++ch) {
    const float clamped = std::clamp(back_ap1[ch], 0.0f, ap1_max);
    worst = std::max(worst, std::abs(CeAcesccEncode(clamped) - CeAcesccEncode(ap1[ch])));
  }
  return worst;
}

TEST(LutBakeTest, IdentityLutIsNoOpForEveryEqualDisplayEncodingPairInsideTheForwardLimit) {
  // The pair is OCIO's ACES 2.0 forward and the R2 inverse. "Inside the forward limit" is the L2
  // rule, applied where the inverse clamps: AP1 at most the forward limit, and a forward result
  // whose limiting RGB lies in [0, 99 % of the peak]. The node's display code values must also
  // lie in the LUT domain (an HLG signal of a bright saturated color is above 1). The tolerance
  // is 1e-4 ACEScc per channel, widened to the R2 accuracy below linear 1e-2
  // (DisplayIdentityTolerance). A node that misses it passes only when its error is at most
  // OCIO's own pair error there plus that tolerance. At least 99.5 % of the nodes must meet the
  // tolerance directly, not counting missed nodes where OCIO's own pair errs by more than 1e-4
  // (at 48 nits OCIO's pair is not an identity at about 3 % of these nodes).
  const auto identity = IdentityTable();
  const auto ap1_to_ap0 =
      color::GamutConversionMatrix(color::ColorGamutId::Ap1, color::ColorGamutId::Ap0);
  for (const auto& encoding : color::ColorEncodings()) {
    if (encoding.referral_ != ColorReferral::DisplayReferred) continue;
    SCOPED_TRACE(std::string(encoding.id_));
    const auto   runtime   = ResolveAces2ReferenceRuntime(color::GamutPrimariesXy(encoding.gamut_),
                                                          encoding.peak_luminance_nits_);
    const float* p         = runtime->packed_.data();
    const float  ap1_max   = p[ALCEDO_A2R_AP1_MAX];
    const float  limit_max = 0.99f * p[ALCEDO_A2R_INPUT_MAX];
    const LmtEncodingConversion conversion(encoding, encoding);
    const auto                  composite = BakeLmtCompositeTable(identity, encoding, encoding);
    const aces2_ocio_reference::Aces2Case ocio_case{encoding.id_.data(),
                                                    color::GamutPrimariesXy(encoding.gamut_),
                                                    encoding.peak_luminance_nits_};
    std::size_t checked = 0, direct = 0, explained = 0, ocio_not_identity = 0;
    float       worst = 0.0f;
    for (std::size_t b = 0; b < kEdge; ++b) {
      for (std::size_t g = 0; g < kEdge; ++g) {
        for (std::size_t r = 0; r < kEdge; ++r) {
          const LutRgb node = NodeCode(r, g, b);
          const LutRgb ap1  = DecodeAcescc(node);
          if (std::max({ap1[0], ap1[1], ap1[2]}) > ap1_max) continue;
          const LutRgb    ap0     = Multiply(ap1_to_ap0, ap1);
          const A2rFloat3 display = A2rAp0ToDisplay(A2rMake3(ap0[0], ap0[1], ap0[2]), p);
          const A2rFloat3 limit   = A2rMul(p + ALCEDO_A2R_DISPLAY_TO_LIMIT, display);
          if (std::min({limit.x, limit.y, limit.z}) < 0.0f ||
              std::max({limit.x, limit.y, limit.z}) >= limit_max) {
            continue;
          }
          if (!InsideUnitCube(conversion.AcesccToLutInput(node))) continue;
          ++checked;
          const LutRgb actual = CompositeNode(composite, r, g, b);
          worst               = std::max(worst, MaxAbsDifference(actual, node));
          if (DisplayIdentityRatio(actual, node, ap1, 0.0f) <= 1.0f) {
            ++direct;
            continue;
          }
          const float ocio_error = OcioSceneRoundTripError(ocio_case, ap1, ap1_max);
          EXPECT_LE(DisplayIdentityRatio(actual, node, ap1, ocio_error), 1.0f)
              << "node " << r << ' ' << g << ' ' << b << ": error "
              << MaxAbsDifference(actual, node) << ", OCIO pair error " << ocio_error;
          ++explained;
          if (ocio_error > 1.0e-4f) ++ocio_not_identity;
        }
      }
    }
    std::cout << "[display identity " << encoding.id_ << "] checked " << checked
              << ", within tolerance " << direct << ", within OCIO pair error + tolerance "
              << explained << " (OCIO pair not an identity at " << ocio_not_identity << ")"
              << ", worst " << worst << '\n';
    EXPECT_GT(checked, 1000U);
    EXPECT_GE(static_cast<double>(direct),
              0.995 * static_cast<double>(checked - ocio_not_identity));
  }
}

auto SLog3InputTransform() -> OCIO::BuiltinTransformRcPtr {
  auto idt = OCIO::BuiltinTransform::Create();
  idt->setStyle("SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1");
  return idt;
}

/// OCIO 2.5.1 processor from S-Log3 / S-Gamut3.Cine code values to ACES2065-1 (builtin).
auto OcioSLog3ToAp0() -> OCIO::ConstCPUProcessorRcPtr {
  return OCIO::Config::CreateRaw()
      ->getProcessor(SLog3InputTransform())
      ->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_NONE);
}

/// OCIO 2.5.1 processor from S-Log3 / S-Gamut3.Cine code values to ACEScc (builtins).
auto OcioSLog3ToAcescc() -> OCIO::ConstCPUProcessorRcPtr {
  auto group = OCIO::GroupTransform::Create();
  group->appendTransform(SLog3InputTransform());
  auto to_acescc = OCIO::BuiltinTransform::Create();
  to_acescc->setStyle("ACEScc_to_ACES2065-1");
  to_acescc->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
  group->appendTransform(to_acescc);
  return OCIO::Config::CreateRaw()->getProcessor(group)->getOptimizedCPUProcessor(
      OCIO::OPTIMIZATION_NONE);
}

auto ApplyOcio(const OCIO::ConstCPUProcessorRcPtr& cpu, const LutRgb& code) -> LutRgb {
  float rgb[3] = {code[0], code[1], code[2]};
  cpu->applyRGB(rgb);
  return LutRgb{rgb[0], rgb[1], rgb[2]};
}

/// The OCIO transform above sampled as a 65^3 table.
auto OcioSLog3ToAcesccTable() -> PackedGradeLut {
  const auto cpu = OcioSLog3ToAcescc();
  return MakeTable(65, [&cpu](const LutRgb& code) { return ApplyOcio(cpu, code); });
}

TEST(LutBakeTest, OcioSLog3ToAcesccLutWithSLog3InputBakesToIdentity) {
  // Nodes whose S-Log3 code lies in the LUT domain, channel spans up to kSceneFloatSpan as for
  // the identity LUT. OCIO's ACES2065-1 to ACEScc clamps twice: negative AP0 to 0, and AP1 at or
  // below 0 to the code -0.36. Where a clamp applies, OCIO holds the ACEScc of another color,
  // which Alcedo's ACEScc (linear below the floor) does not reproduce; those nodes are skipped.
  //
  // 1. The input conversion inverts OCIO's direct transform: OCIO(AcesccToLutInput(node)) is
  //    the node within 1e-4 wherever no clamp applies at that code.
  // 2. The composite table is the identity within 1e-3 wherever the 65^3 table represents the
  //    OCIO transform at that code within 5e-4. Elsewhere the deviation is the table's own
  //    trilinear error (dark channels of saturated colors, where ACEScc is steep in linear
  //    light, and cells next to a clamp), which no bake can remove.
  const auto ocio    = OcioSLog3ToAcescc();
  const auto to_ap0  = OcioSLog3ToAp0();
  const auto source  = OcioSLog3ToAcesccTable();
  const auto clamped = [&](const LutRgb& code) {
    const LutRgb ap0    = ApplyOcio(to_ap0, code);
    const LutRgb acescc = ApplyOcio(ocio, code);
    // CeAcesccEncode of the smallest positive value is -0.35845; OCIO's clamp code is -0.36.
    return std::min({ap0[0], ap0[1], ap0[2]}) < 0.0f ||
           std::min({acescc[0], acescc[1], acescc[2]}) <= -0.3585f;
  };
  const auto&                 slog3  = Encoding("sony_slog3_sgamut3cine");
  const auto&                 acescc = Encoding("acescc");
  const LmtEncodingConversion conversion(slog3, acescc);
  const auto                  composite = BakeLmtCompositeTable(source, slog3, acescc);
  std::size_t                 in_domain = 0, unclamped = 0, represented = 0, unclamped_within = 0;
  float                       worst_conversion = 0.0f, worst_bake = 0.0f;
  LutRgb                      worst_node{};
  for (std::size_t b = 0; b < kEdge; ++b) {
    for (std::size_t g = 0; g < kEdge; ++g) {
      for (std::size_t r = 0; r < kEdge; ++r) {
        const LutRgb node = NodeCode(r, g, b);
        const LutRgb code = conversion.AcesccToLutInput(node);
        if (!InsideUnitCube(code) || Span(node) > kSceneFloatSpan) continue;
        ++in_domain;
        if (clamped(code)) continue;
        ++unclamped;
        const LutRgb direct = ApplyOcio(ocio, code);
        worst_conversion    = std::max(worst_conversion, MaxAbsDifference(direct, node));
        const float error   = MaxAbsDifference(CompositeNode(composite, r, g, b), node);
        if (error <= 1.0e-3f) ++unclamped_within;
        if (MaxAbsDifference(SampleLutTable(source, code), direct) > 5.0e-4f) continue;
        ++represented;
        if (error > worst_bake) {
          worst_bake = error;
          worst_node = node;
        }
      }
    }
  }
  std::cout << "[slog3 identity] in domain " << in_domain << ", without OCIO clamp " << unclamped
            << " (conversion worst " << worst_conversion << ", composite within 1e-3 "
            << unclamped_within << "), represented by the table " << represented << " (bake worst "
            << worst_bake << " at ACEScc (" << worst_node[0] << ", " << worst_node[1] << ", "
            << worst_node[2] << "))\n";
  EXPECT_GT(unclamped, kEdge * kEdge * kEdge / 8);
  EXPECT_LE(worst_conversion, 1.0e-4f);
  EXPECT_GT(represented, kEdge * kEdge * kEdge / 32);
  EXPECT_LE(worst_bake, 1.0e-3f);
}

TEST(LutBakeTest, Rec709Bt1886OutputMatchesRasterDisplayToAp1HostResult) {
  // A 65^3 source with deterministic BT.1886 code values: with an ACEScc input, composite node i
  // samples source node i exactly, so it must equal the R2 conversion of that display value.
  std::mt19937                          random(20261005);
  std::uniform_real_distribution<float> code(0.0f, 1.0f);
  const auto                            source = MakeTable(
      65, [&](const LutRgb&) { return LutRgb{code(random), code(random), code(random)}; });
  const auto&            rec709    = Encoding("rec709_bt1886");
  const auto             composite = BakeLmtCompositeTable(source, Encoding("acescc"), rec709);

  RasterColorDescription description;
  description.referral_     = RasterReferral::DisplayReferred;
  description.primaries_xy_ = color::GamutPrimariesXy(color::ColorGamutId::Rec709);
  for (auto& transfer : description.transfer_) {
    transfer.kind_ = RasterTransferKind::Bt1886;
  }
  description.peak_luminance_nits_ = 100.0f;
  const auto linearize             = PackRasterLinearize(description, HostPixelFormat::F32Rgba);
  const auto block                 = ResolveDisplayToAp1Block(description);
  const auto packed                = block.Packed();

  float      worst                 = 0.0f;
  for (std::size_t b = 0; b < kEdge; ++b) {
    for (std::size_t g = 0; g < kEdge; ++g) {
      for (std::size_t r = 0; r < kEdge; ++r) {
        const LutRgb    lut   = Voxel(source, r, g, b);
        const RlRgb     light = RlLinearize(lut[0], lut[1], lut[2], linearize.data());
        const A2rFloat3 expected =
            D2aSourceToAcesccAp1(A2rMake3(light.r, light.g, light.b), packed.data());
        worst = std::max(worst, MaxAbsDifference(CompositeNode(composite, r, g, b),
                                                 {expected.x, expected.y, expected.z}));
      }
    }
  }
  EXPECT_LE(worst, 1.0e-4f);
}

TEST(LutBakeTest, DomainMinMaxMapsCodeValuesBeforeSampling) {
  // An identity table over its own domain: L(code) = u = (code - min) / (max - min), clamped.
  auto table          = IdentityTable();
  table.domain_min    = {-0.5f, 0.0f, 0.25f};
  table.domain_max    = {1.5f, 2.0f, 0.75f};
  const auto expected = [&table](const LutRgb& code) {
    LutRgb u{};
    for (std::size_t c = 0; c < 3; ++c) {
      u[c] =
          std::clamp((code[c] - table.domain_min[c]) / (table.domain_max[c] - table.domain_min[c]),
                     0.0f, 1.0f);
    }
    return u;
  };
  EXPECT_FALSE(HasUnitDomain(table));
  for (const LutRgb code :
       {LutRgb{0.0f, 0.0f, 0.0f}, LutRgb{0.5f, 0.5f, 0.5f}, LutRgb{1.0f, 1.0f, 1.0f},
        LutRgb{-1.0f, 3.0f, 0.3f}, LutRgb{0.25f, 0.75f, 0.6f}}) {
    EXPECT_LE(MaxAbsDifference(SampleLutTable(table, code), expected(code)), 1.0e-6f);
  }
  // The domain changes the sampled position, not the table: the same table with a unit domain
  // returns the code value.
  EXPECT_LE(
      MaxAbsDifference(SampleLutTable(IdentityTable(), {0.25f, 0.75f, 0.6f}), {0.25f, 0.75f, 0.6f}),
      1.0e-6f);

  const auto& acescc    = Encoding("acescc");
  const auto  composite = BakeLmtCompositeTable(table, acescc, acescc);
  float       worst     = 0.0f;
  for (std::size_t b = 0; b < kEdge; b += 8) {
    for (std::size_t g = 0; g < kEdge; g += 8) {
      for (std::size_t r = 0; r < kEdge; r += 8) {
        worst = std::max(worst, MaxAbsDifference(CompositeNode(composite, r, g, b),
                                                 expected(NodeCode(r, g, b))));
      }
    }
  }
  EXPECT_LE(worst, 1.0e-6f);
}

/// Error of the trilinear composite table against direct per-point composition.
struct CompositeErrorStats {
  std::size_t points = 0;
  float       median = 0.0f;
  float       p99    = 0.0f;
  float       max    = 0.0f;
  LutRgb      max_at{};
};

auto Summarize(std::vector<std::pair<float, LutRgb>> errors) -> CompositeErrorStats {
  CompositeErrorStats stats;
  stats.points = errors.size();
  if (errors.empty()) return stats;
  const auto by_error = [](const auto& a, const auto& b) { return a.first < b.first; };
  const auto max      = std::max_element(errors.begin(), errors.end(), by_error);
  stats.max           = max->first;
  stats.max_at        = max->second;
  std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end(), by_error);
  stats.median = errors[errors.size() / 2].first;
  std::nth_element(errors.begin(), errors.begin() + errors.size() * 99 / 100, errors.end(),
                   by_error);
  stats.p99 = errors[errors.size() * 99 / 100].first;
  return stats;
}

/// Error statistics on 10^6 random ACEScc points: all points, and the points whose LUT input
/// code values lie in the LUT domain.
struct CompositeErrorMeasurement {
  CompositeErrorStats all;
  CompositeErrorStats in_domain;
};

auto MeasureCompositeError(const PackedGradeLut& source, const ColorEncoding& input,
                           const ColorEncoding& output) -> CompositeErrorMeasurement {
  PackedGradeLut composite;
  composite.edge = kLmtCompositeEdge;
  composite.rgba = BakeLmtCompositeTable(source, input, output);
  const LmtEncodingConversion           conversion(input, output);
  std::mt19937                          random(1006);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  constexpr std::size_t                 kPoints = 1000000;
  std::vector<std::pair<float, LutRgb>> all;
  std::vector<std::pair<float, LutRgb>> in_domain;
  all.reserve(kPoints);
  for (std::size_t i = 0; i < kPoints; ++i) {
    const LutRgb point = {unit(random), unit(random), unit(random)};
    const float  error =
        MaxAbsDifference(SampleLutTable(composite, point), conversion.Compose(source, point));
    all.emplace_back(error, point);
    if (InsideUnitCube(conversion.AcesccToLutInput(point))) {
      in_domain.emplace_back(error, point);
    }
  }
  return {Summarize(std::move(all)), Summarize(std::move(in_domain))};
}

void PrintStats(const std::string& name, const CompositeErrorStats& stats) {
  std::cout << "[" << name << "] points " << stats.points << ", median " << stats.median << ", p99 "
            << stats.p99 << ", max " << stats.max << " at ACEScc (" << stats.max_at[0] << ", "
            << stats.max_at[1] << ", " << stats.max_at[2] << ")\n";
}

/// An ACEScc to Rec.709 BT.1886 rendering LUT (33^3): the OCIO ACES 2.0 reference forward for
/// Rec.709 at 100 nits, as a published display LUT would hold it.
auto Rec709RenderingTable() -> PackedGradeLut {
  const auto runtime =
      ResolveAces2ReferenceRuntime(color::GamutPrimariesXy(color::ColorGamutId::Rec709), 100.0f);
  const auto ap1_to_ap0 =
      color::GamutConversionMatrix(color::ColorGamutId::Ap1, color::ColorGamutId::Ap0);
  return MakeTable(33, [&](const LutRgb& acescc) {
    const LutRgb    ap0 = Multiply(ap1_to_ap0, DecodeAcescc(acescc));
    const A2rFloat3 d = A2rAp0ToDisplay(A2rMake3(ap0[0], ap0[1], ap0[2]), runtime->packed_.data());
    return LutRgb{CeEncode(CE_TF_BT1886, std::max(d.x, 0.0f)),
                  CeEncode(CE_TF_BT1886, std::max(d.y, 0.0f)),
                  CeEncode(CE_TF_BT1886, std::max(d.z, 0.0f))};
  });
}

TEST(LutBakeTest, CompositeTableTrilinearErrorAgainstDirectCompositionStaysWithinRecordedBounds) {
  // Measured on 10^6 random ACEScc points (plan section 4.4); the bounds are the values recorded
  // in the L3 completion record with a margin, so a change that makes the composite table less
  // accurate fails.
  const auto display =
      MeasureCompositeError(Rec709RenderingTable(), Encoding("acescc"), Encoding("rec709_bt1886"));
  PrintStats("display output, all points", display.all);
  const auto scene = MeasureCompositeError(OcioSLog3ToAcesccTable(),
                                           Encoding("sony_slog3_sgamut3cine"), Encoding("acescc"));
  PrintStats("scene only, all points", scene.all);
  PrintStats("scene only, S-Log3 code in [0, 1]", scene.in_domain);

  EXPECT_LE(display.all.median, 4.0e-4f);
  EXPECT_LE(display.all.p99, 6.0e-2f);
  EXPECT_LE(display.all.max, 1.6f);
  EXPECT_LE(scene.all.median, 1.0e-3f);
  EXPECT_LE(scene.all.p99, 1.6e-1f);
  EXPECT_LE(scene.all.max, 6.0e-1f);
}

TEST(LutBakeTest, DisplayOutputBakeOf65CubeIsTimed) {
  // Plan target: at most 100 ms on the release build. Debug builds only report the time.
  const auto  source = Rec709RenderingTable();
  const auto& acescc = Encoding("acescc");
  const auto& rec709 = Encoding("rec709_bt1886");
  (void)BakeLmtCompositeTable(source, acescc, rec709);  // Builds the ACES 2.0 runtime once.
  double best_ms = 1.0e9;
  for (int run = 0; run < 3; ++run) {
    const auto start = std::chrono::steady_clock::now();
    const auto table = BakeLmtCompositeTable(source, acescc, rec709);
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(table.size(), kEdge * kEdge * kEdge * 4 * sizeof(float));
    best_ms = std::min(best_ms, ms);
  }
  std::cout << "[bake time] 65^3 display output: " << best_ms << " ms (best of 3)\n";
  RecordProperty("bake_ms", std::to_string(best_ms));
#ifdef NDEBUG
  EXPECT_LE(best_ms, 100.0);
#endif
}

TEST(LutBakeTest, SampledTableKeepsDefaultSourceAndCachesCompositeByEncodingPair) {
  auto        source = std::make_shared<PackedGradeLut>(IdentityTable());
  ContentHash hash;
  hash.MixBytes(source->rgba);
  hash.MixU32(source->edge);
  source->key = hash.Key();

  EXPECT_EQ(ResolveLmtSampledTable(source, "acescc", "acescc").get(), source.get());
  const auto bakes = LmtCompositeBakeCount();
  const auto slog3 = ResolveLmtSampledTable(source, "sony_slog3_sgamut3cine", "acescc");
  ASSERT_NE(slog3, nullptr);
  EXPECT_EQ(slog3->edge, kLmtCompositeEdge);
  EXPECT_TRUE(HasUnitDomain(*slog3));
  EXPECT_NE(slog3->key, source->key);
  EXPECT_EQ(LmtCompositeBakeCount(), bakes + 1);
  EXPECT_EQ(ResolveLmtSampledTable(source, "sony_slog3_sgamut3cine", "acescc").get(), slog3.get());
  EXPECT_EQ(LmtCompositeBakeCount(), bakes + 1);

  const auto swapped = ResolveLmtSampledTable(source, "acescc", "sony_slog3_sgamut3cine");
  EXPECT_NE(swapped->key, slog3->key);
  EXPECT_EQ(LmtCompositeBakeCount(), bakes + 2);
  EXPECT_THROW((void)ResolveLmtSampledTable(source, "acescc", "unknown"), std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
