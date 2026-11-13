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
 * ParseEditorParameterWrite, which resolves the identity in DcpProfileLibrary::Shared(). When the
 * parse or the submit fails (for example, the file was removed after the scan), the model logs the
 * reason and returns to the row of the stored reference.
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

  /**
   * @brief Programmatic load of the `camera_profile_file` entry of an adjustment snapshot.
   *
   * Reads `raster`, `embedded_default`, `selected`, `resolved`, `camera_model`, and
   * `profile_name`, composes the rows, and sets the current row to the stored reference (row 0
   * when no reference is stored). Does not submit. A snapshot without the entry leaves the model
   * unchanged.
   */
  Q_INVOKABLE void   loadFromSnapshot(const QVariantMap& snapshot);

 signals:
  void libraryChanged();
  void cameraChanged();
  void storedReferenceChanged();

 protected:
  auto submitSelection() -> bool override;

 private:
  /// The reference of the last loaded snapshot.
  struct StoredReference {
    bool        raster           = false;
    bool        embedded_default = false;
    bool        selected         = false;
    bool        resolved         = false;
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
};

}  // namespace alcedo::ui
