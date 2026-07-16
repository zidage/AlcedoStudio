//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>

namespace alcedo::ui {

class EditorController;

/// Owns the editor-session boundary exposed to workspace routing. The legacy
/// editor implementation remains behind this narrow module until the QML
/// editor surface is migrated.
class EditorSessionController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY StateChanged)
  Q_PROPERTY(uint elementId READ element_id NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ image_id NOTIFY StateChanged)

 public:
  explicit EditorSessionController(EditorController* editor, QObject* parent = nullptr);

  [[nodiscard]] bool active() const;
  [[nodiscard]] uint element_id() const;
  [[nodiscard]] uint image_id() const;

  Q_INVOKABLE void Open(uint elementId = 0, uint imageId = 0);
  Q_INVOKABLE void Close();
  void Finalize(bool persistChanges);

 signals:
  void StateChanged();

 private:
  EditorController* editor_ = nullptr;
};

}  // namespace alcedo::ui
