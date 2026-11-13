//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <algorithm>
#include <exception>
#include <optional>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "json.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"

namespace alcedo::ui::test {

struct RecordedAdjustmentCall {
  QString                      fieldKey;
  QString                      params;
  bool                         settled = false;
  alcedo::EditorParameterWrite write   = alcedo::EditorScalarWrite{};
};

class RecordingSubmitter : public QObject, public IEditorAdjustmentSubmitter {
 public:
  std::vector<RecordedAdjustmentCall> calls;
  bool                                canEditState = true;
  bool                                inSubmit     = false;
  bool                                reentered    = false;
  int                                 submitCount  = 0;

  auto submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
      -> bool override {
    if (inSubmit) {
      reentered = true;
    }
    inSubmit = true;
    ++submitCount;
    if (!canEditState) {
      inSubmit = false;
      return false;
    }
    calls.push_back({std::move(fieldKey), QString(), settled, std::move(write)});
    inSubmit = false;
    return true;
  }

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    nlohmann::json parsed;
    try {
      parsed = paramsJson.isEmpty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(paramsJson.toStdString());
    } catch (const std::exception&) {
      return false;
    }
    std::string error;
    auto        write = ParseEditorParameterWrite(fieldKey.toStdString(), parsed, &error);
    if (!write.has_value()) {
      return false;
    }
    auto call_ok = submitWrite(std::move(fieldKey), std::move(*write), settled);
    if (call_ok && !calls.empty()) {
      calls.back().params = std::move(paramsJson);
    }
    return call_ok;
  }

  auto canEdit() const -> bool override { return canEditState; }

  auto settledCount() const -> int {
    return static_cast<int>(std::count_if(calls.begin(), calls.end(),
                                          [](const RecordedAdjustmentCall& c) { return c.settled; }));
  }
  auto interactiveCount() const -> int {
    return static_cast<int>(
        std::count_if(calls.begin(), calls.end(),
                      [](const RecordedAdjustmentCall& c) { return !c.settled; }));
  }
  auto lastSettled() const -> const RecordedAdjustmentCall* {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      if (it->settled) {
        return &*it;
      }
    }
    return nullptr;
  }
  auto lastSettledWrite() const -> const alcedo::EditorParameterWrite* {
    const auto* call = lastSettled();
    if (call == nullptr) {
      return nullptr;
    }
    return &call->write;
  }
  static auto scalarValue(const alcedo::EditorParameterWrite& write) -> float {
    const auto* scalar = std::get_if<alcedo::EditorScalarWrite>(&write);
    return scalar == nullptr ? 0.0f : scalar->value;
  }
  static auto enumValue(const alcedo::EditorParameterWrite& write) -> QString {
    const auto* value = std::get_if<alcedo::EditorEnumWrite>(&write);
    return value == nullptr ? QString() : QString::fromStdString(value->value);
  }
  static auto toggleValue(const alcedo::EditorParameterWrite& write) -> bool {
    const auto* value = std::get_if<alcedo::EditorToggleWrite>(&write);
    return value != nullptr && value->value;
  }
};

}  // namespace alcedo::ui::test
