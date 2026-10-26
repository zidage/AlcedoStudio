//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM3.E (#537): the AI Mask panel shows the training data notice of the Sky model before the
// first Sky run (lab decision U18).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRectF>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitFor(const std::function<bool()>& condition, int timeout_ms = 2000) -> bool {
  QDeadlineTimer deadline(timeout_ms);
  while (!condition()) {
    if (deadline.hasExpired()) {
      return false;
    }
    ProcessEvents(20);
  }
  return true;
}

/// The first item named @p object_name in the visual tree below @p item.
auto FindVisualItem(QQuickItem* item, const QString& object_name) -> QQuickItem* {
  if (item == nullptr) {
    return nullptr;
  }
  if (item->objectName() == object_name) {
    return item;
  }
  for (auto* child : item->childItems()) {
    if (auto* found = FindVisualItem(child, object_name)) {
      return found;
    }
  }
  return nullptr;
}

/// The production AI Mask panel without a tool: the state before any task runs.
class AiMaskPanelHarness {
 public:
  AiMaskPanelHarness() {
    static bool registered = false;
    if (!registered) {
      RegisterEditorAdjustmentQmlTypes();
      registered = true;
    }
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QQmlComponent component(
        &engine_, QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAiMaskPanel.qml")));
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(QVariantMap{})));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(320);
    root_->setHeight(760);
    root_->setParentItem(window_.contentItem());
    window_.resize(320, 760);
    window_.show();
    ProcessEvents(100);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

TEST(EditorAiMaskPanelQmlTest, SkyTrainingDataNoticeIsVisibleBeforeFirstSkyRun) {
  AiMaskPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  QQuickItem* notice = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    notice = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskSkyNotice"));
    return notice != nullptr && notice->isVisible() && notice->height() > 0.0;
  })) << "the Sky notice is missing or hidden";

  const auto text = notice->property("text").toString();
  EXPECT_TRUE(text.contains(QStringLiteral("training data of the Sky model")))
      << text.toStdString();
  EXPECT_TRUE(text.contains(QStringLiteral("may not permit commercial use"))) << text.toStdString();
  EXPECT_TRUE(text.contains(QStringLiteral("Do not use Sky for commercial work")))
      << text.toStdString();

  // The notice is in the panel area that shows without a scroll, next to the Sky task button.
  auto* sky = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskSky"));
  ASSERT_NE(sky, nullptr);
  const QRectF panel(0.0, 0.0, harness.root()->width(), harness.root()->height());
  const QRectF notice_rect =
      notice->mapRectToItem(harness.root(), QRectF(0.0, 0.0, notice->width(), notice->height()));
  EXPECT_TRUE(panel.contains(notice_rect)) << notice_rect.x() << "," << notice_rect.y() << " "
                                           << notice_rect.width() << "x" << notice_rect.height();
  const QRectF sky_rect =
      sky->mapRectToItem(harness.root(), QRectF(0.0, 0.0, sky->width(), sky->height()));
  EXPECT_GE(notice_rect.top(), sky_rect.bottom());
  EXPECT_LT(notice_rect.top() - sky_rect.bottom(), 40.0);
}

}  // namespace
}  // namespace alcedo::ui::test
