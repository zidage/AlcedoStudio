//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QJSValue>
#include <QString>
#include <QVariantList>

#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"

class QTimer;

namespace alcedo::ui {

/// Typed adjustment models. Focused QObjects carrying value/range/enum/
/// toggle state, defaults, validation, and pointer-drag state. The models
/// enqueue one field write at a time through the `IEditorAdjustmentSubmitter`
/// seam; they never touch the pipeline scheduler, the render coordinator, or
/// the journal. One `settled=true` enqueue per completed pointer drag seals
/// that sequence. Interactive previews while dragging use `settled=false`.
///
/// `fieldKey` is the stable history identifier; `label` is the display name the
/// panel shows. The history layer derives the human-readable row label from
/// `fieldKey` independently, so no label is threaded into the patch.
///
/// Load vs edit: each model's Q_PROPERTY WRITE setter is a plain no-submit
/// setter (programmatic load from session state). User edits go through the
/// Q_INVOKABLE methods (`editValue` / `selectIndex` / `commitValue` and the
/// pointer-drag methods), which submit. This prevents spurious commits and feedback
/// loops when the panel binds the model to session state.

class EditorAdjustmentModelBase : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString fieldKey READ fieldKey WRITE setFieldKey NOTIFY fieldKeyChanged)
  Q_PROPERTY(QString label READ label WRITE setLabel NOTIFY labelChanged)
  Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
  /// QObject that implements IEditorAdjustmentSubmitter (production:
  /// EditorSessionController; tests: a recording fake). Set in QML via
  /// `submitter: editorSession`. The model dynamic_casts to the interface.
  Q_PROPERTY(QObject* submitter READ submitter WRITE setSubmitter NOTIFY submitterChanged)
  /// Optional JS callback `(value) => string` returning the full params JSON.
  /// When unset, the model builds a default payload ({"value": v} etc.).
  Q_PROPERTY(QJSValue paramsBuilder READ paramsBuilder WRITE setParamsBuilder NOTIFY paramsBuilderChanged)

 public:
  explicit EditorAdjustmentModelBase(QObject* parent = nullptr);
  ~EditorAdjustmentModelBase() override;

  [[nodiscard]] QString fieldKey() const { return fieldKey_; }
  void setFieldKey(const QString& key);
  [[nodiscard]] QString label() const { return label_; }
  void setLabel(const QString& label);
  [[nodiscard]] bool enabled() const { return enabled_; }
  void setEnabled(bool enabled);
  [[nodiscard]] QObject* submitter() const { return submitterObject_; }
  void setSubmitter(QObject* submitter);
  [[nodiscard]] QJSValue paramsBuilder() const { return paramsBuilder_; }
  void setParamsBuilder(const QJSValue& builder);

 signals:
  void fieldKeyChanged();
  void labelChanged();
  void enabledChanged();
  void submitterChanged();
  void paramsBuilderChanged();

 protected:
  /// Default params JSON shapes (used when paramsBuilder is unset).
  [[nodiscard]] static auto numericParamsJson(double value) -> QString;
  [[nodiscard]] static auto enumParamsJson(int index, const QString& value) -> QString;
  [[nodiscard]] static auto toggleParamsJson(bool value) -> QString;
  /// Resolve the params JSON: call `paramsBuilder(arg)` when it is callable,
  /// otherwise return `defaultJson`.
  [[nodiscard]] auto resolveParams(const QJSValue& arg, const QString& defaultJson) const
      -> QString;
  /// Enqueue one typed field write through the seam. Defensive: no-op (returns false)
  /// when there is no submitter or the session cannot accept input
  /// (`canEdit()` false). `settled=false` continues the sequence;
  /// `settled=true` seals it with Release. True means accepted for processing,
  /// not live-applied or history-committed.
  auto submitNow(alcedo::EditorParameterWrite write, bool settled) -> bool;
  /// Parse QML-collected field JSON once, then enqueue the typed write.
  auto submitJsonBoundary(const QString& paramsJson, bool settled) -> bool;
  [[nodiscard]] auto submitterHandle() const -> IEditorAdjustmentSubmitter* {
    return submitter_;
  }

 private:
  QObject* submitterObject_ = nullptr;
  IEditorAdjustmentSubmitter* submitter_ = nullptr;
  QJSValue paramsBuilder_;
  QString fieldKey_;
  QString label_;
  bool enabled_ = true;
};

/// Numeric adjustment (exposure, contrast, saturation, …). Owns the pointer
/// drag state and a debounced settled commit for keyboard/wheel bursts. One
/// settled transaction per completed drag.
class EditorAdjustmentValueModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)
  Q_PROPERTY(double defaultValue READ defaultValue WRITE setDefaultValue NOTIFY
                 defaultValueChanged)
  Q_PROPERTY(double minimum READ minimum WRITE setMinimum NOTIFY minimumChanged)
  Q_PROPERTY(double maximum READ maximum WRITE setMaximum NOTIFY maximumChanged)
  Q_PROPERTY(double step READ step WRITE setStep NOTIFY stepChanged)
  Q_PROPERTY(int precision READ precision WRITE setPrecision NOTIFY precisionChanged)
  Q_PROPERTY(QString suffix READ suffix WRITE setSuffix NOTIFY suffixChanged)
  Q_PROPERTY(bool valid READ valid NOTIFY validChanged)
  Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY validChanged)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  explicit EditorAdjustmentValueModel(QObject* parent = nullptr);

  [[nodiscard]] double value() const { return value_; }
  /// Plain setter (programmatic load): clamps to [minimum, maximum], sets the
  /// value, and clears any invalid state. Does NOT submit a patch.
  void setValue(double v);
  [[nodiscard]] double defaultValue() const { return defaultValue_; }
  void setDefaultValue(double v);
  [[nodiscard]] double minimum() const { return minimum_; }
  void setMinimum(double v);
  [[nodiscard]] double maximum() const { return maximum_; }
  void setMaximum(double v);
  [[nodiscard]] double step() const { return step_; }
  void setStep(double v);
  [[nodiscard]] int precision() const { return precision_; }
  void setPrecision(int p);
  [[nodiscard]] QString suffix() const { return suffix_; }
  void setSuffix(const QString& s);
  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] QString errorMessage() const { return errorMessage_; }
  [[nodiscard]] bool dragActive() const { return dragActive_; }

  // Pointer-drag input. updateDrag submits an interactive preview
  // (settled=false); finishDrag submits one settled transaction.
  Q_INVOKABLE void beginDrag();
  Q_INVOKABLE void updateDrag(double v);
  Q_INVOKABLE void finishDrag();
  // User edit (keyboard / wheel): interactive preview + a debounced settled
  // commit. One settled transaction per stabilized burst.
  Q_INVOKABLE void editValue(double v);
  // Force the debounced settled commit now (Enter / field focus-out).
  Q_INVOKABLE void commitImmediately();
  // Reset to defaultValue and commit one settled transaction immediately.
  Q_INVOKABLE void reset();
  // Report an invalid field entry (non-numeric / unparseable). Marks valid=false
  // and does NOT submit. A subsequent setValue/editValue clears it.
  Q_INVOKABLE void setInvalid(const QString& message);
  [[nodiscard]] Q_INVOKABLE bool hasPendingSettled() const;
  // C++-only test hook: stabilization interval in ms (default 180). Setting 0
  // makes the debounce fire on the next event-loop iteration so QSignalSpy::wait
  // catches it deterministically.
  void setDebounceIntervalMs(int ms);

 signals:
  void valueChanged();
  void defaultValueChanged();
  void minimumChanged();
  void maximumChanged();
  void stepChanged();
  void precisionChanged();
  void suffixChanged();
  void validChanged();
  void dragActiveChanged();
  /// Emitted when a settled commit lands (drag release, debounce fire, reset,
  /// or commitImmediately). Tests QSignalSpy::wait on this.
  void settledCommitted();

 private:
  [[nodiscard]] auto clamp(double v) const -> double;
  // Clamp + set + clear invalid + emit valueChanged. Returns whether the value
  // (or valid state) actually changed.
  auto applyValue(double v) -> bool;
  void submitInteractive(double v);
  void submitSettled(double v);
  void onDebounceTimeout();

  QTimer* debounceTimer_ = nullptr;
  int debounceIntervalMs_ = 180;
  double value_ = 0.0;
  double defaultValue_ = 0.0;
  double minimum_ = 0.0;
  double maximum_ = 1.0;
  double step_ = 0.01;
  int precision_ = 2;
  QString suffix_;
  bool valid_ = true;
  QString errorMessage_;
  bool dragActive_ = false;
  /// True when updateDrag changed the value during the current pointer drag. A
  /// press+release without movement (including half of a double-click) must not
  /// emit a settled seal; double-click reset owns that path instead.
  bool dragMoved_ = false;
};

/// Enum adjustment (color-temp mode, demosaic method, …). A user selection
/// commits one settled transaction immediately.
class EditorAdjustmentEnumModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries WRITE setEntries NOTIFY entriesChanged)
  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY
                 currentIndexChanged)
  Q_PROPERTY(QString currentValue READ currentValue NOTIFY currentIndexChanged)
  Q_PROPERTY(int defaultIndex READ defaultIndex WRITE setDefaultIndex NOTIFY
                 defaultIndexChanged)

 public:
  explicit EditorAdjustmentEnumModel(QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const { return entries_; }
  void setEntries(const QVariantList& entries);
  [[nodiscard]] int currentIndex() const { return currentIndex_; }
  /// Plain setter (programmatic load). Does NOT submit a patch.
  void setCurrentIndex(int i);
  [[nodiscard]] QString currentValue() const;
  [[nodiscard]] int defaultIndex() const { return defaultIndex_; }
  void setDefaultIndex(int i);
  /// User selection: set the index and commit one settled transaction.
  Q_INVOKABLE void selectIndex(int i);
  Q_INVOKABLE void reset();

 signals:
  void entriesChanged();
  void currentIndexChanged();
  void defaultIndexChanged();

 private:
  bool clampIndex(int i) const;

  QVariantList entries_;
  int currentIndex_ = 0;
  int defaultIndex_ = 0;
};

/// Boolean toggle adjustment (lens-calib enabled, highlights reconstruct, …).
/// A user toggle commits one settled transaction immediately.
class EditorAdjustmentToggleModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(bool value READ value WRITE setValue NOTIFY valueChanged)
  Q_PROPERTY(bool defaultValue READ defaultValue WRITE setDefaultValue NOTIFY
                 defaultValueChanged)

 public:
  explicit EditorAdjustmentToggleModel(QObject* parent = nullptr);

  [[nodiscard]] bool value() const { return value_; }
  /// Plain setter (programmatic load). Does NOT submit a patch.
  void setValue(bool v);
  [[nodiscard]] bool defaultValue() const { return defaultValue_; }
  void setDefaultValue(bool v);
  /// User edit: set the value and commit one settled transaction.
  Q_INVOKABLE void commitValue(bool v);
  /// Flip the current value and commit one settled transaction.
  Q_INVOKABLE void toggle();
  Q_INVOKABLE void reset();

 signals:
  void valueChanged();
  void defaultValueChanged();

 private:
  bool value_ = false;
  bool defaultValue_ = false;
};

/// Register the typed adjustment models and shared editor plot item with the
/// Alcedo.Main QML module. Called from ApplicationModuleHost's constructor and
/// by standalone adjustment-stack QML tests.
void RegisterEditorAdjustmentQmlTypes();

}  // namespace alcedo::ui
