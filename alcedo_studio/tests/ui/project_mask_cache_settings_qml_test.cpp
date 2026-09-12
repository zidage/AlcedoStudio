//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file project_mask_cache_settings_qml_test.cpp
/// @brief Project Mask cache settings panel: project-scoped calls, Clear copy.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <mutex>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class FakeProjectModule final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap maskCacheState READ maskCacheState NOTIFY MaskCacheStateChanged)

 public:
  FakeProjectModule() { ResetState(); }

  auto maskCacheState() const -> QVariantMap { return state_; }

  void ResetState() {
    state_.clear();
    state_.insert(QStringLiteral("available"), true);
    state_.insert(QStringLiteral("projectName"), QStringLiteral("Field Session"));
    state_.insert(QStringLiteral("projectUuid"), QStringLiteral("uuid-field-session"));
    state_.insert(QStringLiteral("chosenRoot"), QStringLiteral("/projects/field/cache"));
    state_.insert(QStringLiteral("effectiveRoot"),
                  QStringLiteral("/projects/field/cache/alcedo-mask-cache/uuid-field-session"));
    state_.insert(QStringLiteral("retention"), QStringLiteral("keep"));
    state_.insert(QStringLiteral("fileCount"), 4);
    state_.insert(QStringLiteral("byteCount"), 8192);
    state_.insert(QStringLiteral("pendingWrites"), 0);
    state_.insert(QStringLiteral("dirty"), false);
    state_.insert(QStringLiteral("lastError"), QString());
    state_.insert(QStringLiteral("applyError"), QString());
    emit MaskCacheStateChanged();
  }

  Q_INVOKABLE bool ApplyMaskCacheRoot(const QString& chosenRoot) {
    ++apply_root_calls_;
    last_applied_root_ = chosenRoot;
    state_.insert(QStringLiteral("chosenRoot"), chosenRoot);
    emit MaskCacheStateChanged();
    return true;
  }
  Q_INVOKABLE bool ApplyMaskCacheRetention(const QString& retentionKey) {
    ++apply_retention_calls_;
    last_applied_retention_ = retentionKey;
    state_.insert(QStringLiteral("retention"), retentionKey);
    emit MaskCacheStateChanged();
    return true;
  }
  Q_INVOKABLE bool ClearMaskCache() {
    ++clear_calls_;
    state_.insert(QStringLiteral("fileCount"), 0);
    state_.insert(QStringLiteral("byteCount"), 0);
    emit MaskCacheStateChanged();
    return true;
  }
  Q_INVOKABLE void RefreshMaskCacheState() {
    ++refresh_calls_;
    emit MaskCacheStateChanged();
  }

  auto    applyRootCalls() const -> int { return apply_root_calls_; }
  auto    applyRetentionCalls() const -> int { return apply_retention_calls_; }
  auto    clearCalls() const -> int { return clear_calls_; }
  auto    refreshCalls() const -> int { return refresh_calls_; }
  auto    lastAppliedRoot() const -> QString { return last_applied_root_; }
  auto    lastAppliedRetention() const -> QString { return last_applied_retention_; }

  void SetStateEntry(const QString& key, const QVariant& value) {
    state_.insert(key, value);
    emit MaskCacheStateChanged();
  }

 signals:
  void MaskCacheStateChanged();

 private:
  QVariantMap state_;
  int         apply_root_calls_      = 0;
  int         apply_retention_calls_ = 0;
  int         clear_calls_           = 0;
  int         refresh_calls_         = 0;
  QString     last_applied_root_;
  QString     last_applied_retention_;
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

class CachePanelHarness {
 public:
  CachePanelHarness(FakeProjectModule* module, int width) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                              &AppTheme::Instance());

    QQmlComponent component(
        &engine_, QUrl::fromLocalFile(QmlDirectory() +
                                      QStringLiteral("/ProjectMaskCacheSettingsPanel.qml")));
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }
    QVariantMap initial;
    initial.insert(QStringLiteral("projectModule"),
                   QVariant::fromValue(static_cast<QObject*>(module)));
    initial.insert(QStringLiteral("projectReady"), true);
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(width);
    root_->setParentItem(window_.contentItem());
    window_.resize(width, 900);
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

  auto root() const -> QQuickItem* { return root_.get(); }
  auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

  template <typename T = QQuickItem>
  auto find(const QString& name) const -> T* {
    return root_ ? root_->findChild<T*>(name) : nullptr;
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

TEST(ProjectMaskCacheSettingsQmlTest, CacheSettingsTargetSelectedProjectOnly) {
  FakeProjectModule  module;
  CachePanelHarness  harness(&module, 460);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "reloadPending"));
  ProcessEvents(20);
  EXPECT_EQ(harness.root()->property("pendingRoot").toString(),
            QStringLiteral("/projects/field/cache"));

  // Root change applies through the project module with the pending path only.
  harness.root()->setProperty("pendingRoot", QStringLiteral("/scratch/other/cache"));
  auto* warning = harness.find(QStringLiteral("maskCacheRootWarning"));
  ASSERT_NE(warning, nullptr);
  EXPECT_TRUE(warning->property("visible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "applyPending"));
  ProcessEvents(20);
  EXPECT_EQ(module.applyRootCalls(), 1);
  EXPECT_EQ(module.lastAppliedRoot(), QStringLiteral("/scratch/other/cache"));
  EXPECT_EQ(module.applyRetentionCalls(), 0);

  // Retention applies through the same project-scoped setter.
  harness.root()->setProperty("pendingRetention",
                              QStringLiteral("deleteOnProjectClose"));
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "applyPending"));
  ProcessEvents(20);
  EXPECT_EQ(module.applyRetentionCalls(), 1);
  EXPECT_EQ(module.lastAppliedRetention(), QStringLiteral("deleteOnProjectClose"));
  EXPECT_EQ(module.applyRootCalls(), 1);
}

TEST(ProjectMaskCacheSettingsQmlTest, ProjectClearExplainsThatBrushHistoryIsRetained) {
  FakeProjectModule  module;
  CachePanelHarness  harness(&module, 460);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* hint = harness.find(QStringLiteral("maskCacheClearHint"));
  ASSERT_NE(hint, nullptr);
  const QString hint_text = hint->property("text").toString();
  EXPECT_TRUE(hint_text.contains(QStringLiteral("Brush strokes")));
  EXPECT_TRUE(hint_text.contains(QStringLiteral("history"), Qt::CaseInsensitive));
  EXPECT_TRUE(hint_text.contains(QStringLiteral("rebuilt"), Qt::CaseInsensitive));

  QSignalSpy messages(harness.root(), SIGNAL(messageRequested(QString)));
  auto*       clear = harness.find(QStringLiteral("maskCacheClearButton"));
  ASSERT_NE(clear, nullptr);
  ASSERT_TRUE(clear->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(clear, "clicked"));
  ProcessEvents(20);
  EXPECT_EQ(module.clearCalls(), 1);
  ASSERT_GE(messages.size(), 1);
  EXPECT_TRUE(messages.takeFirst().first().toString().contains(
      QStringLiteral("cleared"), Qt::CaseInsensitive));
}

TEST(ProjectMaskCacheSettingsQmlTest, ErrorsSurfaceFromProjectModuleState) {
  FakeProjectModule  module;
  CachePanelHarness  harness(&module, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "reloadPending"));
  ProcessEvents(20);

  auto* status = harness.find(QStringLiteral("maskCacheStatusLabel"));
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->property("visible").toBool());

  module.SetStateEntry(QStringLiteral("applyError"),
                       QStringLiteral("mask cache folder is not writable"));
  ProcessEvents(20);
  EXPECT_TRUE(status->property("visible").toBool());
  EXPECT_EQ(status->property("text").toString(),
            QStringLiteral("mask cache folder is not writable"));
}

}  // namespace
}  // namespace alcedo::ui::test

#include "project_mask_cache_settings_qml_test.moc"
