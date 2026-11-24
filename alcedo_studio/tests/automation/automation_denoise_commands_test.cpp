//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// denoise.status and denoise.set (AI Denoise integration plan, DN13) through
// AutomationCommandRegistry::Dispatch in an in-process headless session, with the process-wide
// model session reading a graph file of this test.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "ui/alcedo_main/automation/headless_host.hpp"
#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::automation {
namespace {

using test::InProcessAutomationSession;

constexpr auto kGraphFileName = "utnet2_bayer_dark_loss_10k_fp16.onnx";

/// A model folder in the test temp directory whose graph file the process-wide model session
/// reads (the application reads the installed package; the locator has its own tests).
class GraphFolder {
 public:
  explicit GraphFolder(std::filesystem::path folder) : folder_(std::move(folder)) {
    std::filesystem::create_directories(folder_);
    PipelineExecutor::ProcessAiDenoiseSession()->SetGraphLocator(
        [graph = GraphPath()] { return AiDenoiseGraphLookup{graph, {}}; });
  }
  ~GraphFolder() { PipelineExecutor::ProcessAiDenoiseSession()->SetGraphLocator({}); }
  GraphFolder(const GraphFolder&)                    = delete;
  auto operator=(const GraphFolder&) -> GraphFolder& = delete;

  /// Writes @p bytes as the graph file: not an ONNX graph, so a model run fails.
  void Write(const std::string& bytes) const {
    std::ofstream stream(GraphPath(), std::ios::binary | std::ios::trunc);
    stream << bytes;
  }
  [[nodiscard]] auto GraphPath() const -> std::filesystem::path { return folder_ / kGraphFileName; }

 private:
  std::filesystem::path folder_;
};

auto Error(const std::optional<AutomationResponse>& response) -> std::optional<AutomationError> {
  if (!response.has_value() || !response->error.has_value()) {
    return std::nullopt;
  }
  return response->error;
}

auto InstalledVersion() -> QString {
  return QString::fromStdString(
      PipelineExecutor::ProcessAiDenoiseSession()->InstalledModelVersion());
}

/// The editor backend of the in-process host on this platform.
auto PlatformEditorBackend() -> std::optional<editor_rhi::EditorBackend> {
  for (const auto backend : {editor_rhi::EditorBackend::Cuda, editor_rhi::EditorBackend::Metal,
                             editor_rhi::EditorBackend::OpenCl}) {
    if (editor_rhi::IsBackendSupportedOnThisPlatform(backend) &&
        editor_rhi::IsBackendAvailableInThisBuild(backend)) {
      return backend;
    }
  }
  return std::nullopt;
}

class AutomationDenoiseCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  auto Folder() const -> QString { return ui::test::PathToQString(temp_dir_); }

  /// Starts the editor backend, binds the headless sink, creates a project with the first CI
  /// RAW file (a Bayer Sony ARW), and opens it in the editor. Returns false with a test failure
  /// or a skip.
  auto OpenCiPhoto(InProcessAutomationSession& session) -> bool {
    const auto backend = PlatformEditorBackend();
    if (!backend.has_value()) {
      ADD_FAILURE() << "this build has no editor backend";
      return false;
    }
    const QString backend_error = StartHeadlessEditorBackend(*backend);
    if (!backend_error.isEmpty()) {
      ADD_FAILURE() << backend_error.toStdString();
      return false;
    }
    session.host().project()->SetRuntimeAcceleratorPreference(
        HeadlessAcceleratorPreference(*backend));
    if (!session.host().editor_session()->BindHeadlessPresentationSink(&sink_, 1280, 720)) {
      ADD_FAILURE() << "the headless sink was not bound";
      return false;
    }
    const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
    QStringList   raws = QDir(raw_root).entryList(QStringList{"*.ARW"}, QDir::Files, QDir::Name);
    if (raws.isEmpty()) {
      ADD_FAILURE() << "the CI RAW files are missing under " << raw_root.toStdString();
      return false;
    }
    if (session
            .CallResult(QStringLiteral("project.create"),
                        QJsonObject{{"folder", Folder()}, {"name", "denoise_project"}})
            .isEmpty()) {
      return false;
    }
    const QString task_id =
        session
            .CallResult(QStringLiteral("library.import"),
                        QJsonObject{{"paths", QJsonArray{QDir(raw_root).filePath(raws.first())}}})
            .value("task_id")
            .toString();
    session.CallResult(QStringLiteral("tasks.wait"),
                       QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
    const QJsonArray items =
        session.CallResult(QStringLiteral("library.list")).value("items").toArray();
    if (items.size() != 1) {
      ADD_FAILURE() << "expected one photo, listed " << items.size();
      return false;
    }
    element_                 = items.first().toObject().value("element_id");
    const QJsonObject opened = session.CallResult(QStringLiteral("editor.open"),
                                                  QJsonObject{{"element_id", element_}}, 600000);
    if (opened.value("state").toString() != QStringLiteral("Interactive")) {
      ADD_FAILURE() << "editor.open did not reach Interactive";
      return false;
    }
    // The sensor layout of the open photo is read on a worker thread.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (session.host().editor_ai_denoise()->sensor_layout().isEmpty() &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  static auto Photo(InProcessAutomationSession& session) -> QJsonObject {
    return session.CallResult(QStringLiteral("denoise.status")).value("photo").toObject();
  }

  static auto Commits(InProcessAutomationSession& session) -> QJsonArray {
    return session.CallResult(QStringLiteral("editor.history")).value("commits").toArray();
  }

  /// Skips the test when this computer cannot run the model (no supported device).
  static auto PlatformAvailable() -> bool {
    return PipelineExecutor::ProcessAiDenoiseSession()->IsPlatformAvailable();
  }

  HeadlessFrameSink sink_;
  QJsonValue        element_;
};

TEST_F(AutomationDenoiseCommandsTest, StatusWithoutPhotoReportsPlatformAndMissingModel) {
  InProcessAutomationSession session;
  GraphFolder                folder(temp_dir_ / "no_model");

  const QJsonObject          status = session.CallResult(QStringLiteral("denoise.status"));
  EXPECT_EQ(status.value("available").toBool(), PlatformAvailable());
  const QJsonObject model = status.value("model").toObject();
  EXPECT_FALSE(model.value("installed").toBool());
  EXPECT_TRUE(model.value("version").isNull());
  EXPECT_EQ(model.value("problem").toString(), QStringLiteral("model_not_installed"));
  EXPECT_TRUE(model.value("reason").toString().contains(QStringLiteral("not installed")))
      << model.value("reason").toString().toStdString();
  EXPECT_TRUE(status.value("photo").isNull());

  const auto set =
      Error(session.Call(QStringLiteral("denoise.set"), QJsonObject{{"enabled", true}}));
  ASSERT_TRUE(set.has_value());
  EXPECT_EQ(set->code, AutomationErrorCode::NotReady);
}

TEST_F(AutomationDenoiseCommandsTest, StatusReportsVersionOfInstalledGraph) {
  InProcessAutomationSession session;
  GraphFolder                folder(temp_dir_ / "model");
  folder.Write("graph v1");

  const QJsonObject model =
      session.CallResult(QStringLiteral("denoise.status")).value("model").toObject();
  EXPECT_TRUE(model.value("installed").toBool());
  EXPECT_EQ(model.value("version").toString(), InstalledVersion());
  EXPECT_EQ(model.value("version").toString().size(), 64);
  EXPECT_TRUE(model.value("problem").isNull());
}

TEST_F(AutomationDenoiseCommandsTest, SetRejectsUnknownAndMissingParameters) {
  InProcessAutomationSession session;
  const auto missing = Error(session.Call(QStringLiteral("denoise.set"), QJsonObject{}));
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ(missing->code, AutomationErrorCode::InvalidParams);
  const auto unknown = Error(session.Call(QStringLiteral("denoise.set"),
                                          QJsonObject{{"enabled", true}, {"strength", 50}}));
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(unknown->code, AutomationErrorCode::InvalidParams);
}

TEST_F(AutomationDenoiseCommandsTest, TurnOnWithoutModelIsRefusedWithoutCommit) {
  InProcessAutomationSession session;
  GraphFolder                folder(temp_dir_ / "no_model");
  ASSERT_TRUE(OpenCiPhoto(session));
  const QJsonObject before = Photo(session);
  EXPECT_EQ(before.value("element_id"), element_);
  EXPECT_EQ(before.value("sensor").toString(), QStringLiteral("bayer"));
  EXPECT_FALSE(before.value("enabled").toBool());
  EXPECT_TRUE(before.value("model_version").isNull());
  EXPECT_TRUE(before.value("error").isNull());
  const auto commits = Commits(session).size();

  const auto refused =
      Error(session.Call(QStringLiteral("denoise.set"), QJsonObject{{"enabled", true}}));
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, AutomationErrorCode::Rejected);
  EXPECT_EQ(refused->data.toObject().value("refusal").toString(),
            PlatformAvailable() ? QStringLiteral("model_not_installed")
                                : QStringLiteral("platform_unavailable"));
  EXPECT_EQ(Commits(session).size(), commits);
  EXPECT_FALSE(Photo(session).value("enabled").toBool());
  session.host().Shutdown();
}

// The recovery of an agent: a model run that fails keeps the commit of the switch and reports
// the error in denoise.set, denoise.status, and render.preview; denoise.set off renders again.
TEST_F(AutomationDenoiseCommandsTest, FailedModelRunKeepsCommitAndReportsErrorUntilTurnedOff) {
  if (!PlatformAvailable()) {
    GTEST_SKIP() << "This computer has no AI Denoise device.";
  }
  InProcessAutomationSession session;
  GraphFolder                folder(temp_dir_ / "broken_model");
  folder.Write("not an onnx graph");
  ASSERT_TRUE(OpenCiPhoto(session));
  const auto commits = Commits(session).size();

  const auto failed  = Error(
      session.Call(QStringLiteral("denoise.set"),
                    QJsonObject{{"enabled", true}, {"note", "agent: high ISO noise"}}, 600000));
  ASSERT_TRUE(failed.has_value());
  EXPECT_EQ(failed->code, AutomationErrorCode::Failed);
  const QJsonObject data = failed->data.toObject();
  EXPECT_TRUE(data.value("enabled").toBool());
  EXPECT_FALSE(data.value("head_commit").toString().isEmpty());
  EXPECT_EQ(data.value("ai_denoise_error").toObject().value("kind").toString(),
            QStringLiteral("run_failed"));
  ASSERT_EQ(Commits(session).size(), commits + 1);
  const QJsonObject commit = Commits(session).last().toObject();
  EXPECT_EQ(commit.value("field").toString(), QStringLiteral("raw_decode"));
  EXPECT_EQ(commit.value("author").toString(), QStringLiteral("agent"));
  EXPECT_EQ(commit.value("note").toString(), QStringLiteral("agent: high ISO noise"));

  const QJsonObject on = Photo(session);
  EXPECT_TRUE(on.value("enabled").toBool());
  EXPECT_EQ(on.value("model_version").toString(), InstalledVersion());
  EXPECT_EQ(on.value("error").toObject().value("kind").toString(), QStringLiteral("run_failed"));

  const auto preview =
      Error(session.Call(QStringLiteral("render.preview"),
                         QJsonObject{{"out_dir", QDir(Folder()).filePath("previews")},
                                     {"compare", "none"},
                                     {"long_edge", 256}},
                         600000));
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->data.toObject().value("ai_denoise_error").toObject().value("kind").toString(),
            QStringLiteral("run_failed"));

  const QJsonObject off =
      session.CallResult(QStringLiteral("denoise.set"),
                         QJsonObject{{"enabled", false}, {"note", "agent: turn off"}}, 600000);
  const QJsonObject photo = off.value("photo").toObject();
  EXPECT_FALSE(photo.value("enabled").toBool());
  EXPECT_TRUE(photo.value("model_version").isNull());
  EXPECT_TRUE(photo.value("error").isNull());
  EXPECT_EQ(off.value("head_commit"),
            session.CallResult(QStringLiteral("editor.history")).value("head_commit"));
  EXPECT_EQ(Commits(session).size(), commits + 2);
  session.host().Shutdown();
}

// Needs the FP16 graph of the lab release in ALCEDO_AI_DENOISE_TEST_ASSETS.
TEST_F(AutomationDenoiseCommandsTest, TurnOnWithModelRendersDenoisedPhoto) {
  const char* assets = std::getenv("ALCEDO_AI_DENOISE_TEST_ASSETS");
  const auto  graph =
      assets != nullptr ? std::filesystem::path(assets) / kGraphFileName : std::filesystem::path{};
  if (graph.empty() || !std::filesystem::is_regular_file(graph)) {
    GTEST_SKIP() << "Set ALCEDO_AI_DENOISE_TEST_ASSETS to a folder with the FP16 graph.";
  }
  if (!PlatformAvailable()) {
    GTEST_SKIP() << "This computer has no AI Denoise device.";
  }
  InProcessAutomationSession session;
  GraphFolder                folder(temp_dir_ / "model");
  std::filesystem::copy_file(graph, folder.GraphPath(),
                             std::filesystem::copy_options::overwrite_existing);
  ASSERT_TRUE(OpenCiPhoto(session));
  const QJsonValue head_before =
      session.CallResult(QStringLiteral("editor.history")).value("head_commit");

  const QJsonObject on =
      session.CallResult(QStringLiteral("denoise.set"),
                         QJsonObject{{"enabled", true}, {"note", "agent: denoise"}}, 600000);
  const QJsonObject photo = on.value("photo").toObject();
  EXPECT_TRUE(photo.value("enabled").toBool());
  EXPECT_EQ(photo.value("model_version").toString(), InstalledVersion());
  EXPECT_TRUE(photo.value("error").isNull());
  EXPECT_NE(on.value("head_commit"), head_before);

  // A second write of the same switch makes no commit.
  const auto commits = Commits(session).size();
  EXPECT_EQ(
      session.CallResult(QStringLiteral("denoise.set"), QJsonObject{{"enabled", true}}, 600000)
          .value("head_commit"),
      on.value("head_commit"));
  EXPECT_EQ(Commits(session).size(), commits);

  const QJsonObject preview =
      session.CallResult(QStringLiteral("render.preview"),
                         QJsonObject{{"out_dir", QDir(Folder()).filePath("previews")},
                                     {"compare", "none"},
                                     {"long_edge", 512}},
                         600000);
  const QJsonObject current = preview.value("current").toObject();
  EXPECT_TRUE(QFileInfo::exists(current.value("path").toString()));
  EXPECT_EQ(std::max(current.value("width").toInt(), current.value("height").toInt()), 512);
  EXPECT_TRUE(Photo(session).value("error").isNull());
  session.host().Shutdown();
}

}  // namespace
}  // namespace alcedo::automation
