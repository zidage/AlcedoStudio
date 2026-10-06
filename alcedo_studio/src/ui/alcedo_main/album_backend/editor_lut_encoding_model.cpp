//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_lut_encoding_model.hpp"

#include <QVariantList>
#include <QVariantMap>

#include "color/color_encoding_catalog.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

auto ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto EncodingEntries() -> QVariantList {
  QVariantList entries;
  for (const auto& encoding : color::ColorEncodings()) {
    const bool  display = encoding.referral_ == color::ColorReferral::DisplayReferred;
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), ToQString(encoding.id_));
    entry.insert(QStringLiteral("label"), ToQString(encoding.display_name_));
    entry.insert(QStringLiteral("group"),
                 display ? QStringLiteral("display") : QStringLiteral("scene"));
    entry.insert(QStringLiteral("groupLabel"), display ? Tr("Display") : Tr("Scene"));
    entries.push_back(entry);
  }
  return entries;
}

auto IndexOfEncoding(const QVariantList& entries, const QString& id) -> int {
  for (int index = 0; index < entries.size(); ++index) {
    if (entries[index].toMap().value(QStringLiteral("value")).toString() == id) return index;
  }
  return -1;
}

}  // namespace

EditorLutEncodingModel::EditorLutEncodingModel(QObject* parent)
    : EditorAdjustmentEnumModel(parent) {
  setFieldKey(QStringLiteral("lut"));
  setEntries(EncodingEntries());
  const int default_index = IndexOfEncoding(entries(), ToQString(kDefaultLutEncodingId));
  setDefaultIndex(default_index);
  setCurrentIndex(default_index);
}

void EditorLutEncodingModel::setTarget(LutLibraryController* target) {
  if (target_ == target) return;
  if (target_) disconnect(target_, nullptr, this, nullptr);
  target_ = target;
  if (target_) {
    connect(target_, &LutLibraryController::associationChanged, this,
            &EditorLutEncodingModel::loadFromTarget);
  }
  emit targetChanged();
  loadFromTarget();
}

auto EditorLutEncodingModel::side() const -> QString {
  return output_side_ ? QStringLiteral("output") : QStringLiteral("input");
}

void EditorLutEncodingModel::setSide(const QString& side) {
  if (side != QLatin1String("input") && side != QLatin1String("output")) return;
  const bool output = side == QLatin1String("output");
  if (output == output_side_) return;
  output_side_ = output;
  emit sideChanged();
  loadFromTarget();
}

auto EditorLutEncodingModel::displayReferred() const -> bool {
  const auto* encoding = color::FindColorEncoding(currentValue().toStdString());
  return encoding != nullptr && encoding->referral_ == color::ColorReferral::DisplayReferred;
}

void EditorLutEncodingModel::loadFromTarget() {
  QString stored = ToQString(kDefaultLutEncodingId);
  if (target_) stored = output_side_ ? target_->outputEncoding() : target_->inputEncoding();
  // The LMT Model accepts catalog ids only, so a stored id is always listed.
  const int index = IndexOfEncoding(entries(), stored);
  if (index >= 0) setCurrentIndex(index);
}

auto EditorLutEncodingModel::selectionWrite(const QString& value) const
    -> alcedo::EditorParameterWrite {
  alcedo::EditorLutWrite write;
  if (output_side_) {
    write.output_encoding = value.toStdString();
  } else {
    write.input_encoding = value.toStdString();
  }
  return write;
}

void EditorLutEncodingModel::onSettledWriteAccepted() {
  if (!target_) return;
  target_->RememberEncodingSide(output_side_ ? LutEncodingSide::kOutput : LutEncodingSide::kInput,
                                currentValue());
}

}  // namespace alcedo::ui
