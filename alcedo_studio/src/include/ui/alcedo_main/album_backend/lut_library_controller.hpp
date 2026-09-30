//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/lut_library_service.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {

/**
 * @brief Editor state the LUT target is read from, and the owner that accepts LUT writes.
 *
 * Production: an adapter over EditorSessionController (LutLibraryController::setEditorSession).
 * Tests supply their own implementation. All methods run on the GUI thread.
 */
class LutTargetSource {
 public:
  virtual ~LutTargetSource()                                                            = default;
  /// Image ID of the open photo; 0 when no photo is open.
  [[nodiscard]] virtual auto ImageId() const -> std::uint64_t                           = 0;
  /// Immutable document published for the open photo; null without one.
  [[nodiscard]] virtual auto Document() const -> std::shared_ptr<const PipelineDocument> = 0;
  /// Primary selected node; empty when nothing is selected. A multi-selection has one
  /// primary node, and only that node is a LUT target.
  [[nodiscard]] virtual auto SelectedNodeId() const -> NodeId                           = 0;
  /// Mask being edited on the selected node; empty when the node itself is selected.
  [[nodiscard]] virtual auto SelectedMaskId() const -> std::string                      = 0;
  /// True when the session accepts edits (a photo is open and interactive).
  [[nodiscard]] virtual auto CanEdit() const -> bool                                    = 0;
  /// Enqueue one settled LUT write for @p target, which the caller captured from Document().
  /// The session owner validates the target again when it applies the write.
  virtual auto SubmitLutWrite(const EditorParameterTarget& target, EditorLutWrite write)
      -> bool = 0;
};

/// Why the current selection can or cannot receive a LUT (LutLibraryController::targetState).
enum class LutTargetState : std::uint8_t {
  kReady,
  kNoImage,
  kNoNode,
  kNotColorGrade,
  kMaskSelected,
  kNoLutAdjustment,
  kNotEditable,
};

/**
 * @brief Binds the LUT browser to exactly one Color Grade: the primary selected node.
 *
 * Reads: the target (node identity and name) and its current association (reference, last
 * known name, strength, and availability through the library) are read from the published
 * document and the library. Reading never submits an edit, so changing nodes or reopening
 * the workspace is load-only (plan 6.4).
 *
 * Writes: applyEntry and clearAssociation capture the complete target from the current
 * document, check it, and submit one settled LUT write for that target. A later selection
 * change cannot redirect it; the session owner rejects it if the node or its LMT adjustment
 * no longer exists. The strength is kept: the write carries only the reference and name.
 * Nothing resolves to PrimaryGrade or to another selected node.
 *
 * Without a photo or a Color Grade target, browsing stays available through LutLibraryModel,
 * and apply requests are rejected with targetMessage.
 * Thread affinity: GUI thread.
 */
class LutLibraryController : public QObject {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::EditorSessionController* editorSession READ editorSession WRITE
                 setEditorSession NOTIFY editorSessionChanged)
  Q_PROPERTY(alcedo::LutLibraryService* library READ library WRITE setLibrary NOTIFY libraryChanged)
  /// `ready`, `noImage`, `noNode`, `notColorGrade`, `maskSelected`, `noLutAdjustment`,
  /// or `notEditable`.
  Q_PROPERTY(QString targetState READ targetState NOTIFY targetChanged)
  /// Reason apply is unavailable; empty when ready.
  Q_PROPERTY(QString targetMessage READ targetMessage NOTIFY targetChanged)
  Q_PROPERTY(bool hasImage READ hasImage NOTIFY targetChanged)
  Q_PROPERTY(bool canApply READ canApply NOTIFY targetChanged)
  Q_PROPERTY(QString targetNodeId READ targetNodeId NOTIFY targetChanged)
  Q_PROPERTY(QString targetNodeName READ targetNodeName NOTIFY targetChanged)
  Q_PROPERTY(bool hasAssociation READ hasAssociation NOTIFY associationChanged)
  /// Library entry ID the association resolves to; empty when none is listed.
  Q_PROPERTY(QString associationEntryId READ associationEntryId NOTIFY associationChanged)
  Q_PROPERTY(QString associationName READ associationName NOTIFY associationChanged)
  /// Print option of the associated official film simulation, shown on its own line; empty
  /// when the LUT has no print or is not listed.
  Q_PROPERTY(QString associationPrintName READ associationPrintName NOTIFY associationChanged)
  /// Configured strength in [0, 1].
  Q_PROPERTY(double associationStrength READ associationStrength NOTIFY associationChanged)
  /// True when the associated LUT file is missing; the reference and strength are kept.
  Q_PROPERTY(bool associationMissing READ associationMissing NOTIFY associationChanged)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

 public:
  explicit LutLibraryController(QObject* parent = nullptr);
  ~LutLibraryController() override;

  [[nodiscard]] auto editorSession() const -> EditorSessionController* { return session_; }
  /// Read the target from @p session and follow its image, document, and node selection.
  void               setEditorSession(EditorSessionController* session);
  /// Use @p source (not owned; must outlive this object or be replaced) instead of a session.
  /// The caller requests reload() when the source changes.
  void               SetTargetSource(LutTargetSource* source);
  [[nodiscard]] auto library() const -> alcedo::LutLibraryService* { return library_; }
  void               setLibrary(alcedo::LutLibraryService* library);

  [[nodiscard]] auto state() const -> LutTargetState { return state_; }
  [[nodiscard]] auto targetState() const -> QString;
  [[nodiscard]] auto targetMessage() const -> QString;
  [[nodiscard]] auto hasImage() const -> bool { return state_ != LutTargetState::kNoImage; }
  [[nodiscard]] auto canApply() const -> bool { return state_ == LutTargetState::kReady; }
  [[nodiscard]] auto targetNodeId() const -> QString { return target_node_id_; }
  [[nodiscard]] auto targetNodeName() const -> QString { return target_node_name_; }
  [[nodiscard]] auto hasAssociation() const -> bool { return !IsEmptyLutReference(reference_); }
  [[nodiscard]] auto associationReference() const -> const LutReference& { return reference_; }
  [[nodiscard]] auto associationEntryId() const -> QString { return association_entry_id_; }
  [[nodiscard]] auto associationName() const -> QString { return association_name_; }
  [[nodiscard]] auto associationPrintName() const -> QString { return association_print_name_; }
  [[nodiscard]] auto associationStrength() const -> double { return strength_; }
  [[nodiscard]] auto associationMissing() const -> bool { return missing_; }
  [[nodiscard]] auto lastError() const -> QString { return last_error_; }

  /// Read the target and its association again. Load-only: never submits.
  Q_INVOKABLE void   reload();
  /// Apply the library entry @p entry_id to the captured target, keeping its strength.
  /// Returns false and emits applyRejected when there is no valid target, the entry is not
  /// listed or cannot be applied, or the session rejects the write.
  Q_INVOKABLE bool   applyEntry(const QString& entry_id);
  /// Remove the LUT from the captured target, keeping its strength. Same failures as applyEntry.
  Q_INVOKABLE bool   clearAssociation();

 signals:
  void editorSessionChanged();
  void libraryChanged();
  void targetChanged();
  void associationChanged();
  void lastErrorChanged();
  void applyRejected(const QString& message);

 private:
  /// The target of the current selection, read from the current document.
  struct TargetRead {
    LutTargetState                       state = LutTargetState::kNoImage;
    std::shared_ptr<const PipelineDocument> document;
    std::optional<EditorParameterTarget> target;
    QString                              node_id;
    QString                              node_name;
  };

  [[nodiscard]] static auto MessageForState(LutTargetState state) -> QString;
  [[nodiscard]] auto ReadTarget() const -> TargetRead;
  void               LoadAssociation(const TargetRead& read);
  [[nodiscard]] auto EntryIdForReference(const LutReference& reference) const -> QString;
  auto               SubmitReference(LutReference reference, std::string display_name) -> bool;
  auto               Reject(const QString& message) -> bool;
  void               SetLastError(const QString& message);

  QPointer<EditorSessionController>   session_;
  std::unique_ptr<LutTargetSource>    session_source_;
  LutTargetSource*                    source_ = nullptr;
  QPointer<alcedo::LutLibraryService> library_;
  LutTargetState                      state_ = LutTargetState::kNoImage;
  QString                             target_node_id_;
  QString                             target_node_name_;
  LutReference                        reference_;
  QString                             association_entry_id_;
  QString                             association_name_;
  QString                             association_print_name_;
  double                              strength_ = 1.0;
  bool                                missing_  = false;
  QString                             last_error_;
};

}  // namespace alcedo::ui
