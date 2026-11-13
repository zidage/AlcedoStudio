//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

namespace alcedo::ui {

/// Calls of SearchSession into its owner (SearchController). Tests give fakes.
struct SearchSessionPort {
  /// Route name of ClassifyQuery: "empty", "traditional", "label", or "semantic".
  std::function<QString(const QString& query)> classify_query;
  /// True when natural language search is on.
  std::function<bool()>                        natural_language_enabled;
  /// Queues one page request on the search worker and returns its request id. @p submit is
  /// an explicit submit (the only request that reaches the semantic provider). When
  /// @p allow_semantic is false the query uses the SQL path for each route.
  std::function<qulonglong(const QString& query, int offset, int limit, const QString& mode,
                           bool submit, bool allow_semantic)>
      request_page;
};

/**
 * @brief Routing, response matching, and the result rows of the global search.
 *
 * @details SearchPalette.qml and the search.query command read the search through this
 * object. A request replaces the active request and its rows; only the response whose request
 * id is the active request id sets the rows. The typing preview of a field query asks for
 * kPreviewRows rows; search.query asks for one page of at most kMaxPageRows rows. The session
 * has no paging of its own. GUI thread only.
 */
class SearchSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString query READ Query NOTIFY StateChanged)
  Q_PROPERTY(QString route READ Route NOTIFY StateChanged)
  Q_PROPERTY(QVariantList results READ Results NOTIFY ResultsChanged)
  Q_PROPERTY(bool loading READ Loading NOTIFY StateChanged)
  Q_PROPERTY(qulonglong activeRequestId READ ActiveRequestId NOTIFY StateChanged)
  Q_PROPERTY(bool semanticActive READ SemanticActive NOTIFY StateChanged)
  Q_PROPERTY(bool awaitingSubmit READ AwaitingSubmit NOTIFY StateChanged)
  Q_PROPERTY(bool tooLong READ TooLong NOTIFY StateChanged)
  Q_PROPERTY(bool semanticUnavailable READ SemanticUnavailable NOTIFY StateChanged)
  Q_PROPERTY(QString semanticErrorText READ SemanticErrorText NOTIFY StateChanged)
  Q_PROPERTY(QString searchErrorText READ SearchErrorText NOTIFY StateChanged)

 public:
  /// Rows of the typing preview of a field query.
  static constexpr int kPreviewRows = 3;
  /// Default rows of one search.query page.
  static constexpr int kPageSize    = 24;
  /// Largest number of rows of one search.query page.
  static constexpr int kMaxPageRows = 48;

  explicit SearchSession(SearchSessionPort port, QObject* parent = nullptr);

  [[nodiscard]] const QString&      Query() const { return query_; }
  [[nodiscard]] const QString&      Route() const { return route_; }
  [[nodiscard]] const QVariantList& Results() const { return results_; }
  /// Offset of the first row of the applied response.
  [[nodiscard]] int                 Offset() const { return offset_; }
  /// Matches of the whole query, as the applied response reports them (search.query).
  [[nodiscard]] int                 Total() const { return total_; }
  /// True when matches follow the rows of the applied response (search.query).
  [[nodiscard]] bool                HasMore() const { return has_more_; }
  [[nodiscard]] bool                Loading() const { return active_request_id_ != 0; }
  [[nodiscard]] bool                SemanticActive() const { return semantic_active_; }
  [[nodiscard]] bool                AwaitingSubmit() const { return awaiting_submit_; }
  [[nodiscard]] bool                TooLong() const { return too_long_; }
  [[nodiscard]] bool                SemanticUnavailable() const { return semantic_unavailable_; }
  [[nodiscard]] const QString&      SemanticErrorText() const { return semantic_error_text_; }
  [[nodiscard]] const QString&      SearchErrorText() const { return search_error_text_; }
  [[nodiscard]] qulonglong          ActiveRequestId() const { return active_request_id_; }

  /// "empty" for an empty query; "semantic" when ClassifyQuery returns `semantic` and natural
  /// language search is on; else "fuzzy".
  Q_INVOKABLE QString               RouteFor(const QString& query) const;
  /// Typing preview. An empty query clears the rows; a semantic query only waits for Enter
  /// (SearchController::ApplySemanticSearch) and requests nothing; a field query requests the
  /// first kPreviewRows rows.
  Q_INVOKABLE void                  Preview(const QString& query);
  /// Drops the active request and clears the rows.
  Q_INVOKABLE void                  Reset();
  /// Requests one page of @p limit rows (at most kMaxPageRows) that replaces the rows.
  /// @p allow_semantic false keeps a semantic query on the SQL path. Returns the request id,
  /// or 0 for an empty query.
  auto Replace(const QString& query, int offset, int limit, bool submit, bool allow_semantic)
      -> qulonglong;

  /// Response of the search worker (SearchController::SearchResponseReady). Applied only when
  /// @p request_id is the active request id.
  void ApplyResponse(qulonglong request_id, const QString& mode, const QVariantMap& response);

 signals:
  void StateChanged();
  /// The rows or the response values changed.
  void ResultsChanged();
  /// The response of @p requestId set the rows.
  void ResponseApplied(qulonglong requestId, const QString& mode);
  /// @p requestId stopped being the active request before its response arrived.
  void RequestDropped(qulonglong requestId);

 private:
  void Request(const QString& query, int offset, int limit, bool submit, bool allow_semantic);
  void DropActiveRequest();
  /// Returns true when the rows or the response values were not empty.
  auto ClearRows() -> bool;
  void ClearStatus();

  SearchSessionPort port_;
  QString           query_{};
  QString           route_{};
  QVariantList      results_{};
  int               offset_               = 0;
  int               total_                = 0;
  bool              has_more_             = false;
  bool              semantic_active_      = false;
  bool              awaiting_submit_      = false;
  bool              too_long_             = false;
  bool              semantic_unavailable_ = false;
  QString           semantic_error_text_{};
  QString           search_error_text_{};
  qulonglong        active_request_id_ = 0;
};

}  // namespace alcedo::ui
