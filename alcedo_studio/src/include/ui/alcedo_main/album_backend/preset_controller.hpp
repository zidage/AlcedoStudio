//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>

#include "app/adjustment_transfer_types.hpp"
#include "app/preset_library_service.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/preset_list_model.hpp"

namespace alcedo::ui {

/**
 * @brief Default file name of an exported Version (issue #1124).
 *
 * `<photo file name without extension>_<Version name>.alcpreset`, made valid with
 * PresetNameFromLabel; only the Version name when @p photoFileName is empty.
 */
[[nodiscard]] auto VersionExportFileName(const QString& photoFileName, const QString& versionName)
    -> QString;

/**
 * @brief QML commands of the preset browser (edit presets plan 6.1, `appModules.presets`).
 *
 * Routes each command to its owner: preset file changes to PresetLibraryService, an apply to the
 * open photo to EditorSessionController::PasteAdjustmentPackage (the Paste of the Versions
 * panel), an apply to library photos to AdjustmentTransferController::PastePackageToTargets (the
 * multi-target Paste of the Apply Preset dialog), and Save as Preset of the Copy dialog to
 * AdjustmentTransferDialogModel::BuildPackage.
 * Owns the preset list model and the stored height of the preset part; keeps no package and no
 * entry of its own. Owner (GUI) thread only.
 *
 * Each command returns `{success, message?}` at once. The file operations of the service end
 * later; @ref operationFinished reports them. An apply to the open photo is refused while an
 * automation agent holds control (the refusal of PasteAdjustmentPackage).
 */
class PresetController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(alcedo::ui::PresetListModel* presets READ presets CONSTANT)
  Q_PROPERTY(alcedo::PresetLibraryService* library READ library CONSTANT)
  /// Height of the preset part of the Versions panel, in pixels; stored in `presets/browserHeight`.
  Q_PROPERTY(
      double browserHeight READ browser_height WRITE SetBrowserHeight NOTIFY BrowserHeightChanged)
  /// Layout of the preset browsers, "grid" or "list"; stored in `presets/browserViewMode`.
  Q_PROPERTY(QString browserViewMode READ browser_view_mode WRITE SetBrowserViewMode NOTIFY
                 BrowserViewModeChanged)

 public:
  /// Opens a folder in the file manager. Returns false when the system refuses.
  using OpenUrl        = std::function<bool(const QUrl&)>;
  /// Asks for a file to write in the system save dialog: (title, suggested path, name filter).
  /// Returns the chosen native path, or an empty string when the user cancels.
  using ChooseSaveFile =
      std::function<QString(const QString&, const QString&, const QString&)>;

  /**
   * @param library Owner of the preset folder; must outlive this object.
   * @param copy_dialog Selection of the Copy dialog, for Save as Preset; may be null.
   * @param open_url Defaults to QDesktopServices::openUrl. Tests pass a stand-in.
   * @param choose_save_file Defaults to QFileDialog::getSaveFileName. Tests pass a stand-in.
   */
  PresetController(alcedo::PresetLibraryService*  library,
                   AdjustmentTransferDialogModel* copy_dialog, OpenUrl open_url = {},
                   ChooseSaveFile choose_save_file = {}, QObject* parent = nullptr);

  [[nodiscard]] auto      presets() -> PresetListModel* { return presets_; }
  [[nodiscard]] auto      library() -> alcedo::PresetLibraryService* { return library_; }
  [[nodiscard]] auto      browser_height() const -> double { return browser_height_; }
  void                    SetBrowserHeight(double height);
  [[nodiscard]] auto      browser_view_mode() const -> QString { return browser_view_mode_; }
  /// Ignores a value other than "grid" and "list".
  void                    SetBrowserViewMode(const QString& view_mode);
  /// The session of the open photo: the source of Save by drag and the target of an apply.
  void                    SetEditorSession(EditorSessionController* editor_session);
  /// The owner of the multi-target Paste, for an apply to library photos (plan 2.3).
  void                    SetAdjustmentTransfer(AdjustmentTransferController* adjustment_transfer);

  /**
   * @brief Save the Version @p versionId of the open photo as a preset (plan 2.2, D4).
   *
   * Reads the history of the open photo through the editor session, replays the Version (it does
   * not need to be active, and stays as it is), selects every transferable item except Crop and
   * Rotate and Lens Correction, and saves the package under the Version name made valid
   * (PresetNameFromLabel). The service makes the name unique.
   */
  Q_INVOKABLE QVariantMap SaveVersionAsPreset(const QString& versionId);
  /**
   * @brief Export the Version @p versionId of the open photo as a preset file (issue #1124).
   *
   * The same package as SaveVersionAsPreset. Opens the system save dialog in the folder of the
   * last export (else Documents) with VersionExportFileName, then queues
   * PresetLibraryService::Export to the chosen file; the file does not enter the preset folder.
   *
   * @return `{success, message?, pending}`, or `{success: true, canceled: true}` when the user
   *         closes the dialog. @ref operationFinished reports the write as `export`.
   */
  Q_INVOKABLE QVariantMap ExportVersion(const QString& versionId);
  /// Save the selection of the Copy dialog as a preset named after the selected Version.
  Q_INVOKABLE QVariantMap SaveDialogSelectionAsPreset();
  /// Copy the `.alcpreset` files of @p urls (local file URLs or paths) into the folder.
  Q_INVOKABLE QVariantMap AddFiles(const QVariantList& urls);
  /// Apply the listed preset @p presetId to the open photo as a new active Version (plan D7).
  Q_INVOKABLE QVariantMap ApplyToEditor(const QString& presetId);
  /**
   * @brief Apply the preset file @p url to the open photo one time (requirement R8).
   *
   * Reads the file with ReadPresetFile and pastes its package; the file does not enter the
   * folder, the inventory, or the package cache. The Version name is the file name without the
   * extension.
   */
  Q_INVOKABLE QVariantMap ApplyFileToEditor(const QVariant& url);
  /**
   * @brief Apply the listed preset @p presetId to the photos @p targetEntries (plan 2.3, R6).
   *
   * Each target gets one new Version named after the preset. The package is read one time
   * (PresetLibraryService::PackageFor) for all targets. The routing is the multi-target Paste
   * (AdjustmentTransferController::PastePackageToTargets): the photo open in the editor goes
   * through the editor session, the others through the AdjustmentPaste background task, whose
   * result AdjustmentTransferController::PasteFinished reports.
   *
   * @param targetEntries `{elementId, imageId}` maps, as the image context menu gives them.
   * @return `{success, message?, pending}` at once; a refused preset starts nothing.
   */
  Q_INVOKABLE QVariantMap ApplyToTargets(const QString&      presetId,
                                         const QVariantList& targetEntries);
  /**
   * @brief Apply the preset file @p url one time to the photos @p targetEntries (R8).
   *
   * Reads the file with ReadPresetFile; it does not enter the folder, the inventory, or the
   * package cache. The Versions are named after the file name without the extension. Otherwise
   * as @ref ApplyToTargets.
   */
  Q_INVOKABLE QVariantMap ApplyFileToTargets(const QVariant&     url,
                                             const QVariantList& targetEntries);
  Q_INVOKABLE QVariantMap Rename(const QString& presetId, const QString& name);
  /// Move the preset @p presetId to the system trash (plan D8).
  Q_INVOKABLE QVariantMap Delete(const QString& presetId);
  /// Open the folder that holds the preset @p presetId.
  Q_INVOKABLE QVariantMap ShowInFolder(const QString& presetId);
  /// Open the preset folder.
  Q_INVOKABLE QVariantMap ShowFolder();
  Q_INVOKABLE void        Refresh();

 signals:
  void BrowserHeightChanged();
  void BrowserViewModeChanged();
  /**
   * @brief A file operation of the service ended.
   *
   * `{operation: "scan"|"useRoot"|"save"|"add"|"rename"|"delete"|"export", success, message,
   * presetIds}`;
   * presetIds lists the new or renamed presets.
   */
  void OperationFinished(const QVariantMap& result);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void operationFinished(const QVariantMap& result);

 private:
  /// The checked package of the listed, available preset @p presetId and its name; null with
  /// @p error otherwise.
  auto ListedPackage(const QString& presetId, QString* name, QString* error)
      -> std::shared_ptr<const alcedo::AdjustmentTransferPackage>;
  /// The preset package of the Version @p versionId of the open photo (every transferable item
  /// except Crop and Rotate and Lens Correction) and its Version name; null with @p error.
  auto VersionPackage(const QString& versionId, QString* versionName, QString* error)
      -> std::shared_ptr<const alcedo::AdjustmentTransferPackage>;
  auto PasteIntoEditor(const alcedo::AdjustmentTransferPackage& package, const QString& name)
      -> QVariantMap;
  auto PasteIntoTargets(const alcedo::AdjustmentTransferPackage& package,
                        const QVariantList& targetEntries, const QString& name) -> QVariantMap;
  auto SavePackage(const alcedo::AdjustmentTransferPackage& package, const QString& label)
      -> QVariantMap;
  auto OpenFolder(const std::filesystem::path& folder) -> QVariantMap;
  void PublishOperation();

  QPointer<alcedo::PresetLibraryService>  library_;
  QPointer<AdjustmentTransferDialogModel> copy_dialog_;
  QPointer<EditorSessionController>       editor_session_;
  QPointer<AdjustmentTransferController>  adjustment_transfer_;
  OpenUrl                                 open_url_;
  ChooseSaveFile                          choose_save_file_;
  PresetListModel*                       presets_        = nullptr;  // owned child
  double                                  browser_height_ = 0.0;
  QString                                 browser_view_mode_;
};

}  // namespace alcedo::ui
