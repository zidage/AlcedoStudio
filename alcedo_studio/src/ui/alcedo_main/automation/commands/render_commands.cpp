//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/editor_image_render_port.hpp"
#include "app/editor_session_service.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"

namespace alcedo::automation {
namespace {

constexpr int kDefaultPreviewLongEdge = 2048;
constexpr int kMinPreviewLongEdge     = 16;

/// One written PNG file.
struct PreviewFile {
  QString path_;
  int     width_  = 0;
  int     height_ = 0;
};

auto FileJson(const PreviewFile& file) -> QJsonObject {
  return QJsonObject{{"path", file.path_}, {"width", file.width_}, {"height", file.height_}};
}

/// Converts @p rendered and writes it to @p path. Returns the reason of a failure.
auto WritePng(const RenderedPipelineImage& rendered, const QString& path, PreviewFile* file)
    -> QString {
  ui::ComparisonPresentationImage converted;
  try {
    converted = ui::ConvertRenderedImageForComparison(rendered);
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  }
  if (!converted.image.save(path, "PNG")) {
    return QStringLiteral("The PNG file cannot be written: %1").arg(path);
  }
  file->path_   = path;
  file->width_  = converted.image.width();
  file->height_ = converted.image.height();
  return {};
}

/// The region parameter as a normalized edit-space rectangle; writes the reason to @p error.
auto ReadRegion(const QJsonObject& region, QString* error) -> std::optional<NormalizedRect> {
  NormalizedRect rect;
  rect.x = static_cast<float>(region.value("x").toDouble());
  rect.y = static_cast<float>(region.value("y").toDouble());
  rect.w = static_cast<float>(region.value("w").toDouble());
  rect.h = static_cast<float>(region.value("h").toDouble());
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    *error = QStringLiteral("the region must have a positive width and height");
    return std::nullopt;
  }
  if (rect.x + rect.w > 1.0f || rect.y + rect.h > 1.0f) {
    *error = QStringLiteral("the region must lie inside the image: x + w and y + h are at most 1");
    return std::nullopt;
  }
  return rect;
}

}  // namespace

auto RegisterAutomationRenderCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host, QString* error) -> bool {
  const QJsonObject unit{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  const QJsonObject file_schema =
      AutomationObjectSchema(QJsonObject{{"path", QJsonObject{{"type", "string"}}},
                                         {"width", QJsonObject{{"type", "integer"}}},
                                         {"height", QJsonObject{{"type", "integer"}}}},
                             QJsonArray{"path", "width", "height"});

  AutomationCommandSpec preview;
  preview.method      = QStringLiteral("render.preview");
  preview.description = QStringLiteral(
      "Renders the open image twice, with the current edits and as imported (the root), and "
      "writes <file_stem>_current.png and <file_stem>_root.png into out_dir. The long edge of "
      "each file is long_edge pixels at most. 'region' (x, y, w, h in 0..1 of the cropped image) "
      "renders only that part. Answers -32005 while the editor renders another image job, for "
      "example an open comparison; that job continues.");
  preview.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"out_dir", QJsonObject{{"type", "string"}, {"description", "Folder of the PNG files."}}},
          {"long_edge", QJsonObject{{"type", "integer"},
                                    {"minimum", kMinPreviewLongEdge},
                                    {"maximum", static_cast<int>(kQualityBaseMaxLongEdge)},
                                    {"default", kDefaultPreviewLongEdge}}},
          {"region", AutomationClosedParamsSchema(
                         QJsonObject{{"x", unit}, {"y", unit}, {"w", unit}, {"h", unit}},
                         QJsonArray{"x", "y", "w", "h"})}},
      QJsonArray{"out_dir"});
  preview.result_schema = AutomationObjectSchema(
      QJsonObject{{"current", file_schema}, {"root", file_schema}}, QJsonArray{"current", "root"});
  preview.handler = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor  = host->editor_session();
    IEditorSessionBackend*       backend = editor != nullptr ? editor->session_backend() : nullptr;
    if (backend == nullptr || !editor->has_image()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("No editor image is open."));
      return;
    }
    RenderRequest geometry = EditorImageRenderRequest{}.geometry;
    geometry.resolution.max_edge =
        static_cast<std::uint32_t>(params.value("long_edge").toInt(kDefaultPreviewLongEdge));
    if (params.contains("region")) {
      QString    reason;
      const auto region = ReadRegion(params.value("region").toObject(), &reason);
      if (!region.has_value()) {
        SendAutomationParamError(reply, QStringLiteral("/region"), reason);
        return;
      }
      geometry.view.visible_rect_in_edit_space = *region;
    }

    // The file stem of the photo names both files.
    QString stem = QStringLiteral("preview");
    if (const auto service = host->project()->handler().project()) {
      if (const auto browse = service->GetAlbumBrowseService()) {
        try {
          const std::array<sl_element_id_t, 1> ids{
              static_cast<sl_element_id_t>(editor->element_id())};
          const auto rows = browse->ReadAlbumFileRows(ids);
          if (!rows.empty() && !rows.front().file_name_.empty()) {
            stem = QFileInfo(QString::fromStdString(rows.front().file_name_)).completeBaseName();
          }
        } catch (const std::exception& e) {
          SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
          return;
        }
      }
    }
    const QString out_dir = QFileInfo(params.value("out_dir").toString()).absoluteFilePath();
    if (!QDir().mkpath(out_dir)) {
      SendAutomationParamError(reply, QStringLiteral("/out_dir"),
                               QStringLiteral("the folder cannot be created"));
      return;
    }
    const QString current_path = QDir(out_dir).filePath(stem + QStringLiteral("_current.png"));
    const QString root_path    = QDir(out_dir).filePath(stem + QStringLiteral("_root.png"));

    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    const auto answer = [wait](std::function<void(AutomationCommandWait*)> send) {
      QMetaObject::invokeMethod(
          QCoreApplication::instance(),
          [wait, send = std::move(send)] {
            if (wait != nullptr) {
              send(wait);
            }
          },
          Qt::QueuedConnection);
    };
    const auto submitted = backend->RenderPreviewImages(
        geometry, [answer, current_path, root_path](EditorPreviewImagesResult result) {
          const QString message = QString::fromStdString(result.message);
          if (result.status != EditorPreviewImagesStatus::Completed) {
            const AutomationErrorCode code = result.status == EditorPreviewImagesStatus::Busy
                                                 ? AutomationErrorCode::Busy
                                             : result.status == EditorPreviewImagesStatus::Rejected
                                                 ? AutomationErrorCode::Rejected
                                                 : AutomationErrorCode::Failed;
            answer([code, message](AutomationCommandWait* wait) {
              wait->SendOwnerError(code, message);
            });
            return;
          }
          // The completion runs on the editor render worker; the PNG encode runs off it so the
          // next viewport frame does not wait for it.
          auto images = std::make_shared<EditorPreviewImagesResult>(std::move(result));
          QThreadPool::globalInstance()->start([answer, images, current_path, root_path] {
            PreviewFile current;
            PreviewFile root;
            QString     failure = WritePng(images->current, current_path, &current);
            if (failure.isEmpty()) {
              failure = WritePng(images->root, root_path, &root);
            }
            answer([failure, current, root](AutomationCommandWait* wait) {
              if (!failure.isEmpty()) {
                wait->SendOwnerError(AutomationErrorCode::Failed, failure);
                return;
              }
              wait->SendResult(
                  QJsonObject{{"current", FileJson(current)}, {"root", FileJson(root)}});
            });
          });
        });
    if (submitted.kind == EditorSessionResultKind::Rejected && wait != nullptr) {
      wait->SendOwnerError(AutomationErrorCode::Rejected,
                           QString::fromStdString(submitted.message));
    }
  };
  return registry.Register(std::move(preview), error);
}

}  // namespace alcedo::automation
