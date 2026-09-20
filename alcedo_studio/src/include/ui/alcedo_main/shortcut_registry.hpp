//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QAction>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <vector>

namespace alcedo::ui {

using ShortcutCommandId = QString;
using ShortcutScope     = QString;

namespace shortcut_scope {
// Stable scope identifiers stored on every command. These strings are registry
// data, not translated labels.
inline constexpr const char* kShortcutCapture  = "shortcut.capture";
inline constexpr const char* kTextInput        = "text.input";
inline constexpr const char* kEditorMaskEdit   = "editor.maskEdit";
inline constexpr const char* kEditorFilmstrip  = "editor.filmstrip";
inline constexpr const char* kEditorVersions   = "editor.versions";
inline constexpr const char* kEditorLut        = "editor.lut";
inline constexpr const char* kEditorNodes      = "editor.nodes";
inline constexpr const char* kWorkspaceEditor  = "workspace.editor";
inline constexpr const char* kWorkspaceLibrary = "workspace.library";
inline constexpr const char* kApplication      = "application";
}  // namespace shortcut_scope

namespace shortcut_id {
inline constexpr const char* kLibrarySelectAll              = "library.selectAll";
inline constexpr const char* kLibraryExtendSelection        = "library.extendSelection";
inline constexpr const char* kLibraryToggleSelection        = "library.toggleSelection";
inline constexpr const char* kLibrarySaveProject            = "library.saveProject";

inline constexpr const char* kEditorUndo                    = "editor.undo";
inline constexpr const char* kEditorRedo                    = "editor.redo";
inline constexpr const char* kEditorSaveCurrentImage        = "editor.saveCurrentImage";

inline constexpr const char* kFilmstripPreviousImage        = "filmstrip.previousImage";
inline constexpr const char* kFilmstripNextImage            = "filmstrip.nextImage";
inline constexpr const char* kFilmstripSelectAll            = "filmstrip.selectAll";

inline constexpr const char* kVersionsCreateDefaultFromRoot = "versions.createDefaultFromRoot";

inline constexpr const char* kLutSelectPrevious             = "lut.selectPrevious";
inline constexpr const char* kLutSelectNext                 = "lut.selectNext";

inline constexpr const char* kMaskFinishEdit                = "mask.finishEdit";
inline constexpr const char* kMaskDeleteSelection           = "mask.deleteSelection";
inline constexpr const char* kMaskConfirmEdit               = "mask.confirmEdit";

inline constexpr const char* kNodesAddColorGrade            = "nodes.addColorGrade";
inline constexpr const char* kNodesFitGraph                 = "nodes.fitGraph";
inline constexpr const char* kNodesRenameColorGrade         = "nodes.renameColorGrade";
inline constexpr const char* kNodesDeleteSelection          = "nodes.deleteSelection";
inline constexpr const char* kNodesBeginConnect             = "nodes.beginConnect";
inline constexpr const char* kNodesCompleteConnect          = "nodes.completeConnect";
inline constexpr const char* kNodesSelectPrevious           = "nodes.selectPrevious";
inline constexpr const char* kNodesSelectNext               = "nodes.selectNext";
inline constexpr const char* kNodesSelectDevelop            = "nodes.selectDevelop";
inline constexpr const char* kNodesSelectDrt                = "nodes.selectDrt";
inline constexpr const char* kNodesCancel                   = "nodes.cancel";
inline constexpr const char* kNodesExtendSelection          = "nodes.extendSelection";
}  // namespace shortcut_id

/// Physical input classes a command can accept. KeyChord is a key plus optional
/// modifiers. Modifier is one modifier key pressed alone; UI owners query it
/// through modifierMatches() and never trigger an action from it.
enum class ShortcutInputKind {
  KeyChord = 0,
  Modifier = 1,
};

struct ShortcutInput {
  ShortcutInputKind     kind      = ShortcutInputKind::KeyChord;
  Qt::Key               key       = Qt::Key_unknown;
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

struct ShortcutBindingSpec {
  ShortcutCommandId     id;
  QString               group;
  QString               description;
  QList<ShortcutInput>  default_bindings;
  ShortcutScope         scope;
  ShortcutInputKind     input_kind       = ShortcutInputKind::KeyChord;
  bool                  auto_repeat      = false;
  bool                  settings_visible = true;
  Qt::ShortcutContext   context          = Qt::WidgetWithChildrenShortcut;
  std::function<bool()> enabled_when{};
  std::function<void()> on_trigger{};
};

/**
 * @brief Named command catalog and list model for product keyboard bindings.
 *
 * Owns command ids, groups, labels, default bindings, effective bindings, input
 * scope metadata, conflict validation, tooltip decoration, and the versioned
 * QSettings override store. UI components keep ownership of the actions that
 * bindings invoke; the registry never copies their state.
 *
 * Registration order: construct, call the Register* functions, then
 * FinalizeRegistration(). Finalization validates the built-in set, loads saved
 * overrides, resolves saved-binding conflicts, and publishes model rows. All
 * mutation happens on the GUI thread.
 *
 * Threading: GUI thread. Does not own editor session, project, or graph state.
 */
class ShortcutRegistry final : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Role {
    CommandIdRole = Qt::UserRole + 1,
    GroupTextRole,
    DescriptionTextRole,
    BindingTextRole,
    DefaultBindingTextRole,
    AssignedRole,
    UsesDefaultRole,
    InputKindRole,
    ScopeTextRole,
    AutoRepeatRole,
    SettingsVisibleRole,
    ValidationErrorRole,
  };

  /// Production path. Reads and writes the application QSettings (the
  /// application organization and application names must already be set).
  explicit ShortcutRegistry(QObject* parent = nullptr);
  /// Test path. The registry owns the supplied settings instance so tests can
  /// use an isolated INI file instead of real application settings.
  explicit ShortcutRegistry(std::unique_ptr<QSettings> settings, QObject* parent = nullptr);

  auto rowCount(const QModelIndex& parent = QModelIndex()) const -> int override;
  auto data(const QModelIndex& index, int role = Qt::DisplayRole) const -> QVariant override;
  auto roleNames() const -> QHash<int, QByteArray> override;

  /// Registers one command before FinalizeRegistration(). Returns the created
  /// QAction only when spec.on_trigger is set; QML input remains the live path.
  /// Duplicate or late registrations are recorded as registration errors.
  auto Register(ShortcutBindingSpec spec) -> QAction*;
  /// Validates the built-in set, loads saved overrides, and publishes rows.
  /// Returns every registration error found; an empty list means the built-in
  /// defaults are valid and unique.
  auto FinalizeRegistration() -> QStringList;
  auto RegistrationErrors() const -> QStringList { return registration_errors_; }
  /// Row of a command id, or -1 when the id is not registered.
  auto RowForCommand(const ShortcutCommandId& id) const -> int;

  auto Action(const ShortcutCommandId& id) const -> QAction*;
  /// Native-text (or PortableText) rendering of the first effective binding.
  auto ShortcutText(const ShortcutCommandId&     id,
                    QKeySequence::SequenceFormat format = QKeySequence::NativeText) const
      -> QString;
  auto DecorateTooltip(const QString& base_tooltip, const ShortcutCommandId& id) const -> QString;
  void RefreshEnabledStates();

  /// True when a key event matches one effective KeyChord binding of the
  /// command. Modifier-kind commands never match; use modifierMatches().
  Q_INVOKABLE bool        matches(const QString& command_id, int key, int modifiers) const;
  /// True when the pressed modifier bits contain all bits of one effective
  /// Modifier binding of the command. Pointer and selection handlers use this.
  Q_INVOKABLE bool        modifierMatches(const QString& command_id, int modifiers) const;
  /// Modifier bits currently held on the keyboard, masked to
  /// Shift/Ctrl/Alt/Meta. Pointer handlers read modifiers here because Qan
  /// node press signals do not carry them.
  Q_INVOKABLE int         currentKeyboardModifiers() const;
  /// Union of every effective Modifier-kind binding of the command, as
  /// Qt::KeyboardModifier bits. Zero when the command binds no modifier.
  Q_INVOKABLE int         modifierBitsForCommand(const QString& command_id) const;
  /// Resolves a key event to a command id inside one scope only. Returns an
  /// empty string when no command in that scope binds the key.
  Q_INVOKABLE QString     commandIdForKey(const QString& scope, int key, int modifiers) const;
  /// Portable text of every effective binding, in binding order. Suitable for
  /// feeding Qt Quick Shortcut.sequences; use shortcutText()/decorateTooltip()
  /// for user-facing native text.
  Q_INVOKABLE QStringList keySequenceTexts(const QString& command_id) const;
  /// Registered scope string of a command, or empty when the id is unknown.
  Q_INVOKABLE QString     scopeForCommand(const QString& command_id) const;
  /// True when the command's bindings auto-repeat while the key is held.
  Q_INVOKABLE bool        commandAutoRepeat(const QString& command_id) const;

  /// Validates a capture candidate without changing state. The result map
  /// carries succeeded, errorCode, message, and conflictingCommandId.
  Q_INVOKABLE QVariantMap validateCandidate(const QString& command_id, int key, int modifiers,
                                            int input_kind) const;
  /// Validates, persists, then applies a candidate. On a settings write
  /// failure the prior effective binding stays active and the result is a
  /// persistence error.
  Q_INVOKABLE QVariantMap saveCandidate(const QString& command_id, int key, int modifiers,
                                        int input_kind);
  /// Persists and applies the explicit unassigned state. An empty saved value
  /// is different from a missing setting: defaults are not restored.
  Q_INVOKABLE QVariantMap clearBinding(const QString& command_id);
  /// Removes the saved override and restores every built-in default alias.
  Q_INVOKABLE QVariantMap restoreDefault(const QString& command_id);
  Q_INVOKABLE QString     shortcutText(const QString& command_id) const;
  Q_INVOKABLE QString decorateTooltip(const QString& base_text, const QString& command_id) const;
  /// Native-text rendering of a captured-but-not-yet-saved candidate for the
  /// settings capture field: a modifier mask without a key renders as modifier
  /// text ("Ctrl+Shift"), a key chord renders as "Ctrl+K". Returns empty for
  /// empty input. Display-only; saving still runs through saveCandidate().
  Q_INVOKABLE QString candidateText(int key, int modifiers, int input_kind) const;

 signals:
  /// Emitted after a successful save, clear, or restore so QML input wrappers
  /// can re-read the effective bindings for one command.
  void commandBindingChanged(const QString& command_id);

 private:
  enum class BindingSource { Default, Custom, Unassigned, InvalidSaved };

  struct Entry {
    ShortcutBindingSpec  spec;
    QList<ShortcutInput> bindings;
    BindingSource        source = BindingSource::Default;
    QString              row_error;
    QAction*             action             = nullptr;
    bool                 return_enter_alias = false;
  };

  auto FindEntry(const ShortcutCommandId& id) -> Entry*;
  auto FindEntry(const ShortcutCommandId& id) const -> const Entry*;
  auto ValidateUserCandidate(const Entry& entry, const ShortcutInput& candidate) const
      -> QVariantMap;
  void LoadOverrides();
  void MigrateLegacyNodesDeleteId();
  void ResolveSavedConflicts();
  auto WriteSavedState(const ShortcutCommandId& id, const QString& state,
                       const QStringList& serialized) -> bool;
  auto RemoveSavedState(const ShortcutCommandId& id) -> bool;
  void ApplyBindings(Entry& entry, QList<ShortcutInput> bindings, BindingSource source);
  void NotifyRowChanged(int row);
  void UpdateActionShortcuts(Entry& entry);

  std::vector<Entry>            entries_;
  QHash<ShortcutCommandId, int> row_for_id_;
  std::unique_ptr<QSettings>    settings_;
  QStringList                   registration_errors_;
  bool                          finalized_ = false;
};

void RegisterLibraryShortcuts(ShortcutRegistry* registry);
void RegisterEditorShortcuts(ShortcutRegistry* registry);
void RegisterFilmstripShortcuts(ShortcutRegistry* registry);
void RegisterVersionsShortcuts(ShortcutRegistry* registry);
void RegisterLutShortcuts(ShortcutRegistry* registry);
void RegisterMaskShortcuts(ShortcutRegistry* registry);
void RegisterNodesPanelShortcuts(ShortcutRegistry* registry);
/// Registers every built-in command group. Call FinalizeRegistration() after.
void RegisterBuiltinShortcuts(ShortcutRegistry* registry);
void RegisterShortcutRegistryQmlType();

}  // namespace alcedo::ui
