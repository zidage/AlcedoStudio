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
#include <QStringList>
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

/// Converts @p rendered and writes it as PNG to @p out, which the caller created. Returns the
/// reason of a failure.
auto WritePng(const RenderedPipelineImage& rendered, AutomationNewOutputFile& out,
              PreviewFile* file) -> QString {
  ui::ComparisonPresentationImage converted;
  try {
    converted = ui::ConvertRenderedImageForComparison(rendered);
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  }
  if (const QString failure = out.WritePng(converted.image); !failure.isEmpty()) {
    return failure;
  }
  file->path_   = out.path();
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

/// The file_stem parameter: one file name part, without a folder. Writes the reason to @p error.
auto ReadFileStem(const QString& stem, QString* error) -> bool {
  if (stem.isEmpty() || stem == QStringLiteral(".") || stem == QStringLiteral("..")) {
    *error = QStringLiteral("the file stem must be a file name");
    return false;
  }
  if (stem.contains(QLatin1Char('/')) || stem.contains(QLatin1Char('\\')) ||
      stem.contains(QLatin1Char(':'))) {
    *error = QStringLiteral("the file stem must not contain '/', '\\', or ':'");
    return false;
  }
  return true;
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
      "Renders the open image with the current edits and writes <file_stem>_current.png into "
      "out_dir. With compare 'root' (the default), it also renders the image as imported (the "
      "root) and writes <file_stem>_root.png; with compare 'none', it renders and writes only the "
      "current file. file_stem defaults to the photo file name without its extension. The long "
      "edge of each file is long_edge pixels at most. 'region' (x, y, w, h in 0..1 of the "
      "cropped image) renders only that part. The files must not exist: the command creates new "
      "files and does not replace existing ones. Answers -32005 while the editor renders another "
      "image job, for example an open comparison; that job continues.");
  preview.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"out_dir", QJsonObject{{"type", "string"}, {"description", "Folder of the PNG files."}}},
          {"long_edge", QJsonObject{{"type", "integer"},
                                    {"minimum", kMinPreviewLongEdge},
                                    {"maximum", static_cast<int>(kQualityBaseMaxLongEdge)},
                                    {"default", kDefaultPreviewLongEdge}}},
          {"region", AutomationClosedParamsSchema(
                         QJsonObject{{"x", unit}, {"y", unit}, {"w", unit}, {"h", unit}},
                         QJsonArray{"x", "y", "w", "h"})},
          {"compare", QJsonObject{{"type", "string"},
                                  {"enum", QJsonArray{"none", "root"}},
                                  {"default", "root"},
                                  {"description", "'root' also writes the imported image."}}},
          {"file_stem",
           QJsonObject{{"type", "string"},
                       {"description",
                        "First part of the file names. Default: the photo file name without "
                        "its extension."}}}},
      QJsonArray{"out_dir"});
  preview.result_schema = AutomationObjectSchema(
      QJsonObject{{"current", file_schema}, {"root", file_schema}}, QJsonArray{"current"});
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

    const bool with_root =
        params.value("compare").toString(QStringLiteral("root")) == QStringLiteral("root");
    QString stem = params.value("file_stem").toString(QStringLiteral("preview"));
    if (params.contains("file_stem")) {
      QString reason;
      if (!ReadFileStem(stem, &reason)) {
        SendAutomationParamError(reply, QStringLiteral("/file_stem"), reason);
        return;
      }
    } else if (const auto service = host->project()->handler().project()) {
      // Without file_stem, the file name of the photo names the files.
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
    // An early answer before the render. The exclusive create below keeps a file that appears
    // after this check.
    for (const QString& path :
         with_root ? QStringList{current_path, root_path} : QStringList{current_path}) {
      if (const QString existing = AutomationExistingOutputReason(path); !existing.isEmpty()) {
        SendAutomationParamError(reply, QStringLiteral("/out_dir"), existing);
        return;
      }
    }

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
    const EditorPreviewImageSet image_set =
        with_root ? EditorPreviewImageSet::CurrentAndRoot : EditorPreviewImageSet::CurrentOnly;
    const auto submitted = backend->RenderPreviewImages(
        geometry, image_set,
        [answer, current_path, root_path, with_root](EditorPreviewImagesResult result) {
          const QString message = QString::fromStdString(result.message);
          if (result.status != EditorPreviewImagesStatus::Completed) {
            const AutomationErrorCode code = result.status == EditorPreviewImagesStatus::Rejected
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
          QThreadPool::globalInstance()->start(
              [answer, images, current_path, root_path, with_root] {
                // Both files are created before either is written, and a failure removes both,
                // so an error answer leaves no file of this request.
                AutomationNewOutputFile current_file(current_path);
                AutomationNewOutputFile root_file(root_path);
                PreviewFile             current;
                PreviewFile             root;
                QString                 failure = current_file.Create();
                if (failure.isEmpty() && with_root) {
                  failure = root_file.Create();
                }
                if (failure.isEmpty()) {
                  failure = WritePng(images->current, current_file, &current);
                }
                if (failure.isEmpty() && with_root) {
                  failure = WritePng(images->root, root_file, &root);
                }
                if (failure.isEmpty()) {
                  current_file.Keep();
                  root_file.Keep();
                } else {
                  current_file.Discard();
                  root_file.Discard();
                }
                answer([failure, current, root, with_root](AutomationCommandWait* wait) {
                  if (!failure.isEmpty()) {
                    wait->SendOwnerError(AutomationErrorCode::Failed, failure);
                    return;
                  }
                  QJsonObject files{{"current", FileJson(current)}};
                  if (with_root) {
                    files.insert("root", FileJson(root));
                  }
                  wait->SendResult(files);
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
