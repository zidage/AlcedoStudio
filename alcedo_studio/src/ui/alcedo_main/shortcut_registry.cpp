//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/shortcut_registry.hpp"

#include <qqml.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyCombination>
#include <QQmlEngine>

namespace alcedo::ui {
namespace {

constexpr Qt::KeyboardModifiers kModifierMask =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

// Versioned settings root. Each command owns one child group keyed by its id.
const QString kSettingsRoot        = QStringLiteral("keyboardShortcuts/v1");
// One-time migration source for the renamed Nodes delete command.
const QString kLegacyNodesDeleteId = QStringLiteral("nodes.deleteColorGrade");

const QString kStateCustom         = QStringLiteral("custom");
const QString kStateUnassigned     = QStringLiteral("unassigned");

const QString kErrUnknownCommand   = QStringLiteral("unknownCommand");
const QString kErrInvalidInput     = QStringLiteral("invalidInput");
const QString kErrReservedKey      = QStringLiteral("reservedKey");
const QString kErrConflict         = QStringLiteral("conflict");
const QString kErrPersistence      = QStringLiteral("persistence");

auto          Translate(const char* text) -> QString {
  return QCoreApplication::translate("ShortcutRegistry", text);
}

auto Result(bool succeeded, const QString& error_code = {}, const QString& message = {},
            const QString& conflicting_command_id = {}) -> QVariantMap {
  QVariantMap result;
  result.insert(QStringLiteral("succeeded"), succeeded);
  result.insert(QStringLiteral("errorCode"), error_code);
  result.insert(QStringLiteral("message"), message);
  result.insert(QStringLiteral("conflictingCommandId"), conflicting_command_id);
  return result;
}

auto Chord(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) -> ShortcutInput {
  return ShortcutInput{ShortcutInputKind::KeyChord, key, modifiers & kModifierMask};
}

auto Modifier(Qt::KeyboardModifier modifier) -> ShortcutInput {
  return ShortcutInput{ShortcutInputKind::Modifier, Qt::Key_unknown,
                       Qt::KeyboardModifiers{modifier} & kModifierMask};
}

auto MaskedModifiers(int modifiers) -> Qt::KeyboardModifiers {
  return static_cast<Qt::KeyboardModifiers>(modifiers) & kModifierMask;
}

auto IsModifierKey(int key) -> bool {
  switch (static_cast<Qt::Key>(key)) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
      return true;
    default:
      return false;
  }
}

// Capture never produces Return, Enter, or Escape as a base key: Enter and
// Escape commit the candidate and the capture field reserves them as control
// keys. Built-in defaults may still use them.
auto IsReservedBaseKey(Qt::Key key) -> bool {
  return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape;
}

/// Single-bit modifier name used in settings files. Stable, never translated.
auto ModifierStableName(Qt::KeyboardModifiers modifiers) -> QString {
  if (modifiers == Qt::ShiftModifier) {
    return QStringLiteral("Shift");
  }
  if (modifiers == Qt::ControlModifier) {
    return QStringLiteral("Control");
  }
  if (modifiers == Qt::AltModifier) {
    return QStringLiteral("Alt");
  }
  if (modifiers == Qt::MetaModifier) {
    return QStringLiteral("Meta");
  }
  return {};
}

auto ModifierFromStableName(const QString& name) -> Qt::KeyboardModifiers {
  if (name == QLatin1String("Shift")) {
    return Qt::ShiftModifier;
  }
  if (name == QLatin1String("Control")) {
    return Qt::ControlModifier;
  }
  if (name == QLatin1String("Alt")) {
    return Qt::AltModifier;
  }
  if (name == QLatin1String("Meta")) {
    return Qt::MetaModifier;
  }
  return Qt::KeyboardModifiers{};
}

auto SequenceText(Qt::Key key, Qt::KeyboardModifiers modifiers, QKeySequence::SequenceFormat format)
    -> QString {
  return QKeySequence(QKeyCombination(modifiers, key)).toString(format);
}

auto ModifierText(Qt::KeyboardModifiers modifiers, QKeySequence::SequenceFormat format) -> QString {
  if (format == QKeySequence::PortableText) {
    return ModifierStableName(modifiers);
  }
  QString text = QKeySequence(static_cast<int>(modifiers)).toString(format);
  if (text.endsWith(QLatin1Char('+'))) {
    text.chop(1);
  }
  return text.isEmpty() ? ModifierStableName(modifiers) : text;
}

auto InputText(const ShortcutInput& input, QKeySequence::SequenceFormat format) -> QString {
  if (input.kind == ShortcutInputKind::Modifier) {
    return ModifierText(input.modifiers, format);
  }
  return SequenceText(input.key, input.modifiers, format);
}

auto BindingListText(const QList<ShortcutInput>& inputs, QKeySequence::SequenceFormat format)
    -> QString {
  QStringList texts;
  texts.reserve(inputs.size());
  for (const auto& input : inputs) {
    texts.push_back(InputText(input, format));
  }
  return texts.join(QStringLiteral(", "));
}

auto SerializeInput(const ShortcutInput& input) -> QString {
  if (input.kind == ShortcutInputKind::Modifier) {
    return QStringLiteral("modifier:") + ModifierStableName(input.modifiers);
  }
  return QStringLiteral("chord:") +
         SequenceText(input.key, input.modifiers, QKeySequence::PortableText);
}

auto ParseInput(const QString& serialized, ShortcutInput& out) -> bool {
  if (serialized.startsWith(QLatin1String("modifier:"))) {
    const auto modifiers =
        ModifierFromStableName(serialized.mid(QLatin1String("modifier:").size()));
    if (modifiers == Qt::NoModifier) {
      return false;
    }
    out = ShortcutInput{ShortcutInputKind::Modifier, Qt::Key_unknown, modifiers};
    return true;
  }
  if (serialized.startsWith(QLatin1String("chord:"))) {
    const auto sequence = QKeySequence::fromString(serialized.mid(QLatin1String("chord:").size()),
                                                   QKeySequence::PortableText);
    if (sequence.count() != 1) {
      return false;
    }
    const auto combination = sequence[0];
    if (combination.key() == Qt::Key_unknown || IsModifierKey(combination.key())) {
      return false;
    }
    out = ShortcutInput{ShortcutInputKind::KeyChord, combination.key(),
                        combination.keyboardModifiers() & kModifierMask};
    return true;
  }
  return false;
}

/// Well-formedness of a stored or built-in input. Does not apply the
/// user-capture reserved-key restriction.
auto IsWellFormed(const ShortcutInput& input) -> bool {
  if (input.kind == ShortcutInputKind::Modifier) {
    return !ModifierStableName(input.modifiers).isEmpty();
  }
  return input.key != Qt::Key_unknown && !IsModifierKey(input.key) &&
         (input.modifiers & kModifierMask) == input.modifiers;
}

/// Exclusive modes shadow lower-priority product scopes, so an overlap with a
/// shadowed scope is allowed by the conflict rules.
auto IsExclusiveScope(const ShortcutScope& scope) -> bool {
  return scope == QLatin1String(shortcut_scope::kShortcutCapture) ||
         scope == QLatin1String(shortcut_scope::kTextInput) ||
         scope == QLatin1String(shortcut_scope::kEditorMaskEdit);
}

auto IsEditorLocalScope(const ShortcutScope& scope) -> bool {
  return scope == QLatin1String(shortcut_scope::kEditorFilmstrip) ||
         scope == QLatin1String(shortcut_scope::kEditorVersions) ||
         scope == QLatin1String(shortcut_scope::kEditorLut) ||
         scope == QLatin1String(shortcut_scope::kEditorNodes);
}

/// Whether two scopes can own the active input path at the same time. The
/// editor workspace stays active while one of its local surfaces or mask edit
/// owns focus; the two workspace scopes and the local surfaces are mutually
/// exclusive inside their own tier.
auto ScopesCanCoactivate(const ShortcutScope& first, const ShortcutScope& second) -> bool {
  if (first == second) {
    return true;
  }
  const QString application = QLatin1String(shortcut_scope::kApplication);
  if (first == application || second == application) {
    return true;
  }
  const QString editor    = QLatin1String(shortcut_scope::kWorkspaceEditor);
  const QString mask_edit = QLatin1String(shortcut_scope::kEditorMaskEdit);
  const auto    pair_with = [&editor, &mask_edit](const ShortcutScope& scope) {
    return IsEditorLocalScope(scope) || scope == mask_edit;
  };
  if (first == editor && pair_with(second)) {
    return true;
  }
  if (second == editor && pair_with(first)) {
    return true;
  }
  // Mask edit rides on top of a focused local surface (the Nodes panel).
  if (first == mask_edit && IsEditorLocalScope(second)) {
    return true;
  }
  return second == mask_edit && IsEditorLocalScope(first);
}

auto MayShareScopes(const ShortcutScope& first, const ShortcutScope& second) -> bool {
  if (!ScopesCanCoactivate(first, second)) {
    return true;
  }
  return IsExclusiveScope(first) != IsExclusiveScope(second);
}

auto InputsOverlap(const ShortcutInput& first, const ShortcutInput& second) -> bool {
  if (first.kind != second.kind) {
    return false;
  }
  if (first.kind == ShortcutInputKind::Modifier) {
    return first.modifiers == second.modifiers;
  }
  return first.key == second.key && first.modifiers == second.modifiers;
}

/// True when the default set lists both Return and keypad Enter as plain key
/// chords. Those commands normalize the two keys during matching.
auto HasReturnEnterAlias(const QList<ShortcutInput>& defaults) -> bool {
  bool has_return = false;
  bool has_enter  = false;
  for (const auto& input : defaults) {
    if (input.kind != ShortcutInputKind::KeyChord || input.modifiers != Qt::NoModifier) {
      continue;
    }
    has_return |= input.key == Qt::Key_Return;
    has_enter |= input.key == Qt::Key_Enter;
  }
  return has_return && has_enter;
}

auto CanonicalKey(bool return_enter_alias, int key) -> Qt::Key {
  const auto qt_key = static_cast<Qt::Key>(key);
  if (return_enter_alias && qt_key == Qt::Key_Enter) {
    return Qt::Key_Return;
  }
  return qt_key;
}

/// Validates the shape of a user-captured candidate. Reserved base keys are
/// rejected here; the command's declared input kind is checked by the caller.
auto BuildUserCandidate(int input_kind, int key, int modifiers, ShortcutInput& out,
                        QString& error_code, QString& error_message) -> bool {
  const auto kind = static_cast<ShortcutInputKind>(input_kind);
  if (kind == ShortcutInputKind::Modifier) {
    const auto mods = MaskedModifiers(modifiers);
    if (ModifierStableName(mods).isEmpty()) {
      error_code    = kErrInvalidInput;
      error_message = Translate(
          "A modifier binding needs exactly one of Shift, Ctrl, Alt, "
          "or Meta.");
      return false;
    }
    out = ShortcutInput{ShortcutInputKind::Modifier, Qt::Key_unknown, mods};
    return true;
  }
  if (kind != ShortcutInputKind::KeyChord) {
    error_code    = kErrInvalidInput;
    error_message = Translate("Unknown shortcut input kind.");
    return false;
  }
  if (key == Qt::Key_unknown || IsModifierKey(key)) {
    error_code    = kErrInvalidInput;
    error_message = Translate("The shortcut needs a non-modifier key.");
    return false;
  }
  if (IsReservedBaseKey(static_cast<Qt::Key>(key))) {
    error_code    = kErrReservedKey;
    error_message = Translate(
        "Return, Enter, and Escape cannot be recorded "
        "as a new shortcut.");
    return false;
  }
  out = ShortcutInput{ShortcutInputKind::KeyChord, static_cast<Qt::Key>(key),
                      MaskedModifiers(modifiers)};
  return true;
}

}  // namespace

ShortcutRegistry::ShortcutRegistry(QObject* parent)
    : ShortcutRegistry(std::make_unique<QSettings>(), parent) {}

ShortcutRegistry::ShortcutRegistry(std::unique_ptr<QSettings> settings, QObject* parent)
    : QAbstractListModel(parent), settings_(std::move(settings)) {
  if (settings_ == nullptr) {
    settings_ = std::make_unique<QSettings>();
  }
}

int ShortcutRegistry::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() || !finalized_) {
    return 0;
  }
  return static_cast<int>(entries_.size());
}

QVariant ShortcutRegistry::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(entries_.size())) {
    return {};
  }
  const auto& entry = entries_[static_cast<size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
    case DescriptionTextRole:
      return entry.spec.description;
    case CommandIdRole:
      return entry.spec.id;
    case GroupTextRole:
      return entry.spec.group;
    case BindingTextRole:
      return entry.bindings.isEmpty() ? Translate("Unassigned")
                                      : BindingListText(entry.bindings, QKeySequence::NativeText);
    case DefaultBindingTextRole:
      return BindingListText(entry.spec.default_bindings, QKeySequence::NativeText);
    case AssignedRole:
      return !entry.bindings.isEmpty();
    case UsesDefaultRole:
      return entry.source == BindingSource::Default;
    case InputKindRole:
      return static_cast<int>(entry.spec.input_kind);
    case ScopeTextRole:
      return entry.spec.scope;
    case AutoRepeatRole:
      return entry.spec.auto_repeat;
    case SettingsVisibleRole:
      return entry.spec.settings_visible;
    case ValidationErrorRole:
      return entry.row_error;
    default:
      return {};
  }
}

QHash<int, QByteArray> ShortcutRegistry::roleNames() const {
  return {
      {CommandIdRole, "commandId"},
      {GroupTextRole, "groupText"},
      {DescriptionTextRole, "descriptionText"},
      {BindingTextRole, "bindingText"},
      {DefaultBindingTextRole, "defaultBindingText"},
      {AssignedRole, "assigned"},
      {UsesDefaultRole, "usesDefault"},
      {InputKindRole, "inputKind"},
      {ScopeTextRole, "scopeText"},
      {AutoRepeatRole, "autoRepeat"},
      {SettingsVisibleRole, "settingsVisible"},
      {ValidationErrorRole, "validationError"},
  };
}

auto ShortcutRegistry::Register(ShortcutBindingSpec spec) -> QAction* {
  if (spec.id.isEmpty()) {
    registration_errors_ += Translate("Shortcut command registration is missing an id.");
    return nullptr;
  }
  if (finalized_) {
    registration_errors_ +=
        QStringLiteral("Shortcut command registered after finalization: %1").arg(spec.id);
    return nullptr;
  }
  if (row_for_id_.contains(spec.id)) {
    registration_errors_ += QStringLiteral("Duplicate shortcut command id: %1").arg(spec.id);
    return nullptr;
  }

  QAction* action = nullptr;
  if (spec.on_trigger) {
    action = new QAction(this);
    action->setObjectName(spec.id);
    if (!spec.description.isEmpty()) {
      action->setText(spec.description);
      action->setToolTip(spec.description);
      action->setStatusTip(spec.description);
    }
    action->setShortcutContext(spec.context);
  }

  const int row = static_cast<int>(entries_.size());
  Entry     entry;
  entry.spec               = std::move(spec);
  entry.bindings           = entry.spec.default_bindings;
  entry.source             = BindingSource::Default;
  entry.action             = action;
  entry.return_enter_alias = HasReturnEnterAlias(entry.spec.default_bindings);
  row_for_id_.insert(entry.spec.id, row);
  entries_.push_back(std::move(entry));
  auto& stored = entries_.back();

  UpdateActionShortcuts(stored);
  if (stored.action != nullptr) {
    const ShortcutCommandId id = stored.spec.id;
    QObject::connect(stored.action, &QAction::triggered, this, [this, id](bool) {
      auto* target = FindEntry(id);
      if (target == nullptr) {
        return;
      }
      if (target->spec.enabled_when) {
        const bool enabled = target->spec.enabled_when();
        target->action->setEnabled(enabled);
        if (!enabled) {
          return;
        }
      }
      target->spec.on_trigger();
    });
  }

  RefreshEnabledStates();
  return action;
}

auto ShortcutRegistry::FinalizeRegistration() -> QStringList {
  QStringList errors = registration_errors_;

  for (const auto& entry : entries_) {
    if (entry.spec.default_bindings.isEmpty()) {
      errors += QStringLiteral("Shortcut command %1 has no default binding.").arg(entry.spec.id);
      continue;
    }
    for (const auto& input : entry.spec.default_bindings) {
      if (!IsWellFormed(input)) {
        errors += QStringLiteral(
                      "Shortcut command %1 has an invalid default "
                      "binding.")
                      .arg(entry.spec.id);
        break;
      }
      if (input.kind != entry.spec.input_kind) {
        errors += QStringLiteral(
                      "Shortcut command %1 default does not match "
                      "its input kind.")
                      .arg(entry.spec.id);
        break;
      }
    }
  }

  for (size_t i = 0; i < entries_.size(); ++i) {
    for (size_t j = i + 1; j < entries_.size(); ++j) {
      const auto& first  = entries_[i];
      const auto& second = entries_[j];
      if (MayShareScopes(first.spec.scope, second.spec.scope)) {
        continue;
      }
      for (const auto& a : first.spec.default_bindings) {
        for (const auto& b : second.spec.default_bindings) {
          if (InputsOverlap(a, b)) {
            errors += QStringLiteral(
                          "Shortcut defaults for %1 and %2 overlap "
                          "in scopes that can be active together.")
                          .arg(first.spec.id, second.spec.id);
          }
        }
      }
    }
  }

  registration_errors_ = errors;
  for (const auto& error : errors) {
    qWarning().noquote() << error;
  }

  beginResetModel();
  LoadOverrides();
  finalized_ = true;
  endResetModel();
  return errors;
}

auto ShortcutRegistry::RowForCommand(const ShortcutCommandId& id) const -> int {
  return row_for_id_.value(id, -1);
}

auto ShortcutRegistry::Action(const ShortcutCommandId& id) const -> QAction* {
  if (const auto* entry = FindEntry(id)) {
    return entry->action;
  }
  return nullptr;
}

auto ShortcutRegistry::ShortcutText(const ShortcutCommandId&     id,
                                    QKeySequence::SequenceFormat format) const -> QString {
  const auto* entry = FindEntry(id);
  if (entry == nullptr || entry->bindings.isEmpty()) {
    return {};
  }
  return InputText(entry->bindings.first(), format);
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
  for (auto& entry : entries_) {
    if (entry.action == nullptr) {
      continue;
    }
    const bool enabled = !entry.spec.enabled_when || entry.spec.enabled_when();
    entry.action->setEnabled(enabled);
  }
}

bool ShortcutRegistry::matches(const QString& command_id, int key, int modifiers) const {
  const auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return false;
  }
  const auto pressed_modifiers = MaskedModifiers(modifiers);
  const auto pressed_key       = CanonicalKey(entry->return_enter_alias, key);
  for (const auto& input : entry->bindings) {
    if (input.kind != ShortcutInputKind::KeyChord) {
      continue;
    }
    const auto stored_key = CanonicalKey(entry->return_enter_alias, input.key);
    if (stored_key == pressed_key && input.modifiers == pressed_modifiers) {
      return true;
    }
  }
  return false;
}

bool ShortcutRegistry::modifierMatches(const QString& command_id, int modifiers) const {
  const auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return false;
  }
  const auto pressed = MaskedModifiers(modifiers);
  for (const auto& input : entry->bindings) {
    if (input.kind == ShortcutInputKind::Modifier &&
        (pressed & input.modifiers) == input.modifiers) {
      return true;
    }
  }
  return false;
}

int ShortcutRegistry::currentKeyboardModifiers() const {
  return static_cast<int>(MaskedModifiers(
      static_cast<int>(QGuiApplication::keyboardModifiers())));
}

int ShortcutRegistry::modifierBitsForCommand(const QString& command_id) const {
  const auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return 0;
  }
  int bits = 0;
  for (const auto& input : entry->bindings) {
    if (input.kind == ShortcutInputKind::Modifier) {
      bits |= static_cast<int>(input.modifiers);
    }
  }
  return bits;
}

QString ShortcutRegistry::commandIdForKey(const QString& scope, int key, int modifiers) const {
  const auto pressed_modifiers = MaskedModifiers(modifiers);
  for (const auto& entry : entries_) {
    if (entry.spec.scope != scope) {
      continue;
    }
    const auto pressed_key = CanonicalKey(entry.return_enter_alias, key);
    for (const auto& input : entry.bindings) {
      if (input.kind != ShortcutInputKind::KeyChord) {
        continue;
      }
      const auto stored_key = CanonicalKey(entry.return_enter_alias, input.key);
      if (stored_key == pressed_key && input.modifiers == pressed_modifiers) {
        return entry.spec.id;
      }
    }
  }
  return {};
}

QStringList ShortcutRegistry::keySequenceTexts(const QString& command_id) const {
  const auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return {};
  }
  // PortableText so QML consumers (Shortcut.sequences) can re-parse the
  // strings on every platform; native text is display-only on macOS.
  QStringList texts;
  texts.reserve(entry->bindings.size());
  for (const auto& input : entry->bindings) {
    texts.push_back(InputText(input, QKeySequence::PortableText));
  }
  return texts;
}

QString ShortcutRegistry::scopeForCommand(const QString& command_id) const {
  const auto* entry = FindEntry(command_id);
  return entry == nullptr ? QString{} : entry->spec.scope;
}

bool ShortcutRegistry::commandAutoRepeat(const QString& command_id) const {
  const auto* entry = FindEntry(command_id);
  return entry != nullptr && entry->spec.auto_repeat;
}

QVariantMap ShortcutRegistry::validateCandidate(const QString& command_id, int key, int modifiers,
                                                int input_kind) const {
  const auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return Result(false, kErrUnknownCommand, Translate("Unknown shortcut command."));
  }
  ShortcutInput candidate;
  QString       error_code;
  QString       error_message;
  if (!BuildUserCandidate(input_kind, key, modifiers, candidate, error_code, error_message)) {
    return Result(false, error_code, error_message);
  }
  if (candidate.kind != entry->spec.input_kind) {
    return Result(false, kErrInvalidInput,
                  Translate("This command accepts a different kind of input."));
  }
  return ValidateUserCandidate(*entry, candidate);
}

QVariantMap ShortcutRegistry::ValidateUserCandidate(const Entry&         entry,
                                                    const ShortcutInput& candidate) const {
  for (const auto& other : entries_) {
    if (other.spec.id == entry.spec.id) {
      continue;
    }
    for (const auto& binding : other.bindings) {
      if (!InputsOverlap(candidate, binding)) {
        continue;
      }
      if (!MayShareScopes(entry.spec.scope, other.spec.scope)) {
        return Result(false, kErrConflict,
                      Translate("Conflicts with \"%1\".").arg(other.spec.description),
                      other.spec.id);
      }
    }
  }
  return Result(true);
}

QVariantMap ShortcutRegistry::saveCandidate(const QString& command_id, int key, int modifiers,
                                            int input_kind) {
  const auto verdict = validateCandidate(command_id, key, modifiers, input_kind);
  if (!verdict.value(QStringLiteral("succeeded")).toBool()) {
    return verdict;
  }
  auto*         entry = FindEntry(command_id);
  ShortcutInput candidate;
  QString       ignored_code;
  QString       ignored_message;
  BuildUserCandidate(input_kind, key, modifiers, candidate, ignored_code, ignored_message);

  if (!WriteSavedState(command_id, kStateCustom, {SerializeInput(candidate)})) {
    return Result(false, kErrPersistence, Translate("The shortcut could not be saved."));
  }
  ApplyBindings(*entry, {candidate}, BindingSource::Custom);
  NotifyRowChanged(row_for_id_.value(command_id));
  return Result(true);
}

QVariantMap ShortcutRegistry::clearBinding(const QString& command_id) {
  auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return Result(false, kErrUnknownCommand, Translate("Unknown shortcut command."));
  }
  if (!WriteSavedState(command_id, kStateUnassigned, {})) {
    return Result(false, kErrPersistence, Translate("The shortcut could not be saved."));
  }
  ApplyBindings(*entry, {}, BindingSource::Unassigned);
  NotifyRowChanged(row_for_id_.value(command_id));
  return Result(true);
}

QVariantMap ShortcutRegistry::restoreDefault(const QString& command_id) {
  auto* entry = FindEntry(command_id);
  if (entry == nullptr) {
    return Result(false, kErrUnknownCommand, Translate("Unknown shortcut command."));
  }
  if (!RemoveSavedState(command_id)) {
    return Result(false, kErrPersistence, Translate("The shortcut could not be saved."));
  }
  ApplyBindings(*entry, entry->spec.default_bindings, BindingSource::Default);
  NotifyRowChanged(row_for_id_.value(command_id));
  return Result(true);
}

QString ShortcutRegistry::shortcutText(const QString& command_id) const {
  return ShortcutText(command_id);
}

QString ShortcutRegistry::decorateTooltip(const QString& base_text,
                                          const QString& command_id) const {
  return DecorateTooltip(base_text, command_id);
}

QString ShortcutRegistry::candidateText(int key, int modifiers, int input_kind) const {
  const auto modifiers_masked = MaskedModifiers(modifiers);
  const auto qt_key           = static_cast<Qt::Key>(key);
  // QML passes 0 for "no key captured"; that is not Qt::Key_unknown.
  if (static_cast<ShortcutInputKind>(input_kind) == ShortcutInputKind::Modifier || key <= 0 ||
      qt_key == Qt::Key_unknown || IsModifierKey(qt_key)) {
    return modifiers_masked == Qt::NoModifier
               ? QString{}
               : ModifierText(modifiers_masked, QKeySequence::NativeText);
  }
  return SequenceText(qt_key, modifiers_masked, QKeySequence::NativeText);
}

auto ShortcutRegistry::FindEntry(const ShortcutCommandId& id) -> Entry* {
  const auto it = row_for_id_.constFind(id);
  if (it == row_for_id_.constEnd()) {
    return nullptr;
  }
  return &entries_[static_cast<size_t>(it.value())];
}

auto ShortcutRegistry::FindEntry(const ShortcutCommandId& id) const -> const Entry* {
  const auto it = row_for_id_.constFind(id);
  if (it == row_for_id_.constEnd()) {
    return nullptr;
  }
  return &entries_[static_cast<size_t>(it.value())];
}

void ShortcutRegistry::LoadOverrides() {
  MigrateLegacyNodesDeleteId();

  for (auto& entry : entries_) {
    entry.bindings  = entry.spec.default_bindings;
    entry.source    = BindingSource::Default;
    entry.row_error = QString();

    settings_->beginGroup(kSettingsRoot);
    settings_->beginGroup(entry.spec.id);
    const bool        has_state    = settings_->contains(QStringLiteral("state"));
    const QString     state        = settings_->value(QStringLiteral("state")).toString();
    const QStringList raw_bindings = settings_->value(QStringLiteral("bindings")).toStringList();
    settings_->endGroup();
    settings_->endGroup();

    if (!has_state) {
      continue;
    }
    if (state == kStateUnassigned) {
      entry.bindings.clear();
      entry.source = BindingSource::Unassigned;
      continue;
    }
    if (state != kStateCustom || raw_bindings.isEmpty()) {
      entry.bindings.clear();
      entry.source    = BindingSource::InvalidSaved;
      entry.row_error = Translate("The saved shortcut could not be read.");
      continue;
    }
    QList<ShortcutInput> parsed;
    parsed.reserve(raw_bindings.size());
    bool well_formed = true;
    for (const auto& serialized : raw_bindings) {
      ShortcutInput input;
      if (!ParseInput(serialized, input) || input.kind != entry.spec.input_kind ||
          (input.kind == ShortcutInputKind::KeyChord && IsReservedBaseKey(input.key))) {
        well_formed = false;
        break;
      }
      parsed.push_back(input);
    }
    if (!well_formed || parsed.isEmpty()) {
      entry.bindings.clear();
      entry.source    = BindingSource::InvalidSaved;
      entry.row_error = Translate("The saved shortcut could not be read.");
      continue;
    }
    entry.bindings = parsed;
    entry.source   = BindingSource::Custom;
  }

  ResolveSavedConflicts();

  for (auto& entry : entries_) {
    UpdateActionShortcuts(entry);
  }
}

void ShortcutRegistry::MigrateLegacyNodesDeleteId() {
  settings_->beginGroup(kSettingsRoot);
  const auto groups = settings_->childGroups();
  if (groups.contains(kLegacyNodesDeleteId)) {
    const QString new_id = QLatin1String(shortcut_id::kNodesDeleteSelection);
    if (!groups.contains(new_id)) {
      settings_->beginGroup(kLegacyNodesDeleteId);
      const auto state    = settings_->value(QStringLiteral("state"));
      const auto bindings = settings_->value(QStringLiteral("bindings"));
      settings_->endGroup();
      settings_->beginGroup(new_id);
      settings_->setValue(QStringLiteral("state"), state);
      settings_->setValue(QStringLiteral("bindings"), bindings);
      settings_->endGroup();
    }
    settings_->remove(kLegacyNodesDeleteId);
    settings_->sync();
  }
  settings_->endGroup();
}

void ShortcutRegistry::ResolveSavedConflicts() {
  // Evaluate every pair against the bindings parsed at load time so the
  // outcome does not depend on entry order. A saved custom binding loses to a
  // default; two conflicting saved customs both stay inactive.
  QList<int> invalidated;
  for (size_t i = 0; i < entries_.size(); ++i) {
    for (size_t j = i + 1; j < entries_.size(); ++j) {
      auto& first  = entries_[i];
      auto& second = entries_[j];
      if (MayShareScopes(first.spec.scope, second.spec.scope)) {
        continue;
      }
      bool overlaps = false;
      for (const auto& a : first.bindings) {
        for (const auto& b : second.bindings) {
          overlaps |= InputsOverlap(a, b);
        }
      }
      if (!overlaps) {
        continue;
      }
      const bool first_custom  = first.source == BindingSource::Custom;
      const bool second_custom = second.source == BindingSource::Custom;
      if (first_custom) {
        invalidated.push_back(static_cast<int>(i));
        first.row_error =
            Translate("The saved shortcut conflicts with \"%1\".").arg(second.spec.description);
      }
      if (second_custom) {
        invalidated.push_back(static_cast<int>(j));
        second.row_error =
            Translate("The saved shortcut conflicts with \"%1\".").arg(first.spec.description);
      }
    }
  }
  for (const int row : invalidated) {
    auto& entry    = entries_[static_cast<size_t>(row)];
    entry.bindings = {};
    entry.source   = BindingSource::InvalidSaved;
  }
}

auto ShortcutRegistry::WriteSavedState(const ShortcutCommandId& id, const QString& state,
                                       const QStringList& serialized) -> bool {
  settings_->beginGroup(kSettingsRoot);
  settings_->beginGroup(id);
  settings_->setValue(QStringLiteral("state"), state);
  if (serialized.isEmpty()) {
    settings_->remove(QStringLiteral("bindings"));
  } else {
    settings_->setValue(QStringLiteral("bindings"), serialized);
  }
  settings_->endGroup();
  settings_->endGroup();
  settings_->sync();
  return settings_->status() == QSettings::NoError;
}

auto ShortcutRegistry::RemoveSavedState(const ShortcutCommandId& id) -> bool {
  settings_->beginGroup(kSettingsRoot);
  settings_->remove(id);
  settings_->endGroup();
  settings_->sync();
  return settings_->status() == QSettings::NoError;
}

void ShortcutRegistry::ApplyBindings(Entry& entry, QList<ShortcutInput> bindings,
                                     BindingSource source) {
  entry.bindings  = std::move(bindings);
  entry.source    = source;
  entry.row_error = QString();
  UpdateActionShortcuts(entry);
}

void ShortcutRegistry::NotifyRowChanged(int row) {
  if (row < 0) {
    return;
  }
  const auto model_index = index(row, 0);
  emit       dataChanged(model_index, model_index,
                         {BindingTextRole, AssignedRole, UsesDefaultRole, ValidationErrorRole});
  emit       commandBindingChanged(entries_[static_cast<size_t>(row)].spec.id);
}

void ShortcutRegistry::UpdateActionShortcuts(Entry& entry) {
  if (entry.action == nullptr) {
    return;
  }
  QList<QKeySequence> sequences;
  for (const auto& input : entry.bindings) {
    if (input.kind == ShortcutInputKind::KeyChord) {
      sequences.push_back(QKeySequence(QKeyCombination(input.modifiers, input.key)));
    }
  }
  entry.action->setShortcuts(sequences);
  entry.action->setAutoRepeat(entry.spec.auto_repeat);
}

void RegisterShortcutRegistryQmlType() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  qmlRegisterSingletonType<ShortcutRegistry>("Alcedo.Main", 1, 0, "ShortcutRegistry",
                                             [](QQmlEngine* engine, QJSEngine*) -> QObject* {
                                               auto* registry = new ShortcutRegistry(engine);
                                               RegisterBuiltinShortcuts(registry);
                                               const auto errors = registry->FinalizeRegistration();
                                               for (const auto& error : errors) {
                                                 qWarning().noquote() << error;
                                               }
                                               return registry;
                                             });
}

}  // namespace alcedo::ui
