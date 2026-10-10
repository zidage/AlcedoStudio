//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace alcedo::ui {

/**
 * @brief QML view over EditorParameterCatalog (QML type `EditorParameterCatalog`).
 *
 * The RAW Decode and Geometry panels read their option lists, ranges, and defaults here and
 * build their parameter writes with the catalog conversions. Labels are the English catalog
 * text; the panels translate them with qsTr in their own translation context.
 */
class EditorParameterCatalogAdapter : public QObject {
  Q_OBJECT

 public:
  explicit EditorParameterCatalogAdapter(QObject* parent = nullptr);

  /// The `editor.catalog` form of @p field. An empty map for a field that is not in the catalog.
  Q_INVOKABLE QVariantMap  entry(const QString& field) const;

  /// The `editor.catalog` form of property @p name of object field @p field, or an empty map.
  Q_INVOKABLE QVariantMap  propertySpec(const QString& field, const QString& name) const;

  /// The `{value, label}` choices of option property @p name of @p field, in menu order.
  Q_INVOKABLE QVariantList options(const QString& field, const QString& name) const;

  /// UI value of the default of object field @p field.
  Q_INVOKABLE QVariantMap  uiDefault(const QString& field) const;

  /// UI value of object field @p field from its snapshot entry (Model JSON or the panel
  /// projection form). An empty map (and a warning) when the entry is not valid.
  Q_INVOKABLE QVariantMap  uiValue(const QString& field, const QVariant& snapshotEntry) const;

  /**
   * @brief Parameter JSON of one write of object field @p field from the complete panel state.
   *
   * @return The compact JSON for submitPatch, or an empty string when the state is not valid.
   *         The failure detail goes to the warning log; submitPatch rejects the empty string.
   */
  Q_INVOKABLE QString      modelParamsJson(const QString& field, const QVariantMap& uiState) const;

  /**
   * @brief Apply the Geometry panel constraint to the crop state after the change @p driver.
   *
   * @param driver `none`, `position`, `width`, `height`, `rotation`, `aspect_preset`, or
   *        `aspect_size`.
   * @param sourceWidth,sourceHeight Presented source size, zero when no frame is presented.
   * @return The complete constrained state, or an empty map (and a warning) when @p cropState
   *         is not valid.
   */
  Q_INVOKABLE QVariantMap  constrainCrop(const QVariantMap& cropState, const QString& driver,
                                         int sourceWidth, int sourceHeight) const;

  /// Width / height of the locked crop frame oriented to the source, or undefined when the
  /// aspect is not locked.
  Q_INVOKABLE QVariant     cropLockedAspectRatio(const QVariantMap& cropState, int sourceWidth,
                                                 int sourceHeight) const;
};

}  // namespace alcedo::ui
