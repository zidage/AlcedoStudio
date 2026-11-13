//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QtGlobal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "edit/scope/scope_analyzer.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_item.hpp"

namespace alcedo::ui::test {

class EditorScopeControllerTestPeer {
 public:
  explicit EditorScopeControllerTestPeer(EditorScopeController& controller)
      : controller_(controller) {}

  auto PublishSnapshot(const alcedo::ScopeRenderSnapshot& snapshot) -> bool {
    return controller_.publishSnapshot(snapshot, kImageIdentity, kSessionEpoch,
                                       controller_.request_revision_);
  }

  static constexpr std::uint64_t kImageIdentity = 42;
  static constexpr std::uint64_t kSessionEpoch  = 3;

 private:
  EditorScopeController& controller_;
};

namespace {

constexpr int           kBins       = 256;
constexpr int           kWidth      = 300;
constexpr int           kHeight     = 160;
constexpr qreal         kPadding    = 4.0;
const QColor            kBackground = QColor(0x16, 0x17, 0x19);
const QColor            kMarker     = QColor(0xF0, 0xF0, 0xF0);

class IdleScopeAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView&, const ScopeRequest&) override {}
  auto GetLatestOutput() -> ScopeOutputSet override { return {}; }
  void ResizeResources(const ScopeRequest&) override {}
  void ReleaseResources() override {}
};

class PaintableScopeItem final : public EditorScopeItem {
 public:
  using EditorScopeItem::paint;
};

auto Index(int channel, int bin) -> size_t {
  return static_cast<size_t>(channel) * static_cast<size_t>(kBins) + static_cast<size_t>(bin);
}

// Equal counts per channel: red in bins 20..159, green in 60..199, blue in 100..239. Bins
// 20..59 hold red only, 60..99 red and green, 100..159 all three.
auto StaggeredChannelCounts() -> std::vector<uint32_t> {
  std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 3U, 0U);
  for (int channel = 0; channel < 3; ++channel) {
    const int first = 20 + channel * 40;
    for (int bin = first; bin < first + 140; ++bin) {
      counts[Index(channel, bin)] = 1000U;
    }
  }
  return counts;
}

auto BinX(int bin) -> int {
  return static_cast<int>(kPadding +
                          (static_cast<qreal>(bin) / (kBins - 1)) * (kWidth - 2 * kPadding));
}

auto ColorDistance(const QColor& a, const QColor& b) -> int {
  return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
         std::abs(a.blue() - b.blue());
}

class EditorScopeItemPaintTest : public ::testing::Test {
 protected:
  EditorScopeItemPaintTest()
      : controller_(std::make_shared<IdleScopeAnalyzer>()), peer_(controller_) {
    controller_.SetImageIdentity(EditorScopeControllerTestPeer::kImageIdentity,
                                 EditorScopeControllerTestPeer::kSessionEpoch);
    controller_.set_visual_active(true);
    item_.setSize(QSizeF(kWidth, kHeight));
    item_.set_background_color(kBackground);
    item_.set_marker_color(kMarker);
    item_.set_plot_padding(kPadding);
    item_.set_controller(&controller_);
  }

  ~EditorScopeItemPaintTest() override { controller_.set_visual_active(false); }

  auto Render(const std::vector<uint32_t>& counts) -> QImage {
    ScopeRenderSnapshot snapshot;
    snapshot.generation = ++generation_;
    snapshot.histogram  = BuildScopeHistogramRenderData(counts, kBins);
    EXPECT_TRUE(peer_.PublishSnapshot(snapshot));

    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    item_.paint(&painter);
    painter.end();
    if (const QString dump = qEnvironmentVariable("ALCEDO_SCOPE_PAINT_DUMP_DIR"); !dump.isEmpty()) {
      image.save(
          QStringLiteral("%1/%2.png")
              .arg(dump, QString::fromLatin1(
                             ::testing::UnitTest::GetInstance()->current_test_info()->name())));
    }
    return image;
  }

  EditorScopeController         controller_;
  EditorScopeControllerTestPeer peer_;
  PaintableScopeItem            item_;
  std::uint64_t                 generation_ = 0;
};

TEST_F(EditorScopeItemPaintTest, OverlappingChannelsAddUpToNeutral) {
  const QImage image    = Render(StaggeredChannelCounts());
  const int    y        = kHeight / 2;

  const QColor red_only = image.pixelColor(BinX(40), y);
  EXPECT_GT(red_only.red(), red_only.green() + 30);
  EXPECT_GT(red_only.red(), red_only.blue() + 30);

  const QColor red_green = image.pixelColor(BinX(85), y);
  EXPECT_GT(red_green.red(), red_green.blue() + 30);
  EXPECT_GT(red_green.green(), red_green.blue() + 30);

  const QColor all_three = image.pixelColor(BinX(140), y);
  EXPECT_LE(std::abs(all_three.red() - all_three.green()), 12);
  EXPECT_LE(std::abs(all_three.green() - all_three.blue()), 12);
  EXPECT_GT(all_three.green(), kBackground.green() + 50);

  // No channel holds pixels in bin 250: no fill, and no edge line along the baseline.
  EXPECT_LE(ColorDistance(image.pixelColor(BinX(250), y), kBackground), 3);
  EXPECT_LE(ColorDistance(image.pixelColor(BinX(250), kHeight - static_cast<int>(kPadding) - 1),
                          kBackground),
            3);
}

TEST_F(EditorScopeItemPaintTest, ClipMarkersShowOnlyAtTheClippedEnd) {
  auto counts = StaggeredChannelCounts();
  for (int channel = 0; channel < 3; ++channel) {
    counts[Index(channel, kBins - 1)] = 200000U;
  }
  const QImage clipped  = Render(counts);
  const int    marker_y = static_cast<int>(kPadding) + 1;
  EXPECT_LE(
      ColorDistance(clipped.pixelColor(kWidth - static_cast<int>(kPadding) - 2, marker_y), kMarker),
      40);
  EXPECT_LE(
      ColorDistance(clipped.pixelColor(static_cast<int>(kPadding) + 1, marker_y), kBackground), 3);

  const QImage unclipped = Render(StaggeredChannelCounts());
  EXPECT_GT(ColorDistance(unclipped.pixelColor(kWidth - static_cast<int>(kPadding) - 2, marker_y),
                          kMarker),
            200);
}

TEST_F(EditorScopeItemPaintTest, CutPeakShowsABreakNearTheTop) {
  std::vector<uint32_t> counts(static_cast<size_t>(kBins) * 3U, 0U);
  for (int channel = 0; channel < 3; ++channel) {
    for (int bin = 20; bin < 120; ++bin) {
      counts[Index(channel, bin)] = 1000U;
    }
    for (int bin = 190; bin < 200; ++bin) {
      counts[Index(channel, bin)] = 90000U;
    }
  }
  const QImage image = Render(counts);
  const int    x     = BinX(195);
  // The break is two pixels high, four pixels under the top of the plot.
  EXPECT_LE(ColorDistance(image.pixelColor(x, static_cast<int>(kPadding) + 4), kBackground), 3);
  EXPECT_GT(ColorDistance(image.pixelColor(x, static_cast<int>(kPadding) + 12), kBackground), 100);
  // The bins under the reference are not cut.
  EXPECT_GT(ColorDistance(image.pixelColor(BinX(70), kHeight - static_cast<int>(kPadding) - 10),
                          kBackground),
            100);
}

}  // namespace
}  // namespace alcedo::ui::test
