//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Builders of the preset controller tests (edit_presets_plan.md, PS2 and PS4): an editor session
// backend over a real CommitGraph, and an in-memory preset folder preference.

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "app/preset_library_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "support/document_transfer_test_support.hpp"
#include "type/type.hpp"

namespace alcedo::test {

/**
 * Open photo whose history is a real CommitGraph over the default document. Paste runs the real
 * root-relative Paste of AdjustmentTransferService, as the editor history port does.
 */
class HistorySessionBackend final : public IEditorSessionBackend {
 public:
  explicit HistorySessionBackend(sl_element_id_t element_id)
      : graph_(std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(element_id))),
        root_document_(std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument())) {}

  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage;
  }
  [[nodiscard]] auto has_image() const -> bool override {
    return identity_.element_id > 0 && EditorSessionHasImage(state_);
  }
  [[nodiscard]] auto last_error() const -> std::string override { return {}; }
  void               SetPresentationSinkId(PresentationSinkId) override {}
  void               SetPresentationSize(int, int) override {}
  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    state_ = element_id == 0 ? EditorSessionState::NoImage : EditorSessionState::Interactive;
    NotifyChange();
    return Result(EditorSessionResultKind::StateChanged, {});
  }
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    return Open(element_id, image_id);
  }
  auto Close(bool) -> EditorSessionResult override { return Open(0, 0); }
  auto Shutdown() -> EditorSessionResult override { return Open(0, 0); }
  auto Discard() -> EditorSessionResult override {
    return Result(EditorSessionResultKind::Accepted, {});
  }
  auto Undo() -> EditorSessionResult override {
    return Result(EditorSessionResultKind::Accepted, {});
  }
  auto Redo() -> EditorSessionResult override {
    return Result(EditorSessionResultKind::Accepted, {});
  }

  auto SnapshotHistorySource(std::shared_ptr<const CommitGraph>*      graph,
                             std::shared_ptr<const PipelineDocument>* root_document, std::string*)
      -> bool override {
    *graph         = std::make_shared<const CommitGraph>(*graph_);
    *root_document = root_document_;
    return true;
  }

  auto PasteAdjustments(const AdjustmentTransferPackage& package, std::string version_display_name,
                        std::optional<EditCommitAttribution> attribution)
      -> EditorSessionResult override {
    ++paste_count_;
    const auto pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(
        *graph_, *root_document_, package, std::move(version_display_name), {},
        std::move(attribution));
    return pasted.pasted ? Result(EditorSessionResultKind::Accepted, "Pasted")
                         : Result(EditorSessionResultKind::Failed, pasted.error);
  }

  [[nodiscard]] auto graph() -> CommitGraph& { return *graph_; }
  [[nodiscard]] auto root_document() const -> const PipelineDocument& { return *root_document_; }
  /// Pastes that reached this session.
  [[nodiscard]] auto paste_count() const -> int { return paste_count_; }

 private:
  auto Result(EditorSessionResultKind kind, std::string message) const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = kind;
    result.state    = state_;
    result.identity = identity_;
    result.message  = std::move(message);
    return result;
  }

  std::shared_ptr<CommitGraph>      graph_;
  std::shared_ptr<PipelineDocument> root_document_;
  EditorSessionState                state_ = EditorSessionState::NoImage;
  EditorSessionIdentity             identity_{};
  int                               paste_count_ = 0;
};

/// Preset folder setting held in memory, for a service on a temporary folder.
class MemoryPresetPreferences final : public PresetLibraryPreferences {
 public:
  explicit MemoryPresetPreferences(std::filesystem::path root) : root_(std::move(root)) {}
  [[nodiscard]] auto LoadRoot() const -> std::optional<std::filesystem::path> override {
    return root_;
  }
  [[nodiscard]] auto SaveRoot(const std::filesystem::path& root) -> bool override {
    root_ = root;
    return true;
  }

 private:
  std::filesystem::path root_;
};

/// Service options on @p root: the folder setting in memory and Delete as a plain remove.
inline auto PresetLibraryOptionsOn(const std::filesystem::path& root)
    -> PresetLibraryServiceOptions {
  PresetLibraryServiceOptions options;
  options.preferences   = std::make_unique<MemoryPresetPreferences>(root);
  options.default_root  = root;
  options.move_to_trash = [](const std::filesystem::path& path, std::string*) {
    std::error_code error;
    return std::filesystem::remove(path, error);
  };
  return options;
}

inline void WritePresetBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// The canonical preset file text of a package that sets exposure to @p exposure_ev.
inline auto ExposurePresetText(double exposure_ev) -> std::string {
  return ExportDocumentTransfer(MakeExposureTransferPackage(exposure_ev)).dump();
}

}  // namespace alcedo::test
