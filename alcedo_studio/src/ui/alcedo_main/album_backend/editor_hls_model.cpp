//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_hls_model.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>

namespace alcedo::ui {
namespace {

auto ClampHueShift(float v) -> float {
  return std::clamp(v, -hls::kMaxHueShiftDegrees, hls::kMaxHueShiftDegrees);
}

auto ClampAdj(float v) -> float { return std::clamp(v, hls::kAdjUiMin, hls::kAdjUiMax); }

auto ClampHueRange(float v) -> float { return std::max(v, 1.0f); }

}  // namespace

EditorHlsModel::EditorHlsModel(QObject* parent) : EditorAdjustmentModelBase(parent) {
  setFieldKey(QStringLiteral("hls"));
  setLabel(QStringLiteral("Selective Color"));
  loadActiveProfile();
}

void EditorHlsModel::setActiveHueIndex(int index) {
  const int clamped =
      std::clamp(index, 0, static_cast<int>(hls::kCandidateHues.size()) - 1);
  if (activeHueIndex_ == clamped) {
    return;
  }
  saveActiveProfile();
  activeHueIndex_ = clamped;
  loadActiveProfile();
  emit activeHueIndexChanged();
  emit activeProfileChanged();
}

auto EditorHlsModel::targetHue() const -> double {
  return hls::kCandidateHues[static_cast<size_t>(activeHueIndex_)];
}

void EditorHlsModel::setHueShift(double value) {
  applyHueShift(value);
}

void EditorHlsModel::setLightness(double value) {
  applyLightness(value);
}

void EditorHlsModel::setChroma(double value) {
  applyChroma(value);
}

void EditorHlsModel::setHueSmoothness(double value) {
  applyHueSmoothness(value);
}

auto EditorHlsModel::hueSwatches() const -> QVariantList {
  QVariantList list;
  list.reserve(static_cast<int>(hls::kCandidateHues.size()));
  for (int i = 0; i < static_cast<int>(hls::kCandidateHues.size()); ++i) {
    const float hue = hls::kCandidateHues[static_cast<size_t>(i)];
    QVariantMap entry;
    entry.insert(QStringLiteral("index"), i);
    entry.insert(QStringLiteral("hue"), hue);
    entry.insert(QStringLiteral("color"), hls::CandidateColor(hue));
    list.push_back(entry);
  }
  return list;
}

void EditorHlsModel::selectHueIndex(int index) { setActiveHueIndex(index); }

void EditorHlsModel::beginHueShiftDrag() { beginDrag(DragTarget::HueShift); }

void EditorHlsModel::updateHueShiftDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::HueShift) {
    return;
  }
  if (!applyHueShift(value)) {
    return;
  }
  submitInteractive();
}

void EditorHlsModel::finishHueShiftDrag() { finishDrag(); }

void EditorHlsModel::beginLightnessDrag() { beginDrag(DragTarget::Lightness); }

void EditorHlsModel::updateLightnessDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::Lightness) {
    return;
  }
  if (!applyLightness(value)) {
    return;
  }
  submitInteractive();
}

void EditorHlsModel::finishLightnessDrag() { finishDrag(); }

void EditorHlsModel::beginChromaDrag() { beginDrag(DragTarget::Chroma); }

void EditorHlsModel::updateChromaDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::Chroma) {
    return;
  }
  if (!applyChroma(value)) {
    return;
  }
  submitInteractive();
}

void EditorHlsModel::finishChromaDrag() { finishDrag(); }

void EditorHlsModel::beginHueSmoothnessDrag() { beginDrag(DragTarget::HueSmoothness); }

void EditorHlsModel::updateHueSmoothnessDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::HueSmoothness) {
    return;
  }
  if (!applyHueSmoothness(value)) {
    return;
  }
  submitInteractive();
}

void EditorHlsModel::finishHueSmoothnessDrag() { finishDrag(); }

void EditorHlsModel::editHueShift(double value) {
  applyHueShift(value);
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::editLightness(double value) {
  applyLightness(value);
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::editChroma(double value) {
  applyChroma(value);
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::editHueSmoothness(double value) {
  applyHueSmoothness(value);
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::commitImmediately() {
  if (dragActive_) {
    dragActive_ = false;
    dragTarget_ = DragTarget::None;
    emit dragActiveChanged();
  }
  saveActiveProfile();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::resetActiveField(const QString& field) {
  if (field.compare(QStringLiteral("hueShift"), Qt::CaseInsensitive) == 0) {
    applyHueShift(0.0);
  } else if (field.compare(QStringLiteral("lightness"), Qt::CaseInsensitive) == 0) {
    applyLightness(0.0);
  } else if (field.compare(QStringLiteral("chroma"), Qt::CaseInsensitive) == 0) {
    applyChroma(0.0);
  } else if (field.compare(QStringLiteral("hueSmoothness"), Qt::CaseInsensitive) == 0) {
    applyHueSmoothness(hls::kDefaultHueRange);
  } else {
    applyHueShift(0.0);
    applyLightness(0.0);
    applyChroma(0.0);
    applyHueSmoothness(hls::kDefaultHueRange);
  }
  saveActiveProfile();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::resetAllProfiles() {
  hueShiftTable_  = {};
  lightnessTable_ = {};
  chromaTable_    = {};
  hueRangeTable_  = hls::MakeFilledArray(hls::kDefaultHueRange);
  loadActiveProfile();
  emit activeProfileChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorHlsModel::paramsJson() const -> QString { return buildParamsJson(); }

void EditorHlsModel::loadFromTables(const QVariantList& hueAdjTable,
                                    const QVariantList& hueRangeTable, double targetHue) {
  for (int i = 0; i < static_cast<int>(hls::kCandidateHues.size()); ++i) {
    if (i < hueAdjTable.size()) {
      const QVariant entry = hueAdjTable[i];
      if (entry.canConvert<QVariantList>()) {
        const QVariantList triple = entry.toList();
        if (triple.size() >= 3) {
          hueShiftTable_[static_cast<size_t>(i)]  = ClampHueShift(triple[0].toFloat());
          lightnessTable_[static_cast<size_t>(i)] = ClampAdj(triple[1].toFloat());
          chromaTable_[static_cast<size_t>(i)]    = ClampAdj(triple[2].toFloat());
        }
      }
    }
    if (i < hueRangeTable.size()) {
      hueRangeTable_[static_cast<size_t>(i)] = ClampHueRange(hueRangeTable[i].toFloat());
    }
  }
  activeHueIndex_ = hls::ClosestCandidateHueIndex(static_cast<float>(targetHue));
  loadActiveProfile();
  emit activeHueIndexChanged();
  emit activeProfileChanged();
}

void EditorHlsModel::saveActiveProfile() {
  const size_t idx                = static_cast<size_t>(activeHueIndex_);
  hueShiftTable_[idx]             = hueShift_;
  lightnessTable_[idx]            = lightness_;
  chromaTable_[idx]               = chroma_;
  hueRangeTable_[idx]             = hueSmoothness_;
}

void EditorHlsModel::loadActiveProfile() {
  const size_t idx = static_cast<size_t>(activeHueIndex_);
  hueShift_        = hueShiftTable_[idx];
  lightness_       = lightnessTable_[idx];
  chroma_          = chromaTable_[idx];
  hueSmoothness_   = hueRangeTable_[idx];
}

auto EditorHlsModel::applyHueShift(double value) -> bool {
  const float next = ClampHueShift(static_cast<float>(value));
  if (std::abs(next - hueShift_) < 1e-6f) {
    return false;
  }
  hueShift_ = next;
  saveActiveProfile();
  emit activeProfileChanged();
  return true;
}

auto EditorHlsModel::applyLightness(double value) -> bool {
  const float next = ClampAdj(static_cast<float>(value));
  if (std::abs(next - lightness_) < 1e-6f) {
    return false;
  }
  lightness_ = next;
  saveActiveProfile();
  emit activeProfileChanged();
  return true;
}

auto EditorHlsModel::applyChroma(double value) -> bool {
  const float next = ClampAdj(static_cast<float>(value));
  if (std::abs(next - chroma_) < 1e-6f) {
    return false;
  }
  chroma_ = next;
  saveActiveProfile();
  emit activeProfileChanged();
  return true;
}

auto EditorHlsModel::applyHueSmoothness(double value) -> bool {
  const float next = ClampHueRange(static_cast<float>(value));
  if (std::abs(next - hueSmoothness_) < 1e-6f) {
    return false;
  }
  hueSmoothness_ = next;
  saveActiveProfile();
  emit activeProfileChanged();
  return true;
}

void EditorHlsModel::beginDrag(DragTarget target) {
  dragActive_ = true;
  dragTarget_ = target;
  emit dragActiveChanged();
}

void EditorHlsModel::finishDrag() {
  if (!dragActive_) {
    return;
  }
  dragActive_ = false;
  dragTarget_ = DragTarget::None;
  emit dragActiveChanged();
  saveActiveProfile();
  submitSettled();
  emit settledCommitted();
}

void EditorHlsModel::submitInteractive() { submitNow(currentHlsWrite(), false); }

void EditorHlsModel::submitSettled() { submitNow(currentHlsWrite(), true); }

auto EditorHlsModel::currentHlsWrite() const -> alcedo::HlsUpdate {
  alcedo::HlsUpdate update;
  std::array<float, alcedo::kHlsHueBinCount> hue_bins{};
  std::array<alcedo::HlsVec3, alcedo::kHlsHueBinCount> adj_table{};
  std::array<float, alcedo::kHlsHueBinCount> hue_range{};
  for (size_t i = 0; i < hls::kCandidateHues.size(); ++i) {
    hue_bins[i]     = hls::kCandidateHues[i];
    adj_table[i]    = {ClampHueShift(hueShiftTable_[i]),
                       ClampAdj(lightnessTable_[i]) / hls::kAdjUiToParamScale,
                       ClampAdj(chromaTable_[i]) / hls::kAdjUiToParamScale};
    hue_range[i]    = ClampHueRange(hueRangeTable_[i]);
  }
  const size_t active = static_cast<size_t>(activeHueIndex_);
  update.hue_bins       = hue_bins;
  update.hls_adj_table  = adj_table;
  update.h_range_table  = hue_range;
  update.target_hls     = {hls::WrapHueDegrees(hls::kCandidateHues[active]),
                           hls::kFixedTargetLightness, hls::kFixedTargetSaturation};
  update.hls_adj        = {ClampHueShift(hueShiftTable_[active]),
                           ClampAdj(lightnessTable_[active]) / hls::kAdjUiToParamScale,
                           ClampAdj(chromaTable_[active]) / hls::kAdjUiToParamScale};
  update.h_range        = ClampHueRange(hueRangeTable_[active]);
  update.l_range        = hls::kFixedLightnessRange;
  update.s_range        = hls::kFixedSaturationRange;
  return update;
}

auto EditorHlsModel::buildParamsJson() const -> QString {
  QJsonArray hue_bins;
  QJsonArray hls_adj_table;
  QJsonArray h_range_table;
  for (size_t i = 0; i < hls::kCandidateHues.size(); ++i) {
    hue_bins.append(hls::kCandidateHues[i]);
    QJsonArray triple;
    triple.append(ClampHueShift(hueShiftTable_[i]));
    triple.append(ClampAdj(lightnessTable_[i]) / hls::kAdjUiToParamScale);
    triple.append(ClampAdj(chromaTable_[i]) / hls::kAdjUiToParamScale);
    hls_adj_table.append(triple);
    h_range_table.append(ClampHueRange(hueRangeTable_[i]));
  }

  const size_t active = static_cast<size_t>(activeHueIndex_);
  QJsonArray   hls_adj;
  hls_adj.append(ClampHueShift(hueShiftTable_[active]));
  hls_adj.append(ClampAdj(lightnessTable_[active]) / hls::kAdjUiToParamScale);
  hls_adj.append(ClampAdj(chromaTable_[active]) / hls::kAdjUiToParamScale);

  QJsonArray target_hls;
  target_hls.append(hls::WrapHueDegrees(hls::kCandidateHues[active]));
  target_hls.append(hls::kFixedTargetLightness);
  target_hls.append(hls::kFixedTargetSaturation);

  QJsonObject hls_obj;
  hls_obj.insert(QStringLiteral("hue_bins"), hue_bins);
  hls_obj.insert(QStringLiteral("hls_adj_table"), hls_adj_table);
  hls_obj.insert(QStringLiteral("h_range_table"), h_range_table);
  hls_obj.insert(QStringLiteral("target_hls"), target_hls);
  hls_obj.insert(QStringLiteral("hls_adj"), hls_adj);
  hls_obj.insert(QStringLiteral("h_range"), ClampHueRange(hueRangeTable_[active]));
  hls_obj.insert(QStringLiteral("l_range"), hls::kFixedLightnessRange);
  hls_obj.insert(QStringLiteral("s_range"), hls::kFixedSaturationRange);

  QJsonObject root;
  root.insert(QStringLiteral("HLS"), hls_obj);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

}  // namespace alcedo::ui
