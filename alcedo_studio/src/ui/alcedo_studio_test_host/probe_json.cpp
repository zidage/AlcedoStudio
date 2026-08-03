//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "probe_json.hpp"

#include <QColor>
#include <QJsonArray>
#include <QUrl>

namespace alcedo::ui::probe_json {

auto BaseResponse(const QJsonObject& request) -> QJsonObject {
  QJsonObject response;
  if (request.contains(QStringLiteral("id"))) {
    response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
  }
  return response;
}

auto ErrorResponse(const QJsonObject& request, const QString& code, const QString& message,
                   const QJsonObject& extra) -> QJsonObject {
  QJsonObject response = BaseResponse(request);
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);
  for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
    error.insert(it.key(), it.value());
  }
  response.insert(QStringLiteral("error"), error);
  return response;
}

auto OkStatusResponse(const QJsonObject& request) -> QJsonObject {
  QJsonObject response = BaseResponse(request);
  response.insert(QStringLiteral("ok"), true);
  response.insert(QStringLiteral("result"), QStringLiteral("ok"));
  return response;
}

auto JsonPath(const QStringList& path) -> QJsonArray {
  QJsonArray result;
  for (const QString& part : path) {
    result.append(part);
  }
  return result;
}

auto VariantToJson(const QVariant& value) -> QJsonValue {
  if (!value.isValid() || value.isNull()) {
    return QJsonValue(QJsonValue::Null);
  }

  switch (value.typeId()) {
    case QMetaType::Bool:
      return QJsonValue(value.toBool());
    case QMetaType::Int:
    case QMetaType::Short:
    case QMetaType::Char16:
    case QMetaType::Char32:
      return QJsonValue(value.toInt());
    case QMetaType::UInt:
    case QMetaType::UShort:
    case QMetaType::UChar:
      return QJsonValue(static_cast<qint64>(value.toUInt()));
    case QMetaType::LongLong:
      return QJsonValue(value.toLongLong());
    case QMetaType::ULongLong:
      return QJsonValue(static_cast<qint64>(value.toULongLong()));
    case QMetaType::Float:
    case QMetaType::Double:
      return QJsonValue(value.toDouble());
    case QMetaType::QString:
      return QJsonValue(value.toString());
    case QMetaType::QByteArray:
      return QJsonValue(QString::fromUtf8(value.toByteArray()));
    case QMetaType::QUrl:
      return QJsonValue(value.toUrl().toString());
    case QMetaType::QColor:
      return QJsonValue(value.value<QColor>().name(QColor::HexArgb));
    case QMetaType::QStringList: {
      QJsonArray result;
      for (const QString& entry : value.toStringList()) {
        result.append(entry);
      }
      return result;
    }
    case QMetaType::QVariantList: {
      QJsonArray result;
      for (const QVariant& entry : value.toList()) {
        result.append(VariantToJson(entry));
      }
      return result;
    }
    case QMetaType::QVariantMap: {
      QJsonObject       result;
      const QVariantMap map = value.toMap();
      for (auto it = map.cbegin(); it != map.cend(); ++it) {
        result.insert(it.key(), VariantToJson(it.value()));
      }
      return result;
    }
    default:
      return QJsonValue(value.toString());
  }
}

}  // namespace alcedo::ui::probe_json
