//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_comparison_controller.hpp"

#include <QVariantMap>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "app/editor_history_types.hpp"
#include "app/editor_session_service.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "type/hash_type.hpp"

namespace alcedo::ui {
namespace {

constexpr auto kRootValue    = "root";
constexpr auto kCurrentValue = "current";
constexpr auto kVersionValue = "version:";

}  // namespace

EditorComparisonController::EditorComparisonController(EditorSessionController*              session,
                                                       std::shared_ptr<ComparisonImageStore> store,
                                                       QObject*                              parent)
    : QObject(parent), session_(session), store_(std::move(store)) {
  if (session_) {
    connect(session_, &EditorSessionController::StateChanged, this,
            &EditorComparisonController::Refresh);
    connect(session_, &EditorSessionController::ActionAvailabilityChanged, this,
            &EditorComparisonController::AvailabilityChanged);
  }
  Refresh();
}

EditorComparisonController::~EditorComparisonController() {
  if (store_) {
    store_->Clear();
  }
}

auto EditorComparisonController::Backend() const -> alcedo::IEditorSessionBackend* {
  return session_ ? session_->session_backend() : nullptr;
}

auto EditorComparisonController::status() const -> QString {
  switch (state_.status) {
    case alcedo::EditorComparisonStatus::Inactive:
      return QStringLiteral("idle");
    case alcedo::EditorComparisonStatus::Rendering:
      return QStringLiteral("loading");
    case alcedo::EditorComparisonStatus::Ready:
      if (failed_pair_id_ != 0 && failed_pair_id_ == state_.pair_id) {
        return QStringLiteral("failed");
      }
      // The pair is shown only after both images are in the store.
      return published_pair_id_ == state_.pair_id ? QStringLiteral("ready")
                                                  : QStringLiteral("loading");
    case alcedo::EditorComparisonStatus::Failed:
      return QStringLiteral("failed");
  }
  return QStringLiteral("idle");
}

auto EditorComparisonController::error_text() const -> QString {
  if (state_.status == alcedo::EditorComparisonStatus::Failed) {
    return QString::fromStdString(state_.error);
  }
  if (state_.status == alcedo::EditorComparisonStatus::Ready && failed_pair_id_ != 0 &&
      failed_pair_id_ == state_.pair_id) {
    return failed_pair_reason_;
  }
  return {};
}

auto EditorComparisonController::comparison_kind() const -> QString {
  return state_.kind == alcedo::EditorComparisonKind::Versions ? QStringLiteral("versions")
                                                               : QStringLiteral("beforeAfter");
}

auto EditorComparisonController::can_open() const -> bool {
  return session_ && session_->actions()->can_open_comparison();
}

auto EditorComparisonController::open_unavailable_reason() const -> QString {
  return session_ ? session_->actions()->open_comparison_reason() : QString{};
}

void EditorComparisonController::openBeforeAfter() { Open(alcedo::EditorComparisonKind::BeforeAfter); }

void EditorComparisonController::openVersions() { Open(alcedo::EditorComparisonKind::Versions); }

void EditorComparisonController::Open(alcedo::EditorComparisonKind kind) {
  if (auto* backend = Backend()) {
    (void)backend->OpenComparison(kind);
  }
}

void EditorComparisonController::selectKind(const QString& kind) {
  if (!state_.active()) {
    return;
  }
  if (kind == QLatin1String("beforeAfter")) {
    SelectSources(alcedo::EditorComparisonKind::BeforeAfter, alcedo::EditorComparisonSource::Root(),
                  alcedo::EditorComparisonSource::Current());
  } else if (kind == QLatin1String("versions")) {
    SelectSources(alcedo::EditorComparisonKind::Versions, state_.a, state_.b);
  }
}

void EditorComparisonController::selectASource(const QString& value) {
  alcedo::EditorComparisonSource source;
  if (!state_.active() || !ParseSourceValue(value, &source)) {
    return;
  }
  SelectSources(alcedo::EditorComparisonKind::Versions, source, state_.b);
}

void EditorComparisonController::selectBSource(const QString& value) {
  alcedo::EditorComparisonSource source;
  if (!state_.active() || !ParseSourceValue(value, &source)) {
    return;
  }
  SelectSources(alcedo::EditorComparisonKind::Versions, state_.a, source);
}

void EditorComparisonController::SelectSources(alcedo::EditorComparisonKind          kind,
                                               const alcedo::EditorComparisonSource& a,
                                               const alcedo::EditorComparisonSource& b) {
  if (auto* backend = Backend()) {
    (void)backend->SelectComparisonSources(kind, a, b);
  }
}

void EditorComparisonController::setDisplayMode(const QString& mode) {
  const QString next =
      mode == QLatin1String("complete") ? QStringLiteral("complete") : QStringLiteral("divider");
  if (next == display_mode_) {
    return;
  }
  display_mode_ = next;
  emit ViewChanged();
}

void EditorComparisonController::setOrientation(const QString& orientation) {
  const QString next = orientation == QLatin1String("vertical") ? QStringLiteral("vertical")
                                                                : QStringLiteral("horizontal");
  if (next == orientation_) {
    return;
  }
  orientation_ = next;
  emit ViewChanged();
}

void EditorComparisonController::setDividerPosition(double position) {
  const double next = std::clamp(position, 0.0, 1.0);
  if (next == divider_position_) {
    return;
  }
  divider_position_ = next;
  emit ViewChanged();
}

void EditorComparisonController::swap() {
  swapped_ = !swapped_;
  emit ViewChanged();
}

void EditorComparisonController::retry() {
  if (!state_.active()) {
    return;
  }
  failed_pair_id_ = 0;
  failed_pair_reason_.clear();
  if (auto* backend = Backend()) {
    (void)backend->RetryComparison();
  }
}

void EditorComparisonController::close() {
  if (session_) {
    session_->CloseComparison(true);
  }
}

void EditorComparisonController::reportImageLoadFailed(const QString& message) {
  if (!state_.active() || state_.pair_id == 0) {
    return;
  }
  failed_pair_id_     = state_.pair_id;
  failed_pair_reason_ = message;
  ClearPair();
  emit ComparisonChanged();
}

void EditorComparisonController::Refresh() {
  auto* backend = Backend();
  auto  next    = backend ? backend->comparison_state() : alcedo::EditorComparisonState{};
  if (!next.active()) {
    if (state_.active() || published_pair_id_ != 0) {
      ClearPair();
      state_          = {};
      failed_pair_id_ = 0;
      failed_pair_reason_.clear();
      source_options_.clear();
      emit ComparisonChanged();
    }
    return;
  }
  if (next.operation_id != state_.operation_id) {
    // A new comparison: fresh view defaults and the Version list of the image now open.
    ClearPair();
    failed_pair_id_ = 0;
    failed_pair_reason_.clear();
    ResetViewOptions();
    state_ = next;
    RebuildSourceOptions();
  } else {
    state_ = next;
  }
  if (state_.status == alcedo::EditorComparisonStatus::Ready) {
    PublishReadyPair();
  } else if (published_pair_id_ != 0) {
    // A new selection renders: the previous pair leaves the display.
    ClearPair();
  }
  emit ComparisonChanged();
}

void EditorComparisonController::PublishReadyPair() {
  const auto pair_id = state_.pair_id;
  if (pair_id == 0 || pair_id == published_pair_id_ || pair_id == failed_pair_id_) {
    return;
  }
  ClearPair();
  auto* backend = Backend();
  auto  images  = backend ? backend->TakeComparisonImages(pair_id)
                          : std::vector<alcedo::RenderedPipelineImage>{};
  try {
    if (images.size() != 2) {
      throw std::runtime_error("The comparison images are not available");
    }
    if (!store_) {
      throw std::runtime_error("The comparison image store is not available");
    }
    const auto publication = PublishComparisonPair(*store_, pair_id, images[0], images[1]);
    pair_                  = publication.ToVariantMap();
    published_pair_id_     = pair_id;
    emit PairChanged();
  } catch (const std::exception& ex) {
    failed_pair_id_     = pair_id;
    failed_pair_reason_ = QString::fromStdString(ex.what());
  }
  // The float images are released here; only the 8-bit store images remain.
}

void EditorComparisonController::ClearPair() {
  if (store_) {
    store_->Clear();
  }
  const bool had_pair = pair_.isValid();
  pair_               = QVariant{};
  published_pair_id_  = 0;
  if (had_pair) {
    emit PairChanged();
  }
}

void EditorComparisonController::ResetViewOptions() {
  display_mode_     = QStringLiteral("divider");
  orientation_      = QStringLiteral("horizontal");
  divider_position_ = 0.5;
  swapped_          = false;
  emit ViewChanged();
}

void EditorComparisonController::RebuildSourceOptions() {
  source_options_.clear();
  auto add = [this](QString value, QString label) {
    QVariantMap option;
    option.insert(QStringLiteral("value"), std::move(value));
    option.insert(QStringLiteral("label"), std::move(label));
    source_options_.push_back(option);
  };
  add(QString::fromLatin1(kRootValue), tr("Unadjusted"));
  add(QString::fromLatin1(kCurrentValue), tr("Current working state"));
  if (session_ == nullptr) {
    return;
  }
  // Versions come from the history owner's projection; the controller keeps no catalog.
  const auto history = session_->history_snapshot();
  for (const auto& version : history.versions) {
    const auto name  = QString::fromStdString(version.display_name);
    const auto label = version.active ? tr("%1 (working state)").arg(name) : name;
    add(SourceValue(alcedo::EditorComparisonSource::Version(version.version_id)), label);
  }
}

auto EditorComparisonController::SourceLabel(const alcedo::EditorComparisonSource& source) const
    -> QString {
  const auto value = SourceValue(source);
  for (const auto& entry : source_options_) {
    const auto option = entry.toMap();
    if (option.value(QStringLiteral("value")).toString() == value) {
      return option.value(QStringLiteral("label")).toString();
    }
  }
  return {};
}

auto EditorComparisonController::SourceValue(const alcedo::EditorComparisonSource& source)
    -> QString {
  switch (source.kind) {
    case alcedo::EditorComparisonSourceKind::Root:
      return QString::fromLatin1(kRootValue);
    case alcedo::EditorComparisonSourceKind::Current:
      return QString::fromLatin1(kCurrentValue);
    case alcedo::EditorComparisonSourceKind::Version:
      return QString::fromLatin1(kVersionValue) + QString::fromStdString(source.version_id.ToString());
  }
  return {};
}

auto EditorComparisonController::ParseSourceValue(const QString&                  value,
                                                  alcedo::EditorComparisonSource* source) -> bool {
  if (source == nullptr) {
    return false;
  }
  if (value == QLatin1String(kRootValue)) {
    *source = alcedo::EditorComparisonSource::Root();
    return true;
  }
  if (value == QLatin1String(kCurrentValue)) {
    *source = alcedo::EditorComparisonSource::Current();
    return true;
  }
  if (!value.startsWith(QLatin1String(kVersionValue))) {
    return false;
  }
  try {
    *source = alcedo::EditorComparisonSource::Version(alcedo::Hash128::FromString(
        value.mid(static_cast<int>(qstrlen(kVersionValue))).toStdString()));
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace alcedo::ui
