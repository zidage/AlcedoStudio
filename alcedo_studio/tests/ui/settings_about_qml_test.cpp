//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > About and the Settings type scale (issue 824) on the production SettingDialog and
// ApplicationModuleHost:
// - Open Log Folder opens the log directory through the LogFolderController stand-in.
// - Every Text and Label of every Settings page uses a theme size token and no weight above 600
//   (the Alcedo Studio wordmark of About keeps its heading weight).
// - The acknowledgements of About list every link of README.md "Acknowledgments".
// Buttons are activated through their `clicked` signal (offscreen pointer delivery into the
// dialog's scrolled page is not reliable, AGENTS.md).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <vector>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui::test {
namespace {

constexpr int kSettingsCategoryCount = 12;
constexpr int kAboutCategory         = 11;

void          ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

const char* const kDialogHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: 1280
    height: 800
    visible: true

    SettingDialog {
        objectName: "settingsDialog"
        languageOptions: languageManager.availableLanguages
    }
}
)QML";

/// Sets an environment variable for the lifetime of the object and restores the old value.
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const QByteArray& value)
      : name_(name), had_value_(qEnvironmentVariableIsSet(name)), old_value_(qgetenv(name)) {
    qputenv(name_, value);
  }
  ~ScopedEnvironment() {
    if (had_value_) {
      qputenv(name_, old_value_);
    } else {
      qunsetenv(name_);
    }
  }
  ScopedEnvironment(const ScopedEnvironment&)            = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  const char* name_;
  bool        had_value_;
  QByteArray  old_value_;
};

class SettingsDialogHarness {
 public:
  SettingsDialogHarness() {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SettingsAboutHarness"));
    AppTheme::RegisterFonts();
    // The style can be set once per process, before the first Controls import.
    static const bool style_set = [] {
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      return true;
    }();
    Q_UNUSED(style_set);
    host_             = std::make_unique<ApplicationModuleHost>();
    language_manager_ = std::make_unique<LanguageManager>(QCoreApplication::instance());
    AppTheme::SetEffectiveLanguageCode(language_manager_->EffectiveLanguageCode());
    RegisterShortcutRegistryQmlType();
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), host_.get());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                              language_manager_.get());
    host_->AttachQmlEngine(&engine_);

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///SettingsAboutHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
      }
    }
    ProcessEvents(120);
  }

  auto dialog() -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(QStringLiteral("settingsDialog"))
                              : nullptr;
  }
  auto item(const QString& name) -> QQuickItem* {
    return window_ != nullptr ? window_->findChild<QQuickItem*>(name) : nullptr;
  }

  void OpenCategory(int category) {
    dialog()->setProperty("requestedCategory", category);
    QMetaObject::invokeMethod(dialog(), "open");
    dialog()->setProperty("currentCategory", category);
    ProcessEvents(150);
  }

  QTemporaryDir                          settings_dir_;
  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<LanguageManager>       language_manager_;
  QQmlApplicationEngine                  engine_;
  QQuickWindow*                          window_ = nullptr;
  QStringList                            warnings_;
};

/// True when @p item or one of its parents has @p object_name.
auto HasAncestorNamed(QQuickItem* item, const QString& object_name) -> bool {
  for (QQuickItem* current = item; current != nullptr; current = current->parentItem()) {
    if (current->objectName() == object_name) {
      return true;
    }
  }
  return false;
}

void CollectTextItems(QQuickItem* item, std::vector<QQuickItem*>& out) {
  if (item == nullptr) {
    return;
  }
  if (item->inherits("QQuickText")) {
    out.push_back(item);
  }
  for (QQuickItem* child : item->childItems()) {
    CollectTextItems(child, out);
  }
}

/// One line per Text or Label of @p root whose size is not a theme token or whose weight is
/// above 600. Empty and blank texts are skipped; the wordmark keeps its heading weight.
auto TypeScaleViolations(QQuickItem* root) -> QStringList {
  const AppTheme&    theme = AppTheme::Instance();
  const QSet<int>    token_sizes{theme.fontSizeCaption(), theme.fontSizeBody(),
                              theme.fontSizeTitle(),   theme.fontSizeSection(),
                              theme.fontSizeHeadline(), theme.fontSizeDisplay()};
  std::vector<QQuickItem*> texts;
  CollectTextItems(root, texts);
  QStringList violations;
  for (QQuickItem* text_item : texts) {
    const QString text = text_item->property("text").toString();
    if (text.trimmed().isEmpty()) {
      continue;
    }
    const QFont font      = text_item->property("font").value<QFont>();
    const bool  wordmark  = HasAncestorNamed(text_item, QStringLiteral("aboutWordmark"));
    const bool  size_ok   = token_sizes.contains(font.pixelSize());
    const bool  weight_ok = wordmark || font.weight() <= 600;
    if (!size_ok || !weight_ok) {
      violations.push_back(QStringLiteral("\"%1\" (%2): pixelSize %3, weight %4")
                               .arg(text.left(48), text_item->objectName())
                               .arg(font.pixelSize())
                               .arg(static_cast<int>(font.weight())));
    }
  }
  return violations;
}

auto ReadmeAcknowledgementUrls() -> QStringList {
  // ALCEDO_TEST_SRC_DIR is alcedo_studio/src (spelled through tests/..); README.md is two up.
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / ".." / ".." / "README.md";
  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  const QString readme = QString::fromUtf8(file.readAll());
  const auto    start  = readme.indexOf(QStringLiteral("\n## Acknowledgments"));
  if (start < 0) {
    return {};
  }
  auto end = readme.indexOf(QStringLiteral("\n## "), start + 1);
  if (end < 0) {
    end = readme.size();
  }
  const QString section = readme.mid(start, end - start);
  // Links of the bullets; the paragraph after them points at files of this repository.
  static const QRegularExpression kLink(QStringLiteral(R"(\]\((https?://[^)\s]+)\))"));
  QStringList                     urls;
  for (auto it = kLink.globalMatch(section); it.hasNext();) {
    const QString url = it.next().captured(1);
    if (!urls.contains(url)) {
      urls.push_back(url);
    }
  }
  return urls;
}

auto PageUrls(QObject* about_page) -> QStringList {
  QStringList urls;
  for (const char* list_name : {"acknowledgements", "components"}) {
    const QVariantList entries = about_page->property(list_name).toList();
    for (const QVariant& entry : entries) {
      const QVariantList links = entry.toMap().value(QStringLiteral("links")).toList();
      for (const QVariant& link : links) {
        urls.push_back(link.toMap().value(QStringLiteral("url")).toString());
      }
    }
  }
  return urls;
}

TEST(SettingsAboutQmlTest, OpenLogFolderButtonOpensTheLogDirectory) {
  QTemporaryDir log_root;
  ASSERT_TRUE(log_root.isValid());
  const QString     expected_dir = QDir(log_root.path()).filePath(QStringLiteral("logs"));
  ScopedEnvironment log_dir_env("ALCEDO_LOG_DIR", expected_dir.toUtf8());

  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  std::vector<QUrl> opened;
  harness.host_->log_folder()->SetOpenUrlForTesting([&opened](const QUrl& url) {
    opened.push_back(url);
    return true;
  });
  harness.OpenCategory(kAboutCategory);

  QQuickItem* button = harness.item(QStringLiteral("aboutOpenLogFolderButton"));
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->isVisible());
  EXPECT_TRUE(button->isEnabled());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  ProcessEvents();

  // Without an open log file the directory is the one InitializeApplicationLogging would use.
  const QString log_directory = alcedo::diag::LogDirectory();
  if (alcedo::diag::CurrentLogFilePath().isEmpty()) {
    EXPECT_EQ(log_directory, QDir(expected_dir).absolutePath());
  }
  ASSERT_EQ(opened.size(), 1u);
  EXPECT_EQ(opened.front(), QUrl::fromLocalFile(log_directory));
  // The button creates the directory before the file manager opens it.
  EXPECT_TRUE(QDir(log_directory).exists());
}

TEST(SettingsAboutQmlTest, SettingsPagesUseTheThemeTypeScale) {
  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.OpenCategory(0);
  auto* content = harness.dialog()->property("contentItem").value<QQuickItem*>();
  ASSERT_NE(content, nullptr);

  QStringList violations;
  for (int category = 0; category < kSettingsCategoryCount; ++category) {
    harness.dialog()->setProperty("currentCategory", category);
    ProcessEvents(80);
    if (category == kAboutCategory) {
      QObject* about_page = harness.window_->findChild<QObject*>(QStringLiteral("aboutPage"));
      ASSERT_NE(about_page, nullptr);
      about_page->setProperty("licensesExpanded", true);
      ProcessEvents(80);
    }
    for (const QString& violation : TypeScaleViolations(content)) {
      if (!violations.contains(violation)) {
        violations.push_back(violation);
      }
    }
  }
  EXPECT_TRUE(violations.isEmpty()) << violations.join('\n').toStdString();
}

TEST(SettingsAboutQmlTest, AcknowledgementsListEveryReadmeLink) {
  const QStringList readme_urls = ReadmeAcknowledgementUrls();
  ASSERT_FALSE(readme_urls.isEmpty());

  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.OpenCategory(kAboutCategory);
  QObject* about_page = harness.window_->findChild<QObject*>(QStringLiteral("aboutPage"));
  ASSERT_NE(about_page, nullptr);

  const QStringList page_urls = PageUrls(about_page);
  QStringList       missing;
  for (const QString& url : readme_urls) {
    if (!page_urls.contains(url)) {
      missing.push_back(url);
    }
  }
  EXPECT_TRUE(missing.isEmpty()) << "README.md links missing on Settings > About:\n"
                                 << missing.join('\n').toStdString();
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
