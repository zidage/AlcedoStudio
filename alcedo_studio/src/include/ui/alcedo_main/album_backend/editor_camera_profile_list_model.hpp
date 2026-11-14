//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <string>
#include <vector>

#include "app/camera_profile_library_service.hpp"
#include "image/dcp_file_reader.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

namespace alcedo::ui {

/**
 * @brief Rows of the Camera Profile combo of the RAW panel (dcp_camera_profile_plan.md, C5).
 *
 * The rows are the `entries` of the enum model, so that AdjustmentCombo and the settled submit of
 * EditorAdjustmentEnumModel serve the combo. Each entry is a map with the keys `value`, `label`,
 * `cameraModel`, `profileName`, `isDefault`, and `isMissing`:
 * - row 0 is the default entry: "Built-in", or "Embedded" for a DNG file with an embedded
 *   profile;
 * - then each profile of DcpProfileLibrary::Match(cameraMake, cameraModel), in name order;
 * - then, when the stored reference names a profile that the match does not hold, one row with
 *   `isMissing` true and the stored profile name.
 * A raster document has no rows.
 *
 * The matched profiles are read again when the camera identity changes and after each completed
 * library scan (CameraProfileLibraryService::ScanFinished), never on a selection. While a scan
 * runs the match is not read (DcpProfileLibrary::Match would block the GUI thread until the scan
 * ends), so the rows hold no matched profile until ScanFinished.
 * @ref loadFromSnapshot reads the stored reference from the panel projection and sets the current
 * row without a submit; the model keeps no selection of its own.
 *
 * A user selection submits `{"camera_model", "profile_name"}` (the default entry: null) through
 * ParseEditorParameterWrite, which resolves the identity in DcpProfileLibrary::Shared(). The
 * selection keeps the Tone Curve switch of the stored reference: `"profile_tone": false` joins it
 * when a stored reference has the switch off. When the parse or the submit fails (for example, the
 * file was removed after the scan), the model logs the reason and returns to the row of the stored
 * reference.
 *
 * The Tone Curve switch of the section (dcp_profile_tone_curve_study.md, section 6.4) reads
 * @ref toneCurveAvailable, @ref profileTone, and @ref filmPrintMethod, and submits its settled
 * write with @ref submitProfileTone.
 *
 * Thread affinity: GUI thread.
 */
class EditorCameraProfileListModel : public EditorAdjustmentEnumModel {
  Q_OBJECT
  /// CameraProfileLibraryService that owns the DCP folder (QML: appModules.cameraProfiles).
  Q_PROPERTY(QObject* library READ library WRITE setLibrary NOTIFY libraryChanged)
  /// Camera identity of the image RAW color context (EditorSessionController::cameraMake).
  Q_PROPERTY(QString cameraMake READ cameraMake WRITE setCameraMake NOTIFY cameraChanged)
  Q_PROPERTY(QString cameraModel READ cameraModel WRITE setCameraModel NOTIFY cameraChanged)
  /// True when the stored reference is not bound: the render fails (plan decision P-4) and the
  /// combo shows the stored name in the warning color.
  Q_PROPERTY(bool currentUnresolved READ currentUnresolved NOTIFY storedReferenceChanged)
  /// True for a raster document, which has no camera profile and no rows.
  Q_PROPERTY(bool raster READ raster NOTIFY storedReferenceChanged)
  /// True when the stored reference is bound and its profile has a `ProfileToneCurve`: the
  /// section shows the Tone Curve switch.
  Q_PROPERTY(bool toneCurveAvailable READ toneCurveAvailable NOTIFY storedReferenceChanged)
  /// The stored `profile_tone` switch; false without a reference.
  Q_PROPERTY(bool profileTone READ profileTone NOTIFY storedReferenceChanged)
  /// True when the DRT method of the loaded snapshot is Film Print, which runs no profile tone
  /// stage (study section 6.2): the switch is disabled.
  Q_PROPERTY(bool filmPrintMethod READ filmPrintMethod NOTIFY filmPrintMethodChanged)

 public:
  explicit EditorCameraProfileListModel(QObject* parent = nullptr);

  [[nodiscard]] auto library() const -> QObject*;
  void               setLibrary(QObject* library);
  [[nodiscard]] auto cameraMake() const -> QString { return camera_make_; }
  void               setCameraMake(const QString& make);
  [[nodiscard]] auto cameraModel() const -> QString { return camera_model_; }
  void               setCameraModel(const QString& model);
  [[nodiscard]] auto currentUnresolved() const -> bool {
    return stored_.selected && !stored_.resolved;
  }
  [[nodiscard]] auto raster() const -> bool { return stored_.raster; }
  [[nodiscard]] auto toneCurveAvailable() const -> bool {
    return stored_.selected && stored_.resolved && stored_.has_tone_curve;
  }
  [[nodiscard]] auto profileTone() const -> bool { return stored_.profile_tone; }
  [[nodiscard]] auto filmPrintMethod() const -> bool { return film_print_method_; }

  /**
   * @brief Programmatic load of the `camera_profile_file` entry of an adjustment snapshot.
   *
   * Reads `raster`, `embedded_default`, `selected`, `resolved`, `has_tone_curve`,
   * `profile_tone`, `camera_model`, and `profile_name`, composes the rows, and sets the current row
   * to the stored reference (row 0 when no reference is stored). The `odt` entry gives
   * @ref filmPrintMethod. Does not submit. A snapshot without an entry leaves its part of the
   * model unchanged.
   */
  Q_INVOKABLE void   loadFromSnapshot(const QVariantMap& snapshot);

  /**
   * @brief Submit one settled `camera_profile_file` write that sets the Tone Curve switch of the
   *        stored reference to @p on: `{"camera_model", "profile_name", "profile_tone"}`.
   *
   * The write resolves the stored identity in DcpProfileLibrary::Shared(), as a selection does.
   * The switch follows the echo of the write (@ref loadFromSnapshot); the model keeps no switch
   * value of its own.
   * @return false, after a log line with the reason, when no reference is stored, @p on equals the
   *         stored value, the parse fails, or the session rejects the write.
   */
  Q_INVOKABLE bool   submitProfileTone(bool on);

 signals:
  void libraryChanged();
  void cameraChanged();
  void storedReferenceChanged();
  void filmPrintMethodChanged();

 protected:
  auto submitSelection() -> bool override;

 private:
  /// The reference of the last loaded snapshot.
  struct StoredReference {
    bool        raster           = false;
    bool        embedded_default = false;
    bool        selected         = false;
    bool        resolved         = false;
    bool        has_tone_curve   = false;
    bool        profile_tone     = false;
    std::string camera_model;
    std::string profile_name;
  };

  /// Query the library for the camera and compose the rows.
  void                                  ReadMatchedProfiles();
  /// Entries from the matched profiles and the stored reference, then the current row.
  void                                  ComposeEntries();
  [[nodiscard]] auto                    StoredRow() const -> int;
  [[nodiscard]] auto                    RowWriteJson(int row) const -> std::string;

  QPointer<CameraProfileLibraryService> library_;
  QString                               camera_make_;
  QString                               camera_model_;
  std::vector<DcpProfileIdentity>       matched_;
  StoredReference                       stored_;
  bool                                  film_print_method_ = false;
};

}  // namespace alcedo::ui
