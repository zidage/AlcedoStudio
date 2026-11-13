//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/search_session.hpp"

#include <algorithm>
#include <utility>

namespace alcedo::ui {

SearchSession::SearchSession(SearchSessionPort port, QObject* parent)
    : QObject(parent), port_(std::move(port)) {}

auto SearchSession::RouteFor(const QString& query) const -> QString {
  const QString trimmed = query.trimmed();
  if (trimmed.isEmpty()) {
    return QStringLiteral("empty");
  }
  if (port_.natural_language_enabled() &&
      port_.classify_query(trimmed) == QStringLiteral("semantic")) {
    return QStringLiteral("semantic");
  }
  return QStringLiteral("fuzzy");
}

void SearchSession::Preview(const QString& query) {
  const QString trimmed = query.trimmed();
  const QString route   = RouteFor(trimmed);
  if (route == QStringLiteral("fuzzy")) {
    if (ClearRows()) {
      emit ResultsChanged();
    }
    Request(trimmed, 0, kPreviewRows, false, true);
    return;
  }
  // An empty query shows the recommendations; a semantic query runs only when Enter applies it
  // to the library, so typing does not reach the semantic provider.
  DropActiveRequest();
  const bool rows_changed = ClearRows();
  ClearStatus();
  query_           = trimmed;
  route_           = route;
  semantic_active_ = false;
  awaiting_submit_ = route == QStringLiteral("semantic");
  if (rows_changed) {
    emit ResultsChanged();
  }
  emit StateChanged();
}

auto SearchSession::Replace(const QString& query, int offset, int limit, bool submit,
                            bool allow_semantic) -> qulonglong {
  const QString trimmed = query.trimmed();
  if (trimmed.isEmpty()) {
    Preview(trimmed);
    return 0;
  }
  if (ClearRows()) {
    emit ResultsChanged();
  }
  Request(trimmed, std::max(0, offset), std::clamp(limit, 0, kMaxPageRows), submit, allow_semantic);
  return active_request_id_;
}

void SearchSession::Reset() {
  DropActiveRequest();
  const bool rows_changed = ClearRows();
  ClearStatus();
  query_.clear();
  route_.clear();
  semantic_active_ = false;
  if (rows_changed) {
    emit ResultsChanged();
  }
  emit StateChanged();
}

void SearchSession::Request(const QString& query, int offset, int limit, bool submit,
                            bool allow_semantic) {
  DropActiveRequest();
  ClearStatus();
  query_           = query;
  semantic_active_ = submit && allow_semantic && RouteFor(query) == QStringLiteral("semantic");
  active_request_id_ =
      port_.request_page(query, offset, limit, QStringLiteral("replace"), submit, allow_semantic);
  emit StateChanged();
}

void SearchSession::ApplyResponse(qulonglong request_id, const QString& mode,
                                  const QVariantMap& response) {
  if (active_request_id_ == 0 || request_id != active_request_id_) {
    return;
  }
  active_request_id_ = 0;

  route_    = response.value(QStringLiteral("route"), QStringLiteral("semantic")).toString();
  too_long_ = response.value(QStringLiteral("tooLong")).toBool();
  semantic_unavailable_ = response.value(QStringLiteral("semanticUnavailable")).toBool();
  semantic_error_text_  = response.value(QStringLiteral("semanticErrorText")).toString();
  search_error_text_    = response.value(QStringLiteral("searchErrorText")).toString();
  awaiting_submit_      = response.value(QStringLiteral("awaitingSubmit")).toBool();

  results_              = response.value(QStringLiteral("rows")).toList();
  offset_               = std::max(0, response.value(QStringLiteral("offset")).toInt());
  total_                = response.contains(QStringLiteral("total"))
                              ? response.value(QStringLiteral("total")).toInt()
                              : static_cast<int>(results_.size());
  has_more_             = response.contains(QStringLiteral("hasMore"))
                              ? response.value(QStringLiteral("hasMore")).toBool()
                              : offset_ + static_cast<int>(results_.size()) < total_;
  emit ResultsChanged();
  emit StateChanged();
  emit ResponseApplied(request_id, mode);
}

void SearchSession::DropActiveRequest() {
  if (active_request_id_ == 0) {
    return;
  }
  const qulonglong dropped = active_request_id_;
  active_request_id_       = 0;
  emit RequestDropped(dropped);
}

auto SearchSession::ClearRows() -> bool {
  const bool changed = !results_.isEmpty() || offset_ != 0 || total_ != 0 || has_more_;
  results_.clear();
  offset_   = 0;
  total_    = 0;
  has_more_ = false;
  return changed;
}

void SearchSession::ClearStatus() {
  awaiting_submit_      = false;
  too_long_             = false;
  semantic_unavailable_ = false;
  semantic_error_text_.clear();
  search_error_text_.clear();
}

}  // namespace alcedo::ui
