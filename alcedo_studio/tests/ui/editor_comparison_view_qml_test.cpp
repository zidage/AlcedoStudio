//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_view_qml_test.cpp
/// @brief Production EditorComparisonView.qml and EditorComparisonPanel.qml with completed pairs
///        from the real comparison store and provider.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <memory>

#include "editor_comparison_test_support.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

constexpr Extent2D kSource{400, 300};
constexpr double   kPointTolerance = 1e-3;

// The view under a host that writes the requested divider position back, as the comparison
// owner does.
constexpr auto     kViewHost       = R"(
import QtQuick
EditorComparisonView {
    width: 900
    height: 600
    onDividerPositionRequested: function (position) { dividerPosition = position }
}
)";

class ComparisonQmlHarness {
 public:
  explicit ComparisonQmlHarness(const char* host_source, const char* host_file_name)
      : store_(std::make_shared<ComparisonImageStore>()),
        observer_(std::make_shared<ObservedComparisonImageProvider::Observer>()) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(ComparisonQmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    // The engine owns the provider.
    engine_.addImageProvider(QString::fromLatin1(kComparisonImageProviderId),
                             new ObservedComparisonImageProvider(store_, observer_));

    QQmlComponent component(&engine_);
    component.setData(QByteArray(host_source),
                      QUrl::fromLocalFile(ComparisonQmlDirectory() + "/" + host_file_name));
    root_.reset(qobject_cast<QQuickItem*>(component.create()));
    if (!root_) {
      QStringList text;
      for (const auto& error : component.errors()) {
        text.push_back(error.toString());
      }
      errors_ = text.join('\n');
      return;
    }
    root_->setParentItem(window_.contentItem());
    window_.resize(static_cast<int>(root_->width()), static_cast<int>(root_->height()));
    window_.show();
    QCoreApplication::processEvents();
  }

  ~ComparisonQmlHarness() {
    // A held provider read must finish before the engine stops its loader thread.
    observer_->Hold(ComparisonSide::A, false);
    observer_->Hold(ComparisonSide::B, false);
    root_.reset();
  }

  auto root() const -> QQuickItem* { return root_.get(); }
  auto errors() const -> QString { return errors_; }
  auto store() const -> ComparisonImageStore& { return *store_; }
  auto observer() const -> ObservedComparisonImageProvider::Observer& { return *observer_; }

  template <typename T = QQuickItem>
  auto find(const QString& name) const -> T* {
    return root_ ? root_->findChild<T*>(name) : nullptr;
  }

  /// Publishes a pair through the production publication and binds it as the owner does.
  auto PublishPair(std::uint64_t operation_id, const RenderedPipelineImage& a,
                   const RenderedPipelineImage& b) -> ComparisonPairPublication {
    auto publication = PublishComparisonPair(*store_, operation_id, a, b);
    root_->setProperty("pair", publication.ToVariantMap());
    root_->setProperty("status", QStringLiteral("ready"));
    return publication;
  }

  auto PairReady() const -> bool { return root_->property("pairReady").toBool(); }

 private:
  std::shared_ptr<ComparisonImageStore>                      store_;
  std::shared_ptr<ObservedComparisonImageProvider::Observer> observer_;
  QQmlEngine                                                 engine_;
  QQuickWindow                                               window_;
  std::unique_ptr<QQuickItem>                                root_;
  QString                                                    errors_;
};

auto FullGeometry() -> ResolvedRenderGeometry { return ResolveComparisonGeometry(kSource); }

auto HalfCropGeometry() -> ResolvedRenderGeometry {
  return ResolveComparisonGeometry(kSource, NormalizedRect{0.0f, 0.0f, 0.5f, 1.0f});
}

auto CanvasImageReady(const ComparisonQmlHarness& harness, const char* canvas) -> bool {
  auto* item = harness.find(QString::fromLatin1(canvas));
  return item != nullptr && item->property("imageReady").toBool();
}

// Stage position at which the image of @p canvas_name draws the reference point.
auto StagePointOfReference(const ComparisonQmlHarness& harness, const char* canvas_name,
                           const ResolvedRenderGeometry& geometry, Vector2 reference) -> QPointF {
  auto*      canvas = harness.find(QString::fromLatin1(canvas_name));
  auto*      image  = canvas->findChild<QQuickItem*>(QStringLiteral("editorComparisonCanvasImage"));
  auto*      stage  = harness.find(QStringLiteral("editorComparisonStage"));
  const auto render = TransformPoint(geometry.reference_to_render, reference);
  return image->mapToItem(stage, QPointF(render.x, render.y));
}

void SendKey(QQuickItem* item, int key) {
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
  QCoreApplication::sendEvent(item, &press);
  QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
  QCoreApplication::sendEvent(item, &release);
}

}  // namespace

TEST(EditorComparisonViewQmlTest, PairRemainsHiddenUntilBothImagesAreReady) {
  ComparisonQmlHarness harness(kViewHost, "EditorComparisonViewHost.qml");
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* region_a = harness.find(QStringLiteral("editorComparisonRegionA"));
  auto* region_b = harness.find(QStringLiteral("editorComparisonRegionB"));
  auto* status   = harness.find(QStringLiteral("editorComparisonStatusText"));
  ASSERT_NE(region_a, nullptr);
  ASSERT_NE(region_b, nullptr);
  ASSERT_NE(status, nullptr);

  harness.root()->setProperty("status", QStringLiteral("loading"));
  EXPECT_TRUE(status->isVisible());
  EXPECT_EQ(status->property("text").toString(), QStringLiteral("Rendering comparison images"));

  // Each order of image-load completion: the side loaded first stays hidden until the other
  // side is Ready too.
  struct Order {
    std::uint64_t  operation_id;
    ComparisonSide late_side;
    const char*    early_canvas;
    const char*    late_canvas;
  };
  for (const Order& order :
       {Order{41, ComparisonSide::B, "editorComparisonCanvasA", "editorComparisonCanvasB"},
        Order{42, ComparisonSide::A, "editorComparisonCanvasB", "editorComparisonCanvasA"}}) {
    harness.root()->setProperty("pair", QVariant{});
    harness.root()->setProperty("status", QStringLiteral("loading"));
    const int held_before = harness.observer().HeldReads();
    harness.observer().Hold(order.late_side, true);

    (void)harness.PublishPair(order.operation_id,
                              MakeFilledRenderedImage(FullGeometry(), cv::Scalar(1, 0, 0, 1)),
                              MakeFilledRenderedImage(HalfCropGeometry(), cv::Scalar(0, 0, 1, 1)));

    ASSERT_TRUE(WaitUntil([&] { return CanvasImageReady(harness, order.early_canvas); }));
    ASSERT_TRUE(WaitUntil([&] { return harness.observer().HeldReads() > held_before; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_FALSE(CanvasImageReady(harness, order.late_canvas));
    EXPECT_FALSE(harness.PairReady());
    EXPECT_FALSE(region_a->isVisible());
    EXPECT_FALSE(region_b->isVisible());
    EXPECT_TRUE(status->isVisible());
    EXPECT_EQ(status->property("text").toString(), QStringLiteral("Rendering comparison images"));

    harness.observer().Hold(order.late_side, false);
    ASSERT_TRUE(WaitUntil([&] { return harness.PairReady(); }));
    EXPECT_TRUE(region_a->isVisible());
    EXPECT_TRUE(region_b->isVisible());
    EXPECT_FALSE(status->isVisible());
  }

  // A failed pair shows its error and no images.
  harness.root()->setProperty("pair", QVariant{});
  harness.root()->setProperty("status", QStringLiteral("failed"));
  harness.root()->setProperty("errorText", QStringLiteral("Comparison image B: no host pixels."));
  EXPECT_FALSE(harness.PairReady());
  EXPECT_FALSE(region_a->isVisible());
  EXPECT_TRUE(status->isVisible());
  EXPECT_EQ(status->property("text").toString(),
            QStringLiteral("Comparison image B: no host pixels."));
}

TEST(EditorComparisonViewQmlTest, LayoutAndDividerChangesDoNotSubmitRenderJobs) {
  ComparisonQmlHarness harness(kViewHost, "EditorComparisonViewHost.qml");
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  const auto publication =
      harness.PublishPair(51, MakeFilledRenderedImage(FullGeometry(), cv::Scalar(1, 0, 0, 1)),
                          MakeFilledRenderedImage(HalfCropGeometry(), cv::Scalar(0, 1, 0, 1)));
  ASSERT_TRUE(WaitUntil([&] { return harness.PairReady(); }));
  ASSERT_EQ(harness.observer().Reads(), 2);

  QSignalSpy load_failures(harness.root(), SIGNAL(imageLoadFailed(QString)));
  auto*      canvas_a = harness.find(QStringLiteral("editorComparisonCanvasA"));
  auto*      canvas_b = harness.find(QStringLiteral("editorComparisonCanvasB"));
  auto*      region_a = harness.find(QStringLiteral("editorComparisonRegionA"));
  auto*      region_b = harness.find(QStringLiteral("editorComparisonRegionB"));
  auto*      divider  = harness.find(QStringLiteral("editorComparisonDivider"));
  auto*      stage    = harness.find(QStringLiteral("editorComparisonStage"));
  ASSERT_NE(divider, nullptr);

  for (const auto* mode : {"complete", "divider"}) {
    for (const auto* orientation : {"horizontal", "vertical"}) {
      for (const bool swapped : {false, true}) {
        for (const double position : {0.0, 0.3, 1.0}) {
          harness.root()->setProperty("displayMode", QString::fromLatin1(mode));
          harness.root()->setProperty("orientation", QString::fromLatin1(orientation));
          harness.root()->setProperty("swapped", swapped);
          harness.root()->setProperty("dividerPosition", position);
          QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

          SCOPED_TRACE(std::string(mode) + " " + orientation + (swapped ? " swapped" : "") + " " +
                       std::to_string(position));
          EXPECT_TRUE(harness.PairReady());
          EXPECT_EQ(canvas_a->property("source").toString(), publication.a_url);
          EXPECT_EQ(canvas_b->property("source").toString(), publication.b_url);
          EXPECT_TRUE(region_a->isVisible());
          EXPECT_TRUE(region_b->isVisible());
          EXPECT_EQ(divider->isVisible(), QString::fromLatin1(mode) == "divider");

          // The first slot is A unless swapped; in complete mode the two slots do not overlap.
          auto* first  = swapped ? region_b : region_a;
          auto* second = swapped ? region_a : region_b;
          if (QString::fromLatin1(orientation) == "horizontal") {
            EXPECT_LE(first->x() + first->width(), second->x() + 1e-6);
          } else {
            EXPECT_LE(first->y() + first->height(), second->y() + 1e-6);
          }
          EXPECT_LE(second->x() + second->width(), stage->width() + 1e-6);
          EXPECT_LE(second->y() + second->height(), stage->height() + 1e-6);
        }
      }
    }
  }

  // Ready-pair view changes never load images again and never request new images.
  EXPECT_EQ(harness.observer().Reads(), 2);
  EXPECT_EQ(load_failures.count(), 0);
}

TEST(EditorComparisonViewQmlTest, BothDividerOrientationsRevealTheSameReferencePoint) {
  ComparisonQmlHarness harness(kViewHost, "EditorComparisonViewHost.qml");
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  const auto a_geometry = FullGeometry();
  const auto b_geometry = HalfCropGeometry();
  (void)harness.PublishPair(61, MakeFilledRenderedImage(a_geometry, cv::Scalar(1, 0, 0, 1)),
                            MakeFilledRenderedImage(b_geometry, cv::Scalar(0, 0, 1, 1)));
  ASSERT_TRUE(WaitUntil([&] { return harness.PairReady(); }));
  harness.root()->setProperty("displayMode", QStringLiteral("divider"));

  auto*         stage    = harness.find(QStringLiteral("editorComparisonStage"));
  auto*         region_a = harness.find(QStringLiteral("editorComparisonRegionA"));
  auto*         region_b = harness.find(QStringLiteral("editorComparisonRegionB"));
  auto*         divider  = harness.find(QStringLiteral("editorComparisonDivider"));

  // A point inside B's crop footprint.
  const Vector2 reference{120.0f, 210.0f};
  for (const auto* orientation : {"horizontal", "vertical"}) {
    SCOPED_TRACE(orientation);
    harness.root()->setProperty("orientation", QString::fromLatin1(orientation));
    // The stage keeps its size within one orientation; a top/bottom layout reserves a caption
    // row under the stage, so positions are compared within an orientation.
    QPointF first_position;
    for (const double position : {0.25, 0.5, 0.8}) {
      harness.root()->setProperty("dividerPosition", position);
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      const QPointF from_a =
          StagePointOfReference(harness, "editorComparisonCanvasA", a_geometry, reference);
      const QPointF from_b =
          StagePointOfReference(harness, "editorComparisonCanvasB", b_geometry, reference);
      // Both images draw the reference point at one stage position, for every divider position
      // and orientation: only the clip changes.
      EXPECT_NEAR(from_a.x(), from_b.x(), kPointTolerance);
      EXPECT_NEAR(from_a.y(), from_b.y(), kPointTolerance);
      if (first_position.isNull()) {
        first_position = from_a;
      }
      EXPECT_NEAR(from_a.x(), first_position.x(), kPointTolerance);
      EXPECT_NEAR(from_a.y(), first_position.y(), kPointTolerance);

      const double fit_x = stage->property("fitX").toDouble();
      const double fit_y = stage->property("fitY").toDouble();
      if (QString::fromLatin1(orientation) == "horizontal") {
        const double line = fit_x + position * stage->property("fitWidth").toDouble();
        EXPECT_NEAR(region_a->width(), line, kPointTolerance);
        EXPECT_NEAR(region_b->x(), line, kPointTolerance);
        EXPECT_NEAR(divider->x() + divider->width() / 2, line, kPointTolerance);
      } else {
        const double line = fit_y + position * stage->property("fitHeight").toDouble();
        EXPECT_NEAR(region_a->height(), line, kPointTolerance);
        EXPECT_NEAR(region_b->y(), line, kPointTolerance);
        EXPECT_NEAR(divider->y() + divider->height() / 2, line, kPointTolerance);
      }
    }
  }

  // Keyboard steps, Home, and End move the divider to its bounds through the owner.
  harness.root()->setProperty("orientation", QStringLiteral("vertical"));
  harness.root()->setProperty("dividerPosition", 0.5);
  QCoreApplication::processEvents();
  EXPECT_TRUE(divider->activeFocusOnTab());
  SendKey(divider, Qt::Key_Down);
  EXPECT_NEAR(harness.root()->property("dividerPosition").toDouble(), 0.51, 1e-9);
  SendKey(divider, Qt::Key_Up);
  SendKey(divider, Qt::Key_Up);
  EXPECT_NEAR(harness.root()->property("dividerPosition").toDouble(), 0.49, 1e-9);
  SendKey(divider, Qt::Key_End);
  EXPECT_DOUBLE_EQ(harness.root()->property("dividerPosition").toDouble(), 1.0);
  SendKey(divider, Qt::Key_Down);
  EXPECT_DOUBLE_EQ(harness.root()->property("dividerPosition").toDouble(), 1.0);
  SendKey(divider, Qt::Key_Home);
  EXPECT_DOUBLE_EQ(harness.root()->property("dividerPosition").toDouble(), 0.0);
  // Left/right keys do not move a top/bottom divider.
  SendKey(divider, Qt::Key_Right);
  EXPECT_DOUBLE_EQ(harness.root()->property("dividerPosition").toDouble(), 0.0);
}

TEST(EditorComparisonViewQmlTest, ClosingComparisonClearsProviderAndItemReferences) {
  ComparisonQmlHarness harness(kViewHost, "EditorComparisonViewHost.qml");
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* canvas_a = harness.find(QStringLiteral("editorComparisonCanvasA"));
  auto* canvas_b = harness.find(QStringLiteral("editorComparisonCanvasB"));
  auto* image_a  = canvas_a->findChild<QQuickItem*>(QStringLiteral("editorComparisonCanvasImage"));

  // Close while the provider is still reading B: the read already holds its image.
  harness.observer().Hold(ComparisonSide::B, true);
  (void)harness.PublishPair(71, MakeFilledRenderedImage(FullGeometry(), cv::Scalar(1, 0, 0, 1)),
                            MakeFilledRenderedImage(FullGeometry(), cv::Scalar(0, 0, 1, 1)));
  ASSERT_TRUE(WaitUntil([&] { return harness.observer().HeldReads() > 0; }));
  ASSERT_TRUE(WaitUntil([&] { return canvas_a->property("imageReady").toBool(); }));

  harness.store().Clear();
  harness.root()->setProperty("pair", QVariant{});
  harness.root()->setProperty("status", QStringLiteral("idle"));
  EXPECT_EQ(harness.store().ImageCount(), 0);

  // The held read still owns a valid B image after the store released it.
  QImage held_b;
  {
    std::lock_guard lock(harness.observer().mutex);
    held_b = harness.observer().last_image_b;
  }
  ASSERT_FALSE(held_b.isNull());
  EXPECT_EQ(qBlue(held_b.pixel(3, 3)), 255);
  held_b = QImage{};
  harness.observer().Hold(ComparisonSide::B, false);

  // The items release their sources; nothing loads again.
  ASSERT_TRUE(WaitUntil([&] {
    return canvas_a->property("imageStatus").toInt() == 0 &&
           canvas_b->property("imageStatus").toInt() == 0;
  }));
  EXPECT_TRUE(canvas_a->property("source").toString().isEmpty());
  EXPECT_TRUE(canvas_b->property("source").toString().isEmpty());
  EXPECT_TRUE(image_a->property("source").toUrl().isEmpty());
  EXPECT_FALSE(harness.PairReady());
  EXPECT_EQ(harness.observer().Reads(), 2);

  // Once Qt drops the cancelled read, only the observer's copy owns B; the store owns none.
  EXPECT_TRUE(WaitUntil([&] {
    std::lock_guard lock(harness.observer().mutex);
    return harness.observer().last_image_b.isDetached();
  }));
  harness.observer().DropLastImages();
  EXPECT_TRUE(harness.store().Get(71, ComparisonSide::A).isNull());
}

TEST(EditorComparisonViewQmlTest, PanelReportsRequestsAndDisablesSelectionWhileRendering) {
  constexpr auto       kPanelHost = R"(
import QtQuick
EditorComparisonPanel {
    width: 320
    height: 640
    sourceOptions: [
        { value: "root", label: "Imported image" },
        { value: "current", label: "Current working state" },
        { value: "version:7", label: "Warm print" }
    ]
    aSourceValue: "root"
    bSourceValue: "current"
}
)";
  ComparisonQmlHarness harness(kPanelHost, "EditorComparisonPanelHost.qml");
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto*      panel = harness.root();

  QSignalSpy a_requests(panel, SIGNAL(aSourceRequested(QString)));
  QSignalSpy b_requests(panel, SIGNAL(bSourceRequested(QString)));
  QSignalSpy kind_requests(panel, SIGNAL(comparisonKindRequested(QString)));
  QSignalSpy mode_requests(panel, SIGNAL(displayModeRequested(QString)));
  QSignalSpy orientation_requests(panel, SIGNAL(orientationRequested(QString)));
  QSignalSpy swap_requests(panel, SIGNAL(swapRequested()));
  QSignalSpy retry_requests(panel, SIGNAL(retryRequested()));
  QSignalSpy close_requests(panel, SIGNAL(closeRequested()));

  auto*      combo_a = harness.find<QObject>(QStringLiteral("editorComparisonSourceACombo"));
  auto*      combo_b = harness.find<QObject>(QStringLiteral("editorComparisonSourceBCombo"));
  auto*      kind    = harness.find(QStringLiteral("editorComparisonKindSwitcher"));
  auto*      mode    = harness.find(QStringLiteral("editorComparisonDisplayModeSwitcher"));
  auto*      orient  = harness.find(QStringLiteral("editorComparisonOrientationSwitcher"));
  auto*      swap    = harness.find(QStringLiteral("editorComparisonSwapButton"));
  auto*      close   = harness.find(QStringLiteral("editorComparisonCloseButton"));
  auto*      retry   = harness.find(QStringLiteral("editorComparisonRetryButton"));
  auto*      status  = harness.find(QStringLiteral("editorComparisonStatusLabel"));
  ASSERT_TRUE(combo_a && combo_b && kind && mode && orient && swap && close && retry && status);

  EXPECT_EQ(combo_a->property("currentIndex").toInt(), 0);
  EXPECT_EQ(combo_b->property("currentIndex").toInt(), 1);
  EXPECT_TRUE(combo_a->property("enabled").toBool());

  // View-only choices report their own request and nothing that needs new images.
  QMetaObject::invokeMethod(mode, "selected", Q_ARG(int, 0), Q_ARG(QString, "complete"));
  QMetaObject::invokeMethod(orient, "selected", Q_ARG(int, 1), Q_ARG(QString, "vertical"));
  QMetaObject::invokeMethod(swap, "clicked");
  ASSERT_EQ(mode_requests.count(), 1);
  EXPECT_EQ(mode_requests.at(0).at(0).toString(), QStringLiteral("complete"));
  ASSERT_EQ(orientation_requests.count(), 1);
  EXPECT_EQ(orientation_requests.at(0).at(0).toString(), QStringLiteral("vertical"));
  EXPECT_EQ(swap_requests.count(), 1);
  EXPECT_EQ(
      a_requests.count() + b_requests.count() + kind_requests.count() + retry_requests.count(), 0);

  // Selecting a source reports its value.
  QMetaObject::invokeMethod(combo_b, "activated", Q_ARG(int, 2));
  ASSERT_EQ(b_requests.count(), 1);
  EXPECT_EQ(b_requests.at(0).at(0).toString(), QStringLiteral("version:7"));

  // While a pair renders, choices that need new images are disabled; the view choices and
  // Close stay available.
  panel->setProperty("status", QStringLiteral("loading"));
  EXPECT_FALSE(combo_a->property("enabled").toBool());
  EXPECT_FALSE(combo_b->property("enabled").toBool());
  EXPECT_FALSE(kind->property("enabled").toBool());
  EXPECT_TRUE(mode->property("enabled").toBool());
  EXPECT_TRUE(orient->property("enabled").toBool());
  EXPECT_TRUE(swap->property("enabled").toBool());
  EXPECT_TRUE(close->property("enabled").toBool());
  EXPECT_EQ(status->property("text").toString(), QStringLiteral("Rendering comparison images"));
  QMetaObject::invokeMethod(close, "clicked");
  EXPECT_EQ(close_requests.count(), 1);

  // A failure shows its error and Retry.
  panel->setProperty("status", QStringLiteral("failed"));
  panel->setProperty("errorText", QStringLiteral("Comparison image A: non-finite pixel."));
  EXPECT_TRUE(combo_a->property("enabled").toBool());
  EXPECT_TRUE(retry->isVisible());
  EXPECT_EQ(status->property("text").toString(),
            QStringLiteral("Comparison image A: non-finite pixel."));
  QMetaObject::invokeMethod(retry, "clicked");
  EXPECT_EQ(retry_requests.count(), 1);

  // HDR output: the reason replaces the controls that need images.
  panel->setProperty("hdrUnavailable", true);
  panel->setProperty("hdrReason", QStringLiteral("HDR comparison is unavailable."));
  EXPECT_FALSE(combo_a->property("enabled").toBool());
  EXPECT_FALSE(retry->isVisible());
  EXPECT_EQ(status->property("text").toString(), QStringLiteral("HDR comparison is unavailable."));
  EXPECT_TRUE(close->property("enabled").toBool());
}

}  // namespace alcedo::ui::test
