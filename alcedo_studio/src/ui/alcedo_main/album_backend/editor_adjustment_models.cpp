//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJSValue>
#include <QTimer>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>

namespace alcedo::ui {

// ── EditorAdjustmentModelBase ───────────────────────────────────────────────

EditorAdjustmentModelBase::EditorAdjustmentModelBase(QObject* parent) : QObject(parent) {}

EditorAdjustmentModelBase::~EditorAdjustmentModelBase() = default;

void EditorAdjustmentModelBase::setFieldKey(const QString& key) {
  if (fieldKey_ == key) {
    return;
  }
  fieldKey_ = key;
  emit fieldKeyChanged();
}

void EditorAdjustmentModelBase::setLabel(const QString& label) {
  if (label_ == label) {
    return;
  }
  label_ = label;
  emit labelChanged();
}

void EditorAdjustmentModelBase::setEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_ = enabled;
  emit enabledChanged();
}

void EditorAdjustmentModelBase::setSubmitter(QObject* submitter) {
  submitterObject_ = submitter;
  submitter_ = submitter != nullptr ? dynamic_cast<IEditorAdjustmentSubmitter*>(submitter)
                                    : nullptr;
  emit submitterChanged();
}

void EditorAdjustmentModelBase::setParamsBuilder(const QJSValue& builder) {
  paramsBuilder_ = builder;
  emit paramsBuilderChanged();
}

auto EditorAdjustmentModelBase::numericParamsJson(double value) -> QString {
  QJsonObject obj;
  obj["value"] = value;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

auto EditorAdjustmentModelBase::enumParamsJson(int index, const QString& value) -> QString {
  QJsonObject obj;
  obj["index"] = index;
  obj["value"] = value;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

auto EditorAdjustmentModelBase::toggleParamsJson(bool value) -> QString {
  QJsonObject obj;
  obj["value"] = value;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

auto EditorAdjustmentModelBase::resolveParams(const QJSValue& arg,
                                              const QString&   defaultJson) const -> QString {
  if (paramsBuilder_.isCallable()) {
    const QJSValue result = paramsBuilder_.call(QJSValueList{arg});
    if (result.isString()) {
      return result.toString();
    }
  }
  return defaultJson;
}

auto EditorAdjustmentModelBase::submitNow(const QString& paramsJson, bool settled) -> bool {
  if (submitter_ == nullptr || !submitter_->canEdit()) {
    return false;
  }
  return submitter_->submitPatch(fieldKey_, paramsJson, settled);
}

// ── EditorAdjustmentValueModel ───────────────────────────────────────────────

EditorAdjustmentValueModel::EditorAdjustmentValueModel(QObject* parent)
    : EditorAdjustmentModelBase(parent) {
  debounceTimer_ = new QTimer(this);
  debounceTimer_->setSingleShot(true);
  debounceTimer_->setInterval(debounceIntervalMs_);
  connect(debounceTimer_, &QTimer::timeout, this, &EditorAdjustmentValueModel::onDebounceTimeout);
}

void EditorAdjustmentValueModel::setValue(double v) {
  applyValue(v);
}

void EditorAdjustmentValueModel::setDefaultValue(double v) {
  if (defaultValue_ == v) {
    return;
  }
  defaultValue_ = v;
  emit defaultValueChanged();
}

void EditorAdjustmentValueModel::setMinimum(double v) {
  if (minimum_ == v) {
    return;
  }
  minimum_ = v;
  emit minimumChanged();
}

void EditorAdjustmentValueModel::setMaximum(double v) {
  if (maximum_ == v) {
    return;
  }
  maximum_ = v;
  emit maximumChanged();
}

void EditorAdjustmentValueModel::setStep(double v) {
  if (step_ == v) {
    return;
  }
  step_ = v;
  emit stepChanged();
}

void EditorAdjustmentValueModel::setPrecision(int p) {
  if (precision_ == p) {
    return;
  }
  precision_ = p;
  emit precisionChanged();
}

void EditorAdjustmentValueModel::setSuffix(const QString& s) {
  if (suffix_ == s) {
    return;
  }
  suffix_ = s;
  emit suffixChanged();
}

void EditorAdjustmentValueModel::beginGesture() {
  if (gestureActive_) {
    return;
  }
  // A drag supersedes any pending keyboard/wheel settled commit.
  debounceTimer_->stop();
  gestureActive_ = true;
  emit gestureActiveChanged();
}

void EditorAdjustmentValueModel::updateGesture(double v) {
  if (!gestureActive_) {
    return;
  }
  if (!applyValue(v)) {
    return;
  }
  submitInteractive(value_);
}

void EditorAdjustmentValueModel::commitGesture() {
  if (!gestureActive_) {
    return;
  }
  gestureActive_ = false;
  emit gestureActiveChanged();
  submitSettled(value_);
  emit settledCommitted();
}

void EditorAdjustmentValueModel::editValue(double v) {
  if (!applyValue(v)) {
    return;
  }
  submitInteractive(value_);
  debounceTimer_->start();
}

void EditorAdjustmentValueModel::commitImmediately() {
  if (!debounceTimer_->isActive()) {
    return;
  }
  debounceTimer_->stop();
  submitSettled(value_);
  emit settledCommitted();
}

void EditorAdjustmentValueModel::reset() {
  debounceTimer_->stop();
  if (gestureActive_) {
    gestureActive_ = false;
    emit gestureActiveChanged();
  }
  const bool changed = applyValue(defaultValue_);
  if (!changed) {
    return;
  }
  submitSettled(value_);
  emit settledCommitted();
}

void EditorAdjustmentValueModel::setInvalid(const QString& message) {
  if (!valid_ && errorMessage_ == message) {
    return;
  }
  valid_ = false;
  errorMessage_ = message;
  emit validChanged();
}

auto EditorAdjustmentValueModel::hasPendingSettled() const -> bool {
  return debounceTimer_->isActive();
}

void EditorAdjustmentValueModel::setDebounceIntervalMs(int ms) {
  debounceIntervalMs_ = ms;
  debounceTimer_->setInterval(ms);
}

auto EditorAdjustmentValueModel::clamp(double v) const -> double {
  if (!std::isfinite(v)) {
    return value_;
  }
  return std::clamp(v, minimum_, maximum_);
}

auto EditorAdjustmentValueModel::applyValue(double v) -> bool {
  const double clamped = clamp(v);
  if (clamped == value_ && valid_) {
    return false;
  }
  const bool wasValid = valid_;
  value_ = clamped;
  valid_ = true;
  errorMessage_.clear();
  emit valueChanged();
  if (!wasValid) {
    emit validChanged();
  }
  return true;
}

void EditorAdjustmentValueModel::submitInteractive(double v) {
  submitNow(resolveParams(QJSValue(v), numericParamsJson(v)), false);
}

void EditorAdjustmentValueModel::submitSettled(double v) {
  submitNow(resolveParams(QJSValue(v), numericParamsJson(v)), true);
}

void EditorAdjustmentValueModel::onDebounceTimeout() {
  submitSettled(value_);
  emit settledCommitted();
}

// ── EditorAdjustmentEnumModel ───────────────────────────────────────────────

EditorAdjustmentEnumModel::EditorAdjustmentEnumModel(QObject* parent)
    : EditorAdjustmentModelBase(parent) {}

void EditorAdjustmentEnumModel::setEntries(const QVariantList& entries) {
  if (entries_ == entries) {
    return;
  }
  entries_ = entries;
  emit entriesChanged();
  // Re-clamp the current index against the new entry set.
  if (entries_.isEmpty()) {
    if (currentIndex_ != 0) {
      currentIndex_ = 0;
      emit currentIndexChanged();
    }
  } else if (currentIndex_ < 0 || currentIndex_ >= entries_.size()) {
    currentIndex_ = 0;
    emit currentIndexChanged();
  }
}

void EditorAdjustmentEnumModel::setCurrentIndex(int i) {
  if (entries_.isEmpty() || i < 0 || i >= entries_.size() || currentIndex_ == i) {
    return;
  }
  currentIndex_ = i;
  emit currentIndexChanged();
}

auto EditorAdjustmentEnumModel::currentValue() const -> QString {
  if (entries_.isEmpty() || currentIndex_ < 0 || currentIndex_ >= entries_.size()) {
    return {};
  }
  return entries_[currentIndex_].toMap().value("value").toString();
}

void EditorAdjustmentEnumModel::setDefaultIndex(int i) {
  if (defaultIndex_ == i) {
    return;
  }
  defaultIndex_ = i;
  emit defaultIndexChanged();
}

void EditorAdjustmentEnumModel::selectIndex(int i) {
  if (entries_.isEmpty() || i < 0 || i >= entries_.size() || currentIndex_ == i) {
    return;
  }
  currentIndex_ = i;
  emit currentIndexChanged();
  submitNow(resolveParams(QJSValue(currentValue()), enumParamsJson(currentIndex_, currentValue())),
            true);
}

void EditorAdjustmentEnumModel::reset() {
  if (defaultIndex_ == currentIndex_ || defaultIndex_ < 0 ||
      (!entries_.isEmpty() && defaultIndex_ >= entries_.size())) {
    return;
  }
  currentIndex_ = defaultIndex_;
  emit currentIndexChanged();
  submitNow(resolveParams(QJSValue(currentValue()), enumParamsJson(currentIndex_, currentValue())),
            true);
}

// ── EditorAdjustmentToggleModel ──────────────────────────────────────────────

EditorAdjustmentToggleModel::EditorAdjustmentToggleModel(QObject* parent)
    : EditorAdjustmentModelBase(parent) {}

void EditorAdjustmentToggleModel::setValue(bool v) {
  if (value_ == v) {
    return;
  }
  value_ = v;
  emit valueChanged();
}

void EditorAdjustmentToggleModel::setDefaultValue(bool v) {
  if (defaultValue_ == v) {
    return;
  }
  defaultValue_ = v;
  emit defaultValueChanged();
}

void EditorAdjustmentToggleModel::commitValue(bool v) {
  if (value_ == v) {
    return;
  }
  value_ = v;
  emit valueChanged();
  submitNow(resolveParams(QJSValue(v), toggleParamsJson(v)), true);
}

void EditorAdjustmentToggleModel::toggle() {
  commitValue(!value_);
}

void EditorAdjustmentToggleModel::reset() {
  commitValue(defaultValue_);
}

// ── QML registration ─────────────────────────────────────────────────────────

void RegisterEditorAdjustmentQmlTypes() {
  qmlRegisterType<EditorAdjustmentValueModel>("Alcedo.Main", 1, 0,
                                              "EditorAdjustmentValueModel");
  qmlRegisterType<EditorAdjustmentEnumModel>("Alcedo.Main", 1, 0,
                                              "EditorAdjustmentEnumModel");
  qmlRegisterType<EditorAdjustmentToggleModel>("Alcedo.Main", 1, 0,
                                               "EditorAdjustmentToggleModel");
}

}  // namespace alcedo::ui