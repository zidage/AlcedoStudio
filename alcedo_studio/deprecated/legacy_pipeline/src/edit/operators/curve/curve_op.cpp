//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/curve/curve_op.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <opencv2/core.hpp>
#include <opencv2/core/types.hpp>

#include "json.hpp"

namespace alcedo {

CurveOp::CurveOp(const std::vector<cv::Point2f>& control_points) { SetCtrlPts(control_points); }

CurveOp::CurveOp(const nlohmann::json& params) { SetParams(params); }

void CurveOp::ComputeTagents() {
  const size_t N = ctrl_pts_.size();
  if (N < 2) {
    m_.assign(N, 0.0f);
    return;
  }

  std::vector<float> delta(N - 1, 0.0f);
  for (size_t i = 0; i < N - 1; ++i) {
    const float dx = h_[i];
    if (std::abs(dx) > 1e-8f) {
      delta[i] = (ctrl_pts_[i + 1].y - ctrl_pts_[i].y) / dx;
    }
  }

  m_[0]     = delta[0];
  m_[N - 1] = delta[N - 2];

  for (size_t i = 1; i < N - 1; ++i) {
    if (delta[i - 1] * delta[i] <= 0) {
      m_[i] = 0.0f;
    } else {
      const float w1    = 2.0f * h_[i] + h_[i - 1];
      const float w2    = h_[i] + 2.0f * h_[i - 1];
      const float denom = (w1 / delta[i - 1]) + (w2 / delta[i]);
      m_[i]             = ((w1 + w2) > 0.0f && std::abs(denom) > 1e-8f) ? (w1 + w2) / denom : 0.0f;
    }
  }
}

auto CurveOp::EvaluateCurve(float x) const -> float {
  const size_t N = ctrl_pts_.size();
  if (N == 0) {
    // Identity fallback: no control points means y = x.
    return x;
  }
  if (N == 1) {
    return std::clamp(ctrl_pts_.front().y, 0.0f, 1.0f);
  }

  if (x <= ctrl_pts_.front().x) return ctrl_pts_.front().y;
  if (x >= ctrl_pts_.back().x) return ctrl_pts_.back().y;

  // Find segment
  int idx = 0;
  for (int i = 0; i < static_cast<int>(ctrl_pts_.size()) - 1; ++i) {
    if (x < ctrl_pts_[i + 1].x) {
      idx = i;
      break;
    }
  }

  const float dx = h_[idx];
  if (std::abs(dx) <= 1e-8f) {
    return std::clamp(ctrl_pts_[idx].y, 0.0f, 1.0f);
  }

  float t   = (x - ctrl_pts_[idx].x) / dx;

  // Hermite interpolation
  float h00 = (2 * t * t * t - 3 * t * t + 1);
  float h10 = (t * t * t - 2 * t * t + t);
  float h01 = (-2 * t * t * t + 3 * t * t);
  float h11 = (t * t * t - t * t);

  float y = h00 * ctrl_pts_[idx].y + h10 * dx * m_[idx] + h01 * ctrl_pts_[idx + 1].y +
            h11 * dx * m_[idx + 1];

  return std::clamp(y, 0.0f, 1.0f);
}

void CurveOp::Apply(std::shared_ptr<ImageBuffer> input) {
  if (ctrl_pts_.empty()) {
    // Identity curve, no-op.
    return;
  }

  // Keep curve adjustments controllable by blending toward the mapped luminance
  // instead of applying the full delta in one step.
  constexpr float kCurveInfluence = 0.65f;

  auto& img = input->GetCPUData();

  img.forEach<cv::Vec3f>([&](cv::Vec3f& pixel, const int*) {
    float lum        = 0.2126f * pixel[2] + 0.7152f * pixel[1] + 0.0722f * pixel[0];
    float mapped_lum = EvaluateCurve(lum);
    float new_lum    = lum + (mapped_lum - lum) * kCurveInfluence;
    float ratio      = (lum > 1e-5f) ? new_lum / lum : 0.0f;
    pixel *= ratio;
  });
}

void CurveOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  // GPU implementation not provided
  throw std::runtime_error("CurveOp::ApplyGPU not implemented");
}

void CurveOp::SetCtrlPts(const std::vector<cv::Point2f>& control_points) {
  ctrl_pts_.clear();
  ctrl_pts_.reserve(control_points.size());

  for (const auto& pt : control_points) {
    if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
      continue;
    }
    ctrl_pts_.push_back({std::clamp(pt.x, 0.0f, 1.0f), std::clamp(pt.y, 0.0f, 1.0f)});
  }

  if (ctrl_pts_.size() > 1) {
    std::sort(ctrl_pts_.begin(), ctrl_pts_.end(),
              [](const cv::Point2f& a, const cv::Point2f& b) {
                if (std::abs(a.x - b.x) <= 1e-6f) {
                  return a.y < b.y;
                }
                return a.x < b.x;
              });

    std::vector<cv::Point2f> deduped;
    deduped.reserve(ctrl_pts_.size());
    for (const auto& pt : ctrl_pts_) {
      if (deduped.empty() || std::abs(pt.x - deduped.back().x) > 1e-6f) {
        deduped.push_back(pt);
      } else {
        deduped.back().y = pt.y;
      }
    }
    ctrl_pts_ = std::move(deduped);
  }

  const size_t N = ctrl_pts_.size();
  h_.clear();
  m_.clear();

  if (N == 0) {
    return;
  }

  m_.assign(N, 0.0f);
  if (N == 1) {
    return;
  }

  h_.resize(N - 1);
  for (size_t i = 0; i < N - 1; ++i) {
    h_[i] = ctrl_pts_[i + 1].x - ctrl_pts_[i].x;
  }
  ComputeTagents();
}

auto CurveOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;

  nlohmann::json inner      = nlohmann::json::object();
  nlohmann::json pts_serial = nlohmann::json::array();
  for (size_t i = 0; i < ctrl_pts_.size(); ++i) {
    const auto& pt = ctrl_pts_[i];
    pts_serial.push_back({{"x", pt.x}, {"y", pt.y}});
    inner[std::format("pts{}", i)] = {{"x", pt.x}, {"y", pt.y}};
  }
  inner["size"]   = ctrl_pts_.size();
  inner["points"] = std::move(pts_serial);

  o[script_name_] = inner;

  return o;
}

void CurveOp::SetParams(const nlohmann::json& params) {
  auto parse_point = [](const nlohmann::json& point_json, cv::Point2f& out) -> bool {
    if (!point_json.is_object() || !point_json.contains("x") || !point_json.contains("y")) {
      return false;
    }
    out.x = point_json["x"].get<float>();
    out.y = point_json["y"].get<float>();
    return true;
  };

  if (!params.contains(script_name_)) {
    SetCtrlPts({});
    return;
  }

  std::vector<cv::Point2f> control_points;
  const auto&              curve_json = params[script_name_];

  // Preferred schema:
  // { "curve": { "points": [ {"x":..., "y":...}, ... ] } }
  if (curve_json.is_object() && curve_json.contains("points") && curve_json["points"].is_array()) {
    for (const auto& point_json : curve_json["points"]) {
      cv::Point2f point;
      if (parse_point(point_json, point)) {
        control_points.push_back(point);
      }
    }
  }

  // Backward-compatible schema:
  // { "curve": { "size": N, "pts0": {"x":...,"y":...}, ... } }
  if (control_points.empty() && curve_json.is_object() && curve_json.contains("size")) {
    size_t size = curve_json["size"].get<size_t>();
    for (size_t i = 0; i < size; ++i) {
      const auto pt_name = std::format("pts{}", i);
      if (curve_json.contains(pt_name)) {
        cv::Point2f point;
        if (parse_point(curve_json[pt_name], point)) {
          control_points.push_back(point);
        }
      }
    }
  }

  SetCtrlPts(control_points);
}

void CurveOp::SetGlobalParams(OperatorParams& params) const {
  params.curve_ctrl_pts_ = ctrl_pts_;
  params.curve_m_        = m_;
  params.curve_h_        = h_;
}

void CurveOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.curve_enabled_ = enable;
}
};  // namespace alcedo
