//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file shortcut_registration.cpp
/// @brief Built-in shortcut command table: ids, labels, scopes, defaults.

#include <QCoreApplication>
#include <QKeyCombination>

#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui {
namespace {

auto Chord(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) -> ShortcutInput {
  return ShortcutInput{ShortcutInputKind::KeyChord, key, modifiers};
}

auto Modifier(Qt::KeyboardModifier modifier) -> ShortcutInput {
  return ShortcutInput{ShortcutInputKind::Modifier, Qt::Key_unknown,
                       Qt::KeyboardModifiers{modifier}};
}

void AddSpec(ShortcutRegistry* registry, const char* id, const char* group, const char* description,
             QList<ShortcutInput> defaults, const char* scope, ShortcutInputKind kind,
             bool auto_repeat) {
  ShortcutBindingSpec spec;
  spec.id               = QString::fromLatin1(id);
  spec.group            = QCoreApplication::translate("ShortcutRegistry", group);
  spec.description      = QCoreApplication::translate("ShortcutRegistry", description);
  spec.default_bindings = std::move(defaults);
  spec.scope            = QString::fromLatin1(scope);
  spec.input_kind       = kind;
  spec.auto_repeat      = auto_repeat;
  registry->Register(std::move(spec));
}

}  // namespace

void RegisterLibraryShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults,
                        ShortcutInputKind kind = ShortcutInputKind::KeyChord) {
    AddSpec(registry, id, "Library", description, std::move(defaults),
            shortcut_scope::kWorkspaceLibrary, kind, false);
  };
  add(shortcut_id::kLibrarySelectAll, "Select all", {Chord(Qt::Key_A, Qt::ControlModifier)});
  add(shortcut_id::kLibraryExtendSelection, "Extend selection", {Modifier(Qt::ShiftModifier)},
      ShortcutInputKind::Modifier);
  add(shortcut_id::kLibraryToggleSelection, "Toggle selection", {Modifier(Qt::ControlModifier)},
      ShortcutInputKind::Modifier);
  add(shortcut_id::kLibrarySaveProject, "Save project", {Chord(Qt::Key_S, Qt::ControlModifier)});
}

void RegisterEditorShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults) {
    AddSpec(registry, id, "Editor", description, std::move(defaults),
            shortcut_scope::kWorkspaceEditor, ShortcutInputKind::KeyChord, false);
  };
  add(shortcut_id::kEditorUndo, "Undo", {Chord(Qt::Key_Z, Qt::ControlModifier)});
  add(shortcut_id::kEditorRedo, "Redo", {Chord(Qt::Key_R, Qt::ControlModifier)});
  add(shortcut_id::kEditorSaveCurrentImage, "Save current image",
      {Chord(Qt::Key_S, Qt::ControlModifier)});
}

void RegisterFilmstripShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults,
                        bool auto_repeat) {
    AddSpec(registry, id, "Filmstrip", description, std::move(defaults),
            shortcut_scope::kEditorFilmstrip, ShortcutInputKind::KeyChord, auto_repeat);
  };
  add(shortcut_id::kFilmstripPreviousImage, "Previous image", {Chord(Qt::Key_Left)}, true);
  add(shortcut_id::kFilmstripNextImage, "Next image", {Chord(Qt::Key_Right)}, true);
  add(shortcut_id::kFilmstripSelectAll, "Select all filmstrip images",
      {Chord(Qt::Key_A, Qt::ControlModifier)}, false);
}

void RegisterVersionsShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  AddSpec(registry, shortcut_id::kVersionsCreateDefaultFromRoot, "Versions",
          "Create default version from root", {Chord(Qt::Key_A, Qt::ControlModifier)},
          shortcut_scope::kEditorVersions, ShortcutInputKind::KeyChord, false);
}

void RegisterLutShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults) {
    AddSpec(registry, id, "LUT", description, std::move(defaults), shortcut_scope::kEditorLut,
            ShortcutInputKind::KeyChord, true);
  };
  add(shortcut_id::kLutSelectPrevious, "Select previous LUT", {Chord(Qt::Key_Up)});
  add(shortcut_id::kLutSelectNext, "Select next LUT", {Chord(Qt::Key_Down)});
}

void RegisterMaskShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults) {
    AddSpec(registry, id, "Masks", description, std::move(defaults),
            shortcut_scope::kEditorMaskEdit, ShortcutInputKind::KeyChord, false);
  };
  add(shortcut_id::kMaskFinishEdit, "Finish mask edit", {Chord(Qt::Key_Escape)});
  add(shortcut_id::kMaskDeleteSelection, "Delete selected mask item", {Chord(Qt::Key_Delete)});
  add(shortcut_id::kMaskConfirmEdit, "Confirm mask edit",
      {Chord(Qt::Key_Return), Chord(Qt::Key_Enter)});
}

void RegisterNodesPanelShortcuts(ShortcutRegistry* registry) {
  if (registry == nullptr) {
    return;
  }
  auto add = [registry](const char* id, const char* description, QList<ShortcutInput> defaults,
                        ShortcutInputKind kind        = ShortcutInputKind::KeyChord,
                        bool              auto_repeat = false) {
    AddSpec(registry, id, "Nodes", description, std::move(defaults), shortcut_scope::kEditorNodes,
            kind, auto_repeat);
  };
  add(shortcut_id::kNodesAddColorGrade, "Add Color Grade",
      {Chord(Qt::Key_Plus, Qt::ControlModifier), Chord(Qt::Key_Equal, Qt::ControlModifier),
       Chord(Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier),
       Chord(Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier)});
  add(shortcut_id::kNodesFitGraph, "Fit", {Chord(Qt::Key_0, Qt::ControlModifier)});
  add(shortcut_id::kNodesRenameColorGrade, "Rename Color Grade", {Chord(Qt::Key_F2)});
  add(shortcut_id::kNodesDeleteSelection, "Delete selected nodes", {Chord(Qt::Key_Delete)});
  add(shortcut_id::kNodesBeginConnect, "Connect", {Chord(Qt::Key_C)});
  add(shortcut_id::kNodesCompleteConnect, "Complete Connect",
      {Chord(Qt::Key_Return), Chord(Qt::Key_Enter)});
  add(shortcut_id::kNodesSelectPrevious, "Select previous node", {Chord(Qt::Key_Up)},
      ShortcutInputKind::KeyChord, true);
  add(shortcut_id::kNodesSelectNext, "Select next node", {Chord(Qt::Key_Down)},
      ShortcutInputKind::KeyChord, true);
  add(shortcut_id::kNodesSelectDevelop, "Select Develop", {Chord(Qt::Key_Home)});
  add(shortcut_id::kNodesSelectDrt, "Select DRT/Post", {Chord(Qt::Key_End)});
  add(shortcut_id::kNodesCancel, "Cancel", {Chord(Qt::Key_Escape)});
  add(shortcut_id::kNodesExtendSelection, "Extend node selection", {Modifier(Qt::ShiftModifier)},
      ShortcutInputKind::Modifier);
}

void RegisterBuiltinShortcuts(ShortcutRegistry* registry) {
  RegisterLibraryShortcuts(registry);
  RegisterEditorShortcuts(registry);
  RegisterFilmstripShortcuts(registry);
  RegisterVersionsShortcuts(registry);
  RegisterLutShortcuts(registry);
  RegisterMaskShortcuts(registry);
  RegisterNodesPanelShortcuts(registry);
}

}  // namespace alcedo::ui
