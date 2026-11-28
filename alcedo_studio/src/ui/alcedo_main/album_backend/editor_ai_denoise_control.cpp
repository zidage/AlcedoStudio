//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_ai_denoise_control.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <exception>
#include <utility>

#include "app/editor_parameter_write.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/ai_denoise_error.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::ui {
namespace {

constexpr auto kRawDecodeField = "raw_decode";

auto           LayoutName(RawSensorLayout layout) -> QString {
  switch (layout) {
    case RawSensorLayout::Bayer:
      return QStringLiteral("bayer");
    case RawSensorLayout::XTrans:
      return QStringLiteral("xtrans");
    case RawSensorLayout::Other:
      return QStringLiteral("other");
  }
  return QStringLiteral("other");
}

auto RawDecodeWrite(std::optional<DevelopAiDenoise> denoise) -> EditorParameterWrite {
  DevelopRawDecodeUpdate update;
  update.ai_denoise = std::move(denoise);
  return update;
}

}  // namespace

EditorAiDenoiseControl::EditorAiDenoiseControl(Services                    services,
                                               IEditorAdjustmentSubmitter* submitter,
                                               QObject*                    parent)
    : QObject(parent), services_(std::move(services)), submitter_(submitter) {}

auto EditorAiDenoiseControl::available() const -> bool {
  if (!available_.has_value()) {
    available_ = services_.platform_available && services_.platform_available();
  }
  return *available_;
}

auto EditorAiDenoiseControl::ReadInstalledModel() const -> InstalledModel {
  InstalledModel model;
  try {
    model.version = services_.installed_model_version();
  } catch (const AiDenoiseError& error) {
    model.refusal = error.Kind() == AiDenoiseErrorKind::ModelNotInstalled
                        ? TurnOnRefusal::ModelNotInstalled
                        : TurnOnRefusal::ModelUnreadable;
    model.reason  = error.Text();
  } catch (const std::exception& error) {
    model.refusal = TurnOnRefusal::ModelUnreadable;
    model.reason  = std::string("AI Denoise: ") + error.what();
  }
  return model;
}

auto EditorAiDenoiseControl::PrepareTurnOn() const -> TurnOnPreparation {
  TurnOnPreparation preparation;
  if (!available()) {
    preparation.refusal = TurnOnRefusal::PlatformUnavailable;
    preparation.reason  = "AI Denoise: this computer has no supported device for the model";
    return preparation;
  }
  auto model = ReadInstalledModel();
  if (model.refusal.has_value()) {
    preparation.refusal = model.refusal;
    preparation.reason  = std::move(model.reason);
    return preparation;
  }
  preparation.write = RawDecodeWrite(DevelopAiDenoise{std::move(model.version)});
  return preparation;
}

auto EditorAiDenoiseControl::TurnOffWrite() -> EditorParameterWrite {
  return RawDecodeWrite(std::nullopt);
}

auto EditorAiDenoiseControl::turnOn() -> QString {
  if (submitter_ == nullptr) {
    return QStringLiteral("refused");
  }
  auto preparation = PrepareTurnOn();
  if (preparation.refusal.has_value()) {
    if (*preparation.refusal == TurnOnRefusal::ModelNotInstalled) {
      return QStringLiteral("model_not_installed");
    }
    if (*preparation.refusal == TurnOnRefusal::ModelUnreadable) {
      ALCEDO_LOG_WARN(Render) << preparation.reason;
    }
    return QStringLiteral("refused");
  }
  const bool accepted = submitter_->submitWrite(QString::fromLatin1(kRawDecodeField),
                                                std::move(*preparation.write), true);
  return accepted ? QStringLiteral("on") : QStringLiteral("refused");
}

auto EditorAiDenoiseControl::turnOff() -> bool {
  if (submitter_ == nullptr) {
    return false;
  }
  return submitter_->submitWrite(QString::fromLatin1(kRawDecodeField), TurnOffWrite(), true);
}

void EditorAiDenoiseControl::SetImage(std::uint32_t image_id) {
  if (image_id == image_id_) {
    return;
  }
  image_id_ = image_id;
  if (!sensor_layout_.isEmpty()) {
    sensor_layout_.clear();
    emit SensorChanged();
  }
  if (image_id == 0 || !services_.image_path || !services_.read_sensor_layout) {
    return;
  }
  const auto path = services_.image_path(image_id);
  if (path.empty()) {
    return;
  }
  QPointer<EditorAiDenoiseControl> self(this);
  QThreadPool::globalInstance()->start(
      [self, image_id, path, read = services_.read_sensor_layout]() {
        std::optional<RawSensorLayout> layout;
        try {
          layout = read(path);
        } catch (const std::exception& error) {
          // The switch stays enabled; a render reports the error of the file.
          ALCEDO_LOG_WARN(Render) << "AI Denoise: the sensor layout is unknown: " << error.what();
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, image_id, layout]() {
              if (self) {
                self->ApplySensorLayout(image_id, layout);
              }
            },
            Qt::QueuedConnection);
      });
}

void EditorAiDenoiseControl::ApplySensorLayout(std::uint32_t                  image_id,
                                               std::optional<RawSensorLayout> layout) {
  if (image_id != image_id_ || !layout.has_value()) {
    return;
  }
  const auto name = LayoutName(*layout);
  if (name == sensor_layout_) {
    return;
  }
  sensor_layout_ = name;
  emit SensorChanged();
}

}  // namespace alcedo::ui
