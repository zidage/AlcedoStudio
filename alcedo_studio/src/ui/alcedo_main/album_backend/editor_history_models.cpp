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
  // Same Version IDs in the same order: only per-row data (display name, head,
  // active flag, updated_at) may have changed. Emit targeted dataChanged for
  // the affected rows instead of repainting the whole list so checkout/rename
  // keep the Versions scroll position stable.
  if (rows.size() == rows_.size() &&
      std::equal(rows.begin(), rows.end(), rows_.begin(), SameVersionIdentity)) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& next  = rows[i];
      const auto& prev  = rows_[i];
      const bool changed = prev.display_name != next.display_name ||
                           prev.head_commit_hash != next.head_commit_hash ||
                           prev.active != next.active || prev.updated_at != next.updated_at;
      rows_[i] = std::move(next);
      if (changed) {
        const auto row = static_cast<int>(i);
        emit dataChanged(index(row), index(row));
      }
    }
    return;
  }

  // Structural change (added/removed/reordered Version ref): reset.
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
  if (history_connection_) QObject::disconnect(history_connection_);
  history_connection_ = {};
}

void EditorHistoryModel::setEditorSession(QObject* session) {
  if (editor_session_object_ == session) return;
  DisconnectSession();
  editor_session_object_ = session;
  editor_session_        = qobject_cast<EditorSessionController*>(session);
  if (editor_session_) {
    // Phase 7A R2: project only on the dedicated history signal (driven by the
    // backend's monotonic history_revision), never on the broad StateChanged
    // that fires for every render/preview/task notification.
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
  const QString active_version_id = QString::fromStdString(snapshot.active_version_id.ToString());
  ApplyCommits(std::move(snapshot.commits));
  versions_->SetRows(std::move(snapshot.versions));
  can_undo_          = snapshot.can_undo;
  can_redo_          = snapshot.can_redo;
  recovered_head_    = snapshot.recovered_head;
  active_version_id_ = active_version_id;
  emit StateChanged();
}

auto EditorHistoryModel::PresentationFor(const alcedo::EditorHistoryCommit& commit)
    -> EditorHistoryCommitPresentation {
  if (const auto it = presentation_cache_.find(commit.commit_hash);
      it != presentation_cache_.end()) {
    return it->second;
  }
  auto pres = PresentEditorHistoryCommit(
      commit.field_key, commit.before_value_json, commit.after_value_json,
      commit.before_enabled, commit.after_enabled, commit.kind, commit.merge_field_keys);
  presentation_cache_.emplace(commit.commit_hash, pres);
  return pres;
}

void EditorHistoryModel::ApplyCommits(std::vector<alcedo::EditorHistoryCommit> commits) {
  // Same commit identities in the same order: only timeline positions may have
  // changed (a head move within the visible set). Refresh the cached rows and
  // emit targeted dataChanged for the rows whose position changed instead of
  // repainting the whole list. Scroll position stays stable on data-only moves.
  if (commits.size() == commits_.size() &&
      std::equal(commits.begin(), commits.end(), commits_.begin(), SameCommitIdentity)) {
    for (std::size_t i = 0; i < commits.size(); ++i) {
      const auto old_position = commits_[i].position;
      commits_[i]        = commits[i];
      presentations_[i]  = PresentationFor(commits_[i]);
      if (commits_[i].position != old_position) {
        const auto row = static_cast<int>(i);
        emit dataChanged(index(row), index(row));
      }
    }
    return;
  }

  // Append path: the existing rows are a prefix of the new list. A settled edit
  // advances the head: the prior current row becomes applied and one new
  // current row is appended. Update the prior current row in place, then insert
  // the new tail instead of resetting the list.
  if (!commits_.empty() && commits.size() > commits_.size() &&
      std::equal(commits_.begin(), commits_.end(), commits.begin(), SameCommitIdentity)) {
    const auto prior_current = static_cast<std::size_t>(commits_.size() - 1);
    const auto old_position  = commits_[prior_current].position;
    commits_[prior_current]       = commits[prior_current];
    presentations_[prior_current] = PresentationFor(commits_[prior_current]);
    if (commits_[prior_current].position != old_position) {
      emit dataChanged(index(static_cast<int>(prior_current)),
                       index(static_cast<int>(prior_current)));
    }
    const auto first_new = static_cast<int>(commits_.size());
    const auto last_new  = static_cast<int>(commits.size() - 1);
    beginInsertRows({}, first_new, last_new);
    for (auto i = commits_.size(); i < commits.size(); ++i) {
      presentations_.push_back(PresentationFor(commits[i]));
      commits_.push_back(std::move(commits[i]));
    }
    endInsertRows();
    return;
  }

  // Structural change (checkout, branch, redo-suffix reorder, first projection):
  // reset the list and rebind presentations through the hash cache so unchanged
  // commits reuse their cached presentation.
  beginResetModel();
  commits_.clear();
  presentations_.clear();
  for (auto& commit : commits) {
    presentations_.push_back(PresentationFor(commit));
    commits_.push_back(std::move(commit));
  }
  endResetModel();
}

void EditorHistoryModel::refresh() {
  if (!editor_session_) {
    SetSnapshot({});
    return;
  }
  SetSnapshot(editor_session_->history_snapshot());
}

void EditorHistoryModel::undo() {
  // The backend bumps history_revision on a successful head move; the
  // dedicated HistoryChanged signal drives exactly one projection. No direct
  // refresh here avoids the triple-projection defect (StateChanged +
  // HistoryChanged + direct refresh).
  if (editor_session_) editor_session_->Undo();
}

void EditorHistoryModel::redo() {
  if (editor_session_) editor_session_->Redo();
}

void EditorHistoryModel::moveHeadToCommit(const QString& commitId) {
  if (editor_session_) editor_session_->MoveHeadToCommit(commitId);
}

void EditorHistoryModel::checkoutVersion(const QString& versionId) {
  if (editor_session_) editor_session_->CheckoutVersion(versionId);
}

void EditorHistoryModel::createRootVersion(const QString& displayName) {
  if (editor_session_) editor_session_->CreateRootVersion(displayName);
}

void EditorHistoryModel::branchFromCommit(const QString& commitId, const QString& displayName) {
  if (editor_session_) editor_session_->BranchFromCommit(commitId, displayName);
}

void EditorHistoryModel::renameVersion(const QString& versionId, const QString& displayName) {
  if (editor_session_) editor_session_->RenameVersion(versionId, displayName);
}

void EditorHistoryModel::removeVersion(const QString& versionId) {
  if (editor_session_) editor_session_->RemoveVersion(versionId);
}


void RegisterEditorHistoryQmlTypes() {
  qmlRegisterType<EditorHistoryModel>("Alcedo.Main", 1, 0, "EditorHistoryModel");
  qmlRegisterType<EditorVersionListModel>("Alcedo.Main", 1, 0, "EditorVersionListModel");
}

}  // namespace alcedo::ui
