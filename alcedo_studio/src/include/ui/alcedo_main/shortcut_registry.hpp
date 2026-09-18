//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <map>

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

class QAction;

namespace alcedo::ui {

using ShortcutCommandId = QString;

namespace shortcut_id {
inline constexpr const char* kNodesAddColorGrade     = "nodes.addColorGrade";
inline constexpr const char* kNodesFitGraph          = "nodes.fitGraph";
inline constexpr const char* kNodesRenameColorGrade  = "nodes.renameColorGrade";
inline constexpr const char* kNodesDeleteColorGrade  = "nodes.deleteColorGrade";
inline constexpr const char* kNodesBeginConnect      = "nodes.beginConnect";
inline constexpr const char* kNodesCompleteConnect   = "nodes.completeConnect";
inline constexpr const char* kNodesSelectPrevious    = "nodes.selectPrevious";
inline constexpr const char* kNodesSelectNext        = "nodes.selectNext";
inline constexpr const char* kNodesSelectDevelop     = "nodes.selectDevelop";
inline constexpr const char* kNodesSelectDrt         = "nodes.selectDrt";
inline constexpr const char* kNodesCancel            = "nodes.cancel";
}  // namespace shortcut_id

struct ShortcutBindingSpec {
  ShortcutCommandId     id;
  QString               description;
  QKeySequence          default_sequence;
  QList<QKeySequence>   extra_sequences{};
  Qt::ShortcutContext   context = Qt::WidgetWithChildrenShortcut;
  std::function<bool()> enabled_when{};
  std::function<void()> on_trigger{};
};

/**
 * @brief Named command catalog for editor keyboard bindings.
 *
 * Owns command ids, default sequences, and tooltip decoration. The Qt Quick
 * shell is a QQuickWindow, so this registry does not install QWidget actions as
 * the live input path. Focus-scoped surfaces (the Nodes graph) match a key
 * event to a command id and invoke the panel action. Optional @c on_trigger
 * remains for C++ callers.
 *
 * Threading: GUI thread. Does not own editor session or graph state.
 */
class ShortcutRegistry final : public QObject {
  Q_OBJECT

 public:
  explicit ShortcutRegistry(QObject* parent = nullptr);

  auto Register(ShortcutBindingSpec spec) -> QAction*;
  auto Action(const ShortcutCommandId& id) const -> QAction*;
  auto ShortcutText(const ShortcutCommandId&     id,
                    QKeySequence::SequenceFormat format = QKeySequence::NativeText) const
      -> QString;
  auto DecorateTooltip(const QString&           base_tooltip,
                       const ShortcutCommandId& id) const -> QString;
  void RefreshEnabledStates();

  Q_INVOKABLE QString shortcutText(const QString& id) const;
  Q_INVOKABLE QString decorateTooltip(const QString& base_tooltip, const QString& id) const;
  Q_INVOKABLE QString commandIdForKey(int key, int modifiers) const;

 private:
  struct Entry {
    ShortcutBindingSpec spec;
    QAction*            action = nullptr;
  };

  std::map<ShortcutCommandId, Entry> entries_{};
};

void RegisterNodesPanelShortcuts(ShortcutRegistry* registry);
void RegisterShortcutRegistryQmlType();

}  // namespace alcedo::ui
