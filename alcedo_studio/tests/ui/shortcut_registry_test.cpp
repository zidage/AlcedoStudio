//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/shortcut_registry.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <memory>

namespace alcedo::ui {
namespace {

constexpr int kKeyChord = static_cast<int>(ShortcutInputKind::KeyChord);
constexpr int kModifier = static_cast<int>(ShortcutInputKind::Modifier);

class ShortcutRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(settings_dir_.isValid()); }

  auto SettingsPath(const QString& file_name) const -> QString {
    return settings_dir_.filePath(file_name);
  }

  /// Registry backed by an isolated INI file so runs never touch the
  /// developer's real application settings.
  auto MakeRegistry(const QString& file_name = QStringLiteral("shortcuts.ini"))
      -> std::unique_ptr<ShortcutRegistry> {
    auto settings = std::make_unique<QSettings>(SettingsPath(file_name), QSettings::IniFormat);
    auto registry = std::make_unique<ShortcutRegistry>(std::move(settings));
    RegisterBuiltinShortcuts(registry.get());
    return registry;
  }

  /// Writes a raw override group the way the persistence schema stores it.
  void WriteSavedOverride(const QString& file_name, const QString& command_id, const QString& state,
                          const QStringList& bindings = {}) {
    QSettings settings(SettingsPath(file_name), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("keyboardShortcuts/v1/") + command_id);
    settings.setValue(QStringLiteral("state"), state);
    if (!bindings.isEmpty()) {
      settings.setValue(QStringLiteral("bindings"), bindings);
    }
    settings.endGroup();
    settings.sync();
  }

  static auto RoleValue(const ShortcutRegistry& registry, const QString& command_id, int role)
      -> QVariant {
    const int row = registry.RowForCommand(command_id);
    if (row < 0) {
      return {};
    }
    return registry.data(registry.index(row, 0), role);
  }

  static auto Assigned(const ShortcutRegistry& registry, const QString& id) -> bool {
    return RoleValue(registry, id, ShortcutRegistry::AssignedRole).toBool();
  }

  static auto RowError(const ShortcutRegistry& registry, const QString& id) -> QString {
    return RoleValue(registry, id, ShortcutRegistry::ValidationErrorRole).toString();
  }

  QTemporaryDir settings_dir_;
};

TEST_F(ShortcutRegistryTest, NodesCommandsMatchTheDocumentedGraphKeys) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString nodes = QLatin1String(shortcut_scope::kEditorNodes);

  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Plus, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Equal, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(
      registry->commandIdForKey(nodes, Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier),
      QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_0, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesFitGraph));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_F2, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesRenameColorGrade));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Delete, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesDeleteSelection));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_C, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesBeginConnect));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_C, Qt::ControlModifier), QString());
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Return, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCompleteConnect));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Enter, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCompleteConnect));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Up, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectPrevious));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Down, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectNext));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Home, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectDevelop));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_End, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectDrt));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Escape, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCancel));
}

TEST_F(ShortcutRegistryTest, RegisteredDefaultsResolveOnlyInsideTheirDeclaredScopes) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  const QString library   = QLatin1String(shortcut_scope::kWorkspaceLibrary);
  const QString editor    = QLatin1String(shortcut_scope::kWorkspaceEditor);
  const QString filmstrip = QLatin1String(shortcut_scope::kEditorFilmstrip);
  const QString versions  = QLatin1String(shortcut_scope::kEditorVersions);
  const QString lut       = QLatin1String(shortcut_scope::kEditorLut);
  const QString mask      = QLatin1String(shortcut_scope::kEditorMaskEdit);
  const QString nodes     = QLatin1String(shortcut_scope::kEditorNodes);

  EXPECT_EQ(registry->commandIdForKey(library, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kLibrarySelectAll));
  EXPECT_EQ(registry->commandIdForKey(library, Qt::Key_S, Qt::ControlModifier),
            QLatin1String(shortcut_id::kLibrarySaveProject));
  EXPECT_EQ(registry->commandIdForKey(editor, Qt::Key_Z, Qt::ControlModifier),
            QLatin1String(shortcut_id::kEditorUndo));
  EXPECT_EQ(registry->commandIdForKey(editor, Qt::Key_R, Qt::ControlModifier),
            QLatin1String(shortcut_id::kEditorRedo));
  EXPECT_EQ(registry->commandIdForKey(editor, Qt::Key_S, Qt::ControlModifier),
            QLatin1String(shortcut_id::kEditorSaveCurrentImage));
  EXPECT_EQ(registry->commandIdForKey(filmstrip, Qt::Key_Left, Qt::NoModifier),
            QLatin1String(shortcut_id::kFilmstripPreviousImage));
  EXPECT_EQ(registry->commandIdForKey(filmstrip, Qt::Key_Right, Qt::NoModifier),
            QLatin1String(shortcut_id::kFilmstripNextImage));
  EXPECT_EQ(registry->commandIdForKey(filmstrip, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kFilmstripSelectAll));
  EXPECT_EQ(registry->commandIdForKey(versions, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kVersionsCreateDefaultFromRoot));
  EXPECT_EQ(registry->commandIdForKey(lut, Qt::Key_Up, Qt::NoModifier),
            QLatin1String(shortcut_id::kLutSelectPrevious));
  EXPECT_EQ(registry->commandIdForKey(lut, Qt::Key_Down, Qt::NoModifier),
            QLatin1String(shortcut_id::kLutSelectNext));
  EXPECT_EQ(registry->commandIdForKey(mask, Qt::Key_Escape, Qt::NoModifier),
            QLatin1String(shortcut_id::kMaskFinishEdit));
  EXPECT_EQ(registry->commandIdForKey(mask, Qt::Key_Delete, Qt::NoModifier),
            QLatin1String(shortcut_id::kMaskDeleteSelection));
  EXPECT_EQ(registry->commandIdForKey(mask, Qt::Key_Return, Qt::NoModifier),
            QLatin1String(shortcut_id::kMaskConfirmEdit));
  EXPECT_EQ(registry->commandIdForKey(mask, Qt::Key_Enter, Qt::NoModifier),
            QLatin1String(shortcut_id::kMaskConfirmEdit));
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_Delete, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesDeleteSelection));

  // Bindings never leak into a scope that did not declare them.
  EXPECT_EQ(registry->commandIdForKey(lut, Qt::Key_Left, Qt::NoModifier), QString());
  EXPECT_EQ(registry->commandIdForKey(library, Qt::Key_Z, Qt::ControlModifier), QString());
  EXPECT_EQ(registry->commandIdForKey(editor, Qt::Key_A, Qt::ControlModifier), QString());
  EXPECT_EQ(registry->commandIdForKey(nodes, Qt::Key_S, Qt::ControlModifier), QString());
  EXPECT_EQ(registry->commandIdForKey(filmstrip, Qt::Key_Up, Qt::NoModifier), QString());
}

TEST_F(ShortcutRegistryTest, DisjointScopesMayShareOneBinding) {
  auto registry = MakeRegistry();
  EXPECT_TRUE(registry->FinalizeRegistration().isEmpty());

  const QString library   = QLatin1String(shortcut_scope::kWorkspaceLibrary);
  const QString editor    = QLatin1String(shortcut_scope::kWorkspaceEditor);
  const QString filmstrip = QLatin1String(shortcut_scope::kEditorFilmstrip);
  const QString versions  = QLatin1String(shortcut_scope::kEditorVersions);

  // Library, Filmstrip, and Versions all own Ctrl+A in exclusive scopes.
  EXPECT_EQ(registry->commandIdForKey(library, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kLibrarySelectAll));
  EXPECT_EQ(registry->commandIdForKey(filmstrip, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kFilmstripSelectAll));
  EXPECT_EQ(registry->commandIdForKey(versions, Qt::Key_A, Qt::ControlModifier),
            QLatin1String(shortcut_id::kVersionsCreateDefaultFromRoot));

  // The two workspace scopes share Ctrl+S because only one is active.
  EXPECT_EQ(registry->commandIdForKey(library, Qt::Key_S, Qt::ControlModifier),
            QLatin1String(shortcut_id::kLibrarySaveProject));
  EXPECT_EQ(registry->commandIdForKey(editor, Qt::Key_S, Qt::ControlModifier),
            QLatin1String(shortcut_id::kEditorSaveCurrentImage));
}

TEST_F(ShortcutRegistryTest, OverlappingScopesRejectDuplicateBindingWithoutChangingPriorValue) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  // Left is filmstrip.previousImage's default; the filmstrip surface can be
  // focused while the editor workspace is active, so a workspace-level Left
  // is ambiguous and must be rejected.
  const auto result = registry->saveCandidate(QLatin1String(shortcut_id::kEditorUndo), Qt::Key_Left,
                                              Qt::NoModifier, kKeyChord);
  EXPECT_FALSE(result.value(QStringLiteral("succeeded")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("conflict"));
  EXPECT_EQ(result.value(QStringLiteral("conflictingCommandId")).toString(),
            QLatin1String(shortcut_id::kFilmstripPreviousImage));

  EXPECT_TRUE(
      registry->matches(QLatin1String(shortcut_id::kEditorUndo), Qt::Key_Z, Qt::ControlModifier));
  EXPECT_EQ(registry->commandIdForKey(QLatin1String(shortcut_scope::kEditorFilmstrip), Qt::Key_Left,
                                      Qt::NoModifier),
            QLatin1String(shortcut_id::kFilmstripPreviousImage));
  EXPECT_TRUE(RoleValue(*registry, QLatin1String(shortcut_id::kEditorUndo),
                        ShortcutRegistry::UsesDefaultRole)
                  .toBool());
}

TEST_F(ShortcutRegistryTest, MaskExclusiveScopeMayUseNodesDeleteBinding) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  // The built-in set already proves coexistence: identical Delete defaults in
  // the exclusive mask scope and the Nodes scope cause no registration error.
  EXPECT_EQ(registry->commandIdForKey(QLatin1String(shortcut_scope::kEditorMaskEdit),
                                      Qt::Key_Delete, Qt::NoModifier),
            QLatin1String(shortcut_id::kMaskDeleteSelection));
  EXPECT_EQ(registry->commandIdForKey(QLatin1String(shortcut_scope::kEditorNodes), Qt::Key_Delete,
                                      Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesDeleteSelection));

  // Validation accepts the overlap because mask edit shadows Nodes commands.
  const auto allowed = registry->validateCandidate(QLatin1String(shortcut_id::kMaskDeleteSelection),
                                                   Qt::Key_Delete, Qt::NoModifier, kKeyChord);
  EXPECT_TRUE(allowed.value(QStringLiteral("succeeded")).toBool());

  // A non-exclusive command in a scope that is active during mask edit still
  // conflicts with the Nodes binding.
  const auto denied = registry->validateCandidate(QLatin1String(shortcut_id::kEditorUndo),
                                                  Qt::Key_Delete, Qt::NoModifier, kKeyChord);
  EXPECT_FALSE(denied.value(QStringLiteral("succeeded")).toBool());
  EXPECT_EQ(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("conflict"));
}

TEST_F(ShortcutRegistryTest, ModifierOnlyBindingMatchesRequiredPointerModifier) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  const QString extend       = QLatin1String(shortcut_id::kLibraryExtendSelection);
  const QString toggle       = QLatin1String(shortcut_id::kLibraryToggleSelection);
  const QString nodes_extend = QLatin1String(shortcut_id::kNodesExtendSelection);

  EXPECT_TRUE(registry->modifierMatches(extend, Qt::ShiftModifier));
  EXPECT_FALSE(registry->modifierMatches(extend, Qt::ControlModifier));
  EXPECT_TRUE(registry->modifierMatches(toggle, Qt::ControlModifier));
  EXPECT_FALSE(registry->modifierMatches(toggle, Qt::ShiftModifier));
  EXPECT_TRUE(registry->modifierMatches(nodes_extend, Qt::ShiftModifier));

  // Both modifier bindings stay active under Ctrl+Shift so range selection
  // remains additive.
  EXPECT_TRUE(registry->modifierMatches(extend, Qt::ShiftModifier | Qt::ControlModifier));
  EXPECT_TRUE(registry->modifierMatches(toggle, Qt::ShiftModifier | Qt::ControlModifier));

  // Modifier commands never resolve through the key-chord paths.
  EXPECT_FALSE(
      registry->modifierMatches(QLatin1String(shortcut_id::kLibrarySelectAll), Qt::ShiftModifier));
  EXPECT_FALSE(registry->matches(extend, Qt::Key_Shift, Qt::ShiftModifier));
  EXPECT_EQ(registry->commandIdForKey(QLatin1String(shortcut_scope::kWorkspaceLibrary),
                                      Qt::Key_Shift, Qt::ShiftModifier),
            QString());
}

TEST_F(ShortcutRegistryTest, CustomBindingReplacesAllDefaultAliases) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString add = QLatin1String(shortcut_id::kNodesAddColorGrade);

  ASSERT_TRUE(registry->matches(add, Qt::Key_Plus, Qt::ControlModifier));
  ASSERT_TRUE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier));
  ASSERT_TRUE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier));

  QSignalSpy binding_spy(registry.get(), &ShortcutRegistry::commandBindingChanged);
  const auto result = registry->saveCandidate(add, Qt::Key_B, Qt::ControlModifier, kKeyChord);
  ASSERT_TRUE(result.value(QStringLiteral("succeeded")).toBool());
  ASSERT_EQ(binding_spy.count(), 1);
  EXPECT_EQ(binding_spy.first().at(0).toString(), add);

  EXPECT_TRUE(registry->matches(add, Qt::Key_B, Qt::ControlModifier));
  EXPECT_FALSE(registry->matches(add, Qt::Key_Plus, Qt::ControlModifier));
  EXPECT_FALSE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier));
  EXPECT_FALSE(registry->matches(add, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier));
  EXPECT_FALSE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier));
  EXPECT_TRUE(Assigned(*registry, add));
  EXPECT_FALSE(RoleValue(*registry, add, ShortcutRegistry::UsesDefaultRole).toBool());
}

TEST_F(ShortcutRegistryTest, ClearBindingPersistsUnassignedStateAcrossRegistryRestart) {
  const QString file = QStringLiteral("restart.ini");
  {
    auto registry = MakeRegistry(file);
    ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
    const auto result = registry->clearBinding(QLatin1String(shortcut_id::kLibrarySelectAll));
    ASSERT_TRUE(result.value(QStringLiteral("succeeded")).toBool());
    EXPECT_FALSE(registry->matches(QLatin1String(shortcut_id::kLibrarySelectAll), Qt::Key_A,
                                   Qt::ControlModifier));
  }

  auto reopened = MakeRegistry(file);
  ASSERT_TRUE(reopened->FinalizeRegistration().isEmpty());
  const QString id = QLatin1String(shortcut_id::kLibrarySelectAll);
  EXPECT_FALSE(reopened->matches(id, Qt::Key_A, Qt::ControlModifier));
  EXPECT_FALSE(Assigned(*reopened, id));
  EXPECT_FALSE(RoleValue(*reopened, id, ShortcutRegistry::UsesDefaultRole).toBool());
  EXPECT_TRUE(reopened->keySequenceTexts(id).isEmpty());
  EXPECT_EQ(reopened->commandIdForKey(QLatin1String(shortcut_scope::kWorkspaceLibrary), Qt::Key_A,
                                      Qt::ControlModifier),
            QString());
}

TEST_F(ShortcutRegistryTest, RestoreDefaultRestoresAllBuiltInAliases) {
  const QString file     = QStringLiteral("restore.ini");
  auto          registry = MakeRegistry(file);
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString add = QLatin1String(shortcut_id::kNodesAddColorGrade);

  ASSERT_TRUE(registry->saveCandidate(add, Qt::Key_B, Qt::ControlModifier, kKeyChord)
                  .value(QStringLiteral("succeeded"))
                  .toBool());
  const auto result = registry->restoreDefault(add);
  ASSERT_TRUE(result.value(QStringLiteral("succeeded")).toBool());

  EXPECT_TRUE(registry->matches(add, Qt::Key_Plus, Qt::ControlModifier));
  EXPECT_TRUE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier));
  EXPECT_TRUE(registry->matches(add, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier));
  EXPECT_TRUE(registry->matches(add, Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier));
  EXPECT_FALSE(registry->matches(add, Qt::Key_B, Qt::ControlModifier));
  EXPECT_TRUE(RoleValue(*registry, add, ShortcutRegistry::UsesDefaultRole).toBool());

  QSettings stored(SettingsPath(file), QSettings::IniFormat);
  stored.beginGroup(QStringLiteral("keyboardShortcuts/v1"));
  EXPECT_FALSE(stored.childGroups().contains(add));
  stored.endGroup();
}

TEST_F(ShortcutRegistryTest, MalformedSavedBindingStaysInactiveAndReportsItsRowError) {
  const QString file = QStringLiteral("malformed.ini");
  WriteSavedOverride(file, QLatin1String(shortcut_id::kEditorUndo), QStringLiteral("custom"),
                     {QStringLiteral("junk-payload-without-kind")});

  auto registry = MakeRegistry(file);
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString undo = QLatin1String(shortcut_id::kEditorUndo);

  // The malformed value is disabled instead of silently returning to Ctrl+Z.
  EXPECT_FALSE(registry->matches(undo, Qt::Key_Z, Qt::ControlModifier));
  EXPECT_FALSE(Assigned(*registry, undo));
  EXPECT_FALSE(RowError(*registry, undo).isEmpty());

  // Other commands still resolve their defaults.
  EXPECT_TRUE(
      registry->matches(QLatin1String(shortcut_id::kEditorRedo), Qt::Key_R, Qt::ControlModifier));
}

TEST_F(ShortcutRegistryTest, ConflictingSavedBindingsStayInactiveUntilUserResolution) {
  const QString file = QStringLiteral("conflict.ini");
  // Saved custom that collides with a built-in default in a coactive scope.
  WriteSavedOverride(file, QLatin1String(shortcut_id::kEditorUndo), QStringLiteral("custom"),
                     {QStringLiteral("chord:Left")});
  // Two saved customs colliding with each other across coactive scopes.
  WriteSavedOverride(file, QLatin1String(shortcut_id::kFilmstripNextImage),
                     QStringLiteral("custom"), {QStringLiteral("chord:F6")});
  WriteSavedOverride(file, QLatin1String(shortcut_id::kEditorRedo), QStringLiteral("custom"),
                     {QStringLiteral("chord:F6")});

  auto registry = MakeRegistry(file);
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  const QString undo     = QLatin1String(shortcut_id::kEditorUndo);
  const QString redo     = QLatin1String(shortcut_id::kEditorRedo);
  const QString previous = QLatin1String(shortcut_id::kFilmstripPreviousImage);
  const QString next     = QLatin1String(shortcut_id::kFilmstripNextImage);

  // The default owner keeps Left; the saved override is inactive with a row
  // error instead of silently replacing the default or restoring Ctrl+Z.
  EXPECT_EQ(registry->commandIdForKey(QLatin1String(shortcut_scope::kEditorFilmstrip), Qt::Key_Left,
                                      Qt::NoModifier),
            previous);
  EXPECT_TRUE(RowError(*registry, previous).isEmpty());
  EXPECT_FALSE(registry->matches(undo, Qt::Key_Z, Qt::ControlModifier));
  EXPECT_FALSE(registry->matches(undo, Qt::Key_Left, Qt::NoModifier));
  EXPECT_FALSE(Assigned(*registry, undo));
  EXPECT_FALSE(RowError(*registry, undo).isEmpty());

  // Conflicting saved customs both stay inactive.
  EXPECT_FALSE(Assigned(*registry, next));
  EXPECT_FALSE(Assigned(*registry, redo));
  EXPECT_FALSE(RowError(*registry, next).isEmpty());
  EXPECT_FALSE(RowError(*registry, redo).isEmpty());
}

TEST_F(ShortcutRegistryTest, OldNodesDeleteIdMovesToDeleteSelectionOnce) {
  const QString file = QStringLiteral("migrate.ini");
  WriteSavedOverride(file, QStringLiteral("nodes.deleteColorGrade"), QStringLiteral("custom"),
                     {QStringLiteral("chord:F8")});

  auto registry = MakeRegistry(file);
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString id = QLatin1String(shortcut_id::kNodesDeleteSelection);

  EXPECT_TRUE(registry->matches(id, Qt::Key_F8, Qt::NoModifier));
  EXPECT_FALSE(registry->matches(id, Qt::Key_Delete, Qt::NoModifier));
  EXPECT_TRUE(Assigned(*registry, id));

  QSettings stored(SettingsPath(file), QSettings::IniFormat);
  stored.beginGroup(QStringLiteral("keyboardShortcuts/v1"));
  EXPECT_FALSE(stored.childGroups().contains(QStringLiteral("nodes.deleteColorGrade")));
  EXPECT_TRUE(stored.childGroups().contains(id));
  stored.endGroup();

  // The moved override persists for the next registry construction.
  auto reopened = MakeRegistry(file);
  ASSERT_TRUE(reopened->FinalizeRegistration().isEmpty());
  EXPECT_TRUE(reopened->matches(id, Qt::Key_F8, Qt::NoModifier));
}

TEST_F(ShortcutRegistryTest, ReservedCaptureBaseKeysFailUserValidation) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString select_all = QLatin1String(shortcut_id::kLibrarySelectAll);

  for (const int key : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Escape}) {
    const auto verdict = registry->validateCandidate(select_all, key, Qt::NoModifier, kKeyChord);
    EXPECT_FALSE(verdict.value(QStringLiteral("succeeded")).toBool());
    EXPECT_EQ(verdict.value(QStringLiteral("errorCode")).toString(), QStringLiteral("reservedKey"));
    const auto saved = registry->saveCandidate(select_all, key, Qt::NoModifier, kKeyChord);
    EXPECT_FALSE(saved.value(QStringLiteral("succeeded")).toBool());
  }

  EXPECT_TRUE(registry->matches(select_all, Qt::Key_A, Qt::ControlModifier));
  const auto ok = registry->validateCandidate(select_all, Qt::Key_F9, Qt::NoModifier, kKeyChord);
  EXPECT_TRUE(ok.value(QStringLiteral("succeeded")).toBool());
}

TEST_F(ShortcutRegistryTest, SettingsWriteFailureKeepsPriorEffectiveBinding) {
  // A file blocks the settings directory so every INI write deterministically
  // fails with an access error.
  QFile blocker(SettingsPath(QStringLiteral("blocked")));
  ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
  blocker.write("occupied");
  blocker.close();

  const QString file     = QStringLiteral("blocked/shortcuts.ini");
  auto          registry = MakeRegistry(file);
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString select_all = QLatin1String(shortcut_id::kLibrarySelectAll);

  const auto    result = registry->saveCandidate(select_all, Qt::Key_F9, Qt::NoModifier, kKeyChord);
  EXPECT_FALSE(result.value(QStringLiteral("succeeded")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("persistence"));

  EXPECT_TRUE(registry->matches(select_all, Qt::Key_A, Qt::ControlModifier));
  EXPECT_FALSE(registry->matches(select_all, Qt::Key_F9, Qt::NoModifier));
  EXPECT_TRUE(RoleValue(*registry, select_all, ShortcutRegistry::UsesDefaultRole).toBool());
}

TEST_F(ShortcutRegistryTest, TooltipUsesCurrentEffectiveBinding) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());
  const QString fit          = QLatin1String(shortcut_id::kNodesFitGraph);

  const auto    default_text = registry->shortcutText(fit);
  ASSERT_FALSE(default_text.isEmpty());
  EXPECT_EQ(registry->decorateTooltip(QStringLiteral("Fit"), fit),
            QStringLiteral("Fit (%1)").arg(default_text));

  ASSERT_TRUE(registry->saveCandidate(fit, Qt::Key_F9, Qt::NoModifier, kKeyChord)
                  .value(QStringLiteral("succeeded"))
                  .toBool());
  const auto custom_text = registry->shortcutText(fit);
  EXPECT_NE(custom_text, default_text);
  EXPECT_EQ(registry->decorateTooltip(QStringLiteral("Fit"), fit),
            QStringLiteral("Fit (%1)").arg(custom_text));

  ASSERT_TRUE(registry->clearBinding(fit).value(QStringLiteral("succeeded")).toBool());
  EXPECT_TRUE(registry->shortcutText(fit).isEmpty());
  EXPECT_EQ(registry->decorateTooltip(QStringLiteral("Fit"), fit), QStringLiteral("Fit"));
}

TEST_F(ShortcutRegistryTest, DuplicateCommandIdFailsRegistrationFinalization) {
  auto settings = std::make_unique<QSettings>(SettingsPath(QStringLiteral("duplicates.ini")),
                                              QSettings::IniFormat);
  ShortcutRegistry registry(std::move(settings));
  RegisterBuiltinShortcuts(&registry);

  ShortcutBindingSpec duplicate;
  duplicate.id               = QLatin1String(shortcut_id::kNodesFitGraph);
  duplicate.group            = QStringLiteral("Nodes");
  duplicate.description      = QStringLiteral("Duplicate Fit");
  duplicate.default_bindings = {
      ShortcutInput{ShortcutInputKind::KeyChord, Qt::Key_F10, Qt::NoModifier}};
  duplicate.scope = QLatin1String(shortcut_scope::kEditorNodes);
  EXPECT_EQ(registry.Register(std::move(duplicate)), nullptr);

  const auto errors = registry.FinalizeRegistration();
  EXPECT_FALSE(errors.isEmpty());
  EXPECT_TRUE(errors.join(QLatin1Char('\n')).contains(QLatin1String(shortcut_id::kNodesFitGraph)));

  // The duplicate did not add a row or replace the original binding.
  EXPECT_EQ(registry.commandIdForKey(QLatin1String(shortcut_scope::kEditorNodes), Qt::Key_0,
                                     Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesFitGraph));
  EXPECT_EQ(registry.commandIdForKey(QLatin1String(shortcut_scope::kEditorNodes), Qt::Key_F10,
                                     Qt::NoModifier),
            QString());
}

TEST_F(ShortcutRegistryTest, DecorateTooltipAppendsTheNativeSequence) {
  auto registry = MakeRegistry();
  ASSERT_TRUE(registry->FinalizeRegistration().isEmpty());

  const auto decorated =
      registry->decorateTooltip(QStringLiteral("Fit"), QLatin1String(shortcut_id::kNodesFitGraph));
  EXPECT_TRUE(decorated.startsWith(QStringLiteral("Fit (")));
  EXPECT_TRUE(decorated.endsWith(QLatin1Char(')')));
  EXPECT_NE(registry->shortcutText(QLatin1String(shortcut_id::kNodesFitGraph)), QString());
}

}  // namespace
}  // namespace alcedo::ui
