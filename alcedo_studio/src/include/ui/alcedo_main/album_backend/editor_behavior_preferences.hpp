//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <string>

namespace alcedo::ui {

/**
 * @brief User-level editor defaults.
 *
 * Owns two persisted values. The first is shown on the Settings "Default Behavior" page: the
 * choice for adding a Mask while a deletion-protected
 * (locked) Color Grade is selected. The default Color Grade carries the
 * application's default adjustments and is locked, so a Mask drawn there also
 * scopes those defaults. The action decides whether the editor asks, inserts a
 * new layer above the stack first, or draws on the locked node.
 *
 * Values: "ask" (default), "newLayer", "currentNode". Stored in QSettings under
 * `editor/lockedNodeMaskAction`; unknown stored values read as "ask".
 *
 * The second is the remembered film print settings (issue 672): the `film_print` object of the
 * last settled user write of the Display Transform panel with method `film_print`. The panel
 * applies them when the user selects method Film Print on a document of another method. Stored in
 * QSettings under `editor/filmPrint/lastSettings` as the JSON text of the object in the form of
 * DrtParamsModel::ToJson. A stored value that DrtParamsModel does not load reads as no value.
 * Automation writes do not pass through the panel, so they do not change it.
 *
 * Threading: GUI thread only. Side effects: QSettings write and
 * lockedNodeMaskActionChanged or filmPrintUiValueChanged on an accepted change.
 */
class EditorBehaviorPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString lockedNodeMaskAction READ locked_node_mask_action WRITE setLockedNodeMaskAction
                 NOTIFY lockedNodeMaskActionChanged)
  /// The `odt` UI value of the remembered film print settings: method `film_print`, every field of
  /// the film print, and the defaults of the other fields. Empty when no settings are remembered.
  Q_PROPERTY(QVariantMap filmPrintUiValue READ film_print_ui_value NOTIFY filmPrintUiValueChanged)

 public:
  static constexpr auto kLockedNodeMaskActionSettingKey = "editor/lockedNodeMaskAction";
  static constexpr auto kAskAction                      = "ask";
  static constexpr auto kNewLayerAction                 = "newLayer";
  static constexpr auto kCurrentNodeAction              = "currentNode";
  static constexpr auto kFilmPrintSettingsKey           = "editor/filmPrint/lastSettings";

  explicit EditorBehaviorPreferences(QObject* parent = nullptr);

  [[nodiscard]] auto locked_node_mask_action() const -> QString { return locked_node_mask_action_; }
  /**
   * @brief Persist @p action for Mask creation on a locked Color Grade.
   * @return false for values outside ask/newLayer/currentNode; nothing changes.
   */
  Q_INVOKABLE bool   setLockedNodeMaskAction(const QString& action);

  [[nodiscard]] static auto IsLockedNodeMaskAction(const QString& action) -> bool;

  [[nodiscard]] auto film_print_ui_value() const -> QVariantMap { return film_print_ui_value_; }
  /**
   * @brief Persist the film print settings of a settled user write of the Display Transform panel.
   *
   * @param odtModelJson The `odt` Model JSON of the write: `{"odt": {...}}` or the object itself.
   * @return false, with nothing changed, when the write does not have method `film_print`, or when
   *         DrtParamsModel does not load its `film_print` object. True when the settings are
   *         stored; settings equal to the stored settings give no notification.
   */
  Q_INVOKABLE bool   rememberFilmPrint(const QString& odtModelJson);

 signals:
  void lockedNodeMaskActionChanged();
  void filmPrintUiValueChanged();

 private:
  QString     locked_node_mask_action_;
  /// JSON text of the remembered `film_print` object in the form of DrtParamsModel::ToJson. Empty
  /// when no settings are remembered.
  std::string film_print_json_;
  QVariantMap film_print_ui_value_;
};

}  // namespace alcedo::ui
