//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Saved AI Keys settings page and the launch reminder of issue #710. Advanced Content
// Analysis is removed; the page lists the stored provider profiles and deletes their saved
// credentials, and the shell reminds the user once per launch while a credential is saved. The
// tests drive the production QML files through a fake profile controller whose properties,
// signals, and methods exist on AiProviderProfileController.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QEventLoop>
#include <QList>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

#include "app/ai_provider_profile.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

constexpr auto kApiKeyProfileId = "11111111-1111-1111-1111-111111111111";
constexpr auto kCodexProfileId  = "22222222-2222-2222-2222-222222222222";

// AiProviderProfileController: the page reads `profiles` and `hasSavedCredentials`, and calls
// DeleteApiKey, DeleteProfile, and DeleteAllApiKeys.
class FakeProfileController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList profiles READ profiles NOTIFY ProfilesChanged)
  Q_PROPERTY(bool hasProfiles READ hasProfiles NOTIFY ProfilesChanged)
  Q_PROPERTY(bool hasSavedCredentials READ hasSavedCredentials NOTIFY ProfilesChanged)

 public:
  FakeProfileController() {
    profiles_ = {
        Row(QString::fromLatin1(kApiKeyProfileId), QStringLiteral("OpenCode"),
            QStringLiteral("https://opencode.ai/zen/go/v1"), false, QStringLiteral("****3456")),
        Row(QString::fromLatin1(kCodexProfileId), QStringLiteral("OpenAI Codex OAuth"),
            QStringLiteral("https://chatgpt.com/backend-api/codex"), true,
            QStringLiteral("Codex OAuth ****abcdef")),
    };
  }

  [[nodiscard]] auto profiles() const -> QVariantList { return profiles_; }
  [[nodiscard]] auto hasProfiles() const -> bool { return !profiles_.isEmpty(); }
  [[nodiscard]] auto hasSavedCredentials() const -> bool {
    for (const auto& row : profiles_) {
      if (row.toMap().value(QStringLiteral("credentialAvailable")).toBool()) {
        return true;
      }
    }
    return false;
  }

  void ClearCredentials() {
    for (auto& row : profiles_) {
      QVariantMap map = row.toMap();
      map.insert(QStringLiteral("credentialAvailable"), false);
      map.insert(QStringLiteral("maskedKeyLabel"), QString{});
      row = map;
    }
    emit ProfilesChanged();
  }

  Q_INVOKABLE QVariantMap Profile(const QString& profile_id) const {
    for (const auto& row : profiles_) {
      if (row.toMap().value(QStringLiteral("uuid")).toString() == profile_id) {
        return row.toMap();
      }
    }
    return {};
  }
  Q_INVOKABLE void DeleteApiKey(const QString& profile_id) {
    deleted_key_ids_.push_back(profile_id);
  }
  Q_INVOKABLE bool DeleteProfile(const QString& profile_id, bool delete_credential) {
    deleted_profile_ids_.push_back(profile_id);
    last_delete_credential_ = delete_credential;
    return true;
  }
  Q_INVOKABLE int DeleteAllApiKeys() {
    ++delete_all_calls_;
    ClearCredentials();
    return 2;
  }

  QStringList deleted_key_ids_;
  QStringList deleted_profile_ids_;
  bool        last_delete_credential_ = false;
  int         delete_all_calls_       = 0;

 signals:
  void ProfilesChanged();

 private:
  static auto Row(const QString& uuid, const QString& name, const QString& base_url,
                  bool codex_oauth, const QString& masked_label) -> QVariantMap {
    return {{QStringLiteral("uuid"), uuid},
            {QStringLiteral("displayName"), name},
            {QStringLiteral("baseUrl"), base_url},
            {QStringLiteral("isCodexOAuth"), codex_oauth},
            {QStringLiteral("maskedKeyLabel"), masked_label},
            {QStringLiteral("credentialAvailable"), true}};
  }

  QVariantList profiles_;
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds = 30) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitFor(const std::function<bool()>& done, int timeout_ms = 2000) -> bool {
  for (int waited = 0; waited < timeout_ms; waited += 20) {
    if (done()) {
      return true;
    }
    ProcessEvents(20);
  }
  return done();
}

// Every item below @p root in the visual tree, @p root included. ListView delegates and the
// content of an open popup are in the visual tree, not in the QObject tree.
void CollectItems(QQuickItem* root, QList<QQuickItem*>* items) {
  if (root == nullptr) {
    return;
  }
  items->push_back(root);
  for (QQuickItem* child : root->childItems()) {
    CollectItems(child, items);
  }
}

auto FindItem(QQuickItem* root, const QString& object_name) -> QQuickItem* {
  QList<QQuickItem*> items;
  CollectItems(root, &items);
  for (QQuickItem* item : items) {
    if (item->objectName() == object_name) {
      return item;
    }
  }
  return nullptr;
}

constexpr char kPanelWindowQml[] = R"(
import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: root
    width: 900
    height: 700
    visible: true
    property var panel: null
    property string createError: ""

    Component.onCompleted: {
        var component = Qt.createComponent(panelComponentUrl)
        if (component.status === Component.Error) {
            root.createError = component.errorString()
            return
        }
        root.panel = component.createObject(root.contentItem, {
            "profileController": fakeProfiles,
            "width": root.width,
            "height": root.height
        })
        if (!root.panel) {
            root.createError = component.errorString()
        }
    }
}
)";

class SavedAiKeysPanelHarness {
 public:
  SavedAiKeysPanelHarness() {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("fakeProfiles"), &profiles_);
    engine_.rootContext()->setContextProperty(
        QStringLiteral("panelComponentUrl"),
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/AiProviderSettingsPanel.qml")));
    engine_.loadData(QByteArray{kPanelWindowQml},
                     QUrl(QStringLiteral("file:///SavedAiKeysPanelWindow.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
    }
    ProcessEvents(100);
  }

  [[nodiscard]] auto panel() const -> QQuickItem* {
    return window_ ? window_->property("panel").value<QQuickItem*>() : nullptr;
  }
  [[nodiscard]] auto createError() const -> std::string {
    return (window_ ? window_->property("createError").toString() : QStringLiteral("no window"))
               .toStdString() +
           "\n" + warnings_.join('\n').toStdString();
  }
  // The root of the whole window scene, so open popups (in the overlay) are included.
  [[nodiscard]] auto sceneRoot() const -> QQuickItem* {
    QQuickItem* item = window_ ? window_->contentItem() : nullptr;
    while (item != nullptr && item->parentItem() != nullptr) {
      item = item->parentItem();
    }
    return item;
  }

  FakeProfileController profiles_;
  QQmlApplicationEngine engine_;
  QStringList           warnings_;
  QQuickWindow*         window_ = nullptr;
};

// Every signal and property of @p fake, and every invokable method that the page calls, must exist
// on @p production with the same signature.
void ExpectFakeMatchesProductionClass(const QMetaObject& fake, const QMetaObject& production) {
  for (int i = QObject::staticMetaObject.methodCount(); i < fake.methodCount(); ++i) {
    const QMetaMethod method = fake.method(i);
    if (method.methodType() == QMetaMethod::Signal ||
        method.methodType() == QMetaMethod::Method) {
      EXPECT_GE(production.indexOfMethod(method.methodSignature().constData()), 0)
          << production.className() << " has no method " << method.methodSignature().constData();
    }
  }
  for (int i = QObject::staticMetaObject.propertyCount(); i < fake.propertyCount(); ++i) {
    EXPECT_GE(production.indexOfProperty(fake.property(i).name()), 0)
        << production.className() << " has no property " << fake.property(i).name();
  }
}

TEST(SavedAiKeysPanelQmlTest, FakeControllerUsesProductionNames) {
  ExpectFakeMatchesProductionClass(FakeProfileController::staticMetaObject,
                                   alcedo::AiProviderProfileController::staticMetaObject);
}

TEST(SavedAiKeysPanelQmlTest, PageShowsRemovalNoticeAndNoAnalysisSettings) {
  SavedAiKeysPanelHarness harness;
  QQuickItem*             panel = harness.panel();
  ASSERT_NE(panel, nullptr) << harness.createError();

  QQuickItem* notice = FindItem(panel, QStringLiteral("savedAiKeysNotice"));
  ASSERT_NE(notice, nullptr);
  EXPECT_TRUE(notice->isVisible());
  EXPECT_TRUE(notice->property("text").toString().startsWith(
      QStringLiteral("Advanced Content Analysis is removed from this version.")));
  QQuickItem* empty_label = FindItem(panel, QStringLiteral("savedAiKeysEmptyLabel"));
  ASSERT_NE(empty_label, nullptr);
  EXPECT_FALSE(empty_label->isVisible());

  QList<QQuickItem*> items;
  CollectItems(panel, &items);
  for (QQuickItem* item : items) {
    EXPECT_FALSE(item->inherits("QQuickComboBox")) << "model box " << item->objectName().toStdString();
    EXPECT_FALSE(item->inherits("QQuickTextField"))
        << "text field " << item->objectName().toStdString();
    EXPECT_NE(item->property("text").toString(), QStringLiteral("Add provider"));
    EXPECT_NE(item->property("tooltipText").toString(), QStringLiteral("Add provider"));
  }
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(SavedAiKeysPanelQmlTest, DeleteKeyAndDisconnectDeleteTheCredentialOfTheirProfile) {
  SavedAiKeysPanelHarness harness;
  QQuickItem*             panel = harness.panel();
  ASSERT_NE(panel, nullptr) << harness.createError();

  QQuickItem* api_key_row = nullptr;
  QQuickItem* codex_row   = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    api_key_row =
        FindItem(panel, QStringLiteral("savedAiKeyRow_") + QString::fromLatin1(kApiKeyProfileId));
    codex_row =
        FindItem(panel, QStringLiteral("savedAiKeyRow_") + QString::fromLatin1(kCodexProfileId));
    return api_key_row != nullptr && codex_row != nullptr;
  }));

  QQuickItem* delete_key = FindItem(api_key_row, QStringLiteral("deleteKeyButton"));
  QQuickItem* disconnect = FindItem(codex_row, QStringLiteral("deleteKeyButton"));
  ASSERT_NE(delete_key, nullptr);
  ASSERT_NE(disconnect, nullptr);
  EXPECT_EQ(delete_key->property("text").toString(), QStringLiteral("Delete Key"));
  EXPECT_EQ(disconnect->property("text").toString(), QStringLiteral("Disconnect"));
  EXPECT_TRUE(delete_key->isEnabled());
  EXPECT_TRUE(disconnect->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(delete_key, "clicked"));
  ASSERT_TRUE(QMetaObject::invokeMethod(disconnect, "clicked"));
  ProcessEvents();

  EXPECT_EQ(harness.profiles_.deleted_key_ids_,
            QStringList({QString::fromLatin1(kApiKeyProfileId),
                         QString::fromLatin1(kCodexProfileId)}));
  EXPECT_TRUE(harness.profiles_.deleted_profile_ids_.isEmpty());
}

TEST(SavedAiKeysPanelQmlTest, DeleteAllSavedKeysAfterConfirmShowsNoSavedKeys) {
  SavedAiKeysPanelHarness harness;
  QQuickItem*             panel = harness.panel();
  ASSERT_NE(panel, nullptr) << harness.createError();

  QQuickItem* delete_all = FindItem(panel, QStringLiteral("deleteAllSavedKeysButton"));
  ASSERT_NE(delete_all, nullptr);
  EXPECT_TRUE(delete_all->isEnabled());
  auto* dialog = panel->findChild<QObject*>(QStringLiteral("deleteAllSavedKeysDialog"));
  ASSERT_NE(dialog, nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(delete_all, "clicked"));
  ASSERT_TRUE(WaitFor([&] { return dialog->property("opened").toBool(); }));
  // The click only asks for confirmation.
  EXPECT_EQ(harness.profiles_.delete_all_calls_, 0);

  auto* dialog_content = dialog->property("contentItem").value<QQuickItem*>();
  ASSERT_NE(dialog_content, nullptr);
  QQuickItem* confirm = FindItem(dialog_content, QStringLiteral("confirmDeleteButton"));
  ASSERT_NE(confirm, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(confirm, "clicked"));
  ProcessEvents();

  EXPECT_EQ(harness.profiles_.delete_all_calls_, 1);
  QQuickItem* empty_label = FindItem(panel, QStringLiteral("savedAiKeysEmptyLabel"));
  ASSERT_NE(empty_label, nullptr);
  EXPECT_TRUE(empty_label->isVisible());
  EXPECT_EQ(empty_label->property("text").toString(), QStringLiteral("No saved keys"));
  EXPECT_FALSE(delete_all->isEnabled());
}

constexpr char kReminderHostQml[] = R"(
import QtQuick

QtObject {
    property var snackbarMessages: []
    function showSnackbar(message) {
        snackbarMessages = snackbarMessages.concat([String(message)])
    }
}
)";

// Creates the production reminder with the fake controller and a host that records snackbars.
class ReminderHarness {
 public:
  ReminderHarness() {
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    QQmlComponent host_component(&engine_);
    host_component.setData(QByteArray{kReminderHostQml}, QUrl(QStringLiteral("file:///Host.qml")));
    host_.reset(host_component.create());
    QQmlComponent reminder_component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/SavedAiKeysReminder.qml")));
    reminder_.reset(reminder_component.create());
    for (const auto& error : reminder_component.errors()) warnings_.push_back(error.toString());
    if (reminder_ && host_) {
      reminder_->setProperty("host", QVariant::fromValue(host_.get()));
      reminder_->setProperty("profileController", QVariant::fromValue<QObject*>(&profiles_));
    }
  }

  [[nodiscard]] auto Messages() const -> QStringList {
    return host_->property("snackbarMessages").toStringList();
  }

  FakeProfileController    profiles_;
  QQmlEngine               engine_;
  QStringList              warnings_;
  std::unique_ptr<QObject> host_;
  std::unique_ptr<QObject> reminder_;
};

TEST(SavedAiKeysPanelQmlTest, LaunchReminderShowsOnceWhileACredentialIsSaved) {
  ReminderHarness harness;
  ASSERT_NE(harness.reminder_, nullptr) << harness.warnings_.join('\n').toStdString();

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.reminder_.get(), "showIfSavedKeys"));
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.reminder_.get(), "showIfSavedKeys"));

  EXPECT_EQ(harness.Messages(),
            QStringList{QStringLiteral(
                "Alcedo Studio still holds AI keys that you saved for Advanced Content Analysis, "
                "which this version removes. Delete them in Settings > Saved AI Keys.")});
}

TEST(SavedAiKeysPanelQmlTest, LaunchReminderStaysHiddenWithoutASavedCredential) {
  ReminderHarness harness;
  ASSERT_NE(harness.reminder_, nullptr) << harness.warnings_.join('\n').toStdString();
  harness.profiles_.ClearCredentials();

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.reminder_.get(), "showIfSavedKeys"));

  EXPECT_TRUE(harness.Messages().isEmpty());
}

}  // namespace
}  // namespace alcedo::ui::test

#include "saved_ai_keys_panel_qml_test.moc"
