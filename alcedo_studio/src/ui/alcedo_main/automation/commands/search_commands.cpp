//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <memory>
#include <utility>

#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/search_session.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_search_commands.hpp"

namespace alcedo::automation {
namespace {

using ui::SearchSession;

auto Matches(const SearchSession& session) -> QJsonArray {
  QJsonArray matches;
  for (const QVariant& value : session.Results()) {
    const QVariantMap row = value.toMap();
    matches.push_back(QJsonObject{
        {"element_id", static_cast<double>(row.value(QStringLiteral("elementId")).toUInt())},
        {"image_id", static_cast<double>(row.value(QStringLiteral("imageId")).toUInt())},
        {"file_name", row.value(QStringLiteral("fileName")).toString()},
        {"camera_model", row.value(QStringLiteral("cameraModel")).toString()},
        {"lens", row.value(QStringLiteral("lens")).toString()},
        {"capture_date", row.value(QStringLiteral("captureDate")).toString()},
        {"rating", row.value(QStringLiteral("rating")).toInt()}});
  }
  return matches;
}

/// The answer to an applied response. The query is too long or the semantic path is not
/// available: no matches.
auto PageResult(const SearchSession& session, int limit) -> QJsonObject {
  QJsonObject result{
      {"route", session.Route()}, {"offset", session.Offset()},    {"limit", limit},
      {"total", session.Total()}, {"has_more", session.HasMore()}, {"matches", Matches(session)}};
  if (session.TooLong()) {
    result.insert("too_long", true);
  }
  if (session.SemanticUnavailable()) {
    result.insert("semantic_unavailable", true);
    if (!session.SemanticErrorText().isEmpty()) {
      result.insert("semantic_error", session.SemanticErrorText());
    }
  }
  return result;
}

}  // namespace

auto RegisterAutomationSearchCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host, QString* error) -> bool {
  if (host == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the search commands need a host");
    }
    return false;
  }

  AutomationCommandSpec query;
  query.method      = QStringLiteral("search.query");
  query.description = QStringLiteral(
      "Searches the photos of the current folder with the rules of the search palette. An "
      "empty text returns the recommendations. With 'mode' auto a natural language query uses "
      "the semantic path when natural language search is on; when that path is not available "
      "the result has 'semantic_unavailable' and no matches. 'mode' fuzzy always uses the text "
      "search. A query that is too long for the semantic path returns 'too_long'.");
  query.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"text", QJsonObject{{"type", "string"}, {"description", "The search text."}}},
          {"offset", QJsonObject{{"type", "integer"},
                                 {"minimum", 0},
                                 {"description", "The first match to return. Default 0."}}},
          {"limit", QJsonObject{{"type", "integer"},
                                {"minimum", 1},
                                {"maximum", SearchSession::kMaxPageRows},
                                {"description", "The number of matches to return. Default 24."}}},
          {"mode", QJsonObject{{"type", "string"},
                               {"enum", QJsonArray{"auto", "fuzzy"}},
                               {"description", "auto (default) or fuzzy."}}}},
      QJsonArray{"text"});
  query.result_schema =
      AutomationObjectSchema(QJsonObject{{"route", QJsonObject{{"type", "string"}}},
                                         {"offset", QJsonObject{{"type", "integer"}}},
                                         {"limit", QJsonObject{{"type", "integer"}}},
                                         {"total", QJsonObject{{"type", "integer"}}},
                                         {"has_more", QJsonObject{{"type", "boolean"}}},
                                         {"matches", QJsonObject{{"type", "array"}}},
                                         {"recommendations", QJsonObject{{"type", "array"}}},
                                         {"too_long", QJsonObject{{"type", "boolean"}}},
                                         {"semantic_unavailable", QJsonObject{{"type", "boolean"}}},
                                         {"semantic_error", QJsonObject{{"type", "string"}}}},
                             QJsonArray{"route", "matches"});
  query.handler = [host](const QJsonObject& params, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    ui::SearchController* search  = host->search();
    SearchSession*        session = search->session();
    const QString         text    = params.value("text").toString().trimmed();
    const int             offset  = params.value("offset").toInt(0);
    const int             limit   = params.value("limit").toInt(SearchSession::kPageSize);
    const bool            fuzzy   = params.value("mode").toString() == QStringLiteral("fuzzy");

    if (text.isEmpty()) {
      session->Reset();
      reply.SendResult(QJsonObject{
          {"route", QStringLiteral("empty")},
          {"matches", QJsonArray{}},
          {"recommendations", QJsonArray::fromVariantList(search->SearchRecommendations(12))}});
      return;
    }

    QPointer<AutomationCommandWait> wait       = new AutomationCommandWait(std::move(reply), host);
    // The request id is known before the response: the worker posts the response as a queued
    // call, so the connections below exist when it arrives.
    auto                            request_id = std::make_shared<qulonglong>(0);
    QObject::connect(session, &SearchSession::ResponseApplied, wait.data(),
                     [wait, session, request_id, limit](qulonglong id, const QString&) {
                       if (wait == nullptr || id != *request_id) {
                         return;
                       }
                       if (!session->SearchErrorText().isEmpty()) {
                         wait->SendOwnerError(AutomationErrorCode::Failed,
                                              session->SearchErrorText());
                         return;
                       }
                       wait->SendResult(PageResult(*session, limit));
                     });
    QObject::connect(session, &SearchSession::RequestDropped, wait.data(),
                     [wait, request_id](qulonglong id) {
                       if (wait == nullptr || id != *request_id) {
                         return;
                       }
                       wait->SendOwnerError(AutomationErrorCode::Rejected,
                                            QStringLiteral("A newer search replaced this search."));
                     });
    *request_id = session->Replace(text, offset, limit, true, !fuzzy);
  };
  if (!registry.Register(std::move(query), error)) {
    return false;
  }

  AutomationCommandSpec prewarm;
  prewarm.method      = QStringLiteral("search.prewarm");
  prewarm.description = QStringLiteral(
      "Does what the search palette does when it opens: when natural language search is on, "
      "starts the AI sidecar and embeds the neutral text in the background, so the next "
      "natural language query loads no model. Returns at once; a failure is written to the "
      "log only.");
  prewarm.params_schema = AutomationClosedParamsSchema(QJsonObject{}, QJsonArray{});
  prewarm.result_schema = AutomationObjectSchema(
      QJsonObject{{"natural_language_search", QJsonObject{{"type", "boolean"}}}},
      QJsonArray{"natural_language_search"});
  prewarm.handler = [host](const QJsonObject&, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    ui::SearchController* search = host->search();
    search->PrewarmNaturalLanguageSearch();
    reply.SendResult(
        QJsonObject{{"natural_language_search", search->natural_language_search_enabled()}});
  };
  return registry.Register(std::move(prewarm), error);
}

}  // namespace alcedo::automation
