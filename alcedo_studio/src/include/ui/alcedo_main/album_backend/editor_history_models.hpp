//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QVariant>
#include <vector>

#include "app/editor_history_types.hpp"

namespace alcedo::ui {

class EditorSessionController;

/// Typed list model for named Version refs. Rows carry stable IDs rather than
/// list positions so checkout and rename survive graph reordering.
class EditorVersionListModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY CountChanged)

 public:
  enum Role {
    VersionIdRole = Qt::UserRole + 1,
    DisplayNameRole,
    HeadCommitHashRole,
    ActiveRole,
    CreatedAtRole,
    UpdatedAtRole,
  };

  explicit EditorVersionListModel(QObject* parent = nullptr);

  [[nodiscard]] auto count() const -> int { return static_cast<int>(rows_.size()); }
  [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role = Qt::DisplayRole) const
      -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;

  void               SetRows(std::vector<alcedo::EditorHistoryVersion> rows);
  [[nodiscard]] auto Rows() const -> const std::vector<alcedo::EditorHistoryVersion>& {
    return rows_;
  }

 signals:
  void CountChanged();

 private:
  std::vector<alcedo::EditorHistoryVersion> rows_;
};

/// QML-facing projection of the active Version's immutable first-parent
/// history and the named Version refs.
class EditorHistoryModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(
      QObject* editorSession READ editorSession WRITE setEditorSession NOTIFY EditorSessionChanged)
  Q_PROPERTY(EditorVersionListModel* versions READ versions CONSTANT)
  Q_PROPERTY(int count READ count NOTIFY StateChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY StateChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY StateChanged)
  Q_PROPERTY(bool recoveredHead READ recoveredHead NOTIFY StateChanged)
  Q_PROPERTY(QString activeVersionId READ activeVersionId NOTIFY StateChanged)

 public:
  enum Role {
    CommitIdRole = Qt::UserRole + 1,
    FirstParentIdRole,
    SecondParentIdRole,
    CommitKindRole,
    CreatedAtNsRole,
    LabelRole,
    FieldKeyRole,
    CurrentRole,
  };

  explicit EditorHistoryModel(QObject* parent = nullptr);
  ~EditorHistoryModel() override;

  [[nodiscard]] auto editorSession() const -> QObject* { return editor_session_object_; }
  void               setEditorSession(QObject* session);
  [[nodiscard]] auto versions() const -> EditorVersionListModel* { return versions_; }
  [[nodiscard]] auto count() const -> int { return static_cast<int>(commits_.size()); }
  [[nodiscard]] auto canUndo() const -> bool { return can_undo_; }
  [[nodiscard]] auto canRedo() const -> bool { return can_redo_; }
  [[nodiscard]] auto recoveredHead() const -> bool { return recovered_head_; }
  [[nodiscard]] auto activeVersionId() const -> QString { return active_version_id_; }

  [[nodiscard]] auto rowCount(const QModelIndex& parent = {}) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role = Qt::DisplayRole) const
      -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;

  Q_INVOKABLE void   refresh();
  Q_INVOKABLE void   undo();
  Q_INVOKABLE void   redo();
  Q_INVOKABLE void   checkoutVersion(const QString& versionId);
  Q_INVOKABLE void   createRootVersion(const QString& displayName);
  Q_INVOKABLE void   branchFromCommit(const QString& commitId, const QString& displayName);
  Q_INVOKABLE void   renameVersion(const QString& versionId, const QString& displayName);
  Q_INVOKABLE void   removeVersion(const QString& versionId);

 signals:
  void EditorSessionChanged();
  void StateChanged();

 private:
  void                                     SetSnapshot(alcedo::EditorHistorySnapshot snapshot);
  void                                     DisconnectSession();

  QObject*                                 editor_session_object_ = nullptr;
  EditorSessionController*                 editor_session_        = nullptr;
  QMetaObject::Connection                  state_connection_;
  QMetaObject::Connection                  history_connection_;
  EditorVersionListModel*                  versions_ = nullptr;
  std::vector<alcedo::EditorHistoryCommit> commits_;
  bool                                     can_undo_       = false;
  bool                                     can_redo_       = false;
  bool                                     recovered_head_ = false;
  QString                                  active_version_id_;
};

void RegisterEditorHistoryQmlTypes();

}  // namespace alcedo::ui
