//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_preview_controller.hpp"

#include <QDebug>
#include <QMetaObject>
#include <exception>
#include <utility>

#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"
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
  auto RenderLutPreview(EditorLutPreviewRequest request, EditorLutPreviewCompletion on_complete,
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
    : QObject(parent), store_(SharedLutPreviewImageStore()) {}

LutPreviewController::~LutPreviewController() {
  for (const auto& shown : std::as_const(shown_)) {
    store_->Remove(shown.display_id);
  }
}

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

void LutPreviewController::SetImageStore(std::shared_ptr<LutPreviewImageStore> store) {
  for (const auto& shown : std::as_const(shown_)) {
    store_->Remove(shown.display_id);
  }
  shown_.clear();
  store_ = store ? std::move(store) : SharedLutPreviewImageStore();
  BumpRevision();
}

void LutPreviewController::RebuildSessionSource() {
  session_source_ = std::make_unique<EditorSessionLutPreviewSource>(session_, library_);
  source_         = session_source_.get();
}

void LutPreviewController::watch(const QString& entry_id) {
  if (entry_id.isEmpty()) return;
  if (++watched_[entry_id] > 1) return;
  RefreshEntry(entry_id);
  Pump();
}

void LutPreviewController::unwatch(const QString& entry_id) {
  const auto found = watched_.find(entry_id);
  if (found == watched_.end()) return;
  if (--found.value() > 0) return;
  watched_.erase(found);
  std::erase(pending_, entry_id);
  // The tile that showed it is gone; the LRU keeps the pixels for the next watch.
  Hide(entry_id, /*notify=*/false);
}

auto LutPreviewController::previewUrl(const QString& entry_id, int /*revision*/) const -> QString {
  const auto found = shown_.constFind(entry_id);
  return found == shown_.constEnd() ? QString{} : found->url;
}

void LutPreviewController::Refresh() {
  const std::uint64_t image_id = source_ != nullptr ? source_->ImageId() : 0;
  if (image_id != image_id_) {
    image_id_          = image_id;
    // Previews of the previous photo must not stay on the tiles.
    const auto entries = shown_.keys();
    for (const auto& entry_id : entries) {
      Hide(entry_id, /*notify=*/false);
    }
    pending_.clear();
    failed_.clear();
    BumpRevision();
  }
  const auto entries = watched_.keys();
  for (const auto& entry_id : entries) {
    RefreshEntry(entry_id);
  }
  Pump();
}

auto LutPreviewController::KeyFor(const QString& entry_id, EditorLutWrite* write) const
    -> std::optional<QString> {
  if (image_id_ == 0 || source_ == nullptr) return std::nullopt;
  EditorLutWrite entry_write;
  if (!source_->EntryWrite(entry_id, &entry_write)) return std::nullopt;
  const QString key = QString::number(image_id_) + QLatin1Char('|') + entry_id + QLatin1Char('|') +
                      ToQString(entry_write.input_encoding.value_or("")) + QLatin1Char('|') +
                      ToQString(entry_write.output_encoding.value_or(""));
  if (write != nullptr) *write = std::move(entry_write);
  return key;
}

void LutPreviewController::RefreshEntry(const QString& entry_id) {
  const auto key = KeyFor(entry_id, nullptr);
  if (!key) {
    Hide(entry_id, /*notify=*/true);
    return;
  }
  const auto shown = shown_.constFind(entry_id);
  if (shown != shown_.constEnd() && shown->key == *key) return;
  if (auto image = CachedImage(*key)) {
    Show(entry_id, *key, *image);
    return;
  }
  // The encodings changed: the old preview shows another LUT result.
  Hide(entry_id, /*notify=*/true);
  if (!failed_.contains(*key) && !(in_flight_ && in_flight_->key == *key)) {
    Enqueue(entry_id);
  }
}

void LutPreviewController::Show(const QString& entry_id, const QString& key, const QImage& image) {
  Hide(entry_id, /*notify=*/false);
  const std::uint64_t display_id = next_display_id_++;
  shown_.insert(entry_id, ShownPreview{key, display_id, store_->Put(display_id, image)});
  BumpRevision();
}

void LutPreviewController::Hide(const QString& entry_id, bool notify) {
  const auto found = shown_.find(entry_id);
  if (found == shown_.end()) return;
  store_->Remove(found->display_id);
  shown_.erase(found);
  if (notify) BumpRevision();
}

void LutPreviewController::Enqueue(const QString& entry_id) {
  std::erase(pending_, entry_id);
  // The most recently watched entry renders first: it is the one the user scrolled to.
  pending_.push_front(entry_id);
}

void LutPreviewController::Pump() {
  // A preview starts only while the viewport does not render. The editor render worker runs
  // frames and previews in order, so a frame never waits behind a run of previews: during a slider
  // drag the previews stop, and the next one starts when the viewport is idle again.
  while (!in_flight_ && !pending_.empty() && source_ != nullptr && !source_->ViewportRendering()) {
    const QString entry_id = pending_.front();
    pending_.pop_front();
    if (!watched_.contains(entry_id)) continue;
    EditorLutWrite write;
    const auto     key = KeyFor(entry_id, &write);
    if (!key || failed_.contains(*key)) continue;
    if (auto image = CachedImage(*key)) {
      Show(entry_id, *key, *image);
      continue;
    }

    in_flight_ = InFlight{entry_id, *key};
    // One render at a time; a refused command stops the queue until the next refresh.
    SubmitRender(entry_id, *key, std::move(write));
    return;
  }
}

void LutPreviewController::ResumeAfterViewport() { Pump(); }

void LutPreviewController::SubmitRender(const QString& entry_id, const QString& key,
                                        EditorLutWrite write) {
  ++render_count_;
  EditorLutPreviewRequest request;
  request.image_id = static_cast<image_id_t>(image_id_);
  request.write    = std::move(write);
  QPointer<LutPreviewController> self(this);
  std::string                    error;
  const bool                     submitted = source_->RenderLutPreview(
      std::move(request),
      [self, entry_id, preview_key = key](EditorLutPreviewResult result) {
        // Runs on the session owner or the editor render worker.
        QMetaObject::invokeMethod(
            self,
            [self, entry_id, preview_key, result = std::move(result)]() mutable {
              if (self) self->OnRendered(entry_id, preview_key, std::move(result));
            },
            Qt::QueuedConnection);
      },
      &error);
  if (!submitted) {
    // The session refused the command; a later state change refreshes the tiles again.
    in_flight_.reset();
    qWarning().noquote() << "LUT preview not started:" << ToQString(error);
  }
}

void LutPreviewController::OnRendered(const QString& entry_id, const QString& key,
                                      EditorLutPreviewResult result) {
  if (in_flight_ && in_flight_->key == key) in_flight_.reset();
  if (result.status == EditorLutPreviewStatus::Completed) {
    try {
      auto presentation = ConvertRenderedImageForComparison(result.image);
      CachePut(key, std::move(presentation.image));
    } catch (const std::exception& ex) {
      failed_.insert(key);
      qWarning().noquote() << "LUT preview of" << entry_id << "cannot be shown:" << ex.what();
    }
  } else if (result.status == EditorLutPreviewStatus::Failed) {
    failed_.insert(key);
    qWarning().noquote() << "LUT preview of" << entry_id << "failed:" << ToQString(result.message);
  }
  // Rejected: the session cannot render now; the entry renders after the next refresh.
  if (watched_.contains(entry_id)) {
    const auto current = KeyFor(entry_id, nullptr);
    if (current && *current == key) {
      if (auto image = CachedImage(key)) Show(entry_id, key, *image);
    }
  }
  Pump();
}

auto LutPreviewController::CachedImage(const QString& key) -> std::optional<QImage> {
  const auto found = lru_index_.find(key);
  if (found == lru_index_.end()) return std::nullopt;
  lru_.splice(lru_.begin(), lru_, found.value());
  return lru_.front().second;
}

void LutPreviewController::CachePut(const QString& key, QImage image) {
  if (image.isNull()) return;
  if (const auto found = lru_index_.find(key); found != lru_index_.end()) {
    lru_.erase(found.value());
    lru_index_.erase(found);
  }
  lru_.emplace_front(key, std::move(image));
  lru_index_.insert(key, lru_.begin());
  while (lru_.size() > kLutPreviewCacheCapacity) {
    lru_index_.remove(lru_.back().first);
    lru_.pop_back();
  }
}

void LutPreviewController::BumpRevision() {
  ++revision_;
  emit revisionChanged();
}

}  // namespace alcedo::ui
