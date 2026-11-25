//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/preset_preview_controller.hpp"

#include <QDebug>
#include <utility>

#include "app/preset_preview_image.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {
namespace {

/// Production source: the open photo of the editor session and the entries of the library.
class EditorSessionPresetPreviewSource final : public PresetPreviewSource {
 public:
  EditorSessionPresetPreviewSource(EditorSessionController*      session,
                                   alcedo::PresetLibraryService* library)
      : session_(session), library_(library) {}

  [[nodiscard]] auto ImageId() const -> std::uint64_t override {
    return session_ ? session_->image_id() : 0;
  }
  [[nodiscard]] auto EntryFingerprint(const QString& entry_id) const
      -> std::optional<std::string> override {
    if (!library_) return std::nullopt;
    const auto* entry = library_->FindEntry(entry_id.toStdString());
    if (entry == nullptr || !entry->available || entry->fingerprint.empty()) return std::nullopt;
    return entry->fingerprint;
  }
  [[nodiscard]] auto SensorKey() const -> std::string override {
    auto document = session_ ? session_->pipeline_document() : nullptr;
    if (!document) return {};
    // The published document is immutable; its key is computed once per publication.
    if (document != keyed_document_) {
      keyed_document_ = std::move(document);
      sensor_key_     = PresetPreviewSensorKey(*keyed_document_);
    }
    return sensor_key_;
  }
  [[nodiscard]] auto ViewportRendering() const -> bool override {
    return session_ && session_->render_busy();
  }
  [[nodiscard]] auto PackageFor(const QString& entry_id) -> PresetPackageResult override {
    if (!library_) {
      PresetPackageResult refused;
      refused.message = "The preset library is not available";
      return refused;
    }
    return library_->PackageFor(entry_id.toStdString());
  }
  auto RenderPresetPreview(EditorPresetPreviewRequest     request,
                           EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool override {
    auto* backend = session_ ? session_->session_backend() : nullptr;
    if (backend == nullptr) {
      if (error != nullptr) *error = "The editor session is not available";
      return false;
    }
    const auto submitted = backend->RenderPresetPreview(std::move(request), std::move(on_complete));
    if (submitted.kind == EditorSessionResultKind::Rejected) {
      if (error != nullptr) *error = submitted.message;
      return false;
    }
    return true;
  }

 private:
  QPointer<EditorSessionController>                       session_;
  QPointer<alcedo::PresetLibraryService>                  library_;
  /// The document whose sensor key is @ref sensor_key_; held so that its address is not reused.
  mutable std::shared_ptr<const alcedo::PipelineDocument> keyed_document_;
  mutable std::string                                     sensor_key_;
};

}  // namespace

auto PresetPreviewSource::ShowLibraryPhoto(sl_element_id_t /*element_id*/, image_id_t /*image_id*/,
                                           std::string* error) -> bool {
  if (error != nullptr) *error = "The previews follow the photo that is open in the editor";
  return false;
}

PresetPreviewController::PresetPreviewController(QObject* parent)
    : BrowserPreviewController(SharedPresetPreviewImageStore(), parent) {}

PresetPreviewController::~PresetPreviewController() = default;

void PresetPreviewController::SetEditorSession(EditorSessionController* session) {
  if (session_ == session) return;
  if (session_) disconnect(session_, nullptr, this, nullptr);
  session_ = session;
  if (session_) {
    // Opening, switching, and closing a photo, and each publication of the working document (a
    // sensor setting change included), report here.
    connect(session_, &EditorSessionController::StateChanged, this,
            &PresetPreviewController::Refresh);
    // The viewport finished its frames: the waiting previews may start.
    connect(session_, &EditorSessionController::RenderBusyChanged, this,
            &PresetPreviewController::ResumeAfterViewport);
  }
  RebuildSessionSource();
  Refresh();
}

void PresetPreviewController::SetLibrary(alcedo::PresetLibraryService* library) {
  if (library_ == library) return;
  if (library_) disconnect(library_, nullptr, this, nullptr);
  library_ = library;
  if (library_) {
    // A scan gives the fingerprints of the files (plan D9: no file watcher).
    connect(library_, &alcedo::PresetLibraryService::InventoryChanged, this,
            &PresetPreviewController::Refresh);
  }
  RebuildSessionSource();
  Refresh();
}

void PresetPreviewController::SetSource(PresetPreviewSource* source) {
  session_source_.reset();
  source_          = source;
  follows_session_ = false;
}

void PresetPreviewController::SetOwnedSource(std::unique_ptr<PresetPreviewSource> source) {
  session_source_  = std::move(source);
  source_          = session_source_.get();
  follows_session_ = false;
  Refresh();
}

auto PresetPreviewController::showLibraryPhoto(uint elementId, uint imageId) -> bool {
  if (source_ == nullptr) return false;
  std::string error;
  const bool  shown = source_->ShowLibraryPhoto(static_cast<sl_element_id_t>(elementId),
                                                static_cast<image_id_t>(imageId), &error);
  if (!shown) {
    qCWarning(alcedo::diag::editorLog).noquote()
        << LogName() << "of photo" << elementId << "not shown:" << QString::fromStdString(error);
  }
  Refresh();
  return shown;
}

void PresetPreviewController::RebuildSessionSource() {
  if (!follows_session_) return;
  session_source_ = std::make_unique<EditorSessionPresetPreviewSource>(session_, library_);
  source_         = session_source_.get();
}

auto PresetPreviewController::PreviewImageId() const -> std::uint64_t {
  return source_ != nullptr ? source_->ImageId() : 0;
}

auto PresetPreviewController::EntryKey(const QString& entry_id) const -> std::optional<QString> {
  if (source_ == nullptr) return std::nullopt;
  const auto fingerprint = source_->EntryFingerprint(entry_id);
  if (!fingerprint) return std::nullopt;
  const std::string sensor_key = source_->SensorKey();
  if (sensor_key.empty()) return std::nullopt;
  return entry_id + QLatin1Char('|') + QString::fromStdString(*fingerprint) + QLatin1Char('|') +
         QString::fromStdString(sensor_key);
}

auto PresetPreviewController::RenderedEntryKey(const QString& requested_entry_key,
                                               const EditorBrowserPreviewResult& result) const
    -> QString {
  if (result.sensor_key.empty()) return requested_entry_key;
  // The last part of the key is the sensor key; the session reports the one that it rendered.
  return requested_entry_key.section(QLatin1Char('|'), 0, -2) + QLatin1Char('|') +
         QString::fromStdString(result.sensor_key);
}

auto PresetPreviewController::ViewportRendering() const -> bool {
  return source_ == nullptr || source_->ViewportRendering();
}

auto PresetPreviewController::StartRender(const QString& entry_id, std::uint64_t image_id,
                                          EditorBrowserPreviewCompletion on_complete,
                                          std::string*                   error) -> bool {
  if (source_ == nullptr) {
    if (error != nullptr) *error = "The preset preview source is not available";
    return false;
  }
  auto package = source_->PackageFor(entry_id);
  if (!package.Ok()) {
    // The file changed since the scan and fails a rule now: the preview fails for this key, as a
    // failed render does, and the tile shows the warning icon.
    EditorBrowserPreviewResult failed;
    failed.status  = EditorBrowserPreviewStatus::Failed;
    failed.message = package.message.empty() ? "The preset cannot be read" : package.message;
    on_complete(std::move(failed));
    return true;
  }
  EditorPresetPreviewRequest request;
  request.image_id = static_cast<image_id_t>(image_id);
  request.package  = std::move(package.package);
  return source_->RenderPresetPreview(std::move(request), std::move(on_complete), error);
}

auto PresetPreviewController::LogName() const -> QString {
  return QStringLiteral("Preset preview");
}

}  // namespace alcedo::ui
