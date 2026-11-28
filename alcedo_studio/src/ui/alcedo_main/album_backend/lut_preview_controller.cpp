//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_preview_controller.hpp"

#include <utility>

#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"

namespace alcedo::ui {
namespace {

auto ToQString(const std::string& text) -> QString { return QString::fromStdString(text); }

/// Production source: the open photo of the editor session and the entries of the library.
class EditorSessionLutPreviewSource final : public LutPreviewSource {
 public:
  EditorSessionLutPreviewSource(EditorSessionController*   session,
                                alcedo::LutLibraryService* library)
      : session_(session), library_(library) {}

  [[nodiscard]] auto ImageId() const -> std::uint64_t override {
    return session_ ? session_->image_id() : 0;
  }
  [[nodiscard]] auto EntryWrite(const QString& entry_id, EditorLutWrite* write) const
      -> bool override {
    return library_ && LutLibraryController::ReadEntryWrite(*library_, entry_id, write, nullptr);
  }
  [[nodiscard]] auto ViewportRendering() const -> bool override {
    return session_ && session_->render_busy();
  }
  auto RenderLutPreview(EditorLutPreviewRequest request, EditorBrowserPreviewCompletion on_complete,
                        std::string* error) -> bool override {
    auto* backend = session_ ? session_->session_backend() : nullptr;
    if (backend == nullptr) {
      if (error != nullptr) *error = "The editor session is not available";
      return false;
    }
    const auto submitted = backend->RenderLutPreview(std::move(request), std::move(on_complete));
    if (submitted.kind == EditorSessionResultKind::Rejected) {
      if (error != nullptr) *error = submitted.message;
      return false;
    }
    return true;
  }

 private:
  QPointer<EditorSessionController>   session_;
  QPointer<alcedo::LutLibraryService> library_;
};

}  // namespace

LutPreviewController::LutPreviewController(QObject* parent)
    : BrowserPreviewController(SharedLutPreviewImageStore(), parent) {}

LutPreviewController::~LutPreviewController() = default;

void LutPreviewController::setEditorSession(EditorSessionController* session) {
  if (session_ == session) return;
  if (session_) disconnect(session_, nullptr, this, nullptr);
  session_ = session;
  if (session_) {
    // Opening, switching, and closing a photo, and the interactive state, all report here.
    connect(session_, &EditorSessionController::StateChanged, this, &LutPreviewController::Refresh);
    // The viewport finished its frames: the waiting previews may start.
    connect(session_, &EditorSessionController::RenderBusyChanged, this,
            &LutPreviewController::ResumeAfterViewport);
  }
  RebuildSessionSource();
  emit editorSessionChanged();
  Refresh();
}

void LutPreviewController::setLibrary(alcedo::LutLibraryService* library) {
  if (library_ == library) return;
  if (library_) disconnect(library_, nullptr, this, nullptr);
  library_ = library;
  if (library_) {
    // Entries and their remembered encodings make the keys.
    connect(library_, &alcedo::LutLibraryService::InventoryChanged, this,
            &LutPreviewController::Refresh);
    connect(library_, &alcedo::LutLibraryService::RootChanged, this,
            &LutPreviewController::Refresh);
    connect(library_, &alcedo::LutLibraryService::RememberedEncodingsChanged, this,
            &LutPreviewController::Refresh);
  }
  RebuildSessionSource();
  emit libraryChanged();
  Refresh();
}

void LutPreviewController::SetSource(LutPreviewSource* source) {
  session_source_.reset();
  source_ = source;
}

void LutPreviewController::RebuildSessionSource() {
  session_source_ = std::make_unique<EditorSessionLutPreviewSource>(session_, library_);
  source_         = session_source_.get();
}

auto LutPreviewController::PreviewImageId() const -> std::uint64_t {
  return source_ != nullptr ? source_->ImageId() : 0;
}

auto LutPreviewController::EntryKey(const QString& entry_id) const -> std::optional<QString> {
  if (source_ == nullptr) return std::nullopt;
  EditorLutWrite write;
  if (!source_->EntryWrite(entry_id, &write)) return std::nullopt;
  return entry_id + QLatin1Char('|') + ToQString(write.input_encoding.value_or("")) +
         QLatin1Char('|') + ToQString(write.output_encoding.value_or(""));
}

auto LutPreviewController::ViewportRendering() const -> bool {
  return source_ == nullptr || source_->ViewportRendering();
}

auto LutPreviewController::StartRender(const QString& entry_id, std::uint64_t image_id,
                                       EditorBrowserPreviewCompletion on_complete,
                                       std::string*                   error) -> bool {
  EditorLutPreviewRequest request;
  request.image_id = static_cast<image_id_t>(image_id);
  if (source_ == nullptr || !source_->EntryWrite(entry_id, &request.write)) {
    if (error != nullptr) *error = "The LUT entry is no longer listed";
    return false;
  }
  return source_->RenderLutPreview(std::move(request), std::move(on_complete), error);
}

auto LutPreviewController::LogName() const -> QString { return QStringLiteral("LUT preview"); }

}  // namespace alcedo::ui
