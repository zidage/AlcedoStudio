//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// presets.list, presets.save, and presets.apply (edit_presets_plan.md, PS4B) in an in-process
// headless session whose preset folder is a temporary folder.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTest>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/pipeline_service.hpp"
#include "app/preset_file.hpp"
#include "app/preset_library_service.hpp"
#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/preset_test_support.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::automation {
namespace {

namespace fs = std::filesystem;
using test::InProcessAutomationSession;

auto ErrorCode(const std::optional<AutomationResponse>& response)
    -> std::optional<AutomationErrorCode> {
  if (!response.has_value() || !response->error.has_value()) {
    return std::nullopt;
  }
  return response->error->code;
}

auto ErrorPointer(const std::optional<AutomationResponse>& response) -> QString {
  if (!response.has_value() || !response->error.has_value()) {
    return {};
  }
  return response->error->data.toObject().value("pointer").toString();
}

/// File names below @p folder, recursive, in sorted order.
auto FolderListing(const fs::path& folder) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& entry : fs::recursive_directory_iterator(folder)) {
    names.push_back(fs::relative(entry.path(), folder).generic_string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

class AutomationPresetCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    preset_root_ = temp_dir_ / "presets";
    fs::create_directories(preset_root_);
    alcedo::test::WritePresetBytes(preset_root_ / "Warm.alcpreset",
                                   alcedo::test::ExposurePresetText(0.5));
  }

  void TearDown() override {
    session_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  /// A headless session on preset_root_. With @p photo_count above 0, a packed project of that
  /// many photos is open.
  auto Start(std::size_t photo_count) -> bool {
    ui::ApplicationModuleHost::PackageServiceFactories factories;
    factories.preset_library_options = [root = preset_root_] {
      return alcedo::test::PresetLibraryOptionsOn(root);
    };
    session_ = std::make_unique<InProcessAutomationSession>(QStringLiteral("headless"),
                                                            std::move(factories));
    if (!QTest::qWaitFor([this] { return !Library().busy(); }, 20000)) {
      return false;
    }
    if (photo_count == 0) {
      return true;
    }
    fs::create_directories(temp_dir_ / "seed");
    auto seeded = ui::test::CreatePopulatedPackedProject(temp_dir_ / "seed", {}, photo_count);
    if (!seeded.has_value()) {
      return false;
    }
    photos_ = seeded->images_;
    return !session_
                ->CallResult(QStringLiteral("project.open"),
                             QJsonObject{{"path", ui::test::PathToQString(seeded->packed_path_)}})
                .isEmpty();
  }

  auto Library() -> PresetLibraryService& { return *session_->host().preset_library(); }
  auto Pipelines() -> std::shared_ptr<PipelineMgmtService> {
    return session_->host().project()->handler().pipeline_service();
  }
  auto PhotoIds() const -> QJsonArray {
    QJsonArray ids;
    for (const auto& photo : photos_) {
      ids.push_back(static_cast<double>(photo.file_id_));
    }
    return ids;
  }

  fs::path                                          preset_root_;
  std::unique_ptr<InProcessAutomationSession>       session_;
  std::vector<ui::test::PopulatedProject::ImageKey> photos_;
};

TEST_F(AutomationPresetCommandsTest, PresetsListShowsFolderPresetsBeforeGroupsWithReasons) {
  alcedo::test::WritePresetBytes(preset_root_ / "Film" / "amber.alcpreset",
                                 alcedo::test::ExposurePresetText(1.0));
  alcedo::test::WritePresetBytes(preset_root_ / "Broken.alcpreset", "{\"schema\":1}");
  ASSERT_TRUE(Start(0));

  const QJsonObject listed =
      session_->CallResult(QStringLiteral("presets.list"), QJsonObject{{"details", true}});
  EXPECT_TRUE(
      fs::equivalent(fs::path(listed.value("folder").toString().toStdWString()), preset_root_))
      << listed.value("folder").toString().toStdString();
  const QJsonArray presets = listed.value("presets").toArray();
  ASSERT_EQ(presets.size(), 3);
  QStringList ids;
  for (const auto& preset : presets) {
    ids.push_back(preset.toObject().value("preset_id").toString());
  }
  EXPECT_EQ(ids, (QStringList{QStringLiteral("Broken.alcpreset"), QStringLiteral("Warm.alcpreset"),
                              QStringLiteral("Film/amber.alcpreset")}));
  const QJsonObject broken = presets.at(0).toObject();
  EXPECT_FALSE(broken.value("available").toBool());
  EXPECT_FALSE(broken.value("reason").toString().isEmpty());
  EXPECT_FALSE(broken.contains("nodes"));
  const QJsonObject warm = presets.at(1).toObject();
  EXPECT_TRUE(warm.value("available").toBool());
  EXPECT_EQ(warm.value("name").toString(), QStringLiteral("Warm"));
  EXPECT_NE(QJsonDocument(warm.value("nodes").toArray()).toJson().indexOf("Exposure"), -1);
  EXPECT_EQ(presets.at(2).toObject().value("group").toString(), QStringLiteral("Film"));
}

TEST_F(AutomationPresetCommandsTest, PresetsListScansFolderForFilesAddedAfterStart) {
  ASSERT_TRUE(Start(0));
  ASSERT_EQ(Library().entry_count(), 1);
  // The service has no file watcher; a file that the user copied in is listed at the next list.
  alcedo::test::WritePresetBytes(preset_root_ / "Copied.alcpreset",
                                 alcedo::test::ExposurePresetText(0.25));

  const QJsonArray presets = session_->CallResult(QStringLiteral("presets.list"), QJsonObject{})
                                 .value("presets")
                                 .toArray();

  ASSERT_EQ(presets.size(), 2);
  EXPECT_EQ(presets.at(0).toObject().value("preset_id").toString(),
            QStringLiteral("Copied.alcpreset"));
  EXPECT_TRUE(presets.at(0).toObject().value("available").toBool());
  EXPECT_FALSE(presets.at(0).toObject().contains("nodes"));
  EXPECT_EQ(Library().entry_count(), 2);
}

TEST_F(AutomationPresetCommandsTest, PresetsSaveAnswersItsOwnPresetWithoutGeometryAndLens) {
  ASSERT_TRUE(Start(1));
  // A save of the GUI waits in the worker queue before the agent save.
  ASSERT_EQ(Library().Save(alcedo::test::MakeExposureTransferPackage(2.0), "GUI Look"),
            PresetLibraryService::Status::kOk);

  const QJsonObject saved = session_->CallResult(
      QStringLiteral("presets.save"),
      QJsonObject{{"element_id", static_cast<double>(photos_.front().file_id_)},
                  {"name", "Agent Look"}});

  EXPECT_EQ(saved.value("preset_id").toString(), QStringLiteral("Agent Look.alcpreset"));
  EXPECT_EQ(saved.value("name").toString(), QStringLiteral("Agent Look"));
  EXPECT_FALSE(saved.value("nodes").toArray().isEmpty());
  ASSERT_TRUE(QTest::qWaitFor([this] { return !Library().busy(); }, 20000));
  EXPECT_NE(Library().FindEntry("GUI Look.alcpreset"), nullptr);
  const auto read = ReadPresetFile(preset_root_ / "Agent Look.alcpreset");
  ASSERT_TRUE(read.Ok()) << read.message;
  EXPECT_FALSE(read.package->develop_.geometry.has_value());
  EXPECT_FALSE(read.package->develop_.lens_calib.has_value());

  const auto bad_name = session_->Call(
      QStringLiteral("presets.save"),
      QJsonObject{{"element_id", static_cast<double>(photos_.front().file_id_)}, {"name", "a/b"}});
  ASSERT_EQ(ErrorCode(bad_name), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(bad_name), QStringLiteral("/name"));
}

TEST_F(AutomationPresetCommandsTest, PresetsApplyCommandStoresAgentAttribution) {
  ASSERT_TRUE(Start(3));
  const QJsonObject applied = session_->CallResult(QStringLiteral("presets.apply"),
                                                   QJsonObject{{"preset_id", "Warm.alcpreset"},
                                                               {"element_ids", PhotoIds()},
                                                               {"note", "warm the series"}});
  EXPECT_EQ(applied.value("target_count").toInt(), 3);
  const QString task_id = applied.value("task_id").toString();
  ASSERT_FALSE(task_id.isEmpty());
  const QJsonObject task = session_->CallResult(
      QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"))
      << QJsonDocument(task).toJson().toStdString();
  EXPECT_EQ(task.value("result").toObject().value("applied_count").toInt(), 3);

  for (const auto& photo : photos_) {
    const auto  history = Pipelines()->LoadHistorySnapshot(photo.file_id_);
    const auto& active  = history.graph_->GetActiveVersionRef();
    EXPECT_EQ(active.display_name, "Warm");
    ASSERT_TRUE(active.head_commit_hash.has_value());
    const auto* attribution = history.graph_->FindCommitAttribution(*active.head_commit_hash);
    ASSERT_NE(attribution, nullptr);
    EXPECT_EQ(attribution->author_, kEditCommitAuthorAgent);
    EXPECT_EQ(attribution->note_, "warm the series");
  }
}

TEST_F(AutomationPresetCommandsTest, PresetsApplyFileLeavesLibraryUnchanged) {
  const fs::path outside = temp_dir_ / "downloads" / "Cool.alcpreset";
  alcedo::test::WritePresetBytes(outside, alcedo::test::ExposurePresetText(-1.0));
  ASSERT_TRUE(Start(2));
  const auto        listing_before = FolderListing(preset_root_);
  const auto        reads_before   = Library().PackageFileReadCount();

  const QJsonObject applied =
      session_->CallResult(QStringLiteral("presets.apply"),
                           QJsonObject{{"file", QString::fromStdWString(outside.wstring())},
                                       {"element_ids", PhotoIds()}});
  const QJsonObject task = session_->CallResult(
      QStringLiteral("tasks.wait"),
      QJsonObject{{"task_id", applied.value("task_id")}, {"timeout_ms", 600000}});
  EXPECT_EQ(task.value("result").toObject().value("applied_count").toInt(), 2);

  for (const auto& photo : photos_) {
    EXPECT_EQ(
        Pipelines()->LoadHistorySnapshot(photo.file_id_).graph_->GetActiveVersionRef().display_name,
        "Cool");
  }
  EXPECT_EQ(FolderListing(preset_root_), listing_before);
  EXPECT_EQ(Library().FindEntry("Cool.alcpreset"), nullptr);
  EXPECT_EQ(Library().PackageFileReadCount(), reads_before);
}

TEST_F(AutomationPresetCommandsTest, PresetsApplyRefusesUnknownPresetBothSourcesAndHostileFile) {
  alcedo::test::WritePresetBytes(preset_root_ / "Broken.alcpreset", "{\"schema\":1}");
  const fs::path hostile = temp_dir_ / "Hostile.alcpreset";
  alcedo::test::WritePresetBytes(
      hostile, ExportDocumentTransfer(alcedo::test::MakeLutTransferPackage("C:/x.cube")).dump());
  ASSERT_TRUE(Start(1));
  const auto commits_before =
      Pipelines()->LoadHistorySnapshot(photos_.front().file_id_).graph_->CommitCount();
  const QJsonArray ids = PhotoIds();

  const auto       unknown =
      session_->Call(QStringLiteral("presets.apply"),
                     QJsonObject{{"preset_id", "Absent.alcpreset"}, {"element_ids", ids}});
  ASSERT_EQ(ErrorCode(unknown), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(unknown), QStringLiteral("/preset_id"));
  EXPECT_EQ(
      ErrorCode(session_->Call(QStringLiteral("presets.apply"),
                               QJsonObject{{"preset_id", "Warm.alcpreset"},
                                           {"file", QString::fromStdWString(hostile.wstring())},
                                           {"element_ids", ids}})),
      AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorCode(session_->Call(
                QStringLiteral("presets.apply"),
                QJsonObject{{"preset_id", "Broken.alcpreset"}, {"element_ids", ids}})),
            AutomationErrorCode::Rejected);
  const auto file = session_->Call(
      QStringLiteral("presets.apply"),
      QJsonObject{{"file", QString::fromStdWString(hostile.wstring())}, {"element_ids", ids}});
  ASSERT_EQ(ErrorCode(file), AutomationErrorCode::Rejected);
  EXPECT_NE(file->error->message.indexOf(QStringLiteral("LUT")), -1)
      << file->error->message.toStdString();
  // The headless host has no library selection, so an apply needs element_ids.
  EXPECT_EQ(ErrorCode(session_->Call(QStringLiteral("presets.apply"),
                                     QJsonObject{{"preset_id", "Warm.alcpreset"}})),
            AutomationErrorCode::NotReady);

  EXPECT_FALSE(session_->host().adjustment_transfer()->paste_in_progress());
  EXPECT_EQ(Pipelines()->LoadHistorySnapshot(photos_.front().file_id_).graph_->CommitCount(),
            commits_before);
}

TEST_F(AutomationPresetCommandsTest, PresetsSaveAndApplyWithoutProjectAreNotReady) {
  ASSERT_TRUE(Start(0));
  EXPECT_EQ(
      ErrorCode(session_->Call(QStringLiteral("presets.save"), QJsonObject{{"element_id", 1}})),
      AutomationErrorCode::NotReady);
  EXPECT_EQ(ErrorCode(session_->Call(
                QStringLiteral("presets.apply"),
                QJsonObject{{"preset_id", "Warm.alcpreset"}, {"element_ids", QJsonArray{1}}})),
            AutomationErrorCode::NotReady);
}

}  // namespace
}  // namespace alcedo::automation
