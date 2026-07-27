//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_models.hpp"

#include <QtQml/qqml.h>

#include <QMetaObject>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {
namespace {

auto HeadText(const alcedo::head_commit_hash_t& head) -> QString {
  return head.has_value() ? QString::fromStdString(head->ToString()) : QString{};
}

auto SameVersionIdentity(const alcedo::EditorHistoryVersion& left,
                         const alcedo::EditorHistoryVersion& right) -> bool {
  return left.version_id == right.version_id;
}

auto SameCommitIdentity(const alcedo::EditorHistoryCommit& left,
                        const alcedo::EditorHistoryCommit& right) -> bool {
  return left.commit_hash == right.commit_hash;
}

auto TimelinePositionName(alcedo::EditorHistoryTimelinePosition position) -> QString {
  switch (position) {
    case alcedo::EditorHistoryTimelinePosition::Applied:
      return QStringLiteral("applied");
    case alcedo::EditorHistoryTimelinePosition::Current:
      return QStringLiteral("current");
    case alcedo::EditorHistoryTimelinePosition::Future:
      return QStringLiteral("future");
  }
  return QStringLiteral("applied");
}

}  // namespace

EditorVersionListModel::EditorVersionListModel(QObject* parent) : QAbstractListModel(parent) {}

auto EditorVersionListModel::rowCount(const QModelIndex& parent) const -> int {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

auto EditorVersionListModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const auto& row = rows_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
    case DisplayNameRole:
      return QString::fromStdString(row.display_name);
    case VersionIdRole:
      return QString::fromStdString(row.version_id.ToString());
    case HeadCommitHashRole:
      return HeadText(row.head_commit_hash);
    case ActiveRole:
      return row.active;
    case CreatedAtRole:
      return static_cast<qlonglong>(row.created_at);
    case UpdatedAtRole:
      return static_cast<qlonglong>(row.updated_at);
    default:
      return {};
  }
}

auto EditorVersionListModel::roleNames() const -> QHash<int, QByteArray> {
  return {{VersionIdRole, "versionId"},           {DisplayNameRole, "displayName"},
          {HeadCommitHashRole, "headCommitHash"}, {ActiveRole, "active"},
          {CreatedAtRole, "createdAt"},           {UpdatedAtRole, "updatedAt"}};
}

void EditorVersionListModel::SetRows(std::vector<alcedo::EditorHistoryVersion> rows) {
  if (rows.size() == rows_.size() &&
      std::equal(rows.begin(), rows.end(), rows_.begin(), SameVersionIdentity)) {
    rows_ = std::move(rows);
    if (!rows_.empty()) {
      emit dataChanged(index(0), index(rowCount() - 1));
    }
    return;
  }

  beginResetModel();
  rows_ = std::move(rows);
  endResetModel();
  emit CountChanged();
}

EditorHistoryModel::EditorHistoryModel(QObject* parent) : QAbstractListModel(parent) {
  versions_ = new EditorVersionListModel(this);
}

EditorHistoryModel::~EditorHistoryModel() { DisconnectSession(); }

void EditorHistoryModel::DisconnectSession() {
  if (state_connection_) QObject::disconnect(state_connection_);
  if (history_connection_) QObject::disconnect(history_connection_);
  state_connection_   = {};
  history_connection_ = {};
}

void EditorHistoryModel::setEditorSession(QObject* session) {
  if (editor_session_object_ == session) return;
  DisconnectSession();
  editor_session_object_ = session;
  editor_session_        = qobject_cast<EditorSessionController*>(session);
  if (editor_session_) {
    state_connection_   = connect(editor_session_, &EditorSessionController::StateChanged, this,
                                  &EditorHistoryModel::refresh);
    history_connection_ = connect(editor_session_, &EditorSessionController::HistoryChanged, this,
                                  &EditorHistoryModel::refresh);
  }
  emit EditorSessionChanged();
  refresh();
}

auto EditorHistoryModel::rowCount(const QModelIndex& parent) const -> int {
  return parent.isValid() ? 0 : static_cast<int>(commits_.size());
}

auto EditorHistoryModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const auto  row_index = static_cast<std::size_t>(index.row());
  const auto& row       = commits_[row_index];
  const auto& pres      = presentations_[row_index];
  switch (role) {
    case Qt::DisplayRole:
    case LabelRole:
    case DisplayNameRole:
      return pres.display_name;
    case CommitIdRole:
      return QString::fromStdString(row.commit_hash.ToString());
    case FirstParentIdRole:
      return HeadText(row.first_parent_hash);
    case SecondParentIdRole:
      return row.second_parent_hash.has_value()
                 ? QString::fromStdString(row.second_parent_hash->ToString())
                 : QString{};
    case CommitKindRole:
      return row.kind == alcedo::EditCommitKind::kMerge ? QStringLiteral("merge")
                                                        : QStringLiteral("edit");
    case CreatedAtNsRole:
      return static_cast<qulonglong>(row.created_at_ns);
    case FieldKeyRole:
      return QString::fromStdString(row.field_key);
    case CurrentRole:
      return row.position == alcedo::EditorHistoryTimelinePosition::Current;
    case TimelinePositionRole:
      return TimelinePositionName(row.position);
    case BeforeTextRole:
      return pres.before_text;
    case AfterTextRole:
      return pres.after_text;
    case DeltaTextRole:
      return pres.delta_text;
    case IconKeyRole:
      return pres.icon_key;
    case IsMergeRole:
      return pres.is_merge;
    case MergeSummaryRole:
      return pres.merge_summary;
    default:
      return {};
  }
}

auto EditorHistoryModel::roleNames() const -> QHash<int, QByteArray> {
  return {{CommitIdRole, "commitId"},
          {FirstParentIdRole, "firstParentId"},
          {SecondParentIdRole, "secondParentId"},
          {CommitKindRole, "commitKind"},
          {CreatedAtNsRole, "createdAtNs"},
          {LabelRole, "label"},
          {FieldKeyRole, "fieldKey"},
          {CurrentRole, "current"},
          {DisplayNameRole, "displayName"},
          {BeforeTextRole, "beforeText"},
          {AfterTextRole, "afterText"},
          {DeltaTextRole, "deltaText"},
          {IconKeyRole, "iconKey"},
          {TimelinePositionRole, "timelinePosition"},
          {IsMergeRole, "isMerge"},
          {MergeSummaryRole, "mergeSummary"}};
}

void EditorHistoryModel::SetSnapshot(alcedo::EditorHistorySnapshot snapshot) {
  const bool    has_versions = !snapshot.versions.empty();
  const QString active_version_id =
      has_versions ? QString::fromStdString(snapshot.active_version_id.ToString()) : QString{};
  if (snapshot.commits.size() == commits_.size() &&
      std::equal(snapshot.commits.begin(), snapshot.commits.end(), commits_.begin(),
                 SameCommitIdentity)) {
    commits_ = std::move(snapshot.commits);
    RebuildPresentations();
    if (!commits_.empty()) {
      emit dataChanged(index(0), index(rowCount() - 1));
    }
  } else {
    beginResetModel();
    commits_ = std::move(snapshot.commits);
    RebuildPresentations();
    endResetModel();
  }
  versions_->SetRows(std::move(snapshot.versions));
  can_undo_          = snapshot.can_undo;
  can_redo_          = snapshot.can_redo;
  recovered_head_    = snapshot.recovered_head;
  active_version_id_ = active_version_id;
  emit StateChanged();
}

void EditorHistoryModel::RebuildPresentations() {
  presentations_.clear();
  presentations_.reserve(commits_.size());
  for (const auto& commit : commits_) {
    presentations_.push_back(PresentEditorHistoryCommit(
        commit.field_key, commit.before_value_json, commit.after_value_json,
        commit.before_enabled, commit.after_enabled, commit.kind, commit.merge_field_keys));
  }
}

void EditorHistoryModel::refresh() {
  if (!editor_session_) {
    SetSnapshot({});
    return;
  }
  SetSnapshot(editor_session_->history_snapshot());
}

void EditorHistoryModel::undo() {
  if (editor_session_) editor_session_->Undo();
  refresh();
}

void EditorHistoryModel::redo() {
  if (editor_session_) editor_session_->Redo();
  refresh();
}

void EditorHistoryModel::moveHeadToCommit(const QString& commitId) {
  if (editor_session_) editor_session_->MoveHeadToCommit(commitId);
  refresh();
}

void EditorHistoryModel::checkoutVersion(const QString& versionId) {
  if (editor_session_) editor_session_->CheckoutVersion(versionId);
  refresh();
}

void EditorHistoryModel::createRootVersion(const QString& displayName) {
  if (editor_session_) editor_session_->CreateRootVersion(displayName);
  refresh();
}

void EditorHistoryModel::branchFromCommit(const QString& commitId, const QString& displayName) {
  if (editor_session_) editor_session_->BranchFromCommit(commitId, displayName);
  refresh();
}

void EditorHistoryModel::renameVersion(const QString& versionId, const QString& displayName) {
  if (editor_session_) editor_session_->RenameVersion(versionId, displayName);
  refresh();
}

void EditorHistoryModel::removeVersion(const QString& versionId) {
  if (editor_session_) editor_session_->RemoveVersion(versionId);
  refresh();
}

void RegisterEditorHistoryQmlTypes() {
  qmlRegisterType<EditorHistoryModel>("Alcedo.Main", 1, 0, "EditorHistoryModel");
  qmlRegisterType<EditorVersionListModel>("Alcedo.Main", 1, 0, "EditorVersionListModel");
}

}  // namespace alcedo::ui
