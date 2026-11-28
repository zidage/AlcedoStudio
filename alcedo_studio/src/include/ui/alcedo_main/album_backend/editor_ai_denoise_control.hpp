//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "app/editor_parameter_write.hpp"
#include "edit/input/raw_sensor_layout.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"

namespace alcedo::ui {

/**
 * @brief The AI Denoise switch of the editor (integration plan, DN7).
 *
 * Writes the switch as one `raw_decode` change through the adjustment submitter of the editor
 * session: on with the installed model version, off with no version. It keeps no copy of the
 * switch: the panel reads it from the panel projection (`raw.ai_denoise`). It also tells the
 * panel whether the platform can run the model (D13) and whether the open photo is a Bayer RAW
 * file (D14); the sensor layout is read from the file on a worker thread.
 *
 * Thread: the GUI thread. The services can be called on the GUI thread, except
 * `read_sensor_layout`, which runs on a worker thread.
 */
class EditorAiDenoiseControl final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ available CONSTANT)
  /// `bayer`, `xtrans`, `other`, or empty while unknown (not read yet, or the file cannot be
  /// read). The switch is enabled for `bayer` and while unknown.
  Q_PROPERTY(QString sensorLayout READ sensor_layout NOTIFY SensorChanged)

 public:
  struct Services {
    /// True when the platform can run the model (DenoiseModelSession::IsPlatformAvailable).
    std::function<bool()>                                        platform_available;
    /// The installed model version; throws AiDenoiseError (ModelNotInstalled) without a model.
    std::function<std::string()>                                 installed_model_version;
    /// The file of an image of the project; empty when it is unknown.
    std::function<std::filesystem::path(std::uint32_t image_id)> image_path;
    /// ReadRawSensorLayout in the product; runs on a worker thread.
    std::function<RawSensorLayout(const std::filesystem::path&)> read_sensor_layout;
  };

  /// Why the switch cannot be turned on (@ref PrepareTurnOn).
  enum class TurnOnRefusal : std::uint8_t {
    /// The platform has no forward code or runtime for the model (D13).
    PlatformUnavailable,
    /// No model is installed (the panel opens the AI Models page).
    ModelNotInstalled,
    /// The installed graph cannot be read.
    ModelUnreadable,
  };

  /// The installed model (@ref ReadInstalledModel).
  struct InstalledModel {
    /// The model version (D10); empty when @ref refusal is set.
    std::string                  version;
    /// ModelNotInstalled or ModelUnreadable when no version can be read.
    std::optional<TurnOnRefusal> refusal;
    /// The text of the refusal.
    std::string                  reason;
  };

  /// The `raw_decode` write that turns the switch on, or the reason why it cannot be written.
  struct TurnOnPreparation {
    /// The write with the installed model version; empty when @ref refusal is set.
    std::optional<EditorParameterWrite> write;
    std::optional<TurnOnRefusal>        refusal;
    /// The text of the refusal.
    std::string                         reason;
  };

  /// @param submitter The editor session; it outlives this object.
  EditorAiDenoiseControl(Services services, IEditorAdjustmentSubmitter* submitter,
                         QObject* parent = nullptr);

  /// The version of the installed model, or why it cannot be read. The first read hashes the
  /// graph file; a later read hashes it again only when its size or write time changed.
  [[nodiscard]] auto        ReadInstalledModel() const -> InstalledModel;

  /**
   * @brief The `raw_decode` write that turns AI Denoise on with the installed model version
   *        (D10), for the switch and for `denoise.set` of alcedo-cli (DN13). Writes nothing.
   *
   * Refuses PlatformUnavailable before it reads the model (@ref ReadInstalledModel).
   */
  [[nodiscard]] auto        PrepareTurnOn() const -> TurnOnPreparation;

  /// The `raw_decode` write that turns AI Denoise off (no version).
  [[nodiscard]] static auto TurnOffWrite() -> EditorParameterWrite;

  /**
   * @brief Turn AI Denoise on for the open photo: one `raw_decode` change with the installed
   *        model version.
   * @return `on` when the session accepted the change; `model_not_installed` when no model is
   *         installed (the panel then opens the AI Models page; nothing is written);
   *         `refused` when the platform cannot run the model, the version cannot be read, or the
   *         session refused the change.
   */
  Q_INVOKABLE QString       turnOn();

  /// Turn AI Denoise off for the open photo (the switch and `Turn Off AI Denoise`): one
  /// `raw_decode` change without a version. False when the session refused it.
  Q_INVOKABLE bool          turnOff();

  /// The open photo of the editor changed. Reads its sensor layout on a worker thread; until the
  /// result arrives, and for a file that cannot be read, the switch stays enabled.
  void                      SetImage(std::uint32_t image_id);

  [[nodiscard]] auto        available() const -> bool;
  [[nodiscard]] auto        sensor_layout() const -> QString { return sensor_layout_; }

 signals:
  void SensorChanged();

 private:
  /// The layout read for @p image_id; ignored when the editor shows another photo since.
  void     ApplySensorLayout(std::uint32_t image_id, std::optional<RawSensorLayout> layout);

  Services services_;
  IEditorAdjustmentSubmitter* submitter_ = nullptr;
  mutable std::optional<bool> available_;
  std::uint32_t               image_id_ = 0;
  QString                     sensor_layout_;
};

}  // namespace alcedo::ui
