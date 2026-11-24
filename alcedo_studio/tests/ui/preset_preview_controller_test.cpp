//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file preset_preview_controller_test.cpp
/// @brief Preset browser previews (edit presets plan 2.4, PS3): keys by image, preset fingerprint,
///        and the sensor settings of the current Version; one render in flight that waits for the
///        viewport; failures; and the rendered sensor key of the session.

#include "ui/alcedo_main/album_backend/preset_preview_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/preset_preview_image.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image_buffer.hpp"
#include "support/document_transfer_test_support.hpp"

namespace alcedo::ui {
namespace {

/// The working document of the current Version with the demosaic method @p method.
auto DocumentWithDemosaic(const std::string& method) -> PipelineDocument {
  auto                  document = CreateDefaultPipelineDocument();
  EditorParameterTarget target;
  target.owner_kind = EditorParameterOwnerKind::Develop;
  target.node_id    = document.Develop()->Id();
  target.field_key  = "raw_decode";
  alcedo::test::PatchDocumentField(&document, target, {{"raw", {{"method", method}}}});
  return document;
}

/// The open photo, its working document, and the preset entries, with renders that complete only
/// when the test runs their completion.
class FakePresetPreviewSource final : public PresetPreviewSource {
 public:
  struct Entry {
    std::string fingerprint;
    bool        readable = true;
  };
  struct Render {
    EditorPresetPreviewRequest     request;
    EditorBrowserPreviewCompletion completion;
  };

  [[nodiscard]] auto ImageId() const -> std::uint64_t override { return image_id; }
  [[nodiscard]] auto EntryFingerprint(const QString& entry_id) const
      -> std::optional<std::string> override {
    const auto found = entries.constFind(entry_id);
    if (found == entries.constEnd()) return std::nullopt;
    return found->fingerprint;
  }
  [[nodiscard]] auto SensorKey() const -> std::string override {
    return PresetPreviewSensorKey(document);
  }
  [[nodiscard]] auto ViewportRendering() const -> bool override { return viewport_rendering; }
  [[nodiscard]] auto PackageFor(const QString& entry_id) -> PresetPackageResult override {
    ++package_reads;
    PresetPackageResult result;
    const auto          found = entries.constFind(entry_id);
    if (found == entries.constEnd() || !found->readable) {
      result.rule    = PresetFileRule::kInvalidJson;
      result.message = "invalid JSON";
      return result;
    }
    result.package = std::make_shared<const AdjustmentTransferPackage>(
        alcedo::test::MakeExposureTransferPackage(0.5));
    return result;
  }
  auto RenderPresetPreview(EditorPresetPreviewRequest     request,
                           EditorBrowserPreviewCompletion on_complete, std::string* error)
      -> bool override {
    if (refuse) {
      *error = "The session refused the command";
      return false;
    }
    renders.push_back(Render{std::move(request), std::move(on_complete)});
    return true;
  }

  /// Complete render @p index as the session would; @p sensor_key is the key of the document that
  /// the session rendered (the current one when empty).
  void Complete(std::size_t index, EditorBrowserPreviewStatus status,
                std::optional<std::string> sensor_key = std::nullopt) {
    EditorBrowserPreviewResult result;
    result.status     = status;
    result.sensor_key = sensor_key.value_or(SensorKey());
    if (status == EditorBrowserPreviewStatus::Completed) {
      result.image.pixels = std::make_shared<ImageBuffer>(
          cv::Mat(4, 6, CV_32FC4, cv::Scalar(0.5f, 0.25f, 1.0f, 1.0f)));
      result.image.geometry.render_extent         = {6, 4};
      result.image.geometry.full_reference_extent = {60, 40};
    } else {
      result.message = "device lost";
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

  std::uint64_t         image_id = 7;
  QHash<QString, Entry> entries;
  PipelineDocument      document = DocumentWithDemosaic("default");
  std::vector<Render>   renders;
  int                   package_reads      = 0;
  bool                  refuse             = false;
  bool                  viewport_rendering = false;
};

class PresetPreviewControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source_.entries.insert(QStringLiteral("Warm.alcpreset"), {"fp-warm"});
    source_.entries.insert(QStringLiteral("Film/Cool.alcpreset"), {"fp-cool"});
    controller_.SetImageStore(store_);
    controller_.SetSource(&source_);
    RefreshAll();
  }

  void Watch(const QString& entry_id) {
    controller_.watch(entry_id);
    FakePresetPreviewSource::Drain();
  }
  void RefreshAll() {
    controller_.Refresh();
    FakePresetPreviewSource::Drain();
  }
  [[nodiscard]] auto Url(const QString& entry_id) const -> QString {
    return controller_.previewUrl(entry_id);
  }

  const QString                             warm_ = QStringLiteral("Warm.alcpreset");
  const QString                             cool_ = QStringLiteral("Film/Cool.alcpreset");
  FakePresetPreviewSource                   source_;
  std::shared_ptr<BrowserPreviewImageStore> store_ =
      std::make_shared<BrowserPreviewImageStore>(QString::fromUtf8(kPresetPreviewImageProviderId));
  PresetPreviewController controller_;
};

TEST_F(PresetPreviewControllerTest, PreviewKeyChangesWithFingerprintImageAndSensorSettings) {
  Watch(warm_);
  ASSERT_EQ(source_.renders.size(), 1u);
  EXPECT_EQ(source_.renders[0].request.image_id, 7u);
  ASSERT_NE(source_.renders[0].request.package, nullptr) << "the request carries the package";
  source_.Complete(0, EditorBrowserPreviewStatus::Completed);
  const QString first_url = Url(warm_);
  ASSERT_TRUE(first_url.startsWith(QStringLiteral("image://alcedo-preset-preview/")));

  // A Color Grade or white balance edit of the current Version: the preview stays.
  alcedo::test::PatchDocumentField(&source_.document,
                                   alcedo::test::ColorGradeFieldTarget("exposure"),
                                   nlohmann::json{{"exposure_ev", 1.0}});
  RefreshAll();
  EXPECT_EQ(Url(warm_), first_url);
  EXPECT_EQ(source_.renders.size(), 1u);

  // The preset file changed (a new fingerprint after a scan): a new render.
  source_.entries[warm_].fingerprint = "fp-warm-2";
  RefreshAll();
  EXPECT_TRUE(Url(warm_).isEmpty()) << "the old preview shows another preset";
  ASSERT_EQ(source_.renders.size(), 2u);
  source_.Complete(1, EditorBrowserPreviewStatus::Completed);
  EXPECT_FALSE(Url(warm_).isEmpty());

  // A sensor setting of the current Version changed: a new render.
  source_.document = DocumentWithDemosaic("legacy");
  RefreshAll();
  EXPECT_TRUE(Url(warm_).isEmpty());
  ASSERT_EQ(source_.renders.size(), 3u);
  source_.Complete(2, EditorBrowserPreviewStatus::Completed);
  EXPECT_FALSE(Url(warm_).isEmpty());

  // Another photo: a new render with its image id.
  source_.image_id = 8;
  RefreshAll();
  EXPECT_TRUE(Url(warm_).isEmpty());
  ASSERT_EQ(source_.renders.size(), 4u);
  EXPECT_EQ(source_.renders[3].request.image_id, 8u);

  // Back to the first settings of photo 7: that preview is still in the LRU.
  source_.Complete(3, EditorBrowserPreviewStatus::Completed);
  source_.image_id                   = 7;
  source_.document                   = DocumentWithDemosaic("default");
  source_.entries[warm_].fingerprint = "fp-warm";
  RefreshAll();
  EXPECT_FALSE(Url(warm_).isEmpty());
  EXPECT_EQ(source_.renders.size(), 4u);
}

TEST_F(PresetPreviewControllerTest, OneRenderRunsAtATimeAndWaitsForViewport) {
  source_.viewport_rendering = true;
  Watch(warm_);
  Watch(cool_);
  EXPECT_TRUE(source_.renders.empty()) << "no preview starts while a frame renders";

  source_.viewport_rendering = false;
  controller_.ResumeAfterViewport();
  ASSERT_EQ(source_.renders.size(), 1u) << "one render in flight";
  EXPECT_TRUE(controller_.rendering());
  // A frame starts while the preview renders: the preview finishes, the next one waits.
  source_.viewport_rendering = true;
  source_.Complete(0, EditorBrowserPreviewStatus::Completed);
  EXPECT_FALSE(Url(cool_).isEmpty()) << "the most recently watched tile renders first";
  EXPECT_EQ(source_.renders.size(), 1u);

  source_.viewport_rendering = false;
  controller_.ResumeAfterViewport();
  ASSERT_EQ(source_.renders.size(), 2u);
  source_.Complete(1, EditorBrowserPreviewStatus::Completed);
  EXPECT_FALSE(Url(warm_).isEmpty());
  EXPECT_FALSE(controller_.rendering());
}

TEST_F(PresetPreviewControllerTest, FailedPreviewIsNotRetriedAndTileStaysAvailable) {
  Watch(warm_);
  ASSERT_EQ(source_.renders.size(), 1u);
  const int revision_before = controller_.revision();
  source_.Complete(0, EditorBrowserPreviewStatus::Failed);
  EXPECT_TRUE(Url(warm_).isEmpty());
  EXPECT_TRUE(controller_.previewFailed(warm_)) << "the tile shows the warning icon";
  EXPECT_NE(controller_.revision(), revision_before) << "the tile reads the failure";

  RefreshAll();
  controller_.unwatch(warm_);
  Watch(warm_);
  EXPECT_EQ(source_.renders.size(), 1u) << "a failed key is not rendered again for this photo";
  EXPECT_TRUE(source_.EntryFingerprint(warm_).has_value())
      << "the preset stays listed and available";

  // A preset whose file fails a rule now fails its preview without a render.
  source_.entries[cool_].readable = false;
  Watch(cool_);
  EXPECT_EQ(source_.renders.size(), 1u);
  EXPECT_TRUE(controller_.previewFailed(cool_));
  EXPECT_FALSE(controller_.previewFailed(QStringLiteral("unlisted.alcpreset")));

  // The failure is of the key: a new sensor setting renders the preset again.
  source_.document = DocumentWithDemosaic("legacy");
  RefreshAll();
  EXPECT_FALSE(controller_.previewFailed(warm_));
  EXPECT_EQ(source_.renders.size(), 2u);
}

// The owner reduced a sensor edit before the preview command: the pixels belong to the newer
// settings, so they are stored under that key and not shown for the older one.
TEST_F(PresetPreviewControllerTest, PreviewIsStoredUnderTheSensorKeyThatTheSessionRendered) {
  const auto legacy_key = PresetPreviewSensorKey(DocumentWithDemosaic("legacy"));
  Watch(warm_);
  ASSERT_EQ(source_.renders.size(), 1u);
  source_.Complete(0, EditorBrowserPreviewStatus::Completed, legacy_key);
  EXPECT_TRUE(Url(warm_).isEmpty()) << "the tile keys the older settings";
  EXPECT_EQ(controller_.cached_count(), 1u);

  // The published working document reaches the GUI: the stored preview is shown, no new render.
  source_.document = DocumentWithDemosaic("legacy");
  RefreshAll();
  EXPECT_FALSE(Url(warm_).isEmpty());
  EXPECT_EQ(source_.renders.size(), 1u);
}

TEST_F(PresetPreviewControllerTest, PresetWithoutFingerprintOrPhotoHasNoPreview) {
  Watch(QStringLiteral("unlisted.alcpreset"));
  EXPECT_TRUE(source_.renders.empty());
  source_.image_id = 0;
  RefreshAll();
  Watch(warm_);
  EXPECT_TRUE(source_.renders.empty()) << "no photo is open";
  EXPECT_EQ(source_.package_reads, 0) << "no package is read without a render";
}

}  // namespace
}  // namespace alcedo::ui
