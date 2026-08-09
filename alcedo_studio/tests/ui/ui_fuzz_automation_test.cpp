//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThread>
#include <QUuid>
#include <filesystem>
#include <optional>
#include <vector>

#include "test_probe.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/application_module_qml_types.hpp"
#include "ui/alcedo_main/language_manager.hpp"

namespace {

class ProbeClient final {
 public:
  bool Connect(const QString& socket_name, int timeout_ms = 5000) {
    socket_.connectToServer(socket_name);
    return socket_.waitForConnected(timeout_ms);
  }

  bool SendRequest(qint64 id, const QString& method, const QJsonObject& fields = {}) {
    QJsonObject request;
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
      request.insert(it.key(), it.value());
    }
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    return socket_.write(line) == line.size() && socket_.flush();
  }

  auto WaitForResponse(qint64 id, int timeout_ms = 5000) -> std::optional<QJsonObject> {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
      while (socket_.canReadLine()) {
        const QByteArray    line = socket_.readLine().trimmed();
        QJsonParseError     parse_error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
          continue;
        }
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("event"))) {
          events_.append(object);
          continue;
        }
        if (object.value(QStringLiteral("id")).toInteger() == id) {
          return object;
        }
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(1);
    }
    return std::nullopt;
  }

  bool WaitForEvent(const QString& event_name, int timeout_ms = 5000) {
    for (const QJsonObject& event : events_) {
      if (event.value(QStringLiteral("event")).toString() == event_name) {
        return true;
      }
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
      while (socket_.canReadLine()) {
        const QByteArray    line = socket_.readLine().trimmed();
        QJsonParseError     parse_error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
          continue;
        }
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("event")).toString() == event_name) {
          events_.append(object);
          return true;
        }
        if (object.contains(QStringLiteral("event"))) {
          events_.append(object);
        }
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(1);
    }
    return false;
  }

 private:
  QLocalSocket       socket_;
  QList<QJsonObject> events_;
};

auto UniqueSocketName() -> QString {
  return QStringLiteral("alcedo_phase1_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void SeedLibraryThumbnails(alcedo::ui::ApplicationModuleHost& host, int count) {
  std::vector<alcedo::ui::AlbumItem> items;
  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    alcedo::ui::AlbumItem item;
    item.element_id     = static_cast<sl_element_id_t>(1000 + i);
    item.image_id       = static_cast<image_id_t>(2000 + i);
    item.file_id        = static_cast<sl_element_id_t>(3000 + i);
    item.file_name      = QStringLiteral("test_%1.arw").arg(i);
    item.extension      = QStringLiteral("arw");
    item.thumb_data_url = QString();
    items.push_back(item);
  }
  host.library()->view_state().all_images_  = items;
  host.library()->view_state().total_count_ = items.size();
  host.library()->model().resetModel(items, items.size());
  host.library()->NotifyThumbnailsChanged();
  host.library()->NotifyCountsChanged();
}

class UiFuzzAutomationFixture : public alcedo::ui::test::ApplicationModuleHostTestFixture {
 protected:
  alcedo::ui::ApplicationModuleHost host_;
  alcedo::ui::LanguageManager       language_manager_{QCoreApplication::instance()};
  QQmlApplicationEngine             engine_;
  QQuickWindow*                     window_ = nullptr;

  bool                              LoadAutomationWindow() {
    alcedo::ui::AppTheme::RegisterFonts();
    alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager_.EffectiveLanguageCode());
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    alcedo::ui::RegisterApplicationModuleTypes();

    engine_.addImportPath(QStringLiteral("qrc:/"));
    language_manager_.AttachEngine(&engine_);
    host_.AttachQmlEngine(&engine_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), &host_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                                                           &alcedo::ui::AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                                                           &language_manager_);
    engine_.rootContext()->setContextProperty(QStringLiteral("automationMode"), true);
    QObject::connect(&engine_, &QQmlEngine::warnings, [](const QList<QQmlError>& warnings) {
      for (const QQmlError& warning : warnings) {
        qWarning().noquote() << warning.toString();
      }
    });

    engine_.loadFromModule("Alcedo.Main", "Main");
    if (engine_.rootObjects().isEmpty()) {
      qWarning("Alcedo.Main did not create a root object.");
      return false;
    }
    window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().constFirst());
    if (window_ == nullptr) {
      qWarning().noquote() << "Alcedo.Main root type:"
                           << engine_.rootObjects().constFirst()->metaObject()->className();
      return false;
    }
    window_->show();
    window_->requestActivate();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    return true;
  }
};

}  // namespace

TEST_F(UiFuzzAutomationFixture, TestHostSkipsWelcomeDialogAndImportsCameraTreeRecursively) {
  ASSERT_TRUE(LoadAutomationWindow());

  ASSERT_TRUE(CreateTestProject(host_));
  ASSERT_TRUE(host_.project()->ServiceReady());

  const auto raw_paths = alcedo::ui::test::CollectRawTestImages("camera");
  ASSERT_FALSE(raw_paths.empty());

  alcedo::ui::TestProbe probe(&engine_, window_);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();

  host_.import_export()->StartImportPaths(raw_paths);
  QElapsedTimer import_timer;
  import_timer.start();
  bool import_observed = false;
  while (import_timer.elapsed() < 900000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    import_observed = import_observed || host_.import_export()->ImportRunning();
    if (import_observed && !host_.import_export()->ImportRunning()) {
      break;
    }
    QThread::msleep(1);
  }

  ASSERT_TRUE(import_observed);
  ASSERT_FALSE(host_.import_export()->ImportRunning());
  EXPECT_EQ(host_.import_export()->ImportTotal(), static_cast<int>(raw_paths.size()));
  EXPECT_EQ(host_.import_export()->ImportCompleted(), static_cast<int>(raw_paths.size()));
  EXPECT_EQ(host_.import_export()->ImportFailed(), 0);

  auto* welcome_dialog = window_->findChild<QObject*>(QStringLiteral("welcomeDialog"));
  ASSERT_NE(welcome_dialog, nullptr);
  EXPECT_FALSE(welcome_dialog->property("visible").toBool());
  EXPECT_FALSE(welcome_dialog->property("opened").toBool());

  auto* workspace_host = window_->findChild<QQuickItem*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  EXPECT_TRUE(workspace_host->isVisible());

  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("heartbeat")));
  ASSERT_TRUE(
      client.SendRequest(1, QStringLiteral("read"),
                         {{QStringLiteral("target"), QStringLiteral("workspaceHost.visible")}}));
  const auto response = client.WaitForResponse(1);
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(
      response->value(QStringLiteral("result")).toObject().value(QStringLiteral("value")).toBool());
}

TEST(UiFuzzAutomation, TestProbeAnswersSnapshotAfterProjectReady) {
  QQuickWindow window;
  window.setTitle(QStringLiteral("probe-test"));
  window.setWidth(640);
  window.setHeight(480);
  auto* workspace_host = new QQuickItem(window.contentItem());
  workspace_host->setObjectName(QStringLiteral("workspaceHost"));
  workspace_host->setWidth(320);
  workspace_host->setHeight(240);
  window.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  alcedo::ui::TestProbe probe(nullptr, &window);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();

  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("heartbeat")));

  ASSERT_TRUE(client.SendRequest(1, QStringLiteral("snapshot")));
  const auto snapshot = client.WaitForResponse(1);
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->value(QStringLiteral("ok")).toBool());
  const QJsonArray elements = snapshot->value(QStringLiteral("result"))
                                  .toObject()
                                  .value(QStringLiteral("elements"))
                                  .toArray();
  bool found_workspace = false;
  for (const QJsonValue& value : elements) {
    if (value.toObject().value(QStringLiteral("objectName")).toString() ==
        QStringLiteral("workspaceHost")) {
      found_workspace = true;
      break;
    }
  }
  EXPECT_TRUE(found_workspace);

  ASSERT_TRUE(client.SendRequest(2, QStringLiteral("ping")));
  const auto ping = client.WaitForResponse(2);
  ASSERT_TRUE(ping.has_value());
  ASSERT_TRUE(ping->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(
      ping->value(QStringLiteral("result")).toObject().value(QStringLiteral("guiThread")).toBool());
  EXPECT_GE(ping->value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("heartbeat"))
                .toInteger(),
            1);

  ASSERT_TRUE(
      client.SendRequest(3, QStringLiteral("read"),
                         {{QStringLiteral("target"), QStringLiteral("workspaceHost.visible")}}));
  const auto read = client.WaitForResponse(3);
  ASSERT_TRUE(read.has_value());
  ASSERT_TRUE(read->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(
      read->value(QStringLiteral("result")).toObject().value(QStringLiteral("value")).toBool());
}

TEST(UiFuzzAutomation, TestProbeReportsStaleTargetAfterDialogDestroyed) {
  QQuickWindow window;
  window.setWidth(320);
  window.setHeight(240);
  auto* temporary_dialog = new QQuickItem(window.contentItem());
  temporary_dialog->setObjectName(QStringLiteral("temporaryDialog"));
  temporary_dialog->setWidth(100);
  temporary_dialog->setHeight(100);
  window.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  alcedo::ui::TestProbe probe(nullptr, &window);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();
  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));

  ASSERT_TRUE(client.SendRequest(1, QStringLiteral("find"),
                                 {{QStringLiteral("target"), QStringLiteral("temporaryDialog")}}));
  const auto found = client.WaitForResponse(1);
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(found->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(
      found->value(QStringLiteral("result")).toObject().value(QStringLiteral("found")).toBool());

  delete temporary_dialog;
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  ASSERT_TRUE(client.SendRequest(2, QStringLiteral("find"),
                                 {{QStringLiteral("target"), QStringLiteral("temporaryDialog")}}));
  const auto stale = client.WaitForResponse(2);
  ASSERT_TRUE(stale.has_value());
  EXPECT_FALSE(stale->value(QStringLiteral("ok")).toBool());
  EXPECT_EQ(
      stale->value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
      QStringLiteral("target_not_found"));
}

TEST_F(UiFuzzAutomationFixture, InputClickReturnsFromEditorToLibrary) {
  ASSERT_TRUE(LoadAutomationWindow());
  ASSERT_TRUE(CreateTestProject(host_));
  ASSERT_TRUE(host_.project()->ServiceReady());
  SeedLibraryThumbnails(host_, 4);
  alcedo::ui::test::ProcessEvents(80);

  alcedo::ui::TestProbe probe(&engine_, window_);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();

  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));

  ASSERT_TRUE(client.SendRequest(
      1, QStringLiteral("click"),
      {{QStringLiteral("target"), QStringLiteral("libraryNavButton")}}));
  const auto library_click = client.WaitForResponse(1);
  ASSERT_TRUE(library_click.has_value());
  ASSERT_TRUE(library_click->value(QStringLiteral("ok")).toBool());
  alcedo::ui::test::ProcessEvents(50);

  ASSERT_TRUE(client.SendRequest(
      2, QStringLiteral("read"),
      {{QStringLiteral("target"), QStringLiteral("libraryWorkspace")},
       {QStringLiteral("property"), QStringLiteral("visible")}}));
  const auto library_visible = client.WaitForResponse(2);
  ASSERT_TRUE(library_visible.has_value());
  ASSERT_TRUE(library_visible->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(library_visible->value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("value"))
                  .toBool());

  ASSERT_TRUE(client.SendRequest(
      3, QStringLiteral("find"),
      {{QStringLiteral("target"), QStringLiteral("thumbnailGridView_firstCard")}}));
  const auto first_card = client.WaitForResponse(3);
  ASSERT_TRUE(first_card.has_value());
  ASSERT_TRUE(first_card->value(QStringLiteral("ok")).toBool())
      << first_card->value(QStringLiteral("error"))
             .toObject()
             .value(QStringLiteral("message"))
             .toString()
             .toStdString();

  ASSERT_TRUE(client.SendRequest(
      4, QStringLiteral("doubleClick"),
      {{QStringLiteral("target"), QStringLiteral("thumbnailGridView_firstCard")}}));
  const auto open_editor = client.WaitForResponse(4);
  ASSERT_TRUE(open_editor.has_value());
  ASSERT_TRUE(open_editor->value(QStringLiteral("ok")).toBool())
      << open_editor->value(QStringLiteral("error"))
             .toObject()
             .value(QStringLiteral("message"))
             .toString()
             .toStdString();
  alcedo::ui::test::ProcessEvents(80);

  EXPECT_EQ(host_.workspace_router()->workspace(), QStringLiteral("editor"));

  ASSERT_TRUE(client.SendRequest(
      5, QStringLiteral("wait"),
      {{QStringLiteral("target"), QStringLiteral("editorWorkspace")},
       {QStringLiteral("property"), QStringLiteral("visible")},
       {QStringLiteral("eq"), true},
       {QStringLiteral("timeoutMs"), 5000}}));
  const auto editor_visible = client.WaitForResponse(5, 6000);
  ASSERT_TRUE(editor_visible.has_value());
  ASSERT_TRUE(editor_visible->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(editor_visible->value(QStringLiteral("actual")).toBool());

  ASSERT_TRUE(client.SendRequest(
      6, QStringLiteral("click"),
      {{QStringLiteral("target"), QStringLiteral("libraryNavButton")}}));
  const auto return_to_library = client.WaitForResponse(6);
  ASSERT_TRUE(return_to_library.has_value());
  ASSERT_TRUE(return_to_library->value(QStringLiteral("ok")).toBool());
  alcedo::ui::test::ProcessEvents(80);

  EXPECT_EQ(host_.workspace_router()->workspace(), QStringLiteral("library"));

  ASSERT_TRUE(client.SendRequest(
      7, QStringLiteral("wait"),
      {{QStringLiteral("target"), QStringLiteral("libraryWorkspace")},
       {QStringLiteral("property"), QStringLiteral("visible")},
       {QStringLiteral("eq"), true},
       {QStringLiteral("timeoutMs"), 5000}}));
  const auto library_visible_again = client.WaitForResponse(7, 6000);
  ASSERT_TRUE(library_visible_again.has_value());
  ASSERT_TRUE(library_visible_again->value(QStringLiteral("ok")).toBool());
  EXPECT_TRUE(library_visible_again->value(QStringLiteral("actual")).toBool());
}

TEST(UiFuzzAutomation, InputClickRejectsElementCoveredByModalOverlay) {
  QQuickWindow window;
  window.setWidth(400);
  window.setHeight(300);
  window.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  auto* button = new QQuickItem(window.contentItem());
  button->setObjectName(QStringLiteral("coveredActionButton"));
  button->setX(40);
  button->setY(40);
  button->setWidth(120);
  button->setHeight(40);

  auto* overlay = new QQuickItem(window.contentItem());
  overlay->setObjectName(QStringLiteral("testModalOverlay"));
  overlay->setProperty("modal", true);
  overlay->setZ(100);
  overlay->setWidth(400);
  overlay->setHeight(300);

  alcedo::ui::TestProbe probe(nullptr, &window);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();
  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));

  ASSERT_TRUE(client.SendRequest(
      1, QStringLiteral("click"),
      {{QStringLiteral("target"), QStringLiteral("coveredActionButton")}}));
  const auto rejected = client.WaitForResponse(1);
  ASSERT_TRUE(rejected.has_value());
  EXPECT_FALSE(rejected->value(QStringLiteral("ok")).toBool());
  EXPECT_EQ(
      rejected->value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
      QStringLiteral("target_covered"));
}

TEST(UiFuzzAutomation, TestProbeWaitForPropertyTimeoutReportsLastObservedValue) {
  QQuickWindow window;
  window.setWidth(320);
  window.setHeight(240);
  auto* status = new QQuickItem(window.contentItem());
  status->setObjectName(QStringLiteral("importStatus"));
  status->setProperty("text", QStringLiteral("Running"));
  status->setWidth(100);
  status->setHeight(24);
  window.show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  alcedo::ui::TestProbe probe(nullptr, &window);
  const QString         socket_name = UniqueSocketName();
  QString               probe_error;
  ASSERT_TRUE(probe.Start(socket_name, &probe_error)) << probe_error.toStdString();
  ProbeClient client;
  ASSERT_TRUE(client.Connect(socket_name));
  probe.MarkReady();
  ASSERT_TRUE(client.WaitForEvent(QStringLiteral("ready")));

  ASSERT_TRUE(client.SendRequest(
      1, QStringLiteral("wait"),
      {{QStringLiteral("target"), QStringLiteral("importStatus")},
       {QStringLiteral("property"), QStringLiteral("text")},
       {QStringLiteral("eq"), QStringLiteral("Finished")},
       {QStringLiteral("timeoutMs"), 200}}));
  const auto timed_out = client.WaitForResponse(1, 2000);
  ASSERT_TRUE(timed_out.has_value());
  EXPECT_FALSE(timed_out->value(QStringLiteral("ok")).toBool());
  const QJsonObject error = timed_out->value(QStringLiteral("error")).toObject();
  EXPECT_EQ(error.value(QStringLiteral("code")).toString(), QStringLiteral("wait_timeout"));
  EXPECT_EQ(error.value(QStringLiteral("actual")).toString(), QStringLiteral("Running"));
}
