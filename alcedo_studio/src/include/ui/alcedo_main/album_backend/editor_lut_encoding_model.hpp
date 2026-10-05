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
 * @brief Editor LUT encoding combo: the input or the output color encoding of the LMT.
 *
 * Entries are the catalog encodings (color::ColorEncodings), scene-referred first:
 * `{value: id, label: display name, group: "scene" | "display", groupLabel: translated group}`.
 * The default entry is ACEScc.
 *
 * Loading: the selected entry follows @ref target (LutLibraryController), which reads the
 * stored encodings of the same Color Grade as the LUT strength. Loading never submits.
 *
 * Writes: a user selection or reset submits one settled `lut` write that sets only this
 * side's encoding (EditorLutWrite::input_encoding or ::output_encoding). The reference,
 * strength and the other encoding stay as stored, so two selections in quick succession do
 * not overwrite each other. One selection is one history entry.
 */
class EditorLutEncodingModel : public EditorAdjustmentEnumModel {
  Q_OBJECT
  Q_PROPERTY(
      alcedo::ui::LutLibraryController* target READ target WRITE setTarget NOTIFY targetChanged)
  /// `input` or `output`: the side of the LUT this combo edits. Default `input`.
  Q_PROPERTY(QString side READ side WRITE setSide NOTIFY sideChanged)
  /// True when the selected encoding is display-referred. For the output side this means the
  /// ACES 2.0 inverse output transform brings the LUT result back to the scene.
  Q_PROPERTY(bool displayReferred READ displayReferred NOTIFY currentIndexChanged)

 public:
  explicit EditorLutEncodingModel(QObject* parent = nullptr);

  [[nodiscard]] auto target() const -> LutLibraryController* { return target_; }
  /// Follow @p target and load its stored encoding without submitting.
  void               setTarget(LutLibraryController* target);
  [[nodiscard]] auto side() const -> QString;
  /// @p side is `input` or `output`; any other value is ignored.
  void               setSide(const QString& side);
  [[nodiscard]] auto displayReferred() const -> bool;

 signals:
  void targetChanged();
  void sideChanged();

 protected:
  /// LMT update that sets this side's encoding to @p value only.
  [[nodiscard]] auto selectionWrite(const QString& value) const
      -> alcedo::EditorParameterWrite override;

 private:
  void                           loadFromTarget();

  QPointer<LutLibraryController> target_;
  bool                           output_side_ = false;
};

}  // namespace alcedo::ui
