//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>

namespace alcedo::ui {

class EditorSessionController;

/// Application route state. Layout remains in QML; this object only owns the
/// selected workspace and the editor route arguments.
class WorkspaceRouter final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString workspace READ workspace NOTIFY RouteChanged)
  Q_PROPERTY(uint elementId READ element_id NOTIFY RouteChanged)
  Q_PROPERTY(uint imageId READ image_id NOTIFY RouteChanged)

 public:
  explicit WorkspaceRouter(EditorSessionController* editor_session, QObject* parent = nullptr);

  [[nodiscard]] QString workspace() const { return workspace_; }
  [[nodiscard]] uint    element_id() const { return element_id_; }
  [[nodiscard]] uint    image_id() const { return image_id_; }

  Q_INVOKABLE void OpenLibrary();
  Q_INVOKABLE void OpenEditor(uint elementId = 0, uint imageId = 0);
  Q_INVOKABLE void openLibrary() { OpenLibrary(); }
  Q_INVOKABLE void openEditor(uint elementId = 0, uint imageId = 0) {
    OpenEditor(elementId, imageId);
  }

 signals:
  void RouteChanged();

 private:
  EditorSessionController* editor_session_ = nullptr;
  QString                  workspace_ = QStringLiteral("library");
  uint                     element_id_ = 0;
  uint                     image_id_ = 0;
};

}  // namespace alcedo::ui
