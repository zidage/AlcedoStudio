//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <cstdint>
#include <memory>

#include "app/editor_comparison_types.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {

/**
 * @brief QML projection of the editor comparison.
 *
 * Reads the comparison state that the session owner publishes and routes the Compare actions to
 * the session backend. It owns no comparison documents and no copy of the session or history
 * state: sources, status, and the rendered pair come from the backend.
 *
 * On the GUI thread it takes a Ready pair from the backend, converts both images to the approved
 * 8-bit SDR presentation, and publishes them into the comparison image store in one operation
 * (PublishComparisonPair). The float images are released after the conversion. It clears the
 * store and the QML pair when the selection changes or the comparison closes.
 *
 * The view options (display mode, orientation, divider position, and swap) only change how a
 * ready pair is shown. They are kept here, so they survive QML panel recreation; they never
 * render. Each new comparison starts with a left/right divider at the midpoint.
 *
 * Thread: GUI thread only.
 */
class EditorComparisonController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY ComparisonChanged)
  /// "idle", "loading", "ready", or "failed".
  Q_PROPERTY(QString status READ status NOTIFY ComparisonChanged)
  Q_PROPERTY(QString errorText READ error_text NOTIFY ComparisonChanged)
  /// "beforeAfter" or "versions".
  Q_PROPERTY(QString comparisonKind READ comparison_kind NOTIFY ComparisonChanged)
  /// Selected sources: "root", "current", or "version:<id>".
  Q_PROPERTY(QString aSourceValue READ a_source_value NOTIFY ComparisonChanged)
  Q_PROPERTY(QString bSourceValue READ b_source_value NOTIFY ComparisonChanged)
  Q_PROPERTY(QString aLabel READ a_label NOTIFY ComparisonChanged)
  Q_PROPERTY(QString bLabel READ b_label NOTIFY ComparisonChanged)
  /// Source choices of the open image: [{ value, label }, ...].
  Q_PROPERTY(QVariantList sourceOptions READ source_options NOTIFY ComparisonChanged)
  /// ComparisonPairPublication::ToVariantMap() of the shown pair, or null.
  Q_PROPERTY(QVariant pair READ pair NOTIFY PairChanged)
  /// "complete" or "divider".
  Q_PROPERTY(QString displayMode READ display_mode NOTIFY ViewChanged)
  /// "horizontal" (left/right) or "vertical" (top/bottom).
  Q_PROPERTY(QString orientation READ orientation NOTIFY ViewChanged)
  Q_PROPERTY(double dividerPosition READ divider_position NOTIFY ViewChanged)
  Q_PROPERTY(bool swapped READ swapped NOTIFY ViewChanged)
  /// Session admission of a new comparison (EditorAction::OpenComparison).
  Q_PROPERTY(bool canOpen READ can_open NOTIFY AvailabilityChanged)
  Q_PROPERTY(QString openUnavailableReason READ open_unavailable_reason NOTIFY AvailabilityChanged)

 public:
  /**
   * @param session Session controller of the editor; its backend owns the comparison.
   * @param store Image store that the `alcedo-comparison` QML image provider reads.
   */
  EditorComparisonController(EditorSessionController*              session,
                             std::shared_ptr<ComparisonImageStore> store,
                             QObject*                              parent = nullptr);
  ~EditorComparisonController() override;

  [[nodiscard]] auto active() const -> bool { return state_.active(); }
  [[nodiscard]] auto status() const -> QString;
  [[nodiscard]] auto error_text() const -> QString;
  [[nodiscard]] auto comparison_kind() const -> QString;
  [[nodiscard]] auto a_source_value() const -> QString { return SourceValue(state_.a); }
  [[nodiscard]] auto b_source_value() const -> QString { return SourceValue(state_.b); }
  [[nodiscard]] auto a_label() const -> QString { return SourceLabel(state_.a); }
  [[nodiscard]] auto b_label() const -> QString { return SourceLabel(state_.b); }
  [[nodiscard]] auto source_options() const -> QVariantList { return source_options_; }
  [[nodiscard]] auto pair() const -> QVariant { return pair_; }
  [[nodiscard]] auto display_mode() const -> QString { return display_mode_; }
  [[nodiscard]] auto orientation() const -> QString { return orientation_; }
  [[nodiscard]] auto divider_position() const -> double { return divider_position_; }
  [[nodiscard]] auto swapped() const -> bool { return swapped_; }
  [[nodiscard]] auto can_open() const -> bool;
  [[nodiscard]] auto open_unavailable_reason() const -> QString;

  /// Open Before/After: A is the imported root, B the current working state.
  Q_INVOKABLE void   openBeforeAfter();
  /// Open Version comparison with the same default sources; both selectors then accept Versions.
  Q_INVOKABLE void   openVersions();
  /// "beforeAfter" selects Root and Current; "versions" keeps the selected sources.
  Q_INVOKABLE void   selectKind(const QString& kind);
  Q_INVOKABLE void   selectASource(const QString& value);
  Q_INVOKABLE void   selectBSource(const QString& value);
  Q_INVOKABLE void   setDisplayMode(const QString& mode);
  Q_INVOKABLE void   setOrientation(const QString& orientation);
  Q_INVOKABLE void   setDividerPosition(double position);
  Q_INVOKABLE void   swap();
  /// Render the selected pair again after a failure.
  Q_INVOKABLE void   retry();
  /// Close the comparison and render the current document with the current view.
  Q_INVOKABLE void   close();
  /// A QML Image item could not load a published image. Shows @p message as the failure.
  Q_INVOKABLE void   reportImageLoadFailed(const QString& message);

  /// Read the backend comparison state again. Connected to the session's change notification.
  void               Refresh();

  /// Encoding of a source for QML: "root", "current", or "version:<id>".
  [[nodiscard]] static auto SourceValue(const alcedo::EditorComparisonSource& source) -> QString;
  /// Parse SourceValue output. False for an unknown value.
  [[nodiscard]] static auto ParseSourceValue(const QString&                  value,
                                             alcedo::EditorComparisonSource* source) -> bool;

 signals:
  void ComparisonChanged();
  void PairChanged();
  void ViewChanged();
  void AvailabilityChanged();

 private:
  [[nodiscard]] auto Backend() const -> alcedo::IEditorSessionBackend*;
  [[nodiscard]] auto SourceLabel(const alcedo::EditorComparisonSource& source) const -> QString;
  void               Open(alcedo::EditorComparisonKind kind);
  void               SelectSources(alcedo::EditorComparisonKind kind,
                                   const alcedo::EditorComparisonSource& a,
                                   const alcedo::EditorComparisonSource& b);
  void               RebuildSourceOptions();
  void               ResetViewOptions();
  /// Take, convert, and publish the Ready pair of @ref state_ once.
  void               PublishReadyPair();
  void               ClearPair();

  QPointer<EditorSessionController>     session_;
  std::shared_ptr<ComparisonImageStore> store_;
  alcedo::EditorComparisonState         state_{};
  QVariantList                          source_options_;
  QVariant                              pair_;
  /// Image job whose pair is in the store; 0 when none is.
  std::uint64_t                         published_pair_id_  = 0;
  /// Pair whose conversion or image load failed in the GUI, with the reason.
  std::uint64_t                         failed_pair_id_     = 0;
  QString                               failed_pair_reason_;
  QString                               display_mode_       = QStringLiteral("divider");
  QString                               orientation_        = QStringLiteral("horizontal");
  double                                divider_position_   = 0.5;
  bool                                  swapped_            = false;
};

}  // namespace alcedo::ui
