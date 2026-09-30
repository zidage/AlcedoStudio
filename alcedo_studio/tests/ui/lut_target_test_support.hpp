//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// LUT target source over a real PipelineDocument with three Color Grades, shared by the LUT
// target binding tests and the LUT browser QML tests.

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "app/editor_parameter_write.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

namespace alcedo::ui::test {

inline const NodeId        kPrimary{"grade.primary"};
inline const NodeId        kGradeB{"grade.b"};
inline const NodeId        kGradeC{"grade.c"};

/**
 * Target source over a real PipelineDocument. Writes are queued like the session's pending
 * input and applied later with ApplyEditorParameterWrite, the owner operation that validates
 * the target again, so a selection change between submit and apply is observable.
 */
class DocumentTargetSource final : public LutTargetSource {
 public:
  DocumentTargetSource()
      : document_(std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument())) {
    EXPECT_TRUE(AddCleanColorGrade(*document_, document_->Drt()->Id(), kGradeB).empty());
    EXPECT_TRUE(AddCleanColorGrade(*document_, document_->Drt()->Id(), kGradeC).empty());
  }

  [[nodiscard]] auto ImageId() const -> std::uint64_t override { return image_id; }
  [[nodiscard]] auto Document() const -> std::shared_ptr<const PipelineDocument> override {
    return image_id == 0 ? nullptr : document_;
  }
  [[nodiscard]] auto SelectedNodeId() const -> NodeId override { return selected; }
  [[nodiscard]] auto SelectedMaskId() const -> std::string override { return mask; }
  [[nodiscard]] auto CanEdit() const -> bool override { return can_edit; }
  auto SubmitLutWrite(const EditorParameterTarget& target, EditorLutWrite write) -> bool override {
    ++submit_count;
    queued.emplace_back(target, std::move(write));
    return true;
  }

  /// Apply queued writes in order; returns the number the owner accepted.
  auto ApplyQueued() -> int {
    int accepted = 0;
    while (!queued.empty()) {
      auto [target, write] = std::move(queued.front());
      queued.pop_front();
      std::string error;
      if (ApplyEditorParameterWrite(*document_, target, write, &error)) ++accepted;
    }
    return accepted;
  }

  [[nodiscard]] auto Mutable() -> PipelineDocument& { return *document_; }
  [[nodiscard]] auto Lmt(const NodeId& node) const -> const LmtModel* {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document_->Graph().FindNode(node));
    if (grade == nullptr) return nullptr;
    const auto* id = grade->FindAdjustmentIdByType(type_ids::Lmt());
    return id == nullptr ? nullptr : dynamic_cast<const LmtModel*>(grade->FindAdjustment(*id));
  }
  [[nodiscard]] auto LmtInstance(const NodeId& node) const -> std::string {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document_->Graph().FindNode(node));
    const auto* id    = grade->FindAdjustmentIdByType(type_ids::Lmt());
    return std::string(id->Value());
  }

  std::uint64_t                                                image_id = 7;
  NodeId                                                       selected = kGradeB;
  std::string                                                  mask;
  bool                                                         can_edit     = true;
  int                                                          submit_count = 0;
  std::deque<std::pair<EditorParameterTarget, EditorLutWrite>> queued;

 private:
  std::shared_ptr<PipelineDocument> document_;
};

}  // namespace alcedo::ui::test
