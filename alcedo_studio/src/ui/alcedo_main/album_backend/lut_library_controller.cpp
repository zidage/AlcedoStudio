//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

#include <utility>
#include <variant>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/lut_library_publication.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "ui/alcedo_main/album_backend/editor_mask_creation_adapter.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr const char* kLutFieldKey = "lut";

auto ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto ToUtf8(const QString& text) -> std::string {
  const QByteArray bytes = text.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

/// Panels whose own node (Develop or DRT) is not a Color Grade, but which still let the LUT
/// browser apply to the Color Grade that the LUT panel edits. Geometry is not in this set.
auto PanelTargetsLutPanelNode(const QString& panel) -> bool {
  return panel == QLatin1String("raw") || panel == QLatin1String("display") ||
         panel == QLatin1String("post");
}

/// Production target source: the editor session and its bound node selection.
class EditorSessionLutTargetSource final : public LutTargetSource {
 public:
  explicit EditorSessionLutTargetSource(EditorSessionController* session) : session_(session) {}

  [[nodiscard]] auto ImageId() const -> std::uint64_t override {
    return session_ ? session_->image_id() : 0;
  }
  [[nodiscard]] auto Document() const -> std::shared_ptr<const PipelineDocument> override {
    return session_ ? session_->pipeline_document() : nullptr;
  }
  [[nodiscard]] auto SelectedNodeId() const -> NodeId override {
    return session_ ? session_->selected_node_id() : NodeId{};
  }
  [[nodiscard]] auto SelectedMaskId() const -> std::string override {
    const EditorMaskCreationAdapter* masks = session_ ? session_->mask_creation() : nullptr;
    if (masks == nullptr || !masks->mask_controls_active()) return {};
    return ToUtf8(masks->selected_mask_id());
  }
  [[nodiscard]] auto ActiveAdjustmentPanel() const -> QString override {
    return session_ ? session_->active_adjustment_panel() : QString{};
  }
  [[nodiscard]] auto LutPanelNodeId() const -> NodeId override {
    return session_ ? session_->lut_panel_node_id() : NodeId{};
  }
  [[nodiscard]] auto CanEdit() const -> bool override { return session_ && session_->can_edit(); }
  auto SubmitLutWrite(const EditorParameterTarget& target, EditorLutWrite write) -> bool override {
    return session_ && session_->SubmitTargetedWrite(target, std::move(write), true);
  }

 private:
  QPointer<EditorSessionController> session_;
};

}  // namespace

LutLibraryController::LutLibraryController(QObject* parent) : QObject(parent) {}

LutLibraryController::~LutLibraryController() = default;

void LutLibraryController::setEditorSession(EditorSessionController* session) {
  if (session_ == session) return;
  if (session_) {
    disconnect(session_, nullptr, this, nullptr);
    if (session_->mask_creation() != nullptr) {
      disconnect(session_->mask_creation(), nullptr, this, nullptr);
    }
  }
  session_ = session;
  session_source_.reset();
  source_ = nullptr;
  if (session_) {
    session_source_ = std::make_unique<EditorSessionLutTargetSource>(session_);
    source_         = session_source_.get();
    // DesktopUiChanged carries active adjustment panel changes, which can move the target.
    for (auto signal : {&EditorSessionController::StateChanged,
                        &EditorSessionController::AdjustmentSnapshotChanged,
                        &EditorSessionController::NodeSelectionChanged,
                        &EditorSessionController::DesktopUiChanged}) {
      connect(session_, signal, this, &LutLibraryController::reload);
    }
    if (session_->mask_creation() != nullptr) {
      connect(session_->mask_creation(), &EditorMaskCreationAdapter::maskCreationChanged, this,
              &LutLibraryController::reload);
    }
  }
  emit editorSessionChanged();
  reload();
}

void LutLibraryController::SetTargetSource(LutTargetSource* source) {
  setEditorSession(nullptr);
  source_ = source;
  reload();
}

void LutLibraryController::setLibrary(alcedo::LutLibraryService* library) {
  if (library_ == library) return;
  if (library_) disconnect(library_, nullptr, this, nullptr);
  library_ = library;
  if (library_) {
    // Availability and entry IDs follow the published inventory and root.
    connect(library_, &alcedo::LutLibraryService::InventoryChanged, this,
            &LutLibraryController::reload);
    connect(library_, &alcedo::LutLibraryService::RootChanged, this,
            &LutLibraryController::reload);
  }
  emit libraryChanged();
  reload();
}

auto LutLibraryController::targetState() const -> QString {
  switch (state_) {
    case LutTargetState::kReady:
      return QStringLiteral("ready");
    case LutTargetState::kNoImage:
      return QStringLiteral("noImage");
    case LutTargetState::kNoNode:
      return QStringLiteral("noNode");
    case LutTargetState::kNotColorGrade:
      return QStringLiteral("notColorGrade");
    case LutTargetState::kMaskSelected:
      return QStringLiteral("maskSelected");
    case LutTargetState::kNoLutAdjustment:
      return QStringLiteral("noLutAdjustment");
    case LutTargetState::kNotEditable:
      return QStringLiteral("notEditable");
  }
  return {};
}

auto LutLibraryController::targetMessage() const -> QString { return MessageForState(state_); }

auto LutLibraryController::MessageForState(LutTargetState state) -> QString {
  switch (state) {
    case LutTargetState::kReady:
      return {};
    case LutTargetState::kNoImage:
      return Tr("Open a photo to apply a LUT.");
    case LutTargetState::kNoNode:
      return Tr("Select a Color Grade node to apply a LUT.");
    case LutTargetState::kNotColorGrade:
      return Tr("LUTs apply to Color Grade nodes. Select a Color Grade node.");
    case LutTargetState::kMaskSelected:
      return Tr("A mask is selected. Select its Color Grade node to apply a LUT.");
    case LutTargetState::kNoLutAdjustment:
      return Tr("The selected Color Grade has no LUT adjustment.");
    case LutTargetState::kNotEditable:
      return Tr("The photo cannot be edited right now.");
  }
  return {};
}

auto LutLibraryController::ReadTarget() const -> TargetRead {
  TargetRead read;
  if (source_ == nullptr || source_->ImageId() == 0) return read;
  read.document = source_->Document();
  if (!read.document) return read;
  NodeId      node_id = source_->SelectedNodeId();
  const auto* node    = node_id.Empty() ? nullptr : read.document->Graph().FindNode(node_id);
  if (node != nullptr && dynamic_cast<const ColorGradeNodeModel*>(node) == nullptr &&
      PanelTargetsLutPanelNode(source_->ActiveAdjustmentPanel())) {
    node_id = source_->LutPanelNodeId();
    node    = node_id.Empty() ? nullptr : read.document->Graph().FindNode(node_id);
  }
  if (node == nullptr) {
    read.state = LutTargetState::kNoNode;
    return read;
  }
  read.node_id   = ToQString(node_id.Value());
  read.node_name = ToQString(node->DisplayName());
  if (dynamic_cast<const ColorGradeNodeModel*>(node) == nullptr) {
    read.state = LutTargetState::kNotColorGrade;
    return read;
  }
  if (!source_->SelectedMaskId().empty()) {
    read.state = LutTargetState::kMaskSelected;
    return read;
  }
  std::string error;
  read.target =
      CompleteSelectedNodeParameterTarget(*read.document, node_id, kLutFieldKey, &error);
  if (!read.target) {
    read.state = LutTargetState::kNoLutAdjustment;
    return read;
  }
  read.state = source_->CanEdit() ? LutTargetState::kReady : LutTargetState::kNotEditable;
  return read;
}

void LutLibraryController::reload() {
  const TargetRead read = ReadTarget();
  if (read.state != state_ || read.node_id != target_node_id_ ||
      read.node_name != target_node_name_) {
    state_            = read.state;
    target_node_id_   = read.node_id;
    target_node_name_ = read.node_name;
    emit targetChanged();
  }
  LoadAssociation(read);
}

void LutLibraryController::LoadAssociation(const TargetRead& read) {
  EditorPanelLutValue value;
  if (read.target) {
    EditorPanelFieldPresentation field;
    std::string                  error;
    if (ReadEditorPanelField(*read.document, *read.target, &field, &error)) {
      if (const auto* lut = std::get_if<EditorPanelLutValue>(&field.value)) value = *lut;
    }
  }
  bool missing = false;
  if (!IsEmptyLutReference(value.reference)) {
    const auto resolution = library_ ? library_->Resources()->Resolve(value.reference)
                                     : DefaultLutResourceResolver()->Resolve(value.reference);
    missing               = resolution.status == LutResourceStatus::kMissing;
  }
  const QString entry_id = EntryIdForReference(value.reference);
  QString       name     = ToQString(value.display_name);
  QString       print_name;
  if (library_ && !entry_id.isEmpty()) {
    library_->ReadEntryById(ToUtf8(entry_id), [&](const LutLibraryEntry& entry) {
      if (name.isEmpty()) name = ToQString(entry.DisplayName());
      print_name = ToQString(entry.PrintOptionName());
    });
  }
  if (name.isEmpty()) {
    if (const auto* file = std::get_if<FileLutReference>(&value.reference)) {
      name = QString::fromStdU16String(LutPathFromUtf8(file->path).stem().u16string());
    } else if (const auto* official = std::get_if<OfficialLutReference>(&value.reference)) {
      name = ToQString(official->lut_id);
    } else if (const auto* library = std::get_if<LibraryLutReference>(&value.reference)) {
      name = QString::fromStdU16String(LutPathFromUtf8(library->relative_path).stem().u16string());
    }
  }
  const QString input_encoding  = ToQString(value.input_encoding);
  const QString output_encoding = ToQString(value.output_encoding);
  if (value.reference == reference_ && entry_id == association_entry_id_ &&
      name == association_name_ && print_name == association_print_name_ &&
      static_cast<double>(value.strength) == strength_ && missing == missing_ &&
      input_encoding == input_encoding_ && output_encoding == output_encoding_) {
    return;
  }
  reference_              = std::move(value.reference);
  association_entry_id_   = entry_id;
  association_name_       = name;
  association_print_name_ = print_name;
  strength_             = static_cast<double>(value.strength);
  missing_              = missing;
  input_encoding_         = input_encoding;
  output_encoding_        = output_encoding;
  emit associationChanged();
}

auto LutLibraryController::EntryIdForReference(const LutReference& reference) const -> QString {
  if (!library_) return {};
  std::string relative;
  if (const auto* file = std::get_if<FileLutReference>(&reference)) {
    const std::optional<std::string> inside =
        library_->RelativePathInRoot(LutPathFromUtf8(file->path));
    if (!inside) return {};
    relative = *inside;
  } else if (const auto* library_reference = std::get_if<LibraryLutReference>(&reference)) {
    relative = library_reference->relative_path;
  } else if (std::holds_alternative<OfficialLutReference>(reference)) {
    return ToQString(DescribeLutReference(reference));
  } else {
    return {};
  }
  QString entry_id;
  library_->ReadEntry(relative, [&](const LutLibraryEntry& entry) {
    entry_id = ToQString(LutLibraryPublication::EntryIdOf(entry));
  });
  return entry_id;
}

auto LutLibraryController::applyEntry(const QString& entry_id) -> bool {
  if (!library_) return Reject(Tr("The LUT library is unavailable."));
  LutReference reference;
  std::string  name;
  QString      problem = Tr("The LUT is no longer in the library. Refresh the library.");
  library_->ReadEntryById(ToUtf8(entry_id), [&](const LutLibraryEntry& entry) {
    if (entry.header_error != LutHeaderError::kNone) {
      problem = Tr("The LUT file is invalid: %1").arg(ToQString(entry.header_message));
    } else if (!entry.header.SupportsGradeApplication()) {
      problem = UnsupportedLutText(entry.header);
    } else {
      reference = LutLibraryPublication::ReferenceForEntry(entry);
      name      = entry.DisplayName();
      problem.clear();
    }
  });
  if (!problem.isEmpty()) return Reject(problem);
  return SubmitReference(std::move(reference), std::move(name));
}

auto LutLibraryController::clearAssociation() -> bool { return SubmitReference({}, {}); }

auto LutLibraryController::SubmitReference(LutReference reference, std::string display_name)
    -> bool {
  // Capture the complete target now; later selection changes cannot redirect this write.
  const TargetRead read = ReadTarget();
  if (read.state != LutTargetState::kReady || !read.target) {
    reload();
    return Reject(MessageForState(read.state));
  }
  EditorLutWrite write;
  write.reference    = std::move(reference);
  write.display_name = std::move(display_name);
  if (source_ == nullptr || !source_->SubmitLutWrite(*read.target, std::move(write))) {
    return Reject(Tr("The editor did not accept the LUT change."));
  }
  SetLastError({});
  return true;
}

auto LutLibraryController::Reject(const QString& message) -> bool {
  SetLastError(message);
  emit applyRejected(message);
  return false;
}

void LutLibraryController::SetLastError(const QString& message) {
  if (last_error_ == message) return;
  last_error_ = message;
  emit lastErrorChanged();
}

}  // namespace alcedo::ui
