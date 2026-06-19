//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QRectF>
#include <QResizeEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "ui/edit_viewer/crop_interaction_controller.hpp"
#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/edit_viewer_surface.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

#ifdef HAVE_CUDA
#include "ui/edit_viewer/frame_mailbox.hpp"
#endif

class QMouseEvent;
class QNativeGestureEvent;
class QWheelEvent;

namespace alcedo {

class EditViewerOverlayWidget;

class QtEditViewer : public QWidget, public alcedo::IFrameSink {
  Q_OBJECT
 public:
  explicit QtEditViewer(QWidget* parent = nullptr);
  ~QtEditViewer() override;

  // Reset zoom/pan to default view
  void ResetView();
  void SetCropToolEnabled(bool enabled);
  void SetCropOverlayVisible(bool visible);
  void SetCropOverlayRectNormalized(float x, float y, float w, float h);
  void SetCropOverlayRotationDegrees(float angle_degrees);
  void SetCropOverlayAspectLock(bool enabled, float aspect_ratio);
  void ResetCropOverlayRectToFull();
  auto GetViewZoom() const -> float;
  auto IsViewInteractionActive() const -> bool;
  void SetDisplayEncoding(ColorUtils::ColorSpace encoding_space,
                          ColorUtils::EOTF       encoding_eotf,
                          float                  peak_luminance);
  void SyncPendingFrameStateForScheduling();
  void SetExpectedDetailToken(std::uint64_t preview_generation, std::uint64_t detail_serial);
  void ClearExpectedDetailToken();

  // Overrides from IFrameSink
  void    EnsureSize(int width, int height) override;
  auto    MapResourceForWrite(
      FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice) -> FrameWriteMapping override;
  void    UnmapResource() override;
  void    NotifyFrameReady() override;
  void    SubmitHostFrame(const ViewerFrame& frame) override;
#ifdef HAVE_METAL
  void    SubmitMetalFrame(const ViewerMetalFrame& frame) override;
#endif
  int     GetWidth() const override;
  int     GetHeight() const override;
  auto    GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> override;
  void    SetNextFramePresentationMode(FramePresentationMode mode) override;
  void    SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) override;
  auto    GetViewerSurface() -> IEditViewerSurface* override;
  auto    GetViewerSurface() const -> const IEditViewerSurface* override;

  void    SetHistogramFrameExpected(bool expected_fast_preview);
  void    SetHistogramUpdateIntervalMs(int interval_ms);
  auto    SupportsHistogram() const -> bool;

  // Consumes and clears the per-frame "frame delivered" flag. Returns whether a
  // frame was actually presented (via NotifyFrameReady / SubmitHostFrame /
  // SubmitMetalFrame) since the last EnsureSize. The render coordinator uses
  // this to detect a skipped presentation (render target not yet sized) and
  // replay the request once the target has been resized.
  auto    ConsumeFrameDelivered() -> bool;

 signals:
  void RequestUpdate();

  void RequestResize(int width, int height);
  void HistogramDataUpdated();
  void CropOverlayRectChanged(float x, float y, float w, float h, bool is_final);
  void CropOverlayRotationChanged(float angle_degrees, bool is_final);
  void ViewZoomChanged(float zoom);
  void ViewInteractionSettled();

 private slots:
  void OnResizeSurface(int w, int h);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  friend class EditViewerOverlayWidget;

  void                    HandleQueuedUpdate();
  void                    ResizeChildWidgets();
  void                    UpdateViewportRenderRegionCache();
  void                    ReconcileViewTransformForRenderReference();
  void                    RefreshFrameDerivedState();
  void                    SyncSurfaceState();
  void                    StopZoomAnimation();
  void                    MarkViewInteractionChanged();
  void                    HandleViewInteractionSettled();
  auto                    CurrentWidgetInfo() const -> ViewportWidgetInfo;
  auto                    CurrentImageInfo() const -> ViewportImageInfo;
  auto                    CurrentInteractionImageInfo() const -> ViewportImageInfo;
  auto                    CurrentPresentationMode() const -> FramePresentationMode;
  auto                    CurrentOverlaySnapshot() const -> EditViewerOverlaySnapshot;
  auto                    CurrentOverlayHover(const QPointF& event_pos) const -> EditViewerOverlayHover;
  void                    ApplyOverlayCursor(std::optional<Qt::CursorShape> cursor, bool unset);
  void                    ApplyViewTransformResult(const ViewTransformResult& result);
  void                    ApplyCropInteractionResult(const CropInteractionResult& result);
  void                    UpdateSurface();
  void                    UpdateOverlay();
  void                    PaintOverlay(QWidget& widget);
  void                    HandleOverlayWheel(QWheelEvent* event);
  void                    HandleOverlayNativeGesture(QNativeGestureEvent* event);
  void                    HandleOverlayMousePress(QMouseEvent* event);
  void                    HandleOverlayMouseMove(QMouseEvent* event);
  void                    HandleOverlayMouseRelease(QMouseEvent* event);
  void                    HandleOverlayMouseDoubleClick(QMouseEvent* event);
  void                    HandleOverlayLeave();

  static constexpr int     kZoomAnimationDurationMs = 170;
  static constexpr int     kViewInteractionSettleDelayMs = 120;

  ViewerState              viewer_state_{};
  ViewTransformController  view_transform_controller_{};
  CropInteractionController crop_interaction_controller_{};
  std::unique_ptr<IEditViewerSurface> surface_{};
  IEditViewerRenderTargetSurface* render_target_surface_ = nullptr;
  EditViewerOverlayWidget* overlay_ = nullptr;
  QVariantAnimation*       zoom_animation_ = nullptr;
  QTimer*                  click_toggle_timer_ = nullptr;
  QTimer*                  view_interaction_settle_timer_ = nullptr;

#ifdef HAVE_CUDA
  FrameMailbox             frame_mailbox_{};
#endif

  ViewerFrame              active_host_frame_{};
  std::optional<ViewerFrame> pending_host_frame_{};
#ifdef HAVE_METAL
  ViewerMetalFrame         active_metal_frame_{};
  std::optional<ViewerMetalFrame> pending_metal_frame_{};
#endif
  ViewerDisplayConfig      active_display_config_{};
  ViewerDisplayConfig      pending_display_config_{};
  int                      active_frame_width_ = 0;
  int                      active_frame_height_ = 0;
  FramePresentationMode    active_presentation_mode_ = FramePresentationMode::FullFrame;
  FramePreviewMetadata     active_preview_metadata_{};
  bool                     pending_presentation_mode_valid_ = false;
  FramePresentationMode    pending_presentation_mode_ = FramePresentationMode::FullFrame;
  bool                     pending_preview_metadata_valid_ = false;
  FramePreviewMetadata     pending_preview_metadata_{};
  bool                     pending_display_config_valid_ = false;
  bool                     prefer_interactive_primary_ = false;
  bool                     allow_detail_patch_ = true;
  bool                     view_interaction_active_ = false;
  bool                     has_expected_detail_token_ = false;
  std::uint64_t            expected_detail_generation_ = 0;
  std::uint64_t            expected_detail_serial_ = 0;
  mutable std::mutex       host_frame_mutex_{};

  // Frame size requested by the render thread in the most recent EnsureSize
  // call. Accessed only on the render thread (EnsureSize -> MapResourceForWrite
  // are consecutive calls on the same thread), so no synchronization needed.
  int                      ensure_width_  = 0;
  int                      ensure_height_ = 0;
  // Set false in EnsureSize (start of a frame) and true when a frame is actually
  // delivered (NotifyFrameReady / SubmitHostFrame / SubmitMetalFrame). Read and
  // cleared by the render coordinator after the render completes to detect a
  // skipped presentation.
  std::atomic<bool>        frame_delivered_{false};
  // Set true in EnsureSize to mark that the present sink was engaged for this
  // frame (GPU/CUDA/OpenCL/Metal paths). CPU-only renders never call EnsureSize,
  // so this stays false; ConsumeFrameDelivered then treats them as delivered so
  // they are not misclassified as a skipped presentation.
  std::atomic<bool>        sink_engaged_{false};
};
};  // namespace alcedo
