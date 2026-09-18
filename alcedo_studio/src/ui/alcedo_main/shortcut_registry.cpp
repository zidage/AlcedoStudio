//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/shortcut_registry.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QKeyCombination>
#include <QQmlEngine>
#include <qqml.h>

namespace alcedo::ui {
namespace {

auto Combination(int key, int modifiers) -> QKeyCombination {
  const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers) &
                    (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
  return QKeyCombination(mods, static_cast<Qt::Key>(key));
}

auto SequenceList(const ShortcutBindingSpec& spec) -> QList<QKeySequence> {
  QList<QKeySequence> sequences;
  if (!spec.default_sequence.isEmpty()) {
    sequences.push_back(spec.default_sequence);
  }
  sequences.append(spec.extra_sequences);
  return sequences;
}

auto Sequence(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) -> QKeySequence {
  return QKeySequence(QKeyCombination(modifiers, key));
}

}  // namespace

ShortcutRegistry::ShortcutRegistry(QObject* parent) : QObject(parent) {}

auto ShortcutRegistry::Register(ShortcutBindingSpec spec) -> QAction* {
  if (spec.id.isEmpty()) {
    return nullptr;
  }

  if (const auto it = entries_.find(spec.id); it != entries_.end()) {
    return it->second.action;
  }

  QAction* action = nullptr;
  if (spec.on_trigger) {
    action = new QAction(this);
    action->setObjectName(spec.id);
    action->setShortcuts(SequenceList(spec));
    action->setAutoRepeat(true);
    action->setShortcutContext(spec.context);
    if (!spec.description.isEmpty()) {
      action->setText(spec.description);
      action->setToolTip(spec.description);
      action->setStatusTip(spec.description);
    }
  }

  const ShortcutCommandId id = spec.id;
  entries_.emplace(id, Entry{.spec = std::move(spec), .action = action});
  if (action != nullptr) {
    QObject::connect(action, &QAction::triggered, this, [this, id](bool) {
      const auto it = entries_.find(id);
      if (it == entries_.end()) {
        return;
      }

      auto& entry = it->second;
      if (entry.spec.enabled_when) {
        const bool enabled = entry.spec.enabled_when();
        entry.action->setEnabled(enabled);
        if (!enabled) {
          return;
        }
      }

      entry.spec.on_trigger();
    });
  }

  RefreshEnabledStates();
  return action;
}

auto ShortcutRegistry::Action(const ShortcutCommandId& id) const -> QAction* {
  if (const auto it = entries_.find(id); it != entries_.end()) {
    return it->second.action;
  }
  return nullptr;
}

auto ShortcutRegistry::ShortcutText(const ShortcutCommandId&     id,
                                    QKeySequence::SequenceFormat format) const -> QString {
  const auto it = entries_.find(id);
  if (it == entries_.end()) {
    return {};
  }
  return it->second.spec.default_sequence.toString(format);
}

auto ShortcutRegistry::DecorateTooltip(const QString&           base_tooltip,
                                       const ShortcutCommandId& id) const -> QString {
  const QString shortcut_text = ShortcutText(id);
  if (shortcut_text.isEmpty()) {
    return base_tooltip;
  }
  if (base_tooltip.isEmpty()) {
    return shortcut_text;
  }
  return QStringLiteral("%1 (%2)").arg(base_tooltip, shortcut_text);
}

void ShortcutRegistry::RefreshEnabledStates() {
  for (auto& [id, entry] : entries_) {
    Q_UNUSED(id);
    if (entry.action == nullptr) {
      continue;
    }
    const bool enabled = !entry.spec.enabled_when || entry.spec.enabled_when();
    entry.action->setEnabled(enabled);
  }
}

QString ShortcutRegistry::shortcutText(const QString& id) const { return ShortcutText(id); }

QString ShortcutRegistry::decorateTooltip(const QString& base_tooltip, const QString& id) const {
  return DecorateTooltip(base_tooltip, id);
}

QString ShortcutRegistry::commandIdForKey(int key, int modifiers) const {
  const auto pressed = Combination(key, modifiers);
  for (const auto& [id, entry] : entries_) {
    for (const auto& sequence : SequenceList(entry.spec)) {
      if (sequence.count() == 1 && sequence[0] == pressed) {
        return id;
      }
    }
  }
  return {};
}

void RegisterNodesPanelShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }

  auto add = [registry](const char* id, const char* description, QKeySequence sequence,
                        QList<QKeySequence> extra = {}) {
    ShortcutBindingSpec spec;
    spec.id                = QString::fromLatin1(id);
    spec.description       = QCoreApplication::translate("ShortcutRegistry", description);
    spec.default_sequence  = std::move(sequence);
    spec.extra_sequences   = std::move(extra);
    registry->Register(std::move(spec));
  };

  add(shortcut_id::kNodesAddColorGrade, "Add Color Grade",
      Sequence(Qt::Key_Plus, Qt::ControlModifier),
      {Sequence(Qt::Key_Equal, Qt::ControlModifier),
       Sequence(Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier),
       Sequence(Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier)});
  add(shortcut_id::kNodesFitGraph, "Fit", Sequence(Qt::Key_0, Qt::ControlModifier));
  add(shortcut_id::kNodesRenameColorGrade, "Rename Color Grade", Sequence(Qt::Key_F2));
  add(shortcut_id::kNodesDeleteColorGrade, "Delete Color Grade", Sequence(Qt::Key_Delete));
  add(shortcut_id::kNodesBeginConnect, "Connect", Sequence(Qt::Key_C));
  add(shortcut_id::kNodesCompleteConnect, "Complete Connect", Sequence(Qt::Key_Return),
      {Sequence(Qt::Key_Enter)});
  add(shortcut_id::kNodesSelectPrevious, "Select previous node", Sequence(Qt::Key_Up));
  add(shortcut_id::kNodesSelectNext, "Select next node", Sequence(Qt::Key_Down));
  add(shortcut_id::kNodesSelectDevelop, "Select Develop", Sequence(Qt::Key_Home));
  add(shortcut_id::kNodesSelectDrt, "Select DRT/Post", Sequence(Qt::Key_End));
  add(shortcut_id::kNodesCancel, "Cancel", Sequence(Qt::Key_Escape));
}

void RegisterShortcutRegistryQmlType() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  qmlRegisterSingletonType<ShortcutRegistry>(
      "Alcedo.Main", 1, 0, "ShortcutRegistry",
      [](QQmlEngine* engine, QJSEngine*) -> QObject* {
        auto* registry = new ShortcutRegistry(engine);
        RegisterNodesPanelShortcuts(registry);
        return registry;
      });
}

}  // namespace alcedo::ui
