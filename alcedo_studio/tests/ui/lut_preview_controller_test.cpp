//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file lut_preview_controller_test.cpp
/// @brief LUT browser previews (issue #598): one render in flight, the most recently watched
///        entry first, the memory cache, keys by image and encodings, and failures.

#include "ui/alcedo_main/album_backend/lut_preview_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "edit/operators/models/lut_reference.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui {
namespace {

/// The open photo and the library entries, with renders that complete only when the test runs
/// their completion.
class FakeLutPreviewSource final : public LutPreviewSource {
 public:
  struct Encodings {
    std::string input  = "acescc";
    std::string output = "acescc";
  };
  struct Render {
    EditorLutPreviewRequest    request;
    EditorLutPreviewCompletion completion;
  };

  [[nodiscard]] auto ImageId() const -> std::uint64_t override { return image_id; }
  [[nodiscard]] auto EntryWrite(const QString& entry_id, EditorLutWrite* write) const
      -> bool override {
    const auto found = entries.constFind(entry_id);
    if (found == entries.constEnd()) return false;
    write->reference       = LutReference{LibraryLutReference{entry_id.toStdString()}};
    write->display_name    = entry_id.toStdString();
    write->input_encoding  = found->input;
    write->output_encoding = found->output;
    return true;
  }
  [[nodiscard]] auto ViewportRendering() const -> bool override { return viewport_rendering; }
  auto RenderLutPreview(EditorLutPreviewRequest request, EditorLutPreviewCompletion on_complete,
                        std::string* error) -> bool override {
    if (refuse) {
      *error = "The session refused the command";
      return false;
    }
    renders.push_back(Render{std::move(request), std::move(on_complete)});
    return true;
  }

  /// Entry ID of render @p index.
  [[nodiscard]] auto EntryOf(std::size_t index) const -> QString {
    const auto& reference =
        std::get<LibraryLutReference>(*renders.at(index).request.write.reference);
    return QString::fromStdString(reference.relative_path);
  }

  /// Complete render @p index as the session would, from outside the GUI thread's call stack.
  void Complete(std::size_t index, EditorLutPreviewStatus status, std::string message = {}) {
    EditorLutPreviewResult result;
    result.status  = status;
    result.message = std::move(message);
    if (status == EditorLutPreviewStatus::Completed) {
      result.image.pixels = std::make_shared<ImageBuffer>(
          cv::Mat(4, 6, CV_32FC4, cv::Scalar(0.5f, 0.25f, 1.0f, 1.0f)));
      result.image.geometry.render_extent         = {6, 4};
      result.image.geometry.full_reference_extent = {60, 40};
    }
    auto completion = std::move(renders.at(index).completion);
    completion(std::move(result));
    Drain();
  }

  /// Deliver the posted render results and the renders that they start.
  static void Drain() {
    for (int round = 0; round < 8; ++round) {
      QCoreApplication::sendPostedEvents();
    }
  }

  std::uint64_t             image_id = 7;
  QHash<QString, Encodings> entries;
  std::vector<Render>       renders;
  bool                      refuse             = false;
  bool                      viewport_rendering = false;
};

class LutPreviewControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source_.entries.insert(QStringLiteral("a"), {});
    source_.entries.insert(QStringLiteral("b"), {});
    source_.entries.insert(QStringLiteral("c"), {});
    controller_.SetImageStore(store_);
    controller_.SetSource(&source_);
    RefreshAll();
  }

  void Watch(const QString& entry_id) {
    controller_.watch(entry_id);
    FakeLutPreviewSource::Drain();
  }
  void RefreshAll() {
    controller_.Refresh();
    FakeLutPreviewSource::Drain();
  }

  [[nodiscard]] auto Url(const QString& entry_id) const -> QString {
    return controller_.previewUrl(entry_id);
  }

  FakeLutPreviewSource                  source_;
  std::shared_ptr<LutPreviewImageStore> store_ = std::make_shared<LutPreviewImageStore>();
  LutPreviewController                  controller_;
};

TEST_F(LutPreviewControllerTest, WatchedEntryRendersOnceAndShowsTheCachedPreviewAgain) {
  const int revision_before = controller_.revision();
  Watch(QStringLiteral("a"));
  ASSERT_EQ(source_.renders.size(), 1u);
  EXPECT_EQ(source_.renders[0].request.image_id, 7u);
  EXPECT_EQ(source_.EntryOf(0), QStringLiteral("a"));
  EXPECT_EQ(source_.renders[0].request.write.input_encoding, "acescc");
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty()) << "no preview before the render ends";

  source_.Complete(0, EditorLutPreviewStatus::Completed);
  const QString url = Url(QStringLiteral("a"));
  ASSERT_FALSE(url.isEmpty());
  EXPECT_NE(controller_.revision(), revision_before) << "tiles read the new URL";
  std::uint64_t display_id = 0;
  ASSERT_TRUE(
      LutPreviewImageStore::ParseProviderId(url.section(QLatin1Char('/'), -1), &display_id));
  const QImage shown = store_->Get(display_id);
  ASSERT_FALSE(shown.isNull());
  EXPECT_EQ(shown.size(), QSize(6, 4));
  EXPECT_EQ(controller_.cached_count(), 1u);

  // A tile that stops showing the entry releases the display image; the LRU keeps the pixels.
  controller_.unwatch(QStringLiteral("a"));
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_EQ(store_->size(), 0u);
  Watch(QStringLiteral("a"));
  EXPECT_FALSE(Url(QStringLiteral("a")).isEmpty()) << "the cached preview is shown at once";
  EXPECT_EQ(source_.renders.size(), 1u) << "a cached preview does not render again";
}

TEST_F(LutPreviewControllerTest, OnlyOneRenderIsInFlightAndTheLastWatchedEntryRendersNext) {
  Watch(QStringLiteral("a"));
  Watch(QStringLiteral("b"));
  Watch(QStringLiteral("c"));
  ASSERT_EQ(source_.renders.size(), 1u) << "a viewport frame waits for at most one render";
  EXPECT_TRUE(controller_.rendering());

  source_.Complete(0, EditorLutPreviewStatus::Completed);
  ASSERT_EQ(source_.renders.size(), 2u);
  EXPECT_EQ(source_.EntryOf(1), QStringLiteral("c")) << "the most recently watched tile first";
  source_.Complete(1, EditorLutPreviewStatus::Completed);
  ASSERT_EQ(source_.renders.size(), 3u);
  EXPECT_EQ(source_.EntryOf(2), QStringLiteral("b"));
  source_.Complete(2, EditorLutPreviewStatus::Completed);
  EXPECT_FALSE(controller_.rendering());
  for (const auto* id : {"a", "b", "c"}) {
    EXPECT_FALSE(Url(QString::fromLatin1(id)).isEmpty()) << id;
  }

  // An entry that the tile left before its turn does not render.
  controller_.unwatch(QStringLiteral("a"));
  source_.entries[QStringLiteral("a")].output = "rec709_gamma24";
  RefreshAll();
  EXPECT_EQ(source_.renders.size(), 3u);
}

TEST_F(LutPreviewControllerTest, EncodingChangeRendersThatEntryAgainAndKeepsTheOthers) {
  Watch(QStringLiteral("a"));
  Watch(QStringLiteral("b"));
  source_.Complete(0, EditorLutPreviewStatus::Completed);
  source_.Complete(1, EditorLutPreviewStatus::Completed);
  ASSERT_FALSE(Url(QStringLiteral("a")).isEmpty());
  const QString url_b = Url(QStringLiteral("b"));
  ASSERT_FALSE(url_b.isEmpty());

  // The remembered encodings of "a" change: its old preview shows another LUT result.
  source_.entries[QStringLiteral("a")].input = "logc3_ei800";
  RefreshAll();
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_EQ(Url(QStringLiteral("b")), url_b) << "an unchanged entry keeps its preview";
  ASSERT_EQ(source_.renders.size(), 3u);
  EXPECT_EQ(source_.EntryOf(2), QStringLiteral("a"));
  EXPECT_EQ(source_.renders[2].request.write.input_encoding, "logc3_ei800");
  source_.Complete(2, EditorLutPreviewStatus::Completed);
  EXPECT_FALSE(Url(QStringLiteral("a")).isEmpty());

  // Back to the first pair: that preview is still in the LRU.
  source_.entries[QStringLiteral("a")].input = "acescc";
  RefreshAll();
  EXPECT_FALSE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_EQ(source_.renders.size(), 3u);
}

TEST_F(LutPreviewControllerTest, ImageSwitchHidesPreviewsAndAnOldResultIsNotShown) {
  Watch(QStringLiteral("b"));
  source_.Complete(0, EditorLutPreviewStatus::Completed);
  ASSERT_FALSE(Url(QStringLiteral("b")).isEmpty());

  Watch(QStringLiteral("a"));
  ASSERT_EQ(source_.renders.size(), 2u);
  source_.image_id = 8;
  RefreshAll();
  EXPECT_TRUE(Url(QStringLiteral("b")).isEmpty()) << "a preview of the previous photo is hidden";
  EXPECT_EQ(source_.renders.size(), 2u) << "the render in flight finishes first";

  // The render of the previous photo ends: it is not shown on the new photo.
  source_.Complete(1, EditorLutPreviewStatus::Completed);
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty());
  ASSERT_EQ(source_.renders.size(), 3u);
  EXPECT_EQ(source_.renders[2].request.image_id, 8u);
  source_.Complete(2, EditorLutPreviewStatus::Completed);
  ASSERT_EQ(source_.renders.size(), 4u);
  EXPECT_EQ(source_.renders[3].request.image_id, 8u);
  source_.Complete(3, EditorLutPreviewStatus::Completed);
  EXPECT_FALSE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_FALSE(Url(QStringLiteral("b")).isEmpty());
}

TEST_F(LutPreviewControllerTest, FailedRenderIsNotTriedAgainAndAnEntryWithoutWriteHasNoPreview) {
  Watch(QStringLiteral("unlisted"));
  EXPECT_TRUE(source_.renders.empty()) << "an entry that cannot be applied has no preview";
  EXPECT_TRUE(Url(QStringLiteral("unlisted")).isEmpty());

  Watch(QStringLiteral("a"));
  ASSERT_EQ(source_.renders.size(), 1u);
  source_.Complete(0, EditorLutPreviewStatus::Failed, "device lost");
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_FALSE(controller_.rendering());
  RefreshAll();
  controller_.unwatch(QStringLiteral("a"));
  Watch(QStringLiteral("a"));
  EXPECT_EQ(source_.renders.size(), 1u) << "a failed key is not rendered again for this photo";
}

// A preview starts only while the viewport is idle, so a slider drag never waits behind a run of
// previews; the queue continues when the viewport stops rendering.
TEST_F(LutPreviewControllerTest, PreviewsWaitWhileTheViewportRendersAndContinueAfterIt) {
  source_.viewport_rendering = true;
  Watch(QStringLiteral("a"));
  Watch(QStringLiteral("b"));
  EXPECT_TRUE(source_.renders.empty()) << "no preview starts while a frame renders";

  source_.viewport_rendering = false;
  controller_.ResumeAfterViewport();
  ASSERT_EQ(source_.renders.size(), 1u);
  EXPECT_EQ(source_.EntryOf(0), QStringLiteral("b"));

  // A frame starts while the preview renders: the preview finishes, the next one waits.
  source_.viewport_rendering = true;
  source_.Complete(0, EditorLutPreviewStatus::Completed);
  EXPECT_FALSE(Url(QStringLiteral("b")).isEmpty());
  EXPECT_EQ(source_.renders.size(), 1u);
  source_.viewport_rendering = false;
  controller_.ResumeAfterViewport();
  ASSERT_EQ(source_.renders.size(), 2u);
  EXPECT_EQ(source_.EntryOf(1), QStringLiteral("a"));
}

TEST_F(LutPreviewControllerTest, RefusedOrRejectedRenderStartsAgainAfterTheNextRefresh) {
  source_.refuse = true;
  Watch(QStringLiteral("a"));
  EXPECT_TRUE(source_.renders.empty());
  EXPECT_FALSE(controller_.rendering()) << "a refused command leaves nothing in flight";

  source_.refuse = false;
  RefreshAll();
  ASSERT_EQ(source_.renders.size(), 1u);
  source_.Complete(0, EditorLutPreviewStatus::Rejected, "not interactive");
  EXPECT_TRUE(Url(QStringLiteral("a")).isEmpty());
  EXPECT_EQ(source_.renders.size(), 1u);

  // A rejected render is not a failure: the next refresh (a session state change) renders it.
  RefreshAll();
  ASSERT_EQ(source_.renders.size(), 2u);
  source_.Complete(1, EditorLutPreviewStatus::Completed);
  EXPECT_FALSE(Url(QStringLiteral("a")).isEmpty());
}

}  // namespace
}  // namespace alcedo::ui
