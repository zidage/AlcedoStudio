//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_comparison_types.hpp"
#include "app/editor_image_render_port.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_request_ids.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/pipeline/rendered_pipeline_image.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief The open editor comparison of the session: captured working values, selected sources,
 *        the image job of the selected pair, and the rendered pair until the GUI takes it.
 */
namespace alcedo {

/**
 * @brief Owner of one open editor comparison.
 *
 * A collaborator of EditorSessionService. The session owns command admission and the comparison
 * action restriction (its operation lease); this class owns what the comparison shows:
 *
 * - the working-values preview captured once when the comparison opened (kept until close, so
 *   Current and the active Version never change while the comparison is open);
 * - the selected kind and sources;
 * - the image job of the selected pair on the editor render worker;
 * - the rendered pair, from its completion until the GUI takes it.
 *
 * It reads history only through IEditorHistoryPort::BuildComparisonInputs and renders only
 * through IEditorImageRenderPort. It never writes the working document, history, or a save.
 *
 * Thread: every member except state() and TakeImages() runs on the session owner thread.
 * state() and TakeImages() are safe on any thread; they read the published state under
 * @ref publish_mutex_.
 *
 * Lifetime: an accepted image job calls Dependencies::post_to_owner from the editor render
 * worker. The image port must finish or cancel that job (its Shutdown waits for the completion)
 * before this object is destroyed.
 */
class EditorComparisonService final {
 public:
  /// Posts a task to the session owner thread. Never runs the task inline.
  using OwnerPoster = std::function<void(std::function<void()>)>;

  struct Dependencies {
    IEditorHistoryPort*                     history = nullptr;
    /// Null when the session has no image render port; every render then fails with its reason.
    std::shared_ptr<IEditorImageRenderPort> images;
    OwnerPoster                             post_to_owner;
  };

  /// The open image that the comparison belongs to.
  struct ImageTarget {
    sl_element_id_t          element_id = 0;
    image_id_t               image_id   = 0;
    ImageLoadRequestId       image_load_request{};
    EditorHistoryGuardHandle guard{};
  };

  explicit EditorComparisonService(Dependencies dependencies);
  /// Cancels an accepted image job. See the class lifetime note.
  ~EditorComparisonService();

  EditorComparisonService(const EditorComparisonService&)                    = delete;
  auto operator=(const EditorComparisonService&) -> EditorComparisonService& = delete;

  /// True while a comparison is open. Owner thread.
  [[nodiscard]] auto active() const -> bool { return activity_.has_value(); }

  /**
   * @brief Open a comparison of @p target and render its default pair: Root and Current.
   *
   * @param operation_id Session command that opened the comparison.
   * @param captured_current Preview of the working values at entry. Kept until Close.
   * @pre No comparison is open; the caller has settled pending input and holds the image.
   * The comparison stays open when the pair cannot be built or rendered; its state is Failed
   * with the real reason, so the user can select other sources, retry, or close.
   */
  void Open(std::uint64_t operation_id, ImageTarget target,
            std::shared_ptr<const PipelineGraphSnapshot> captured_current, EditorComparisonKind kind);

  /**
   * @brief Select the kind and the sources of the open comparison.
   *
   * Renders the new pair when a source changed; a kind-only change renders nothing. The
   * previous pair is dropped before the new render starts.
   * @return false with @p error set when no comparison is open or a pair is rendering.
   */
  auto SelectSources(EditorComparisonKind kind, const EditorComparisonSource& a,
                     const EditorComparisonSource& b, std::string* error) -> bool;

  /// Render the selected pair again. False with @p error when none is open or one is rendering.
  auto Retry(std::string* error) -> bool;

  /**
   * @brief Close the open comparison.
   *
   * Cancels its image job, and releases the captured preview, the pair documents, and the
   * rendered images that the GUI has not taken. A completion of the cancelled job that is already
   * queued for the owner is ignored when it arrives.
   * @return true when a comparison was open.
   */
  auto Close() -> bool;

  /**
   * @brief Reduce the completion of image job @p job_id on the owner thread.
   *
   * Ignored unless @p job_id is the job of the selected pair of the open comparison; a closed
   * comparison or a replaced selection therefore never publishes a pair. A Completed result
   * with both images makes the state Ready; any other result makes it Failed with its reason.
   */
  void HandleImagesFinished(std::uint64_t job_id, EditorImageRenderResult result);

  /// Published read of the open comparison. Any thread.
  [[nodiscard]] auto state() const -> EditorComparisonState;

  /**
   * @brief Move the rendered pair of @p pair_id out of the service. Any thread.
   * @return A and B, or empty when @p pair_id is not the Ready pair or was already taken.
   */
  auto TakeImages(std::uint64_t pair_id) -> std::vector<RenderedPipelineImage>;

 private:
  /// Owner-thread state of the open comparison.
  struct Activity {
    std::uint64_t                                operation_id = 0;
    ImageTarget                                  target{};
    std::shared_ptr<const PipelineGraphSnapshot> captured_current;
    EditorComparisonKind                         kind = EditorComparisonKind::BeforeAfter;
    EditorComparisonSource                       a    = EditorComparisonSource::Root();
    EditorComparisonSource                       b    = EditorComparisonSource::Current();
    /// Image job of the selected pair while it renders; 0 otherwise.
    std::uint64_t                                job_id = 0;
  };

  /// Build the selected pair and schedule its image job. Publishes Rendering or Failed.
  void RenderSelectedPair();
  /// Replace the published state. @p images are the Ready pair; empty for every other status.
  void Publish(EditorComparisonStatus status, std::uint64_t pair_id, std::string error,
               std::vector<RenderedPipelineImage> images = {});
  [[nodiscard]] auto Rendering() const -> bool;

  Dependencies                       dependencies_;
  std::optional<Activity>            activity_;

  mutable std::mutex                 publish_mutex_;
  EditorComparisonState              published_{};
  /// Ready pair (A, B) of published_.pair_id until TakeImages moves it out.
  std::vector<RenderedPipelineImage> ready_images_;
};

}  // namespace alcedo
