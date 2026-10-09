//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>

namespace alcedo::automation {

/// The first location where a schema or a value fails a check.
struct AutomationSchemaViolation {
  /// JSON pointer (RFC 6901) into the checked document. An empty string is the root.
  QString pointer;
  QString reason;
};

/// Checks that @p schema uses only the supported subset of JSON Schema draft 2020-12:
/// `type`, `properties`, `required`, `additionalProperties` (boolean only), `enum`, `minimum`,
/// `maximum`, `items`, and `minItems`, plus the annotations `title`, `description`, and
/// `default`. The violation pointer locates the bad keyword inside @p schema.
auto CheckAutomationSchemaKeywords(const QJsonObject& schema)
    -> std::optional<AutomationSchemaViolation>;

/// Validates @p value against a schema that passed `CheckAutomationSchemaKeywords`. The violation
/// pointer locates the first failing member inside @p value.
auto ValidateAutomationJson(const QJsonValue& value, const QJsonObject& schema)
    -> std::optional<AutomationSchemaViolation>;

}  // namespace alcedo::automation
