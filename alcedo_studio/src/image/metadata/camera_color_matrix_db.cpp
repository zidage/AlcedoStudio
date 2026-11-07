//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "camera_color_matrix_db.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "camera_name_match.hpp"
#include "edit/operators/basic/camera_matrices.hpp"

namespace alcedo::metadata_detail {

// ---------------------------------------------------------------------------
//  Adobe DNG camera colour-matrix database lookup
//  (ported from color_temp_op.cpp – executed once at import time)
// ---------------------------------------------------------------------------

namespace {
auto ResolveCameraColorMatrixAlias(const std::string& camera_make, const std::string& camera_model)
    -> std::string {
  const auto make_tokens   = TokenizeCameraNameLoose(camera_make);
  const auto model_tokens  = TokenizeCameraNameLoose(camera_model);
  const auto model_compact = CompactCameraName(camera_model);

  const bool is_hasselblad = ContainsCameraToken(make_tokens, "hasselblad") ||
                             ContainsCameraToken(model_tokens, "hasselblad");
  if (!is_hasselblad) {
    return {};
  }

  // Adobe's Hasselblad entries in this database are grouped by sensor generation rather than
  // modern retail model names. Current Hasselblad camera names still expose the sensor tier in
  // the model string (e.g. 39, 50C, 100C), so resolve to the closest Adobe family by that tier.
  const bool has_39 =
      ContainsCameraToken(model_tokens, "39") || ContainsCameraTokenFragment(model_tokens, "39") ||
      model_compact.ends_with("39") || model_compact.find("39ms") != std::string::npos;
  if (has_39) {
    return NormalizeCameraName("Hasselblad 39-Coated");
  }

  const bool has_50c = ContainsCameraToken(model_tokens, "50c") ||
                       ContainsCameraTokenFragment(model_tokens, "50c") ||
                       model_compact.find("50c") != std::string::npos;
  if (has_50c) {
    return NormalizeCameraName("Hasselblad 50-15-Coated5");
  }

  const bool has_100c = ContainsCameraToken(model_tokens, "100c") ||
                        ContainsCameraTokenFragment(model_tokens, "100c") ||
                        model_compact.find("100c") != std::string::npos;
  if (!has_100c) {
    return {};
  }

  const bool has_x2d = ContainsCameraToken(model_tokens, "x2d") ||
                       ContainsCameraTokenFragment(model_tokens, "x2d") ||
                       model_compact.find("x2d") != std::string::npos;
  if (has_x2d && (ContainsCameraToken(model_tokens, "ii") ||
                  ContainsCameraTokenFragment(model_tokens, "x2dii") ||
                  model_compact.find("x2dii") != std::string::npos)) {
    return NormalizeCameraName("Hasselblad 100-22-Coated6");
  }
  return NormalizeCameraName("Hasselblad 100-20-Coated6");
}

struct CameraColorMatrixEntry {
  std::string              name_;
  std::string              compact_name_;
  std::vector<std::string> match_tokens_;
  double                   cm1_[9];
  double                   cm2_[9];
};

auto CameraColorMatrixDatabaseSorted() -> const std::vector<CameraColorMatrixEntry>& {
  static const std::vector<CameraColorMatrixEntry> index = [] {
    std::vector<CameraColorMatrixEntry> out;
    const size_t count = sizeof(all_camera_matrices) / sizeof(all_camera_matrices[0]);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      const auto& item = all_camera_matrices[i];
      auto        key  = NormalizeCameraName(item.camera_name_);
      if (key.empty()) {
        continue;
      }
      CameraColorMatrixEntry entry;
      entry.name_         = std::move(key);
      entry.compact_name_ = CompactNormalizedCameraName(entry.name_);
      entry.match_tokens_ =
          UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(item.camera_name_));
      std::memcpy(entry.cm1_, item.color_matrix_1_, sizeof(entry.cm1_));
      std::memcpy(entry.cm2_, item.color_matrix_2_, sizeof(entry.cm2_));
      out.push_back(std::move(entry));
    }

    // Normalization can change ordering (e.g. "7D" -> "7 d"), so sort before binary search.
    std::sort(out.begin(), out.end(),
              [](const CameraColorMatrixEntry& a, const CameraColorMatrixEntry& b) {
                return a.name_ < b.name_;
              });

    // Deduplicate normalized keys while keeping the first matrix entry.
    out.erase(std::unique(out.begin(), out.end(),
                          [](const CameraColorMatrixEntry& a, const CameraColorMatrixEntry& b) {
                            return a.name_ == b.name_;
                          }),
              out.end());
    return out;
  }();
  return index;
}

auto FindInSortedColorMatrixDB(const std::vector<CameraColorMatrixEntry>& db,
                               const std::string& key) -> const CameraColorMatrixEntry* {
  if (key.empty()) {
    return nullptr;
  }
  auto it = std::lower_bound(
      db.begin(), db.end(), key,
      [](const CameraColorMatrixEntry& entry, const std::string& k) { return entry.name_ < k; });
  if (it != db.end() && it->name_ == key) {
    return &(*it);
  }
  return nullptr;
}

struct CameraMatrixCandidateScore {
  int total_score  = 0;
  int model_signal = 0;
};

auto ScoreApproximateColorMatrixCandidate(const CameraColorMatrixEntry&   entry,
                                          const std::string&              make_key,
                                          const std::string&              make_compact,
                                          const std::string&              full_compact,
                                          const std::string&              model_compact,
                                          const std::vector<std::string>& model_tokens)
    -> CameraMatrixCandidateScore {
  CameraMatrixCandidateScore score;

  if (!make_key.empty()) {
    if (entry.name_ == make_key || entry.name_.starts_with(make_key + " ")) {
      score.total_score += 1200;
    } else if (!make_compact.empty() &&
               entry.compact_name_.find(make_compact) != std::string::npos) {
      score.total_score += 700;
    }
  }

  if (!full_compact.empty()) {
    if (entry.compact_name_ == full_compact) {
      score.model_signal += 900;
    } else if (full_compact.size() >= 6 &&
               (entry.compact_name_.find(full_compact) != std::string::npos ||
                full_compact.find(entry.compact_name_) != std::string::npos)) {
      score.model_signal += 260;
    }
  }

  if (!model_compact.empty()) {
    if (entry.compact_name_ == model_compact || entry.compact_name_.ends_with(model_compact)) {
      score.model_signal += 700;
    } else if (model_compact.size() >= 4 &&
               (entry.compact_name_.find(model_compact) != std::string::npos ||
                model_compact.find(entry.compact_name_) != std::string::npos)) {
      score.model_signal += 260;
    }
  }

  for (const auto& token : model_tokens) {
    if (ContainsCameraToken(entry.match_tokens_, token)) {
      score.model_signal += 120;
      continue;
    }
    if (token.size() < 3) {
      continue;
    }
    const bool partial_match =
        std::any_of(entry.match_tokens_.begin(), entry.match_tokens_.end(),
                    [&](const std::string& candidate_token) {
                      return candidate_token.find(token) != std::string::npos ||
                             token.find(candidate_token) != std::string::npos;
                    });
    if (partial_match) {
      score.model_signal += 40;
    }
  }

  score.total_score += score.model_signal;
  score.total_score -= static_cast<int>(std::abs(static_cast<int>(entry.match_tokens_.size()) -
                                                 static_cast<int>(model_tokens.size()))) *
                       15;
  return score;
}

auto FindApproximateColorMatrixMatch(const std::vector<CameraColorMatrixEntry>& db,
                                     const std::string&                         camera_make,
                                     const std::string&                         camera_model)
    -> const CameraColorMatrixEntry* {
  const auto make_key      = NormalizeCameraName(camera_make);
  const auto make_compact  = CompactNormalizedCameraName(make_key);
  const auto full_compact  = CompactCameraName(camera_make + " " + camera_model);
  const auto model_compact = CompactCameraName(camera_model);
  auto       model_tokens  = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(camera_model));
  if (model_tokens.empty()) {
    model_tokens =
        UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(camera_make + " " + camera_model));
  }
  if (model_tokens.empty() && model_compact.empty()) {
    return nullptr;
  }

  const CameraColorMatrixEntry* best       = nullptr;
  int                           best_score = -1;
  int                           runner_up  = -1;
  for (const auto& entry : db) {
    const auto score = ScoreApproximateColorMatrixCandidate(
        entry, make_key, make_compact, full_compact, model_compact, model_tokens);
    if (score.model_signal < 120 || score.total_score < 650) {
      continue;
    }
    if (score.total_score > best_score) {
      runner_up  = best_score;
      best_score = score.total_score;
      best       = &entry;
    } else if (score.total_score > runner_up) {
      runner_up = score.total_score;
    }
  }

  if (!best) {
    return nullptr;
  }
  if (runner_up >= 0 && best_score < runner_up + 140) {
    return nullptr;
  }
  return best;
}
}  // namespace

auto LookupCameraColorMatrices(const std::string& camera_make, const std::string& camera_model,
                               double cm1_out[9], double cm2_out[9]) -> bool {
  const auto                    full_key  = NormalizeCameraName(camera_make + " " + camera_model);
  const auto                    model_key = NormalizeCameraName(camera_model);

  const auto&                   db        = CameraColorMatrixDatabaseSorted();

  const CameraColorMatrixEntry* found     = FindInSortedColorMatrixDB(db, full_key);
  if (!found) {
    found = FindInSortedColorMatrixDB(db, model_key);
  }
  if (!found && !model_key.empty()) {
    const auto make_key = NormalizeCameraName(camera_make);
    if (!make_key.empty() && model_key.starts_with(make_key + " ")) {
      found = FindInSortedColorMatrixDB(db, model_key);
    }
  }
  if (!found) {
    const auto alias_key = ResolveCameraColorMatrixAlias(camera_make, camera_model);
    if (!alias_key.empty()) {
      found = FindInSortedColorMatrixDB(db, alias_key);
    }
  }
  if (!found) {
    found = FindApproximateColorMatrixMatch(db, camera_make, camera_model);
  }

  if (found) {
    std::memcpy(cm1_out, found->cm1_, 9 * sizeof(double));
    std::memcpy(cm2_out, found->cm2_, 9 * sizeof(double));
    return true;
  }
  return false;
}

void MarkAdobeCameraMatrixDatabaseIlluminants(RawRuntimeColorContext& ctx) {
  ctx.calibration_illuminants_valid_ = true;
  ctx.color_matrix_1_cct_            = 2856.0;
  ctx.color_matrix_2_cct_            = 6504.0;
}

}  // namespace alcedo::metadata_detail
