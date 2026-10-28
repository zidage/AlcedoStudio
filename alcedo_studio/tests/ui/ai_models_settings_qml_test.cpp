//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > AI Models (docs/roadmap/alcedo_studio/ai/external_ai_model_download_plan.md, AM4).
//
// Two harnesses:
// - The production SettingDialog on the ApplicationModuleHost. The AI model package service
//   reads the signed test feed of tests/resources/ai_model_packages through a fetch that counts
//   requests, and its downloader records starts and holds transfers. The LUT feed is off, and
//   the model directory is a temporary folder.
// - AiModelsSettingsPanel alone with QML stand-ins for the CLIP controllers, because an
//   installed and active CLIP model needs the full model files and an open project. QML cannot
//   declare the upper-case setters of ModelDownloadController; these tests do not call them.
// Buttons are activated through their `clicked` signal: offscreen pointer delivery into the
// dialog's scrolled page is not reliable (AGENTS.md), and the page logic is in those handlers.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/ai_model_package_service.hpp"
#include "app/lut_library_service.hpp"
#include "app/lut_package_service.hpp"
#include "app/model_asset_catalog.hpp"
#include "app/package_archive_download.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/system_icon_image_provider.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs                    = std::filesystem;

constexpr int kAiModelsCategory = 9;
constexpr int kAboutCategory    = 10;
const char*   kAiMaskId         = "alcedo-ai-mask";
const QUrl    kFeedUrl(QStringLiteral("https://static.aoraw.org/ai-models/v1/manifest.json"));
#ifdef __APPLE__
const char* kPlatform = "macos-arm64";
#else
const char* kPlatform = "windows-x86_64";
#endif

void ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitUntil(const std::function<bool()>& condition, int timeout_ms = 10000) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < timeout_ms) ProcessEvents(10);
  return condition();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = fs::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

auto PanelText(const char* source) -> QString {
  return QCoreApplication::translate("AiModelsSettingsPanel", source);
}

auto ReadFixture(const char* name) -> QByteArray {
  QFile file(QString::fromUtf8(ALCEDO_AI_MODEL_PACKAGE_FIXTURE_DIR) + QLatin1Char('/') +
             QString::fromLatin1(name));
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

auto Text(QObject* item) -> QString {
  return item != nullptr ? item->property("text").toString() : QString();
}

void Press(QObject* button) {
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(button->property("visible").toBool());
  ASSERT_TRUE(button->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  ProcessEvents();
}

// ── Doubles ─────────────────────────────────────────────────────────────────

struct FeedServer {
  QByteArray json;
  QByteArray signature;
  int        manifest_requests  = 0;
  int        signature_requests = 0;
};

/// Records every transfer and holds it until Cancel().
class HeldArchiveDownloader final : public alcedo::PackageArchiveDownloader {
 public:
  struct State {
    std::vector<QString> started_urls;
    FinishedCallback     pending;
  };
  explicit HeldArchiveDownloader(std::shared_ptr<State> state) : state_(std::move(state)) {}

  auto Start(const alcedo::DownloadRequest& request, ProgressCallback, FinishedCallback on_finished)
      -> bool override {
    if (state_->pending) return false;
    state_->started_urls.push_back(request.items.front().url.toString());
    state_->pending = std::move(on_finished);
    return true;
  }
  void Cancel(const QString&) override {
    if (!state_->pending) return;
    FinishedCallback finished = std::move(state_->pending);
    QTimer::singleShot(0, [finished] { finished(false, true, QStringLiteral("canceled")); });
  }

 private:
  std::shared_ptr<State> state_;
};

class MemoryAiModelPreferences final : public alcedo::AiModelPackagePreferences {
 public:
  explicit MemoryAiModelPreferences(std::shared_ptr<quint64> statement_version)
      : statement_version_(std::move(statement_version)) {}
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return 1; }
  void               SaveTrustedSequence(quint64) override {}
  [[nodiscard]] auto LoadAcceptedStatementVersion() const -> quint64 override {
    return *statement_version_;
  }
  void SaveAcceptedStatementVersion(quint64 version) override { *statement_version_ = version; }

 private:
  std::shared_ptr<quint64> statement_version_;
};

class MemoryLibraryPreferences final : public alcedo::LutLibraryPreferences {
 public:
  explicit MemoryLibraryPreferences(fs::path root) : root_(std::move(root)) {}
  [[nodiscard]] auto LoadRoot() const -> std::optional<fs::path> override { return root_; }
  [[nodiscard]] auto SaveRoot(const fs::path& root) -> bool override {
    root_ = root;
    return true;
  }
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override { return {}; }
  void               SaveLegacyFavoritePaths(const QStringList&) override {}

 private:
  fs::path root_;
};

// ── Settings dialog harness ─────────────────────────────────────────────────

const char* const kDialogHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: 1280
    height: 860
    visible: true

    SettingDialog {
        objectName: "settingsDialog"
        languageOptions: languageManager.availableLanguages
    }
}
)QML";

class AiModelsDialogHarness {
 public:
  AiModelsDialogHarness() {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AiModelsSettingsHarness"));
    base_ = fs::weakly_canonical(fs::path(base_dir_.path().toStdWString()));
    fs::create_directories(base_ / "models");
    fs::create_directories(base_ / "luts");
    QSettings{}.setValue(QLatin1String(alcedo::kSemanticModelDirectoryKey),
                         QString::fromStdWString((base_ / "models").wstring()));
    feed_->json      = ReadFixture("manifest.json");
    feed_->signature = ReadFixture("manifest.json.sig");

    ApplicationModuleHost::PackageServiceFactories factories;
    factories.library_options = [this] {
      alcedo::LutLibraryServiceOptions options;
      options.preferences       = std::make_unique<MemoryLibraryPreferences>(base_ / "luts");
      options.default_root      = base_ / "luts";
      options.scan_worker_count = 1;
      return options;
    };
    // No LUT feed: the LUT check makes no request.
    factories.package_options          = [] { return alcedo::LutPackageServiceOptions{}; };
    factories.ai_model_package_options = [this] {
      alcedo::AiModelPackageServiceOptions options;
      options.feed_url   = kFeedUrl;
      options.public_key = QByteArray::fromBase64(ReadFixture("test_public_key.txt").trimmed());
      options.platform   = QString::fromLatin1(kPlatform);
      options.fetch      = [feed = feed_](const QUrl&                              url, qint64,
                                     std::function<void(QByteArray, QString)> done) {
        const bool signature = url.path().endsWith(QStringLiteral(".sig"));
        ++(signature ? feed->signature_requests : feed->manifest_requests);
        QTimer::singleShot(
            0, [feed, signature, done] { done(signature ? feed->signature : feed->json, {}); });
      };
      options.downloader  = std::make_unique<HeldArchiveDownloader>(downloads_);
      options.preferences = std::make_unique<MemoryAiModelPreferences>(statement_version_);
      return options;
    };

    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    host_ = std::make_unique<ApplicationModuleHost>(
        nullptr, ApplicationModuleHost::LifecycleObserver{}, std::move(factories));
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
    WaitUntil([this] { return !host_->lut_library()->busy(); });

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///AiModelsSettingsHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
    }
    ProcessEvents(120);
  }

  auto dialog() -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(QStringLiteral("settingsDialog"))
                              : nullptr;
  }
  auto item(const QString& name) -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(name) : nullptr;
  }
  auto statement() -> QObject* { return item(QStringLiteral("aiModelDownloadStatementDialog")); }
  auto statementButton(const char* name) -> QObject* {
    QObject* popup = statement();
    return popup != nullptr ? popup->findChild<QObject*>(QString::fromLatin1(name)) : nullptr;
  }
  void Open(int category) {
    dialog()->setProperty("requestedCategory", category);
    QMetaObject::invokeMethod(dialog(), "open");
    ProcessEvents(150);
  }
  void Close() {
    QMetaObject::invokeMethod(dialog(), "close");
    ProcessEvents(150);
  }
  auto packages() -> alcedo::AiModelPackageService* { return host_->ai_model_packages(); }
  auto WaitForCheck() -> bool {
    return WaitUntil([this] { return packages()->checked() && !packages()->checking(); });
  }

  QTemporaryDir                                 settings_dir_;
  QTemporaryDir                                 base_dir_;
  fs::path                                      base_;
  std::shared_ptr<FeedServer>                   feed_ = std::make_shared<FeedServer>();
  std::shared_ptr<HeldArchiveDownloader::State> downloads_ =
      std::make_shared<HeldArchiveDownloader::State>();
  std::shared_ptr<quint64>               statement_version_ = std::make_shared<quint64>(0);
  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<LanguageManager>       language_manager_;
  QQmlApplicationEngine                  engine_;
  QQuickWindow*                          window_ = nullptr;
  QStringList                            warnings_;
};

// ── Panel harness with CLIP stand-ins ───────────────────────────────────────

const char* const kPanelHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: 1280
    height: 1400
    visible: true

    QtObject {
        id: downloads
        objectName: "clipDownloads"
        property var models: [
            { profileId: "clip-active", displayName: "Active CLIP", modelId: "test/active",
              language: "multilingual", inferenceBackend: "onnx_runtime", sizeLabel: "400 MB",
              licenseName: "Apache-2.0", licenseUrl: "https://www.apache.org/licenses/LICENSE-2.0",
              installed: true, status: "installed", busy: false, progress: 100,
              message: "", error: "" },
            { profileId: "clip-installed", displayName: "Installed CLIP", modelId: "test/installed",
              language: "en", inferenceBackend: "onnx_runtime", sizeLabel: "200 MB",
              licenseName: "apple-amlr", licenseUrl: "https://example.com/amlr",
              installed: true, status: "installed", busy: false, progress: 100,
              message: "", error: "" },
            { profileId: "clip-missing", displayName: "Missing CLIP", modelId: "test/missing",
              language: "multilingual", inferenceBackend: "onnx_runtime", sizeLabel: "900 MB",
              licenseName: "cc-by-nc-4.0", licenseUrl: "https://example.com/nc",
              installed: false, status: "notInstalled", busy: false, progress: 0,
              message: "", error: "" }
        ]
        property bool modelDownloadRunning: false
        property string modelDownloadDirectory: "C:/models"
        property string modelEndpointPreset: "mirror"
        property string customModelEndpoint: ""
        property var installCalls: []
        property var removeCalls: []
        function installModel(id) { installCalls = installCalls.concat([id]); return true }
        function cancelInstall(id) { return true }
        function removeModel(id) { removeCalls = removeCalls.concat([id]); return true }
        function refreshModels() {}
    }

    QtObject {
        id: semantic
        objectName: "clipSemantic"
        property string activeModelProfileId: "clip-active"
        property string activatingModelProfileId: ""
        property bool modelActivationRunning: false
        property var activateCalls: []
        function activateModel(id) { activateCalls = activateCalls.concat([id]); return true }
    }

    AiModelsSettingsPanel {
        width: parent.width
        downloadController: downloads
        semanticController: semantic
        projectOpen: true
    }
}
)QML";

class AiModelsPanelHarness {
 public:
  AiModelsPanelHarness() {
    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    // The folder button asks the provider that ApplicationModuleHost registers in the app.
    engine_.addImageProvider(QString::fromUtf8(kSystemIconImageProviderId),
                             new SystemIconImageProvider());
    QByteArray qml{kPanelHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///AiModelsPanelHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
    }
    ProcessEvents(120);
  }

  auto item(const QString& name) -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(name) : nullptr;
  }
  auto card(const char* profile_id, const char* part = "") -> QObject* {
    return item(QStringLiteral("aiModelsClip%1:%2")
                    .arg(QString::fromLatin1(part), QString::fromLatin1(profile_id)));
  }
  auto panel() -> QObject* { return item(QStringLiteral("aiModelsSettingsPanel")); }
  auto downloads() -> QObject* { return item(QStringLiteral("clipDownloads")); }
  auto semantic() -> QObject* { return item(QStringLiteral("clipSemantic")); }

  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

auto BorderColor(QObject* card) -> QColor {
  QObject* border = card != nullptr ? card->property("border").value<QObject*>() : nullptr;
  return border != nullptr ? border->property("color").value<QColor>() : QColor();
}

auto BorderWidth(QObject* card) -> qreal {
  QObject* border = card != nullptr ? card->property("border").value<QObject*>() : nullptr;
  return border != nullptr ? border->property("width").toReal() : 0.0;
}

// ── Tests ───────────────────────────────────────────────────────────────────

TEST(AiModelsSettingsQmlTest, PageOpenChecksTheFeedOnceAndStartupDoesNotCheck) {
  AiModelsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();

  // Constructing the modules and opening Settings on another page makes no feed request.
  ProcessEvents(100);
  EXPECT_EQ(harness.feed_->manifest_requests, 0);
  harness.Open(0);
  EXPECT_EQ(harness.feed_->manifest_requests, 0);

  // The AI Models page checks the signed feed when it opens.
  harness.dialog()->setProperty("currentCategory", kAiModelsCategory);
  ASSERT_TRUE(harness.WaitForCheck()) << harness.packages()->last_error().toStdString();
  EXPECT_EQ(Text(harness.item(QStringLiteral("settingsPageTitle"))),
            QCoreApplication::translate("SettingDialog", "AI Models"));
  EXPECT_TRUE(harness.item(QStringLiteral("aiModelsSettingsScroll"))->property("visible").toBool());
  EXPECT_EQ(harness.feed_->manifest_requests, 1);
  EXPECT_EQ(harness.feed_->signature_requests, 1);

  // Leaving and showing the page again in the same opening does not check again.
  harness.dialog()->setProperty("currentCategory", 2);
  harness.dialog()->setProperty("currentCategory", kAiModelsCategory);
  ProcessEvents(100);
  EXPECT_EQ(harness.feed_->manifest_requests, 1);

  // A later opening directly on the page checks once more; nothing downloads.
  harness.Close();
  harness.Open(kAiModelsCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  EXPECT_EQ(harness.feed_->manifest_requests, 2);
  EXPECT_TRUE(harness.downloads_->started_urls.empty());

  // The AI Mask card says in one sentence what the set does (no version text; the revision
  // Pill shows the version), links its GitHub project, and lists the license names.
  const QString mask = QString::fromLatin1(kAiMaskId);
  EXPECT_EQ(Text(harness.item(QStringLiteral("aiModelsMaskSubtitle:") + mask)),
            PanelText("Selects the subject, people, sky, and the objects that you click, box, or "
                      "draw over."));
  QObject* mask_project = harness.item(QStringLiteral("aiModelsMaskUpstream:") + mask);
  ASSERT_NE(mask_project, nullptr);
  EXPECT_TRUE(mask_project->property("visible").toBool());
  EXPECT_EQ(
      harness.item(QStringLiteral("aiModelsMask:") + mask)->property("upstreamUrl").toString(),
      QStringLiteral("https://github.com/zidage/alcedo-ai-mask-lab"));
  EXPECT_EQ(Text(harness.item(QStringLiteral("aiModelsMaskLicense0:") + mask)),
            QStringLiteral("Apache-2.0"));
  EXPECT_EQ(Text(harness.item(QStringLiteral("aiModelsMaskLicense3:") + mask)),
            QStringLiteral("GPL-3.0"));
  EXPECT_EQ(Text(harness.item(QStringLiteral("aiModelsMaskAction:") + mask)),
            PanelText("Download"));

  // The AI Mask card has the column width and the near-square shape of the CLIP cards.
  const QString first_profile = harness.host_->model_download()
                                    ->Models()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("profileId"))
                                    .toString();
  QObject* mask_card = harness.item(QStringLiteral("aiModelsMask:") + mask);
  QObject* clip_card = harness.item(QStringLiteral("aiModelsClip:") + first_profile);
  ASSERT_NE(mask_card, nullptr);
  ASSERT_NE(clip_card, nullptr);
  const qreal mask_width = mask_card->property("width").toReal();
  EXPECT_GT(mask_width, 0.0);
  EXPECT_NEAR(mask_width, clip_card->property("width").toReal(), 1.0);
  EXPECT_GE(mask_card->property("height").toReal(), mask_width * 0.72 - 1.0);

  // Card titles use the section size of the type scale, not a display size, so a model name
  // stays on one line in a two-column page.
  for (QObject* title : {harness.item(QStringLiteral("aiModelsMaskName:") + mask),
                         harness.item(QStringLiteral("aiModelsClipName:") + first_profile)}) {
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->property("font").value<QFont>().pixelSize(),
              AppTheme::Instance().fontSizeSection());
  }
  // Each CLIP card has the one-sentence description of its publisher and, when the publisher
  // has one, its GitHub project.
  const std::map<QString, std::pair<const char*, QString>> clip_descriptions = {
      {QStringLiteral("mobileclip2-s2-en"),
       {"Fast image-text model from Apple, trained with multi-modal reinforced training.",
        QStringLiteral("https://github.com/apple/ml-mobileclip")}},
      {QStringLiteral("jina-clip-v2-int8-multilingual"),
       {"Multilingual embedding model for text and images from Jina AI, for 89 languages.",
        QString()}},
      {QStringLiteral("siglip2-b32-256-multilingual"),
       {"Multilingual vision-language encoder from Google that improves on SigLIP.",
        QStringLiteral("https://github.com/google-research/big_vision")}},
      {QStringLiteral("siglip2-base-256-coreml-macos"),
       {"Multilingual vision-language encoder from Google that improves on SigLIP.",
        QStringLiteral("https://github.com/google-research/big_vision")}}};
  const QVariantList clip_rows = harness.host_->model_download()->Models();
  ASSERT_FALSE(clip_rows.isEmpty());
  for (const QVariant& row : clip_rows) {
    const QString profile = row.toMap().value(QStringLiteral("profileId")).toString();
    ASSERT_TRUE(clip_descriptions.contains(profile)) << profile.toStdString();
    const auto& [description, project] = clip_descriptions.at(profile);
    EXPECT_EQ(Text(harness.item(QStringLiteral("aiModelsClipSubtitle:") + profile)),
              PanelText(description))
        << profile.toStdString();
    EXPECT_EQ(
        harness.item(QStringLiteral("aiModelsClip:") + profile)->property("upstreamUrl").toString(),
        project)
        << profile.toStdString();
  }
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(AiModelsSettingsQmlTest, FirstDownloadOpensTheStatementAndCancelStartsNothing) {
  AiModelsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open(kAiModelsCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  ASSERT_NE(harness.statement(), nullptr);
  EXPECT_FALSE(harness.statement()->property("visible").toBool());

  // AI Mask: the statement opens instead of the download; Cancel starts nothing.
  Press(harness.item(QStringLiteral("aiModelsMaskAction:") + QString::fromLatin1(kAiMaskId)));
  ASSERT_TRUE(WaitUntil([&] { return harness.statement()->property("visible").toBool(); }));
  EXPECT_TRUE(harness.statementButton("aiModelDownloadStatementAcceptButton")
                  ->property("visible")
                  .toBool());
  EXPECT_TRUE(harness.downloads_->started_urls.empty());
  Press(harness.statementButton("aiModelDownloadStatementCancelButton"));
  ASSERT_TRUE(WaitUntil([&] { return !harness.statement()->property("visible").toBool(); }));
  ProcessEvents(100);
  EXPECT_TRUE(harness.downloads_->started_urls.empty());
  EXPECT_EQ(*harness.statement_version_, 0u);
  EXPECT_FALSE(harness.packages()->download_statement_accepted());

  // CLIP: the same statement opens; Cancel starts no CLIP download.
  const QVariantList rows = harness.host_->model_download()->Models();
  ASSERT_FALSE(rows.isEmpty());
  const QString profile_id = rows.front().toMap().value(QStringLiteral("profileId")).toString();
  Press(harness.item(QStringLiteral("aiModelsClipAction:") + profile_id));
  ASSERT_TRUE(WaitUntil([&] { return harness.statement()->property("visible").toBool(); }));
  Press(harness.statementButton("aiModelDownloadStatementCancelButton"));
  ASSERT_TRUE(WaitUntil([&] { return !harness.statement()->property("visible").toBool(); }));
  ProcessEvents(100);
  EXPECT_FALSE(harness.host_->model_download()->ModelDownloadRunning());
  EXPECT_TRUE(harness.downloads_->started_urls.empty());
  EXPECT_EQ(*harness.statement_version_, 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(AiModelsSettingsQmlTest, AcceptStartsOnlyTheChosenDownloadAndTheStatementStaysAccepted) {
  AiModelsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open(kAiModelsCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  const QString mask = QString::fromLatin1(kAiMaskId);

  Press(harness.item(QStringLiteral("aiModelsMaskAction:") + mask));
  ASSERT_TRUE(WaitUntil([&] { return harness.statement()->property("visible").toBool(); }));
  Press(harness.statementButton("aiModelDownloadStatementAcceptButton"));
  ASSERT_TRUE(WaitUntil([&] { return !harness.statement()->property("visible").toBool(); }));
  ProcessEvents(100);

  // The acceptance is recorded with the statement version, and only the AI Mask set starts.
  EXPECT_EQ(*harness.statement_version_, alcedo::kAiModelDownloadStatementVersion);
  ASSERT_EQ(harness.downloads_->started_urls.size(), 1u);
  EXPECT_TRUE(harness.downloads_->started_urls.front().contains(
      QStringLiteral("alcedo-ai-mask-v1-r1-%1.7z").arg(QString::fromLatin1(kPlatform))));
  EXPECT_FALSE(harness.host_->model_download()->ModelDownloadRunning());
  EXPECT_TRUE(
      harness.item(QStringLiteral("aiModelsMaskCancel:") + mask)->property("visible").toBool());

  // Cancel the transfer; the next download does not ask again.
  Press(harness.item(QStringLiteral("aiModelsMaskCancel:") + mask));
  QObject* action = harness.item(QStringLiteral("aiModelsMaskAction:") + mask);
  ASSERT_TRUE(WaitUntil([&] { return action->property("visible").toBool(); }));
  Press(action);
  ProcessEvents(100);
  EXPECT_FALSE(harness.statement()->property("visible").toBool());
  EXPECT_EQ(harness.downloads_->started_urls.size(), 2u);

  // The AI Models page has no statement button; the About page shows the statement
  // read-only, with only Close.
  EXPECT_EQ(harness.item(QStringLiteral("aiModelsStatementButton")), nullptr);
  harness.dialog()->setProperty("currentCategory", kAboutCategory);
  ProcessEvents(100);
  QObject* about_link = harness.item(QStringLiteral("aboutAiModelStatementLink"));
  ASSERT_NE(about_link, nullptr);
  ASSERT_TRUE(about_link->property("visible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(about_link, "activated"));
  QObject* about_statement = harness.item(QStringLiteral("aboutAiModelStatementDialog"));
  ASSERT_NE(about_statement, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return about_statement->property("visible").toBool(); }));
  EXPECT_FALSE(
      about_statement->findChild<QObject*>(QStringLiteral("aiModelDownloadStatementAcceptButton"))
          ->property("visible")
          .toBool());
  Press(
      about_statement->findChild<QObject*>(QStringLiteral("aiModelDownloadStatementCloseButton")));
  ASSERT_TRUE(WaitUntil([&] { return !about_statement->property("visible").toBool(); }));
  EXPECT_EQ(harness.downloads_->started_urls.size(), 2u);
}

TEST(AiModelsSettingsQmlTest, ActiveClipCardHasTheActiveBorderAndActivateStartsActivation) {
  AiModelsPanelHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  const AppTheme& theme     = AppTheme::Instance();

  QObject*        active    = harness.card("clip-active");
  QObject*        installed = harness.card("clip-installed");
  QObject*        missing   = harness.card("clip-missing");
  ASSERT_NE(active, nullptr);
  ASSERT_NE(installed, nullptr);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(BorderColor(active), theme.activeModelBorderColor());
  EXPECT_EQ(BorderWidth(active), 2.0);
  EXPECT_EQ(BorderColor(installed), theme.cardBorderColor());
  EXPECT_EQ(BorderColor(missing), theme.cardBorderColor());
  EXPECT_EQ(Text(harness.card("clip-active", "Status")), PanelText("Active in this project"));
  EXPECT_FALSE(harness.card("clip-active", "Action")->property("visible").toBool());
  EXPECT_EQ(Text(harness.card("clip-missing", "Action")), PanelText("Download"));
  EXPECT_EQ(Text(harness.card("clip-installed", "License0")), QStringLiteral("apple-amlr"));

  // The near-square LUT card shape: height at least 0.72 of the width.
  const qreal width  = active->property("width").toReal();
  const qreal height = active->property("height").toReal();
  EXPECT_GT(width, 0.0);
  EXPECT_GE(height, width * 0.72 - 1.0);

  QObject* activate = harness.card("clip-installed", "Action");
  EXPECT_EQ(Text(activate), PanelText("Activate"));
  Press(activate);
  EXPECT_EQ(harness.semantic()->property("activateCalls").toList(),
            QVariantList{QStringLiteral("clip-installed")});

  // The active model of the project follows the controller.
  harness.semantic()->setProperty("activeModelProfileId", QStringLiteral("clip-installed"));
  ProcessEvents();
  EXPECT_EQ(BorderColor(installed), theme.activeModelBorderColor());
  EXPECT_EQ(BorderColor(active), theme.cardBorderColor());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(AiModelsSettingsQmlTest, ActivateIsDisabledWithoutAnOpenProject) {
  AiModelsPanelHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.panel()->setProperty("projectOpen", false);
  ProcessEvents();

  QObject* activate = harness.card("clip-installed", "Action");
  ASSERT_NE(activate, nullptr);
  EXPECT_TRUE(activate->property("visible").toBool());
  EXPECT_FALSE(activate->property("enabled").toBool());
  EXPECT_EQ(Text(harness.card("clip-installed", "Message")),
            PanelText("Open a project to activate this model."));
  // Without a project no card shows an active model.
  EXPECT_EQ(BorderColor(harness.card("clip-active")), AppTheme::Instance().cardBorderColor());
  // Download does not need a project.
  EXPECT_TRUE(harness.card("clip-missing", "Action")->property("enabled").toBool());
  EXPECT_TRUE(harness.semantic()->property("activateCalls").toList().isEmpty());
}

TEST(AiModelsSettingsQmlTest, RemoveAsksBeforeTheModelIsRemoved) {
  AiModelsPanelHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();

  QObject* remove  = harness.card("clip-active", "Remove");
  QObject* confirm = harness.card("clip-active", "RemoveConfirm");
  ASSERT_NE(remove, nullptr);
  ASSERT_NE(confirm, nullptr);
  EXPECT_FALSE(confirm->property("visible").toBool());
  // Not installed: nothing to remove.
  EXPECT_FALSE(harness.card("clip-missing", "Remove")->property("visible").toBool());

  // Remove asks first and names the active-model case; Keep changes nothing.
  Press(remove);
  EXPECT_TRUE(confirm->property("visible").toBool());
  QObject* warning = harness.card("clip-active", "RemoveWarning");
  EXPECT_TRUE(warning->property("visible").toBool());
  EXPECT_EQ(Text(warning), PanelText("It is the active model of this project. After removal, "
                                     "the project has no active model."));
  EXPECT_TRUE(harness.downloads()->property("removeCalls").toList().isEmpty());
  Press(harness.card("clip-active", "RemoveKeep"));
  EXPECT_FALSE(confirm->property("visible").toBool());
  EXPECT_TRUE(harness.downloads()->property("removeCalls").toList().isEmpty());

  // Confirmed, the controller removes that model only.
  Press(remove);
  Press(harness.card("clip-active", "RemoveConfirmButton"));
  EXPECT_EQ(harness.downloads()->property("removeCalls").toList(),
            QVariantList{QStringLiteral("clip-active")});
  EXPECT_FALSE(confirm->property("visible").toBool());

  // A model that is not active asks without the active-model warning.
  Press(harness.card("clip-installed", "Remove"));
  EXPECT_FALSE(harness.card("clip-installed", "RemoveWarning")->property("visible").toBool());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
