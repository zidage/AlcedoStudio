//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_json_schema.hpp"

#include <QJsonArray>
#include <QStringList>
#include <cmath>

namespace alcedo::automation {
namespace {

auto EscapePointerToken(QString token) -> QString {
  token.replace(QLatin1Char('~'), QStringLiteral("~0"));
  token.replace(QLatin1Char('/'), QStringLiteral("~1"));
  return token;
}

auto ChildPointer(const QString& parent, const QString& token) -> QString {
  return parent + QLatin1Char('/') + EscapePointerToken(token);
}

auto IsKnownTypeName(const QString& name) -> bool {
  static const QStringList kTypeNames{QStringLiteral("object"),  QStringLiteral("array"),
                                      QStringLiteral("string"),  QStringLiteral("number"),
                                      QStringLiteral("integer"), QStringLiteral("boolean"),
                                      QStringLiteral("null")};
  return kTypeNames.contains(name);
}

auto IsIntegral(const QJsonValue& value) -> bool {
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  return std::isfinite(number) && std::floor(number) == number;
}

auto MatchesTypeName(const QJsonValue& value, const QString& name) -> bool {
  if (name == QStringLiteral("object")) return value.isObject();
  if (name == QStringLiteral("array")) return value.isArray();
  if (name == QStringLiteral("string")) return value.isString();
  if (name == QStringLiteral("number")) return value.isDouble();
  if (name == QStringLiteral("integer")) return IsIntegral(value);
  if (name == QStringLiteral("boolean")) return value.isBool();
  if (name == QStringLiteral("null")) return value.isNull();
  return false;
}

auto TypeNames(const QJsonValue& type) -> QStringList {
  if (type.isString()) {
    return {type.toString()};
  }
  QStringList names;
  for (const QJsonValue& entry : type.toArray()) {
    names.push_back(entry.toString());
  }
  return names;
}

auto DescribeJsonKind(const QJsonValue& value) -> QString {
  switch (value.type()) {
    case QJsonValue::Null:
      return QStringLiteral("null");
    case QJsonValue::Bool:
      return QStringLiteral("boolean");
    case QJsonValue::Double:
      return IsIntegral(value) ? QStringLiteral("integer") : QStringLiteral("number");
    case QJsonValue::String:
      return QStringLiteral("string");
    case QJsonValue::Array:
      return QStringLiteral("array");
    case QJsonValue::Object:
      return QStringLiteral("object");
    case QJsonValue::Undefined:
      break;
  }
  return QStringLiteral("undefined");
}

auto CheckKeywordsAt(const QJsonObject& schema, const QString& pointer)
    -> std::optional<AutomationSchemaViolation> {
  for (auto it = schema.constBegin(); it != schema.constEnd(); ++it) {
    const QString    keyword = it.key();
    const QJsonValue value   = it.value();
    const QString    at      = ChildPointer(pointer, keyword);

    if (keyword == QStringLiteral("title") || keyword == QStringLiteral("description")) {
      if (!value.isString()) {
        return AutomationSchemaViolation{at, QStringLiteral("annotation must be a string")};
      }
    } else if (keyword == QStringLiteral("default")) {
      // Any JSON value is a valid default annotation.
    } else if (keyword == QStringLiteral("type")) {
      const bool is_list = value.isArray() && !value.toArray().isEmpty();
      if (!value.isString() && !is_list) {
        return AutomationSchemaViolation{at, QStringLiteral("type must be a name or a list")};
      }
      if (is_list) {
        for (const QJsonValue& entry : value.toArray()) {
          if (!entry.isString()) {
            return AutomationSchemaViolation{at, QStringLiteral("type names must be strings")};
          }
        }
      }
      for (const QString& name : TypeNames(value)) {
        if (!IsKnownTypeName(name)) {
          return AutomationSchemaViolation{at, QStringLiteral("unknown type '%1'").arg(name)};
        }
      }
    } else if (keyword == QStringLiteral("properties")) {
      if (!value.isObject()) {
        return AutomationSchemaViolation{at, QStringLiteral("properties must be an object")};
      }
      const QJsonObject properties = value.toObject();
      for (auto property = properties.constBegin(); property != properties.constEnd(); ++property) {
        const QString property_at = ChildPointer(at, property.key());
        if (!property.value().isObject()) {
          return AutomationSchemaViolation{property_at,
                                           QStringLiteral("property schema must be an object")};
        }
        if (auto violation = CheckKeywordsAt(property.value().toObject(), property_at)) {
          return violation;
        }
      }
    } else if (keyword == QStringLiteral("required")) {
      if (!value.isArray()) {
        return AutomationSchemaViolation{at, QStringLiteral("required must be a list")};
      }
      for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isString()) {
          return AutomationSchemaViolation{at, QStringLiteral("required names must be strings")};
        }
      }
    } else if (keyword == QStringLiteral("additionalProperties")) {
      if (!value.isBool()) {
        return AutomationSchemaViolation{
            at, QStringLiteral("only a boolean additionalProperties is supported")};
      }
    } else if (keyword == QStringLiteral("enum")) {
      if (!value.isArray() || value.toArray().isEmpty()) {
        return AutomationSchemaViolation{at, QStringLiteral("enum must be a non-empty list")};
      }
    } else if (keyword == QStringLiteral("minimum") || keyword == QStringLiteral("maximum")) {
      if (!value.isDouble()) {
        return AutomationSchemaViolation{at, QStringLiteral("bound must be a number")};
      }
    } else if (keyword == QStringLiteral("items")) {
      if (!value.isObject()) {
        return AutomationSchemaViolation{at, QStringLiteral("items must be a schema object")};
      }
      if (auto violation = CheckKeywordsAt(value.toObject(), at)) {
        return violation;
      }
    } else if (keyword == QStringLiteral("minItems")) {
      if (!IsIntegral(value) || value.toDouble() < 0) {
        return AutomationSchemaViolation{at,
                                         QStringLiteral("minItems must be a non-negative integer")};
      }
    } else {
      return AutomationSchemaViolation{
          at, QStringLiteral("unsupported schema keyword '%1'").arg(keyword)};
    }
  }
  return std::nullopt;
}

auto ValidateAt(const QJsonValue& value, const QJsonObject& schema, const QString& pointer)
    -> std::optional<AutomationSchemaViolation> {
  if (schema.contains("type")) {
    const QStringList names   = TypeNames(schema.value("type"));
    bool              matches = false;
    for (const QString& name : names) {
      matches = matches || MatchesTypeName(value, name);
    }
    if (!matches) {
      return AutomationSchemaViolation{
          pointer, QStringLiteral("expected %1, got %2")
                       .arg(names.join(QStringLiteral(" or ")), DescribeJsonKind(value))};
    }
  }

  if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) {
    QStringList allowed;
    for (const QJsonValue& entry : schema.value("enum").toArray()) {
      allowed.push_back(entry.isString() ? entry.toString() : QString::number(entry.toDouble()));
    }
    return AutomationSchemaViolation{
        pointer,
        QStringLiteral("value must be one of: %1").arg(allowed.join(QStringLiteral(", ")))};
  }

  if (value.isDouble()) {
    const double number = value.toDouble();
    if (schema.contains("minimum") && number < schema.value("minimum").toDouble()) {
      return AutomationSchemaViolation{pointer,
                                       QStringLiteral("value %1 is less than the minimum %2")
                                           .arg(number)
                                           .arg(schema.value("minimum").toDouble())};
    }
    if (schema.contains("maximum") && number > schema.value("maximum").toDouble()) {
      return AutomationSchemaViolation{pointer,
                                       QStringLiteral("value %1 is greater than the maximum %2")
                                           .arg(number)
                                           .arg(schema.value("maximum").toDouble())};
    }
  }

  if (value.isObject()) {
    const QJsonObject object     = value.toObject();
    const QJsonObject properties = schema.value("properties").toObject();
    if (schema.value("additionalProperties") == QJsonValue(false)) {
      for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!properties.contains(it.key())) {
          return AutomationSchemaViolation{ChildPointer(pointer, it.key()),
                                           QStringLiteral("parameter is not allowed")};
        }
      }
    }
    for (const QJsonValue& name : schema.value("required").toArray()) {
      if (!object.contains(name.toString())) {
        return AutomationSchemaViolation{ChildPointer(pointer, name.toString()),
                                         QStringLiteral("required parameter is missing")};
      }
    }
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
      if (!object.contains(it.key())) {
        continue;
      }
      if (auto violation = ValidateAt(object.value(it.key()), it.value().toObject(),
                                      ChildPointer(pointer, it.key()))) {
        return violation;
      }
    }
  }

  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    if (schema.contains("minItems") && array.size() < schema.value("minItems").toInt()) {
      return AutomationSchemaViolation{
          pointer,
          QStringLiteral("list must have at least %1 items").arg(schema.value("minItems").toInt())};
    }
    if (schema.contains("items")) {
      const QJsonObject item_schema = schema.value("items").toObject();
      for (qsizetype index = 0; index < array.size(); ++index) {
        if (auto violation = ValidateAt(array.at(index), item_schema,
                                        ChildPointer(pointer, QString::number(index)))) {
          return violation;
        }
      }
    }
  }

  return std::nullopt;
}

}  // namespace

auto CheckAutomationSchemaKeywords(const QJsonObject& schema)
    -> std::optional<AutomationSchemaViolation> {
  return CheckKeywordsAt(schema, QString());
}

auto ValidateAutomationJson(const QJsonValue& value, const QJsonObject& schema)
    -> std::optional<AutomationSchemaViolation> {
  return ValidateAt(value, schema, QString());
}

}  // namespace alcedo::automation
