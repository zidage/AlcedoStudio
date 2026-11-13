//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_controller_test.cpp
/// @brief The GUI side of the editor comparison: Compare-page selection and restore in the
///        production EditorSessionController, and the production EditorComparisonController over
///        a session backend that publishes comparison states and rendered pairs.

#include "ui/alcedo_main/album_backend/editor_comparison_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_session_service.hpp"
#include "editor_comparison_test_support.hpp"
#include "type/hash_type.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui::test {
namespace {

/**
 * @brief Session backend that publishes the comparison state the test sets, as the session owner
 *        does, and records the comparison requests that reach it.
 */
class ComparisonSessionBackend final : public IEditorSessionBackend {
 public:
  ComparisonSessionBackend() {
    availability_.decisions[static_cast<std::size_t>(EditorAction::OpenComparison)].allowed = true;
  }

  [[nodiscard]] auto state() const -> EditorSessionState override {
    return EditorSessionState::Interactive;
  }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return {}; }
  [[nodiscard]] auto active() const -> bool override { return true; }
  [[nodiscard]] auto has_image() const -> bool override { return true; }
  [[nodiscard]] auto last_error() const -> std::string override { return {}; }
  [[nodiscard]] auto history_snapshot() -> EditorHistorySnapshot override { return history; }
  [[nodiscard]] auto action_availability() const -> EditorActionAvailability override {
    return availability_;
  }
  void SetPresentationSinkId(PresentationSinkId) override {}
  void SetPresentationSize(int, int) override {}
  auto Open(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Switch(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Close(bool) -> EditorSessionResult override { return {}; }
  auto Shutdown() -> EditorSessionResult override { return {}; }
  auto Discard() -> EditorSessionResult override { return {}; }
  auto Undo() -> EditorSessionResult override { return {}; }
  auto Redo() -> EditorSessionResult override { return {}; }

  auto OpenComparison(EditorComparisonKind kind) -> EditorSessionResult override {
    opened_kinds.push_back(kind);
    return {};
  }
  auto SelectComparisonSources(EditorComparisonKind kind, EditorComparisonSource a,
                               EditorComparisonSource b) -> EditorSessionResult override {
    selections.push_back({kind, a, b});
    return {};
  }
  auto RetryComparison() -> EditorSessionResult override {
    ++retry_count;
    return {};
  }
  auto CloseComparison(bool refresh_current_view, std::optional<ViewportRenderRegion>)
      -> EditorSessionResult override {
    close_refresh.push_back(refresh_current_view);
    return {};
  }
  [[nodiscard]] auto comparison_state() const -> EditorComparisonState override { return state_; }
  auto TakeComparisonImages(std::uint64_t pair_id) -> std::vector<RenderedPipelineImage> override {
    if (pair_id != state_.pair_id || state_.status != EditorComparisonStatus::Ready) {
      return {};
    }
    ++take_count;
    return std::exchange(ready_images_, {});
  }

  /// Publish @p state (and the Ready pair) and notify, as the owner does after a change.
  void Publish(EditorComparisonState state, std::vector<RenderedPipelineImage> images = {}) {
    state_        = std::move(state);
    ready_images_ = std::move(images);
    NotifyChange();
  }

  struct Selection {
    EditorComparisonKind   kind;
    EditorComparisonSource a;
    EditorComparisonSource b;
  };
  EditorHistorySnapshot                history;
  std::vector<EditorComparisonKind>    opened_kinds;
  std::vector<Selection>               selections;
  std::vector<bool>                    close_refresh;
  int                                  retry_count = 0;
  int                                  take_count  = 0;

 private:
  EditorActionAvailability           availability_{};
  EditorComparisonState              state_{};
  std::vector<RenderedPipelineImage> ready_images_;
};

auto OpenState(std::uint64_t operation_id, EditorComparisonStatus status, std::uint64_t pair_id,
               EditorComparisonSource a = EditorComparisonSource::Root(),
               EditorComparisonSource b = EditorComparisonSource::Current())
    -> EditorComparisonState {
  EditorComparisonState state;
  state.status       = status;
  state.operation_id = operation_id;
  state.pair_id      = pair_id;
  state.a            = a;
  state.b            = b;
  return state;
}

class EditorComparisonControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A process-local settings scope, so the stored startup panel can be read back.
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EditorComparisonControllerTest"));
    QSettings().remove(QStringLiteral("editor/activeAdjustmentPanel"));
    store_      = std::make_shared<ComparisonImageStore>();
    session_    = std::make_unique<EditorSessionController>(&backend_);
    comparison_ = std::make_unique<EditorComparisonController>(session_.get(), store_);
  }
  void TearDown() override {
    comparison_.reset();
    session_.reset();
    QSettings().remove(QStringLiteral("editor/activeAdjustmentPanel"));
  }

  /// A rendered pair of a 400 x 300 source, with weak references to its float buffers.
  auto MakePair(std::vector<std::weak_ptr<ImageBuffer>>* float_buffers)
      -> std::vector<RenderedPipelineImage> {
    const auto geometry = ResolveComparisonGeometry({400, 300});
    std::vector<RenderedPipelineImage> images = {
        MakeFilledRenderedImage(geometry, cv::Scalar(0.25, 0.5, 0.75, 1.0)),
        MakeFilledRenderedImage(geometry, cv::Scalar(0.75, 0.5, 0.25, 1.0))};
    if (float_buffers != nullptr) {
      for (const auto& image : images) float_buffers->push_back(image.pixels);
    }
    return images;
  }

  ComparisonSessionBackend                    backend_;
  std::shared_ptr<ComparisonImageStore>       store_;
  std::unique_ptr<EditorSessionController>    session_;
  std::unique_ptr<EditorComparisonController> comparison_;
};

TEST_F(EditorComparisonControllerTest, CompareNavPageRestoresPreviousPanelAfterClose) {
  session_->set_active_adjustment_panel(QStringLiteral("look"));
  ASSERT_EQ(session_->active_adjustment_panel(), QStringLiteral("look"));
  // The Compare page exists only while a comparison is open.
  session_->set_active_adjustment_panel(QStringLiteral("compare"));
  EXPECT_EQ(session_->active_adjustment_panel(), QStringLiteral("look"));

  backend_.Publish(OpenState(11, EditorComparisonStatus::Rendering, 5));
  EXPECT_EQ(session_->active_adjustment_panel(), QStringLiteral("compare"));
  EXPECT_EQ(QSettings().value(QStringLiteral("editor/activeAdjustmentPanel")).toString(),
            QStringLiteral("look"))
      << "the Compare page is never stored as the startup panel";
  // Other pages stay reachable read-only while comparing; Compare can be selected again.
  session_->set_active_adjustment_panel(QStringLiteral("tone"));
  session_->set_active_adjustment_panel(QStringLiteral("compare"));
  EXPECT_EQ(session_->active_adjustment_panel(), QStringLiteral("compare"));

  backend_.Publish({});
  EXPECT_EQ(session_->active_adjustment_panel(), QStringLiteral("look"));

  // A panel chosen while comparing stays when the comparison closes.
  backend_.Publish(OpenState(12, EditorComparisonStatus::Rendering, 6));
  ASSERT_EQ(session_->active_adjustment_panel(), QStringLiteral("compare"));
  session_->set_active_adjustment_panel(QStringLiteral("raw"));
  backend_.Publish({});
  EXPECT_EQ(session_->active_adjustment_panel(), QStringLiteral("raw"));
}

TEST_F(EditorComparisonControllerTest, RepeatedOpenSelectCloseKeepsOneExecutorAndReleasesTemporaryImages) {
  std::vector<std::weak_ptr<ImageBuffer>> float_buffers;
  for (std::uint64_t cycle = 1; cycle <= 3; ++cycle) {
    comparison_->openBeforeAfter();
    const std::uint64_t operation = 100 + cycle;
    const std::uint64_t first     = cycle * 10;
    backend_.Publish(OpenState(operation, EditorComparisonStatus::Rendering, first));
    EXPECT_EQ(comparison_->status(), QStringLiteral("loading"));
    EXPECT_FALSE(comparison_->pair().isValid());
    EXPECT_EQ(store_->ImageCount(), 0);

    backend_.Publish(OpenState(operation, EditorComparisonStatus::Ready, first),
                     MakePair(&float_buffers));
    ASSERT_EQ(comparison_->status(), QStringLiteral("ready")) << comparison_->error_text().toStdString();
    EXPECT_EQ(store_->ImageCount(), 2);
    const auto pair = comparison_->pair().toMap();
    EXPECT_EQ(pair.value(QStringLiteral("operationId")).toULongLong(), first);
    EXPECT_EQ(pair.value(QStringLiteral("aSource")).toString(),
              ComparisonImageStore::MakeUrl(first, ComparisonSide::A));
    // The float renders are released after the one SDR conversion.
    for (const auto& buffer : float_buffers) EXPECT_TRUE(buffer.expired());
    // Re-reading the same Ready state neither takes nor converts again.
    backend_.Publish(OpenState(operation, EditorComparisonStatus::Ready, first));
    EXPECT_EQ(backend_.take_count, static_cast<int>(cycle));

    // A new selection drops the shown pair while it renders.
    comparison_->selectBSource(QStringLiteral("root"));
    backend_.Publish(OpenState(operation, EditorComparisonStatus::Rendering, first + 1,
                               EditorComparisonSource::Root(), EditorComparisonSource::Root()));
    EXPECT_EQ(store_->ImageCount(), 0);
    EXPECT_FALSE(comparison_->pair().isValid());

    comparison_->close();
    backend_.Publish({});
    EXPECT_FALSE(comparison_->active());
    EXPECT_EQ(store_->ImageCount(), 0);
    EXPECT_FALSE(comparison_->pair().isValid());
  }
  // Every pair was requested from the one session backend, which renders on the editor executor.
  EXPECT_EQ(backend_.opened_kinds.size(), 3u);
  EXPECT_EQ(backend_.selections.size(), 3u);
  EXPECT_EQ(backend_.close_refresh, (std::vector<bool>{true, true, true}));
}

TEST_F(EditorComparisonControllerTest, LeavingTheEditorClosesTheComparisonWithoutARefresh) {
  WorkspaceRouter router(session_.get());
  router.OpenEditor(0, 0);
  backend_.Publish(OpenState(9, EditorComparisonStatus::Rendering, 1));
  ASSERT_TRUE(comparison_->active());

  router.OpenLibrary();
  // The hidden viewport needs no render; the comparison closes before the image persists.
  EXPECT_EQ(backend_.close_refresh, (std::vector<bool>{false}));
}

TEST_F(EditorComparisonControllerTest, ConversionAndImageLoadFailuresShowTheErrorAndRetryRenders) {
  backend_.Publish(OpenState(7, EditorComparisonStatus::Rendering, 1));
  // B's pixels do not match its render extent: no side is published.
  auto images = MakePair(nullptr);
  images[1].pixels =
      std::make_shared<ImageBuffer>(cv::Mat(10, 10, CV_32FC4, cv::Scalar(0.5, 0.5, 0.5, 1.0)));
  backend_.Publish(OpenState(7, EditorComparisonStatus::Ready, 1), std::move(images));
  EXPECT_EQ(comparison_->status(), QStringLiteral("failed"));
  EXPECT_FALSE(comparison_->error_text().isEmpty());
  EXPECT_EQ(store_->ImageCount(), 0);
  EXPECT_FALSE(comparison_->pair().isValid());

  comparison_->retry();
  EXPECT_EQ(backend_.retry_count, 1);
  backend_.Publish(OpenState(7, EditorComparisonStatus::Rendering, 2));
  EXPECT_EQ(comparison_->status(), QStringLiteral("loading"));
  backend_.Publish(OpenState(7, EditorComparisonStatus::Ready, 2), MakePair(nullptr));
  ASSERT_EQ(comparison_->status(), QStringLiteral("ready"));

  // A QML Image that cannot load its provider image fails the pair the same way.
  comparison_->reportImageLoadFailed(QStringLiteral("The comparison images could not be loaded."));
  EXPECT_EQ(comparison_->status(), QStringLiteral("failed"));
  EXPECT_EQ(comparison_->error_text(), QStringLiteral("The comparison images could not be loaded."));
  EXPECT_EQ(store_->ImageCount(), 0);

  // A render failure in the backend shows its real error.
  backend_.Publish([] {
    auto state  = OpenState(7, EditorComparisonStatus::Failed, 0);
    state.error = "CUDA download failed";
    return state;
  }());
  EXPECT_EQ(comparison_->status(), QStringLiteral("failed"));
  EXPECT_EQ(comparison_->error_text(), QStringLiteral("CUDA download failed"));
}

TEST_F(EditorComparisonControllerTest, SourceChoicesComeFromTheVersionListAndMapToBackendSources) {
  const Hash128 look_a{0x1111ULL, 0x2222ULL};
  const Hash128 look_b{0x3333ULL, 0x4444ULL};
  backend_.history.versions = {
      EditorHistoryVersion{.version_id = look_a, .display_name = "Look A", .active = false},
      EditorHistoryVersion{.version_id = look_b, .display_name = "Look B", .active = true}};
  backend_.Publish(OpenState(3, EditorComparisonStatus::Rendering, 1));

  const auto options = comparison_->source_options();
  ASSERT_EQ(options.size(), 4);
  EXPECT_EQ(options[0].toMap().value(QStringLiteral("value")).toString(), QStringLiteral("root"));
  EXPECT_EQ(options[1].toMap().value(QStringLiteral("value")).toString(),
            QStringLiteral("current"));
  EXPECT_EQ(options[2].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Look A"));
  EXPECT_TRUE(options[3].toMap().value(QStringLiteral("label")).toString().startsWith(
      QStringLiteral("Look B")));
  EXPECT_EQ(comparison_->a_source_value(), QStringLiteral("root"));
  EXPECT_FALSE(comparison_->a_label().isEmpty());

  const auto look_a_value = EditorComparisonController::SourceValue(
      EditorComparisonSource::Version(look_a));
  comparison_->selectASource(look_a_value);
  ASSERT_EQ(backend_.selections.size(), 1u);
  EXPECT_EQ(backend_.selections[0].kind, EditorComparisonKind::Versions);
  EXPECT_TRUE(backend_.selections[0].a == EditorComparisonSource::Version(look_a));
  EXPECT_TRUE(backend_.selections[0].b == EditorComparisonSource::Current());
  comparison_->selectKind(QStringLiteral("beforeAfter"));
  ASSERT_EQ(backend_.selections.size(), 2u);
  EXPECT_EQ(backend_.selections[1].kind, EditorComparisonKind::BeforeAfter);
  EXPECT_TRUE(backend_.selections[1].a == EditorComparisonSource::Root());
  comparison_->selectASource(QStringLiteral("version:not-a-hash"));
  EXPECT_EQ(backend_.selections.size(), 2u) << "an unknown value requests nothing";

  // View options never reach the backend; they survive within one comparison and reset for the
  // next one.
  comparison_->setDisplayMode(QStringLiteral("complete"));
  comparison_->setOrientation(QStringLiteral("vertical"));
  comparison_->setDividerPosition(1.5);
  comparison_->swap();
  EXPECT_EQ(comparison_->divider_position(), 1.0);
  backend_.Publish(OpenState(3, EditorComparisonStatus::Rendering, 2));
  EXPECT_EQ(comparison_->display_mode(), QStringLiteral("complete"));
  EXPECT_TRUE(comparison_->swapped());
  backend_.Publish({});
  backend_.Publish(OpenState(4, EditorComparisonStatus::Rendering, 3));
  EXPECT_EQ(comparison_->display_mode(), QStringLiteral("divider"));
  EXPECT_EQ(comparison_->orientation(), QStringLiteral("horizontal"));
  EXPECT_EQ(comparison_->divider_position(), 0.5);
  EXPECT_FALSE(comparison_->swapped());
  EXPECT_EQ(backend_.retry_count, 0);
}

}  // namespace
}  // namespace alcedo::ui::test
