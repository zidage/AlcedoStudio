//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace alcedo::ui::probe_json {

/// Copies the request `id` into a fresh response object when present.
[[nodiscard]] auto BaseResponse(const QJsonObject& request) -> QJsonObject;

[[nodiscard]] auto ErrorResponse(const QJsonObject& request, const QString& code,
                                 const QString& message, const QJsonObject& extra = {})
    -> QJsonObject;

[[nodiscard]] auto OkStatusResponse(const QJsonObject& request) -> QJsonObject;

[[nodiscard]] auto VariantToJson(const QVariant& value) -> QJsonValue;

[[nodiscard]] auto JsonPath(const QStringList& path) -> QJsonArray;

}  // namespace alcedo::ui::probe_json
