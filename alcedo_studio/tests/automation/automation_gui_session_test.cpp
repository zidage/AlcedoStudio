//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// GUI session tests. ApplicationModuleHost runs with the headless sink in this process, opens a
// CI RAW file in the editor, and serves a `gui` automation session: agent commands, the user
// adapters, and the notifications of a watching socket client meet in one editor session.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/editor_session_types.hpp"
#include "automation/automation_protocol.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/agent_control_owner.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_host_session.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "ui/alcedo_main/automation/headless_host.hpp"

namespace alcedo::automation {
namespace {

using ui::ApplicationModuleHost;

auto CollectCiRawFiles() -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

/// The first editor backend of this platform and build.
auto PlatformEditorBackend() -> std::optional<editor_rhi::EditorBackend> {
  for (const auto backend : {editor_rhi::EditorBackend::OpenCl, editor_rhi::EditorBackend::Metal,
                             editor_rhi::EditorBackend::Cuda}) {
    if (editor_rhi::IsBackendSupportedOnThisPlatform(backend) &&
        editor_rhi::IsBackendAvailableInThisBuild(backend)) {
      return backend;
    }
  }
  return std::nullopt;
}

/// A socket client of the session that collects the notifications.
class WatchingClient {
 public:
  auto Connect(const QString& socket_name) -> bool {
    socket_.connectToServer(socket_name);
    return WaitUntil([this]() { return socket_.state() == QLocalSocket::ConnectedState; },
                     std::chrono::seconds(5));
  }

  /// Waits until a notification with @p method arrives and returns its params.
  auto WaitForNotification(const QString& method, std::chrono::milliseconds timeout)
      -> std::optional<QJsonObject> {
    std::optional<QJsonObject> found;
    WaitUntil(
        [&]() {
          buffer_.append(socket_.readAll());
          qsizetype newline = 0;
          while ((newline = buffer_.indexOf('\n')) >= 0) {
            const AutomationParseResult parsed = ParseAutomationLine(buffer_.left(newline));
            buffer_.remove(0, newline + 1);
            if (!parsed.message.has_value()) {
              continue;
            }
            if (const auto* notification = std::get_if<AutomationNotification>(&*parsed.message);
                notification != nullptr && notification->method == method && !found.has_value()) {
              found = notification->params;
            }
          }
          return found.has_value();
        },
        timeout);
    return found;
  }

  /// Drops the messages that arrived so far.
  void Drain() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    buffer_.append(socket_.readAll());
    buffer_.clear();
  }

 private:
  QLocalSocket socket_;
  QByteArray   buffer_;
};

class AutomationGuiSessionTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    const auto backend = PlatformEditorBackend();
    if (!backend.has_value()) {
      GTEST_SKIP() << "no editor backend is available on this platform";
    }
    const auto raw_files = CollectCiRawFiles();
    ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
    const QString backend_error = StartHeadlessEditorBackend(*backend);
    ASSERT_TRUE(backend_error.isEmpty()) << backend_error.toStdString();

    sink_ = std::make_unique<HeadlessFrameSink>();
    host_ = std::make_unique<ApplicationModuleHost>();
    host_->project()->SetRuntimeAcceleratorPreference(HeadlessAcceleratorPreference(*backend));
    // The GUI viewport receives its frames through the scope tap; route the headless sink the
    // same way so the scope controller sees the frames of this session.
    ui::EditorScopeController* scope = host_->editor_session()->scope_controller();
    scope->SetDownstreamSink(sink_.get());
    ASSERT_TRUE(
        host_->editor_session()->BindHeadlessPresentationSink(scope->frame_sink(), 1280, 720));
    ASSERT_TRUE(CreateTestProject(*host_));

    host_->import_export()->StartImport(ui::test::PathsToQStringList({raw_files.front()}));
    ASSERT_TRUE(WaitUntil([&] { return !host_->import_export()->ImportRunning(); },
                          std::chrono::minutes(2)));
    ASSERT_EQ(host_->import_export()->ImportFailed(), 0);
    ASSERT_TRUE(WaitUntil([&] { return !host_->library()->Thumbnails().isEmpty(); },
                          std::chrono::seconds(30)));
    const QVariantMap item     = host_->library()->Thumbnails().front().toMap();
    const uint        element  = item.value("elementId").toUInt();
    const uint        image_id = item.value("imageId").toUInt();
    ASSERT_NE(element, 0U);
    host_->workspace_router()->OpenEditor(element, image_id);
    ASSERT_TRUE(WaitUntil(
        [&] {
          return host_->editor_session_service()->state() == EditorSessionState::Interactive &&
                 host_->editor_session()->can_edit();
        },
        std::chrono::minutes(2)))
        << "state " << EditorSessionStateName(host_->editor_session_service()->state());

    ASSERT_TRUE(session_dir_.isValid());
    AutomationHostSessionOptions options;
    options.host_mode    = QStringLiteral("gui");
    options.session_name = QStringLiteral("gui");
    options.session_dir  = session_dir_.path();
    session_             = std::make_unique<AutomationHostSession>(host_.get(), std::move(options));
    QString error;
    ASSERT_TRUE(session_->Start(&error)) << error.toStdString();
  }

  void TearDown() override {
    if (session_) {
      session_->Stop();
      session_.reset();
    }
    if (host_) {
      host_->Shutdown();
      host_.reset();
    }
    sink_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  /// Dispatches one request through the session registry, the path of every socket request.
  auto Call(const QString& method, QJsonObject params = {}) -> std::optional<AutomationResponse> {
    auto              response = std::make_shared<std::optional<AutomationResponse>>();
    AutomationRequest request{QJsonValue(++next_id_), method, std::move(params)};
    session_->registry().Dispatch(request,
                                  AutomationReply(request.id, [response](AutomationResponse value) {
                                    *response = std::move(value);
                                  }));
    const QDeadlineTimer deadline(120000);
    while (!response->has_value() && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    }
    return *response;
  }

  auto Result(const QString& method, QJsonObject params = {}) -> QJsonObject {
    const auto response = Call(method, std::move(params));
    if (!response.has_value()) {
      ADD_FAILURE() << method.toStdString() << " did not answer";
      return {};
    }
    if (response->error.has_value()) {
      ADD_FAILURE() << method.toStdString()
                    << " failed: " << response->error->message.toStdString();
      return {};
    }
    return response->result.toObject();
  }

  auto ErrorCode(const QString& method, QJsonObject params = {}) -> std::optional<int> {
    const auto response = Call(method, std::move(params));
    if (!response.has_value() || !response->error.has_value()) {
      return std::nullopt;
    }
    return static_cast<int>(response->error->code);
  }

  auto SetExposure(double value) -> std::optional<AutomationResponse> {
    return Call(QStringLiteral("editor.set"), QJsonObject{{"field", "exposure"}, {"value", value}});
  }

  auto          control() -> ui::AgentControlOwner* { return host_->agent_control(); }

  QTemporaryDir session_dir_;
  std::unique_ptr<HeadlessFrameSink>     sink_;
  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<AutomationHostSession> session_;
  int                                    next_id_ = 0;
};

TEST_F(AutomationGuiSessionTest, AgentEditAppearsInControllerProjection) {
  const QJsonObject acquired =
      Result(QStringLiteral("control.acquire"), QJsonObject{{"holder_name", "test-agent"}});
  ASSERT_TRUE(acquired.value("held").toBool());
  EXPECT_EQ(acquired.value("holder_name").toString(), QStringLiteral("test-agent"));
  ASSERT_TRUE(control()->held());
  EXPECT_FALSE(host_->interaction_policy()->CanChangeStateAsUser());

  const QVariantMap snapshot_before = host_->editor_session()->adjustment_snapshot();
  const std::size_t commits_before  = host_->editor_session()->history_snapshot().commits.size();
  const auto        set             = SetExposure(1.25);
  ASSERT_TRUE(set.has_value());
  ASSERT_FALSE(set->error.has_value()) << set->error->message.toStdString();

  // The agent commit shows in the controller that QML reads.
  EXPECT_EQ(host_->editor_session()->history_snapshot().commits.size(), commits_before + 1);
  EXPECT_TRUE(host_->editor_session()->history_snapshot().can_undo);
  EXPECT_TRUE(
      WaitUntil([&] { return host_->editor_session()->adjustment_snapshot() != snapshot_before; },
                std::chrono::seconds(30)));
  EXPECT_NEAR(Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{"exposure"}}})
                  .value("values")
                  .toObject()
                  .value("exposure")
                  .toDouble(),
              1.25, 1e-6);

  // The command ended: the agent task waits for the next command.
  const QVariantList tasks = host_->background_tasks()->Tasks();
  const auto         task  = std::find_if(tasks.begin(), tasks.end(), [&](const QVariant& value) {
    return value.toMap().value("id").toString() == control()->task_id();
  });
  ASSERT_NE(task, tasks.end());
  EXPECT_EQ(task->toMap().value("detail").toString(), ui::AgentControlOwner::WaitingDetail());

  // The user cannot write while the agent holds control.
  EXPECT_FALSE(host_->editor_session()->submitPatch(QStringLiteral("exposure"),
                                                    QStringLiteral("{\"exposure\":0.25}"), true));

  EXPECT_FALSE(Result(QStringLiteral("control.release")).value("held").toBool());
  EXPECT_TRUE(host_->interaction_policy()->CanChangeStateAsUser());
}

TEST_F(AutomationGuiSessionTest, StateChangeWithoutControlIsRejected) {
  EXPECT_FALSE(Result(QStringLiteral("control.get")).value("held").toBool());
  EXPECT_FALSE(
      Result(QStringLiteral("state.get")).value("control").toObject().value("held").toBool());
  const std::size_t commits_before = host_->editor_session()->history_snapshot().commits.size();
  EXPECT_EQ(
      ErrorCode(QStringLiteral("editor.set"), QJsonObject{{"field", "exposure"}, {"value", 1.0}}),
      static_cast<int>(AutomationErrorCode::ControlNotHeld));
  // A read runs without control.
  EXPECT_FALSE(Result(QStringLiteral("editor.history")).isEmpty());
  EXPECT_EQ(host_->editor_session()->history_snapshot().commits.size(), commits_before);

  // After the user stops the agent, its commands are refused until it acquires control again.
  ASSERT_TRUE(Result(QStringLiteral("control.acquire")).value("held").toBool());
  const QString agent_task_id = control()->task_id();
  ASSERT_TRUE(host_->background_tasks()->CancelTask(agent_task_id));
  EXPECT_TRUE(host_->interaction_policy()->CanChangeStateAsUser() ||
              WaitUntil([&] { return host_->interaction_policy()->CanChangeStateAsUser(); },
                        std::chrono::seconds(5)));
  EXPECT_EQ(
      ErrorCode(QStringLiteral("editor.set"), QJsonObject{{"field", "exposure"}, {"value", 1.0}}),
      static_cast<int>(AutomationErrorCode::ControlRevokedByUser));
  EXPECT_TRUE(Result(QStringLiteral("control.get")).value("revoked_by_user").toBool());
  EXPECT_EQ(host_->editor_session()->history_snapshot().commits.size(), commits_before);

  // The user told the agent to continue: the acquire succeeds without a step in the application
  // and the stopped agent task continues.
  ASSERT_TRUE(Result(QStringLiteral("control.acquire")).value("held").toBool());
  EXPECT_FALSE(Result(QStringLiteral("control.get")).value("revoked_by_user").toBool());
  EXPECT_EQ(control()->task_id(), agent_task_id);
  const auto set =
      Call(QStringLiteral("editor.set"),
           QJsonObject{{"field", "exposure"}, {"value", 1.0}, {"note", "agent: lift"}});
  ASSERT_TRUE(set.has_value());
  EXPECT_FALSE(set->error.has_value());
  EXPECT_EQ(host_->editor_session()->history_snapshot().commits.size(), commits_before + 1);

  // The agent task lists the command with its note and its outcome.
  QVariantList activity;
  for (const QVariant& value : host_->background_tasks()->Tasks()) {
    if (value.toMap().value("id").toString() == agent_task_id) {
      activity = value.toMap().value("activity").toList();
    }
  }
  ASSERT_FALSE(activity.isEmpty());
  const QVariantMap last = activity.back().toMap();
  EXPECT_TRUE(last.value("text").toString().startsWith(QStringLiteral("editor.set")));
  EXPECT_EQ(last.value("note").toString(), QStringLiteral("agent: lift"));
  EXPECT_EQ(last.value("state").toString(), QStringLiteral("succeeded"));
}

TEST_F(AutomationGuiSessionTest, UserEditSendsHistoryChangedToWatchingClient) {
  WatchingClient client;
  ASSERT_TRUE(client.Connect(session_->socket_name()));
  client.Drain();

  // The user edits in the GUI without an agent: the watching client hears of the commit.
  ASSERT_TRUE(host_->editor_session()->submitPatch(QStringLiteral("exposure"),
                                                   QStringLiteral("{\"exposure\":0.75}"), true));
  const auto history =
      client.WaitForNotification(QStringLiteral("history.changed"), std::chrono::seconds(60));
  ASSERT_TRUE(history.has_value()) << "no history.changed for the user edit";

  // Agent control changes reach the client too.
  ASSERT_TRUE(Result(QStringLiteral("control.acquire")).value("held").toBool());
  const auto changed =
      client.WaitForNotification(QStringLiteral("control.changed"), std::chrono::seconds(5));
  ASSERT_TRUE(changed.has_value());
  EXPECT_TRUE(changed->value("held").toBool());
}

// When the render of an agent commit fails, editor.set and editor.batch_set answer at once with
// the render error and the head commit; the commit stays (#367). The user writes an OpenDRT
// encoding pair that the catalog does not offer, so every later frame fails to render.
TEST_F(AutomationGuiSessionTest, FailedRenderOfCommitAnswersWithoutWaitingForTimeout) {
  int failed_frames = 0;
  QObject::connect(host_->editor_session(), &ui::EditorSessionController::SessionResultObserved,
                   host_.get(), [&failed_frames](const EditorSessionResult& result) {
                     if (result.kind == EditorSessionResultKind::FrameFailed) {
                       ++failed_frames;
                     }
                   });
  ASSERT_TRUE(host_->editor_session()->submitPatch(
      QStringLiteral("odt"),
      QStringLiteral(
          R"({"odt":{"method":"open_drt","encoding_space":"rec2020","encoding_eotf":"gamma_2_2"}})"),
      true));
  ASSERT_TRUE(WaitUntil([&] { return failed_frames > 0; }, std::chrono::seconds(60)))
      << "the unsupported OpenDRT output did not fail the render";
  ASSERT_TRUE(Result(QStringLiteral("control.acquire"), QJsonObject{{"holder_name", "test-agent"}})
                  .value("held")
                  .toBool());

  const auto error_data = [](const std::optional<AutomationResponse>& response) {
    return response->error->data.toObject();
  };
  const auto  commits = [&] { return host_->editor_session()->history_snapshot().commits.size(); };
  const auto  head = [&] { return Result(QStringLiteral("editor.history")).value("head_commit"); };
  std::size_t commits_before = commits();
  const auto  started        = std::chrono::steady_clock::now();
  const auto  set            = SetExposure(2.3);
  ASSERT_TRUE(set.has_value());
  ASSERT_TRUE(set->error.has_value()) << "the frame of the commit cannot render";
  EXPECT_EQ(set->error->code, AutomationErrorCode::Failed);
  EXPECT_TRUE(error_data(set).value("reason").toString().contains(QStringLiteral("OpenDRT")))
      << error_data(set).value("reason").toString().toStdString();
  EXPECT_EQ(commits(), commits_before + 1) << "the commit stays";
  EXPECT_EQ(error_data(set).value("head_commit"), head());
  EXPECT_NEAR(Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{"exposure"}}})
                  .value("values")
                  .toObject()
                  .value("exposure")
                  .toDouble(),
              2.3, 1e-6);

  // A batch stops at the change whose frame fails; that change is committed, so it is applied.
  commits_before   = commits();
  const auto batch = Call(
      QStringLiteral("editor.batch_set"),
      QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "exposure"}, {"value", 0.35}},
                                         QJsonObject{{"field", "exposure"}, {"value", 0.65}}}}});
  ASSERT_TRUE(batch.has_value());
  ASSERT_TRUE(batch->error.has_value());
  EXPECT_EQ(batch->error->code, AutomationErrorCode::Failed);
  EXPECT_EQ(error_data(batch).value("applied").toInt(), 1);
  EXPECT_EQ(commits(), commits_before + 1);
  EXPECT_EQ(error_data(batch).value("head_commit"), head());

  // An undo to values that cannot render either answers the same way; the head stays moved.
  const auto undo = Call(QStringLiteral("editor.undo"));
  ASSERT_TRUE(undo.has_value());
  ASSERT_TRUE(undo->error.has_value());
  EXPECT_EQ(undo->error->code, AutomationErrorCode::Failed);
  EXPECT_EQ(commits(), commits_before + 1);
  EXPECT_EQ(error_data(undo).value("head_commit"), head());
  EXPECT_NE(error_data(undo).value("head_commit"), error_data(batch).value("head_commit"));

  // The answers came from the failed renders, not from the 120 s command timeout.
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
  EXPECT_FALSE(Result(QStringLiteral("control.release")).value("held").toBool());
}

// With a Color Grade selected in the window, the agent still reads and writes the fields of the
// Develop and DRT nodes, a partial crop write keeps the other crop properties, and a Color Grade
// field goes to the selected Color Grade. The selection does not change.
TEST_F(AutomationGuiSessionTest, OtherNodeFieldsStayReachableWhileColorGradeIsSelected) {
  ui::EditorNodeController nodes;
  nodes.set_editor_session(host_->editor_session());
  const auto document = host_->editor_session()->pipeline_document();
  ASSERT_TRUE(document && document->PrimaryGrade() != nullptr);
  const NodeId grade = document->PrimaryGrade()->Id();
  nodes.selectNode(QString::fromStdString(std::string(grade.Value())));
  ASSERT_EQ(host_->editor_session()->selected_node_id(), grade);
  ASSERT_TRUE(Result(QStringLiteral("control.acquire"), QJsonObject{{"holder_name", "test-agent"}})
                  .value("held")
                  .toBool());

  const auto read = [&](const QString& field) {
    return Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{field}}})
        .value("values")
        .toObject()
        .value(field);
  };
  for (const QString field : {QStringLiteral("crop_rotate"), QStringLiteral("color_temp"),
                              QStringLiteral("odt"), QStringLiteral("exposure")}) {
    EXPECT_FALSE(read(field).isUndefined()) << field.toStdString() << " is not readable";
  }

  const auto set = [&](const QString& field, const QJsonValue& value) {
    const auto response =
        Call(QStringLiteral("editor.set"), QJsonObject{{"field", field}, {"value", value}});
    ASSERT_TRUE(response.has_value()) << field.toStdString() << " did not answer";
    EXPECT_FALSE(response->error.has_value())
        << field.toStdString() << ": " << response->error->message.toStdString();
  };
  set(QStringLiteral("crop_rotate"),
      QJsonObject{{"x", 0.1}, {"y", 0.1}, {"width", 0.8}, {"height", 0.8}});
  set(QStringLiteral("crop_rotate"), QJsonObject{{"y", 0.15}});
  const QJsonObject crop = read(QStringLiteral("crop_rotate")).toObject();
  EXPECT_NEAR(crop.value("x").toDouble(), 0.1, 1e-3);
  EXPECT_NEAR(crop.value("y").toDouble(), 0.15, 1e-3);
  EXPECT_NEAR(crop.value("width").toDouble(), 0.8, 1e-3);
  EXPECT_NEAR(crop.value("height").toDouble(), 0.8, 1e-3);

  set(QStringLiteral("color_temp"), QJsonObject{{"kelvin", 5000}});
  const QJsonObject white_balance = read(QStringLiteral("color_temp")).toObject();
  EXPECT_EQ(white_balance.value("mode").toString(), QStringLiteral("custom"));
  EXPECT_NEAR(white_balance.value("kelvin").toDouble(), 5000.0, 1.0);

  set(QStringLiteral("odt"), QJsonObject{{"look_preset", "arriba"}});
  EXPECT_EQ(read(QStringLiteral("odt")).toObject().value("look_preset").toString(),
            QStringLiteral("arriba"));

  set(QStringLiteral("exposure"), 1.0);
  EXPECT_NEAR(read(QStringLiteral("exposure")).toDouble(), 1.0, 1e-6);

  EXPECT_EQ(host_->editor_session()->selected_node_id(), grade);
  EXPECT_FALSE(Result(QStringLiteral("control.release")).value("held").toBool());
}

// The scopes show the current frame while an agent holds control, after the agent edits, opens,
// closes, and reopens images, and after control comes back to the user (#376). The test writes
// visualActive like EditorScopePanel: the panel is shown and the editor has an image. Agent
// control does not change the value.
TEST_F(AutomationGuiSessionTest, ScopesFollowTheCurrentImageThroughAgentControl) {
  const auto raw_files = CollectCiRawFiles();
  if (raw_files.size() < 2) {
    GTEST_SKIP() << "two CI RAW files are required for the image switch";
  }
  host_->import_export()->StartImport(ui::test::PathsToQStringList({raw_files.at(1)}));
  ASSERT_TRUE(
      WaitUntil([&] { return !host_->import_export()->ImportRunning(); }, std::chrono::minutes(2)));
  ASSERT_EQ(host_->import_export()->ImportFailed(), 0);
  ASSERT_TRUE(WaitUntil([&] { return host_->library()->Thumbnails().size() >= 2; },
                        std::chrono::seconds(30)));

  ui::EditorSessionController* editor          = host_->editor_session();
  ui::EditorScopeController*   scope           = editor->scope_controller();
  const uint                   first_element   = editor->element_id();
  const uint                   first_image_id  = editor->image_id();
  uint                         second_element  = 0;
  uint                         second_image_id = 0;
  for (const QVariant& value : host_->library()->Thumbnails()) {
    const QVariantMap item = value.toMap();
    if (item.value("elementId").toUInt() != first_element) {
      second_element  = item.value("elementId").toUInt();
      second_image_id = item.value("imageId").toUInt();
    }
  }
  ASSERT_NE(second_element, 0U);

  QObject panel;
  QObject::connect(editor, &ui::EditorSessionController::StateChanged, &panel,
                   [editor, scope] { scope->set_visual_active(editor->has_image()); });
  scope->set_visual_active(editor->has_image());

  // Waits for a scope snapshot of @p image_id that is newer than @p after_generation.
  const auto wait_for_snapshot = [&](uint image_id, qulonglong after_generation) {
    return WaitUntil(
        [&] {
          return scope->has_snapshot() && scope->image_identity() == image_id &&
                 scope->scope_generation() > after_generation;
        },
        std::chrono::seconds(60));
  };
  ASSERT_TRUE(wait_for_snapshot(first_image_id, 0)) << "no scope before agent control";

  ASSERT_TRUE(Result(QStringLiteral("control.acquire"), QJsonObject{{"holder_name", "test-agent"}})
                  .value("held")
                  .toBool());
  const qulonglong before_edit = scope->scope_generation();
  const auto       set         = SetExposure(1.25);
  ASSERT_TRUE(set.has_value());
  ASSERT_FALSE(set->error.has_value()) << set->error->message.toStdString();
  EXPECT_TRUE(wait_for_snapshot(first_image_id, before_edit)) << "no scope after the agent edit";

  const auto open = [&](uint element) {
    const QJsonObject opened = Result(QStringLiteral("editor.open"),
                                      QJsonObject{{"element_id", static_cast<qint64>(element)}});
    return static_cast<uint>(opened.value("element_id").toInteger());
  };
  ASSERT_EQ(open(second_element), second_element);
  EXPECT_TRUE(wait_for_snapshot(second_image_id, 0)) << "no scope after the agent image switch";

  ASSERT_TRUE(Result(QStringLiteral("editor.close")).value("closed").toBool());
  ASSERT_EQ(open(first_element), first_element);
  EXPECT_TRUE(wait_for_snapshot(first_image_id, 0)) << "no scope after the agent reopen";

  ASSERT_FALSE(Result(QStringLiteral("control.release")).value("held").toBool());
  ASSERT_TRUE(host_->interaction_policy()->CanChangeStateAsUser());
  const qulonglong before_user_edit = scope->scope_generation();
  ASSERT_TRUE(
      editor->submitPatch(QStringLiteral("exposure"), QStringLiteral("{\"exposure\":0.5}"), true));
  EXPECT_TRUE(wait_for_snapshot(first_image_id, before_user_edit))
      << "no scope after control came back";

  host_->workspace_router()->OpenEditor(second_element, second_image_id);
  EXPECT_TRUE(wait_for_snapshot(second_image_id, 0)) << "no scope after the user image switch";
}

}  // namespace
}  // namespace alcedo::automation
