//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointer>
#include <QString>

#include "app/editor_parameter_write.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

namespace alcedo::ui {

/**
 * @brief Small Editor LUT control: the LUT strength and the target's association summary.
 *
 * The value is the strength in percent (0-100, default 100). Pointer drags, keyboard edits,
 * and reset use the EditorAdjustmentValueModel input rules and submit strength-only `lut`
 * writes through the submitter, so the selected LUT is never changed here (plan 6.2).
 *
 * The strength and association are loaded from @ref target (LutLibraryController), which
 * reads the same Color Grade target as the LUT browser. Loading never submits; a load that
 * arrives during a pointer drag does not move the value under the pointer.
 */
class EditorLutAdjustmentModel : public EditorAdjustmentValueModel {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::LutLibraryController* target READ target WRITE setTarget NOTIFY
                 targetChanged)
  Q_PROPERTY(bool hasAssociation READ hasAssociation NOTIFY associationChanged)
  Q_PROPERTY(QString associationName READ associationName NOTIFY associationChanged)
  /// True when the associated LUT file is missing; its reference and strength are kept.
  Q_PROPERTY(bool missing READ missing NOTIFY associationChanged)
  /// One line describing the association: its name, `Missing: <name>`, or `No LUT`.
  Q_PROPERTY(QString statusText READ statusText NOTIFY associationChanged)

 public:
  explicit EditorLutAdjustmentModel(QObject* parent = nullptr);

  [[nodiscard]] auto target() const -> LutLibraryController* { return target_; }
  /// Follow @p target and load its strength and association without submitting.
  void               setTarget(LutLibraryController* target);
  [[nodiscard]] auto hasAssociation() const -> bool;
  [[nodiscard]] auto associationName() const -> QString;
  [[nodiscard]] auto missing() const -> bool;
  [[nodiscard]] auto statusText() const -> QString;

 signals:
  void targetChanged();
  void associationChanged();

 protected:
  /// A strength-only LMT update; the reference and name stay unchanged.
  [[nodiscard]] auto valueWrite(double v) const -> alcedo::EditorParameterWrite override;

 private:
  void                           loadFromTarget();

  QPointer<LutLibraryController> target_;
};

}  // namespace alcedo::ui
