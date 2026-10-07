//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > LUTs on the production SettingDialog and ApplicationModuleHost
// (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md L6B).
//
// The host's LUT services use a temporary library root, an in-memory signed feed
// that counts requests, and a downloader that records starts and holds transfers.
// Buttons are activated through their `clicked` signal: offscreen pointer delivery
// into the dialog's scrolled page is not reliable (AGENTS.md), and the page logic
// under test is in those handlers.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <ed25519.h>
}

#include "app/lut_library_inventory.hpp"
#include "app/lut_library_service.hpp"
#include "app/lut_package_service.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs                 = std::filesystem;

constexpr int kLutCategory   = 8;
const char*   kSpectralId    = "spectral_film_lut";
const char*   kSpektrafilmId = "spektrafilm_lut";
const QUrl    kFeedUrl(QStringLiteral("https://static.aoraw.org/luts/v1/manifest.json"));

void          ProcessEvents(int milliseconds = 40) {
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
  return QCoreApplication::translate("LutSettingsPanel", source);
}

// ── Signed feed ─────────────────────────────────────────────────────────────

struct TestKey {
  QByteArray                    public_key;
  std::array<unsigned char, 64> private_key{};
};

auto MakeKey() -> TestKey {
  unsigned char seed[32] = {};
  for (std::size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index * 5 + 2);
  }
  TestKey       key;
  unsigned char public_key[32] = {};
  ed25519_create_keypair(public_key, key.private_key.data(), seed);
  key.public_key = QByteArray(reinterpret_cast<const char*>(public_key), sizeof(public_key));
  return key;
}

auto PackageJson(const QString& id, const QString& name) -> QString {
  return QStringLiteral(
             R"({"id":"%1","name":"%2","revision":"r1","file_count":2,"inventory_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","unpacked_bytes":4096,"artifact":{"url":"https://static.aoraw.org/luts/v1/packages/%1/r1/%1-r1.7z","size":3145728,"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"}})")
      .arg(id, name);
}

struct FeedServer {
  QByteArray json;
  QByteArray signature;
  int        manifest_requests  = 0;
  int        signature_requests = 0;
};

void Sign(FeedServer& feed, const TestKey& key) {
  feed.json =
      QStringLiteral(
          R"({"schema":1,"kind":"alcedo-lut-packages","sequence":%1,"packages":[%2,%3]})")
          .arg(20260930000000ULL)
          .arg(PackageJson(QString::fromLatin1(kSpectralId), QStringLiteral("Spectral Film LUT")),
               PackageJson(QString::fromLatin1(kSpektrafilmId), QStringLiteral("Spektrafilm LUT")))
          .toUtf8();
  unsigned char signature[64] = {};
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(feed.json.constData()),
               static_cast<std::size_t>(feed.json.size()),
               reinterpret_cast<const unsigned char*>(key.public_key.constData()),
               key.private_key.data());
  feed.signature = QByteArray(reinterpret_cast<const char*>(signature), 64).toBase64();
}

// ── Transport and preference doubles ────────────────────────────────────────

/// Records every transfer and holds it until Cancel().
class HeldArchiveDownloader final : public alcedo::LutArchiveDownloader {
 public:
  struct State {
    std::vector<QString> started_urls;
    std::vector<QString> canceled_ids;
    FinishedCallback     pending;
  };
  explicit HeldArchiveDownloader(std::shared_ptr<State> state) : state_(std::move(state)) {}

  auto Start(const alcedo::DownloadRequest& request, ProgressCallback on_progress,
             FinishedCallback on_finished) -> bool override {
    if (state_->pending) return false;
    state_->started_urls.push_back(request.items.front().url.toString());
    state_->pending = std::move(on_finished);
    if (on_progress) on_progress(1048576, request.items.front().expected_size);
    return true;
  }
  void Cancel(const QString& request_id) override {
    state_->canceled_ids.push_back(request_id);
    if (!state_->pending) return;
    FinishedCallback finished = std::move(state_->pending);
    QTimer::singleShot(0, [finished] { finished(false, true, QStringLiteral("canceled")); });
  }

 private:
  std::shared_ptr<State> state_;
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

class MemoryPackagePreferences final : public alcedo::LutPackagePreferences {
 public:
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return 1; }
  void               SaveTrustedSequence(quint64) override {}
};

// ── Harness ─────────────────────────────────────────────────────────────────

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

class LutSettingsHarness {
 public:
  explicit LutSettingsHarness(bool open_url_result = true) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LutSettingsHarness"));
    base_ = fs::path(base_dir_.path().toStdWString());
    fs::create_directories(base_ / "library");
    base_ = fs::weakly_canonical(base_);
    root_ = base_ / "library";
    key_  = MakeKey();
    Sign(*feed_, key_);

    ApplicationModuleHost::LutServiceFactories factories;
    factories.library_options = [this, open_url_result] {
      alcedo::LutLibraryServiceOptions options;
      options.preferences       = std::make_unique<MemoryLibraryPreferences>(root_);
      options.default_root      = root_;
      options.scan_worker_count = 1;
      options.open_url          = [this, open_url_result](const QUrl& url) {
        opened_urls_.push_back(url);
        return open_url_result;
      };
      return options;
    };
    factories.package_options = [this] {
      alcedo::LutPackageServiceOptions options;
      options.feed_url   = kFeedUrl;
      options.public_key = key_.public_key;
      options.fetch      = [feed = feed_](const QUrl&                              url, qint64,
                                     std::function<void(QByteArray, QString)> done) {
        const bool signature = url.path().endsWith(QStringLiteral(".sig"));
        ++(signature ? feed->signature_requests : feed->manifest_requests);
        QTimer::singleShot(
            0, [feed, signature, done] { done(signature ? feed->signature : feed->json, {}); });
      };
      options.downloader  = std::make_unique<HeldArchiveDownloader>(downloads_);
      options.preferences = std::make_unique<MemoryPackagePreferences>();
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

    // The library loads its (empty) root without network access.
    WaitUntil([this] { return !host_->lut_library()->busy(); });

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///LutSettingsHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
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
  void Open(int category) {
    dialog()->setProperty("requestedCategory", category);
    QMetaObject::invokeMethod(dialog(), "open");
    ProcessEvents(150);
  }
  void Close() {
    QMetaObject::invokeMethod(dialog(), "close");
    ProcessEvents(150);
  }
  auto packages() -> alcedo::LutPackageService* { return host_->lut_packages(); }
  auto library() -> alcedo::LutLibraryService* { return host_->lut_library(); }
  auto WaitForCheck() -> bool {
    return WaitUntil([this] { return packages()->checked() && !packages()->checking(); });
  }
  static void Press(QQuickItem* button) {
    ASSERT_NE(button, nullptr);
    ASSERT_TRUE(button->isVisible());
    ASSERT_TRUE(button->isEnabled());
    ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
    ProcessEvents();
  }

  QTemporaryDir                                 settings_dir_;
  QTemporaryDir                                 base_dir_;
  fs::path                                      base_;
  fs::path                                      root_;
  TestKey                                       key_;
  std::shared_ptr<FeedServer>                   feed_ = std::make_shared<FeedServer>();
  std::shared_ptr<HeldArchiveDownloader::State> downloads_ =
      std::make_shared<HeldArchiveDownloader::State>();
  std::vector<QUrl>                      opened_urls_;
  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<LanguageManager>       language_manager_;
  QQmlApplicationEngine                  engine_;
  QQuickWindow*                          window_ = nullptr;
  QStringList                            warnings_;
};

auto Text(QQuickItem* item) -> QString {
  return item != nullptr ? item->property("text").toString() : QString();
}

// ── Tests ───────────────────────────────────────────────────────────────────

TEST(LutSettingsQmlTest, SettingsOpenChecksOnceAndStartupDoesNotCheck) {
  LutSettingsHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();

  // Constructing the modules and loading the dialog makes no feed request.
  ProcessEvents(100);
  EXPECT_EQ(harness.feed_->manifest_requests, 0);
  EXPECT_FALSE(harness.packages()->checked());

  harness.Open(0);
  ASSERT_TRUE(harness.WaitForCheck());
  EXPECT_EQ(harness.feed_->manifest_requests, 1);
  EXPECT_EQ(harness.feed_->signature_requests, 1);

  // Changing pages, including the LUT page, does not check again.
  harness.dialog()->setProperty("currentCategory", kLutCategory);
  ProcessEvents(100);
  EXPECT_EQ(Text(harness.item(QStringLiteral("settingsPageTitle"))),
            QCoreApplication::translate("SettingDialog", "LUTs"));
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsScroll"))->isVisible());
  harness.dialog()->setProperty("currentCategory", 2);
  harness.dialog()->setProperty("currentCategory", kLutCategory);
  ProcessEvents(100);
  EXPECT_EQ(harness.feed_->manifest_requests, 1);
  EXPECT_TRUE(harness.downloads_->started_urls.empty());

  // Each later opening transition checks once more.
  harness.Close();
  EXPECT_EQ(harness.feed_->manifest_requests, 1);
  harness.Open(kLutCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  EXPECT_EQ(harness.feed_->manifest_requests, 2);
  EXPECT_TRUE(harness.downloads_->started_urls.empty());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(LutSettingsQmlTest, PackageRowsStartOnlyTheChosenDownloadAndOfferCancelAndRetry) {
  LutSettingsHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open(kLutCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  ProcessEvents(100);

  const QString spectral    = QString::fromLatin1(kSpectralId);
  const QString spektrafilm = QString::fromLatin1(kSpektrafilmId);
  ASSERT_NE(harness.item(QStringLiteral("lutSettingsPackage:") + spectral), nullptr);
  ASSERT_NE(harness.item(QStringLiteral("lutSettingsPackage:") + spektrafilm), nullptr);
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsPackageStatus:") + spektrafilm)),
            PanelText("Not installed"));
  QQuickItem* action = harness.item(QStringLiteral("lutSettingsPackageAction:") + spektrafilm);
  EXPECT_EQ(Text(action), PanelText("Download"));
  EXPECT_FALSE(
      harness.item(QStringLiteral("lutSettingsPackageCancel:") + spektrafilm)->isVisible());

  LutSettingsHarness::Press(action);
  ASSERT_EQ(harness.downloads_->started_urls.size(), 1u);
  EXPECT_TRUE(harness.downloads_->started_urls.front().contains(spektrafilm));
  // The same row object stays; it now offers Cancel and no second action.
  EXPECT_EQ(harness.item(QStringLiteral("lutSettingsPackageAction:") + spektrafilm), action);
  EXPECT_FALSE(action->isVisible());
  QQuickItem* cancel = harness.item(QStringLiteral("lutSettingsPackageCancel:") + spektrafilm);
  EXPECT_TRUE(cancel->isVisible());
  // 1 MiB of 3 MiB reported by the held transfer.
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsPackageStatus:") + spektrafilm)),
            PanelText("Downloading… %1%").arg(33));
  // The other package keeps its own action and is not started.
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsPackageAction:") + spectral)->isVisible());
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsPackageCancel:") + spectral)->isVisible());

  LutSettingsHarness::Press(cancel);
  ASSERT_TRUE(WaitUntil([&] { return !cancel->isVisible(); }));
  EXPECT_EQ(harness.downloads_->canceled_ids.size(), 1u);
  EXPECT_EQ(Text(action), PanelText("Retry"));
  EXPECT_TRUE(action->isVisible());
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsPackageError:") + spektrafilm)->isVisible());
  EXPECT_TRUE(harness.library()->PackageReceipts().empty());

  // Retry starts the same package again.
  LutSettingsHarness::Press(action);
  ASSERT_EQ(harness.downloads_->started_urls.size(), 2u);
  EXPECT_EQ(harness.downloads_->started_urls.back(), harness.downloads_->started_urls.front());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(LutSettingsQmlTest, InstalledPackageIsRemovedOnlyAfterConfirmation) {
  LutSettingsHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  // An installed package: its receipt names one content directory with one LUT.
  const std::string content = std::string("packages/") + kSpectralId + "/content/r1";
  {
    fs::create_directories(harness.root_ / content);
    std::ofstream cube(harness.root_ / content / "look.cube", std::ios::binary);
    cube << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  }
  alcedo::LutPackageReceipt receipt;
  receipt.package_id        = kSpectralId;
  receipt.content_directory = content;
  receipt.revision          = "r1";
  ASSERT_TRUE(alcedo::WriteLutPackageReceiptFile(harness.root_, receipt).empty());
  ASSERT_TRUE(harness.library()->refresh());
  ASSERT_TRUE(WaitUntil([&] {
    return !harness.library()->busy() && harness.library()->PackageReceipts().size() == 1;
  }));
  ASSERT_EQ(harness.library()->entry_count(), 1);

  harness.Open(kLutCategory);
  ASSERT_TRUE(harness.WaitForCheck());
  ProcessEvents(100);
  const QString spectral    = QString::fromLatin1(kSpectralId);
  const QString spektrafilm = QString::fromLatin1(kSpektrafilmId);
  // Only the installed package offers Remove.
  EXPECT_FALSE(
      harness.item(QStringLiteral("lutSettingsPackageRemove:") + spektrafilm)->isVisible());
  QQuickItem* remove  = harness.item(QStringLiteral("lutSettingsPackageRemove:") + spectral);
  QQuickItem* confirm = harness.item(QStringLiteral("lutSettingsPackageRemoveConfirm:") + spectral);
  ASSERT_NE(confirm, nullptr);
  EXPECT_FALSE(confirm->isVisible());

  // Remove is the shared trash glyph.
  ASSERT_NE(remove, nullptr);
  EXPECT_EQ(remove->property("iconSrc").toString(), QStringLiteral("qrc:/panel_icons/trash.svg"));

  // Remove asks first and warns that photos lose the look; Keep changes nothing.
  LutSettingsHarness::Press(remove);
  EXPECT_TRUE(confirm->isVisible());
  QQuickItem* warning =
      harness.item(QStringLiteral("lutSettingsPackageRemoveWarning:") + spectral);
  ASSERT_NE(warning, nullptr);
  EXPECT_TRUE(warning->isVisible());
  EXPECT_EQ(Text(warning), PanelText("Photos that have these LUTs applied lose that look until the "
                                     "package is installed again."));
  EXPECT_EQ(harness.library()->PackageReceipts().size(), 1u);
  LutSettingsHarness::Press(
      harness.item(QStringLiteral("lutSettingsPackageRemoveKeep:") + spectral));
  EXPECT_FALSE(confirm->isVisible());
  EXPECT_EQ(harness.library()->PackageReceipts().size(), 1u);

  // Confirmed, the package leaves the library and the disk; the card offers the download.
  LutSettingsHarness::Press(remove);
  LutSettingsHarness::Press(
      harness.item(QStringLiteral("lutSettingsPackageRemoveConfirmButton:") + spectral));
  ASSERT_TRUE(WaitUntil(
      [&] { return !harness.library()->busy() && harness.library()->PackageReceipts().empty(); }));
  ProcessEvents(100);
  EXPECT_EQ(harness.library()->entry_count(), 0);
  EXPECT_FALSE(fs::exists(harness.root_ / "packages" / kSpectralId))
      << harness.library()->last_error().toStdString();
  EXPECT_FALSE(confirm->isVisible());
  EXPECT_FALSE(remove->isVisible());
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsPackageAction:") + spectral)),
            PanelText("Download"));
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(LutSettingsQmlTest, FolderOpenFailureAndFolderChoiceAreVisibleBeforeAnyChange) {
  LutSettingsHarness harness(/*open_url_result=*/false);
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open(kLutCategory);
  ASSERT_TRUE(harness.WaitForCheck());

  QQuickItem* error = harness.item(QStringLiteral("lutSettingsLibraryError"));
  ASSERT_NE(error, nullptr);
  EXPECT_FALSE(error->isVisible());
  LutSettingsHarness::Press(harness.item(QStringLiteral("lutSettingsOpenFolderButton")));
  EXPECT_EQ(harness.opened_urls_.size(), 1u);
  EXPECT_TRUE(error->isVisible());
  EXPECT_TRUE(Text(error).contains(harness.library()->root_path()));

  // Choosing the current root is refused before anything starts.
  QObject*      panel    = harness.item(QStringLiteral("lutSettingsPanel"));
  const QString root_url = alcedo::LutLibraryDirectoryUrl(harness.root_).toString();
  ASSERT_TRUE(QMetaObject::invokeMethod(panel, "reviewFolder", Q_ARG(QVariant, root_url)));
  ProcessEvents();
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsFolderConfirmation"))->isVisible());
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsPendingError"))->isVisible());
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsConfirmFolderButton"))->isVisible());
  LutSettingsHarness::Press(harness.item(QStringLiteral("lutSettingsCancelFolderButton")));
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsFolderConfirmation"))->isVisible());

  // A new folder shows its path and moves the library only after confirmation.
  const fs::path destination = harness.base_ / "moved library";
  const QString  destination_url =
      QUrl::fromLocalFile(QString::fromStdWString(destination.wstring())).toString();
  ASSERT_TRUE(QMetaObject::invokeMethod(panel, "reviewFolder", Q_ARG(QVariant, destination_url)));
  ProcessEvents();
  EXPECT_TRUE(panel->property("pendingMigrate").toBool());
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsConfirmFolderButton"))),
            PanelText("Move library"));
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsPendingError"))->isVisible());
  EXPECT_TRUE(Text(harness.item(QStringLiteral("lutSettingsPendingPath")))
                  .contains(QStringLiteral("moved library")));
  EXPECT_FALSE(fs::exists(destination));
  EXPECT_EQ(harness.library()->Root(), harness.root_);

  LutSettingsHarness::Press(harness.item(QStringLiteral("lutSettingsConfirmFolderButton")));
  ASSERT_TRUE(WaitUntil([&] { return !harness.library()->busy(); }));
  EXPECT_EQ(harness.library()->Root(), destination);
  EXPECT_TRUE(Text(harness.item(QStringLiteral("lutSettingsRootPath")))
                  .contains(QStringLiteral("moved library")));
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsFolderConfirmation"))->isVisible());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(LutSettingsQmlTest, ChangeFolderSwitchesToAFolderThatAlreadyHoldsFiles) {
  LutSettingsHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open(kLutCategory);
  ASSERT_TRUE(harness.WaitForCheck());

  // One "Change folder" button replaces the separate use/move buttons.
  EXPECT_NE(harness.item(QStringLiteral("lutSettingsChangeFolderButton")), nullptr);
  EXPECT_EQ(harness.item(QStringLiteral("lutSettingsUseFolderButton")), nullptr);
  EXPECT_EQ(harness.item(QStringLiteral("lutSettingsMoveButton")), nullptr);
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsTotalCount"))), QStringLiteral("0"));

  // A folder that holds files becomes the library in place; nothing is moved into it.
  const fs::path existing = harness.base_ / "existing luts";
  fs::create_directories(existing);
  std::ofstream(existing / "notes.txt") << "kept";
  QObject*      panel = harness.item(QStringLiteral("lutSettingsPanel"));
  const QString url = QUrl::fromLocalFile(QString::fromStdWString(existing.wstring())).toString();
  ASSERT_TRUE(QMetaObject::invokeMethod(panel, "reviewFolder", Q_ARG(QVariant, url)));
  ProcessEvents();
  EXPECT_FALSE(panel->property("pendingMigrate").toBool());
  EXPECT_FALSE(harness.item(QStringLiteral("lutSettingsPendingError"))->isVisible());
  EXPECT_TRUE(harness.item(QStringLiteral("lutSettingsPendingDescription"))->isVisible());
  EXPECT_EQ(Text(harness.item(QStringLiteral("lutSettingsConfirmFolderButton"))),
            PanelText("Use folder"));

  LutSettingsHarness::Press(harness.item(QStringLiteral("lutSettingsConfirmFolderButton")));
  ASSERT_TRUE(WaitUntil([&] { return !harness.library()->busy(); }));
  EXPECT_EQ(harness.library()->Root(), existing);
  EXPECT_TRUE(fs::exists(harness.root_));
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
