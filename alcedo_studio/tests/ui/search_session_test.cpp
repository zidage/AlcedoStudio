//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SearchSession rules: routing, the match of a response to the active request, the field
// preview of three rows, and the search.query page of at most 48 rows.

#include "ui/alcedo_main/album_backend/search_session.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

namespace alcedo::ui::test {
namespace {

struct RecordedRequest {
  QString query;
  int     offset = 0;
  int     limit  = 0;
  QString mode;
  bool    submit         = false;
  bool    allow_semantic = true;
};

/// A port that records each request and gives ids 1, 2, 3, ...
class FakeSearch {
 public:
  auto Port() -> SearchSessionPort {
    return SearchSessionPort{.classify_query =
                                 [](const QString& query) {
                                   return query.contains(QLatin1Char(' '))
                                              ? QStringLiteral("semantic")
                                              : QStringLiteral("traditional");
                                 },
                             .natural_language_enabled = [this] { return natural_language_; },
                             .request_page =
                                 [this](const QString& query, int offset, int limit,
                                        const QString& mode, bool submit, bool allow_semantic) {
                                   requests_.push_back(RecordedRequest{query, offset, limit, mode,
                                                                       submit, allow_semantic});
                                   return static_cast<qulonglong>(requests_.size());
                                 }};
  }

  bool                         natural_language_ = false;
  std::vector<RecordedRequest> requests_;
};

/// Rows with element ids first + 1 .. first + count.
auto Rows(int first, int count) -> QVariantList {
  QVariantList rows;
  for (int i = 0; i < count; ++i) {
    rows.push_back(QVariantMap{{QStringLiteral("elementId"), first + i + 1}});
  }
  return rows;
}

auto Response(int offset, int count, int total) -> QVariantMap {
  return QVariantMap{{QStringLiteral("rows"), Rows(offset, count)},
                     {QStringLiteral("offset"), offset},
                     {QStringLiteral("total"), total},
                     {QStringLiteral("hasMore"), offset + count < total},
                     {QStringLiteral("route"), QStringLiteral("traditional")}};
}

auto FirstElementId(const SearchSession& session) -> int {
  return session.Results().front().toMap().value(QStringLiteral("elementId")).toInt();
}

TEST(SearchSessionTest, ResponseWithOldRequestIdIsIgnored) {
  FakeSearch    search;
  SearchSession session(search.Port());
  session.Preview(QStringLiteral("canon"));
  session.ApplyResponse(1, QStringLiteral("replace"), Response(0, 24, 30));
  ASSERT_EQ(session.Results().size(), 24);

  session.Preview(QStringLiteral("nikon"));
  ASSERT_EQ(session.ActiveRequestId(), 2u);
  EXPECT_TRUE(session.Results().isEmpty());
  QSignalSpy results_changed(&session, &SearchSession::ResultsChanged);
  QSignalSpy applied(&session, &SearchSession::ResponseApplied);

  // A late response to the first request, and a response with an unknown id.
  session.ApplyResponse(1, QStringLiteral("replace"), Response(0, 24, 30));
  session.ApplyResponse(7, QStringLiteral("replace"), Response(0, 5, 5));
  EXPECT_TRUE(session.Results().isEmpty());
  EXPECT_EQ(session.Total(), 0);
  EXPECT_TRUE(session.Loading());
  EXPECT_EQ(results_changed.count(), 0);
  EXPECT_EQ(applied.count(), 0);

  session.ApplyResponse(2, QStringLiteral("replace"), Response(0, 3, 3));
  EXPECT_EQ(session.Results().size(), 3);
  EXPECT_FALSE(session.Loading());
  EXPECT_EQ(applied.count(), 1);
  // A second response with the same id does not apply again.
  session.ApplyResponse(2, QStringLiteral("replace"), Response(0, 24, 30));
  EXPECT_EQ(session.Results().size(), 3);

  // After Reset no response applies.
  session.Preview(QStringLiteral("sony"));
  session.Reset();
  session.ApplyResponse(3, QStringLiteral("replace"), Response(0, 24, 30));
  EXPECT_TRUE(session.Results().isEmpty());
}

TEST(SearchSessionTest, PreviewRequestsThreeRowsForFieldQuery) {
  FakeSearch    search;
  SearchSession session(search.Port());
  session.Preview(QStringLiteral("canon"));
  ASSERT_EQ(search.requests_.size(), 1u);
  EXPECT_EQ(search.requests_[0].query, QStringLiteral("canon"));
  EXPECT_EQ(search.requests_[0].offset, 0);
  EXPECT_EQ(search.requests_[0].limit, SearchSession::kPreviewRows);
  EXPECT_EQ(SearchSession::kPreviewRows, 3);
  EXPECT_FALSE(search.requests_[0].submit);

  session.ApplyResponse(1, QStringLiteral("replace"), Response(0, 3, 30));
  EXPECT_EQ(session.Results().size(), 3);
  EXPECT_EQ(FirstElementId(session), 1);
  EXPECT_EQ(session.Total(), 30);
  EXPECT_TRUE(session.HasMore());

  // A new preview replaces the rows; it does not add to them.
  session.Preview(QStringLiteral("nikon"));
  EXPECT_TRUE(session.Results().isEmpty());
  ASSERT_EQ(search.requests_.size(), 2u);
  EXPECT_EQ(search.requests_[1].limit, SearchSession::kPreviewRows);
  session.ApplyResponse(2, QStringLiteral("replace"), Response(0, 2, 2));
  EXPECT_EQ(session.Results().size(), 2);
  EXPECT_FALSE(session.HasMore());
}

TEST(SearchSessionTest, ReplacePageIsLimitedToMaxPageRowsAndKeepsItsOffset) {
  FakeSearch    search;
  SearchSession session(search.Port());
  session.Replace(QStringLiteral("canon"), 10, 60, true, false);
  ASSERT_EQ(search.requests_.size(), 1u);
  EXPECT_EQ(search.requests_[0].offset, 10);
  EXPECT_EQ(search.requests_[0].limit, SearchSession::kMaxPageRows);

  session.ApplyResponse(1, QStringLiteral("replace"), Response(10, 48, 100));
  EXPECT_EQ(session.Results().size(), SearchSession::kMaxPageRows);
  EXPECT_EQ(session.Offset(), 10);
  EXPECT_EQ(FirstElementId(session), 11);
  EXPECT_EQ(session.Total(), 100);
  EXPECT_TRUE(session.HasMore());
}

TEST(SearchSessionTest, SemanticQueryWaitsForSubmitWhenNaturalLanguageSearchIsOn) {
  FakeSearch    search;
  SearchSession session(search.Port());
  EXPECT_EQ(session.RouteFor(QStringLiteral("  ")), QStringLiteral("empty"));
  EXPECT_EQ(session.RouteFor(QStringLiteral("sunset over hills")), QStringLiteral("fuzzy"));
  search.natural_language_ = true;
  EXPECT_EQ(session.RouteFor(QStringLiteral("sunset over hills")), QStringLiteral("semantic"));
  EXPECT_EQ(session.RouteFor(QStringLiteral("canon")), QStringLiteral("fuzzy"));

  session.Preview(QStringLiteral("sunset over hills"));
  EXPECT_TRUE(search.requests_.empty());
  EXPECT_TRUE(session.AwaitingSubmit());
  EXPECT_EQ(session.Route(), QStringLiteral("semantic"));
  EXPECT_FALSE(session.Loading());

  // search.query submits a natural-language page through Replace.
  session.Replace(QStringLiteral("sunset over hills"), 0, 24, true, true);
  ASSERT_EQ(search.requests_.size(), 1u);
  EXPECT_TRUE(search.requests_[0].submit);
  EXPECT_TRUE(search.requests_[0].allow_semantic);
  EXPECT_TRUE(session.SemanticActive());
  EXPECT_FALSE(session.AwaitingSubmit());

  QVariantMap unavailable                            = Response(0, 0, 0);
  unavailable[QStringLiteral("route")]               = QStringLiteral("semantic");
  unavailable[QStringLiteral("semanticUnavailable")] = true;
  session.ApplyResponse(1, QStringLiteral("replace"), unavailable);
  EXPECT_TRUE(session.SemanticUnavailable());

  // A replace without the semantic path keeps the query on the SQL path.
  session.Replace(QStringLiteral("sunset over hills"), 0, 24, true, false);
  EXPECT_FALSE(search.requests_[1].allow_semantic);
  EXPECT_FALSE(session.SemanticActive());
  EXPECT_FALSE(session.SemanticUnavailable());

  // An empty query sends no request.
  EXPECT_EQ(session.Replace(QString(), 0, 24, true, true), 0u);
  EXPECT_EQ(search.requests_.size(), 2u);
  EXPECT_EQ(session.Route(), QStringLiteral("empty"));
}

}  // namespace
}  // namespace alcedo::ui::test
