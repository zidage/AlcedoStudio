//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Preset commands of the Versions panel (edit_presets_plan.md, PS2). The editor session is an
// EditorSessionController over a backend that holds a real CommitGraph and pastes with
// AdjustmentTransferService, so each test reads the real history after a command.

#include "ui/alcedo_main/album_backend/preset_controller.hpp"

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "app/adjustment_transfer_package_builder.hpp"
#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "app/preset_file.hpp"
#include "app/preset_library_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/preset_test_support.hpp"
#include "support/test_temp_directory.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/preset_list_model.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs = std::filesystem;
using alcedo::test::HistorySessionBackend;

constexpr sl_element_id_t kElementId = 7;

void WriteBytes(const fs::path& path, const std::string& bytes) {
  alcedo::test::WritePresetBytes(path, bytes);
}

auto PresetText(double exposure_ev) -> std::string {
  return alcedo::test::ExposurePresetText(exposure_ev);
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

auto EntryIds(const PresetLibraryService& service) -> std::vector<std::string> {
  std::vector<std::string> ids;
  for (const auto& entry : service.Entries()) {
    ids.push_back(entry.id);
  }
  return ids;
}

/// Display name of the active Version of @p graph.
auto ActiveVersionName(const CommitGraph& graph) -> std::string {
  return graph.GetActiveVersionRef().display_name;
}

class PresetControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::create_directories(root_);
    session_.Open(kElementId, 1);
  }

  /// Starts the service on root_ and the controller over it.
  void StartLibrary() {
    library_ =
        std::make_unique<PresetLibraryService>(alcedo::test::PresetLibraryOptionsOn(root_));
    library_->Start();
    ASSERT_TRUE(WaitUntilIdle());
    controller_ = std::make_unique<PresetController>(
        library_.get(), &dialog_, [](const QUrl&) { return true; },
        [this](const QString&, const QString& suggested_path, const QString&) {
          suggested_path_ = suggested_path;
          return chosen_path_;
        });
    controller_->SetEditorSession(&session_);
  }

  auto WaitUntilIdle() -> bool {
    return QTest::qWaitFor([this] { return !library_->busy(); }, 20000);
  }

  /// Pastes @p package onto the image root as a new active Version named @p name.
  auto AddVersion(const AdjustmentTransferPackage& package, const std::string& name)
      -> version_ref_id_t {
    const auto pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(
        backend_.graph(), backend_.root_document(), package, name);
    EXPECT_TRUE(pasted.pasted) << pasted.error;
    return pasted.new_version_id;
  }

  /// The package of the saved preset @p id, read from the folder.
  auto SavedPackage(const std::string& id) -> std::optional<AdjustmentTransferPackage> {
    const auto read = ReadPresetFile(root_ / fs::path(id));
    EXPECT_TRUE(read.Ok()) << read.message;
    return read.package;
  }

  alcedo::test::TestTempDirectory       directory_;
  fs::path                              root_ = directory_ / "presets";
  HistorySessionBackend                 backend_{kElementId};
  EditorSessionController               session_{&backend_};
  AdjustmentTransferDialogModel         dialog_;
  std::unique_ptr<PresetLibraryService> library_;
  std::unique_ptr<PresetController>     controller_;
  // The system save dialog of ExportVersion: the path it suggested and the path it returns.
  QString                               suggested_path_;
  QString                               chosen_path_;
};

/// A package with exposure, a rotation, and Lens Correction: a Version made from it has all
/// three in its document.
auto PackageWithGeometryAndLens(double exposure_ev) -> AdjustmentTransferPackage {
  auto source = alcedo::test::DocumentWithExposureEv(exposure_ev);
  source.Geometry().SetRotationDegrees(12.0f);
  return AdjustmentTransferPackageBuilder::Build(source, SelectAllTransferableItems(source));
}

auto ExposureOf(const AdjustmentTransferPackage& package) -> std::optional<double> {
  for (const auto& grade : package.color_grades_) {
    for (const auto& adjustment : grade.adjustments) {
      if (adjustment.type != type_ids::Exposure()) {
        continue;
      }
      // The scalar Model stores its value as the only number of its parameters.
      for (const auto& [key, value] : adjustment.params.items()) {
        if (value.is_number()) {
          return value.get<double>();
        }
      }
    }
  }
  return std::nullopt;
}

TEST_F(PresetControllerTest, SaveVersionAsPresetWritesAllItemsExceptGeometryAndLens) {
  StartLibrary();
  const auto version = AddVersion(PackageWithGeometryAndLens(1.5), "Look A");
  QSignalSpy finished(controller_.get(), &PresetController::OperationFinished);

  const auto result = controller_->SaveVersionAsPreset(QString::fromStdString(version.ToString()));
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  ASSERT_TRUE(finished.wait(20000));
  const auto done = finished.takeLast().front().toMap();
  EXPECT_EQ(done.value("operation").toString(), QStringLiteral("save"));
  ASSERT_TRUE(done.value("success").toBool()) << done.value("message").toString().toStdString();
  EXPECT_EQ(done.value("presetIds").toList(), (QVariantList{QStringLiteral("Look A.alcpreset")}));

  const auto package = SavedPackage("Look A.alcpreset");
  ASSERT_TRUE(package.has_value());
  EXPECT_FALSE(package->develop_.geometry.has_value());
  EXPECT_FALSE(package->develop_.lens_calib.has_value());
  EXPECT_TRUE(package->develop_.raw_decode.has_value());
  EXPECT_TRUE(package->develop_.color_temp.has_value());
  EXPECT_EQ(ExposureOf(*package), std::optional<double>{1.5});
  EXPECT_TRUE(package->drt_post_.params.has_value());
}

TEST_F(PresetControllerTest, SaveVersionAsPresetReadsVersionThatIsNotActive) {
  StartLibrary();
  const auto look_a = AddVersion(alcedo::test::MakeExposureTransferPackage(1.5), "Look A");
  const auto look_b = AddVersion(alcedo::test::MakeExposureTransferPackage(-0.5), "Look B");
  ASSERT_EQ(backend_.graph().GetActiveVersionId(), look_b);
  const auto  commit_count = backend_.graph().CommitCount();

  std::string error;
  const auto  read = AdjustmentTransferCatalogService::ReadVersion(
      backend_.graph(), backend_.root_document(), look_a, &error);
  ASSERT_TRUE(read.has_value()) << error;
  const auto expected =
      AdjustmentTransferPackageBuilder::Build(read->document, SelectPresetItems(read->document));

  ASSERT_TRUE(controller_->SaveVersionAsPreset(QString::fromStdString(look_a.ToString()))
                  .value("success")
                  .toBool());
  ASSERT_TRUE(WaitUntilIdle());
  const auto package = SavedPackage("Look A.alcpreset");
  ASSERT_TRUE(package.has_value());
  EXPECT_EQ(package->fingerprint_, expected.fingerprint_);
  EXPECT_EQ(ExposureOf(*package), std::optional<double>{1.5});
  EXPECT_EQ(backend_.graph().GetActiveVersionId(), look_b);
  EXPECT_EQ(backend_.graph().CommitCount(), commit_count);
}

TEST_F(PresetControllerTest, SaveDialogSelectionSavesOnlyTheSelectedItems) {
  StartLibrary();
  AddVersion(PackageWithGeometryAndLens(0.75), "Dialog Look");
  std::shared_ptr<const CommitGraph>      graph;
  std::shared_ptr<const PipelineDocument> root_document;
  ASSERT_TRUE(session_.SnapshotHistorySource(&graph, &root_document, nullptr));
  std::string error;
  ASSERT_TRUE(dialog_.OpenSource(graph, root_document, &error)) << error;
  dialog_.SetAllNodesChecked(false);
  // Check the Exposure item of the Color Grade that holds it; ids come from the dialog rows.
  bool  checked = false;
  auto* nodes   = dialog_.nodes();
  for (int row = 0; row < nodes->rowCount() && !checked; ++row) {
    const auto node_id =
        nodes->data(nodes->index(row, 0), AdjustmentTransferNodeListModel::NodeIdRole).toString();
    dialog_.FocusNode(node_id);
    auto* items = dialog_.items();
    for (int item = 0; item < items->rowCount(); ++item) {
      const auto index = items->index(item, 0);
      if (items->data(index, AdjustmentTransferItemListModel::DisplayNameRole).toString() ==
          QStringLiteral("Exposure")) {
        dialog_.SetItemChecked(
            node_id, items->data(index, AdjustmentTransferItemListModel::ItemKeyRole).toString(),
            true);
        checked = true;
        break;
      }
    }
  }
  ASSERT_TRUE(checked);
  ASSERT_TRUE(dialog_.can_copy());

  const auto result = controller_->SaveDialogSelectionAsPreset();
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  ASSERT_TRUE(WaitUntilIdle());
  const auto package = SavedPackage("Dialog Look.alcpreset");
  ASSERT_TRUE(package.has_value());
  EXPECT_TRUE(package->develop_.Empty());
  EXPECT_TRUE(package->drt_post_.Empty());
  ASSERT_EQ(package->color_grades_.size(), 1u);
  ASSERT_EQ(package->color_grades_.front().adjustments.size(), 1u);
  EXPECT_EQ(ExposureOf(*package), std::optional<double>{0.75});
}

TEST_F(PresetControllerTest, ApplyToEditorCreatesActiveVersionNamedAfterPreset) {
  WriteBytes(root_ / "Film" / "Warm.alcpreset", PresetText(0.5));
  StartLibrary();
  const auto version_count = backend_.graph().GetAllVersionRefs().size();

  const auto result        = controller_->ApplyToEditor(QStringLiteral("Film/Warm.alcpreset"));

  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(backend_.graph().GetAllVersionRefs().size(), version_count + 1);
  EXPECT_EQ(ActiveVersionName(backend_.graph()), "Warm");
  std::string error;
  const auto  read = AdjustmentTransferCatalogService::ReadVersion(
      backend_.graph(), backend_.root_document(), backend_.graph().GetActiveVersionId(), &error);
  ASSERT_TRUE(read.has_value()) << error;
  EXPECT_EQ(ExposureOf(AdjustmentTransferPackageBuilder::Build(read->document,
                                                               SelectPresetItems(read->document))),
            std::optional<double>{0.5});
}

TEST_F(PresetControllerTest, ApplyFileToEditorLeavesLibraryUnchanged) {
  WriteBytes(root_ / "Warm.alcpreset", PresetText(0.5));
  const fs::path outside = directory_ / "downloads" / "Cool.alcpreset";
  WriteBytes(outside, PresetText(-1.0));
  StartLibrary();
  const auto listing_before = FolderListing(root_);
  const auto ids_before     = EntryIds(*library_);
  const auto reads_before   = library_->PackageFileReadCount();

  const auto result         = controller_->ApplyFileToEditor(
      QUrl::fromLocalFile(QString::fromStdWString(outside.wstring())).toString());

  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(ActiveVersionName(backend_.graph()), "Cool");
  EXPECT_FALSE(library_->busy());
  EXPECT_EQ(FolderListing(root_), listing_before);
  EXPECT_EQ(EntryIds(*library_), ids_before);
  EXPECT_EQ(library_->PackageFileReadCount(), reads_before);
}

TEST_F(PresetControllerTest, ApplyOfUnavailablePresetChangesNoHistory) {
  WriteBytes(root_ / "Broken.alcpreset", "{\"schema\":1}");
  const fs::path hostile = directory_ / "Hostile.alcpreset";
  auto           json = ExportDocumentTransfer(alcedo::test::MakeLutTransferPackage("C:/x.cube"));
  WriteBytes(hostile, json.dump());
  StartLibrary();
  const auto version_count = backend_.graph().GetAllVersionRefs().size();
  const auto commit_count  = backend_.graph().CommitCount();
  const auto active        = backend_.graph().GetActiveVersionId();

  const auto broken        = controller_->ApplyToEditor(QStringLiteral("Broken.alcpreset"));
  const auto missing       = controller_->ApplyToEditor(QStringLiteral("Absent.alcpreset"));
  const auto file = controller_->ApplyFileToEditor(QString::fromStdWString(hostile.wstring()));

  for (const auto& result : {broken, missing, file}) {
    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_FALSE(result.value("message").toString().isEmpty());
  }
  EXPECT_NE(file.value("message").toString().indexOf(QStringLiteral("LUT")), -1)
      << file.value("message").toString().toStdString();
  EXPECT_EQ(backend_.graph().GetAllVersionRefs().size(), version_count);
  EXPECT_EQ(backend_.graph().CommitCount(), commit_count);
  EXPECT_EQ(backend_.graph().GetActiveVersionId(), active);
}

TEST_F(PresetControllerTest, ApplyIsRefusedWhileAgentHoldsControl) {
  WriteBytes(root_ / "Warm.alcpreset", PresetText(0.5));
  StartLibrary();
  BackgroundTaskController    tasks;
  InteractionPolicyController policy(&tasks);
  BackgroundTaskSnapshot      task;
  task.kind_            = BackgroundTaskKind::AgentControl;
  task.state_           = BackgroundTaskState::Running;
  task.title_           = QStringLiteral("agent");
  task.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  task.locks_           = {InteractionLock{InteractionCapability::UserStateChange, 0,
                                 QStringLiteral("An agent controls the application.")}};
  (void)tasks.RegisterTask(task);
  session_.SetInteractionPolicy(&policy);
  const auto commit_count = backend_.graph().CommitCount();

  const auto result       = controller_->ApplyToEditor(QStringLiteral("Warm.alcpreset"));

  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_EQ(result.value("message").toString(), policy.ChangeStateAsUserReason());
  EXPECT_EQ(backend_.graph().CommitCount(), commit_count);
  session_.SetInteractionPolicy(nullptr);
}

// Issue #1124: Export Version writes the package of a drag-saved preset (no Crop and Rotate,
// no Lens Correction) to the file of the save dialog, replaces a file there that the dialog
// confirmed, and adds no preset to the folder.
TEST_F(PresetControllerTest, ExportVersionWritesPresetFileAtChosenPathAndReplacesIt) {
  StartLibrary();
  const auto version = AddVersion(PackageWithGeometryAndLens(1.5), "Look A");
  const auto target  = directory_ / "shared" / "Look A.alcpreset";
  fs::create_directories(target.parent_path());
  WriteBytes(target, "not a preset");
  chosen_path_ = QString::fromStdWString(target.wstring());
  QSignalSpy finished(controller_.get(), &PresetController::OperationFinished);

  const auto result = controller_->ExportVersion(QString::fromStdString(version.ToString()));
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_TRUE(result.value("pending").toBool());
  EXPECT_EQ(QFileInfo(suggested_path_).fileName(), QStringLiteral("Look A.alcpreset"));
  ASSERT_TRUE(finished.wait(20000));
  const auto done = finished.takeLast().front().toMap();
  EXPECT_EQ(done.value("operation").toString(), QStringLiteral("export"));
  ASSERT_TRUE(done.value("success").toBool()) << done.value("message").toString().toStdString();

  const auto read = ReadPresetFile(target);
  ASSERT_TRUE(read.Ok()) << read.message;
  EXPECT_FALSE(read.package->develop_.geometry.has_value());
  EXPECT_FALSE(read.package->develop_.lens_calib.has_value());
  EXPECT_EQ(ExposureOf(*read.package), std::optional<double>{1.5});
  EXPECT_EQ(FolderListing(target.parent_path()), (std::vector<std::string>{"Look A.alcpreset"}));
  EXPECT_TRUE(FolderListing(root_).empty());
  EXPECT_TRUE(EntryIds(*library_).empty());
}

TEST_F(PresetControllerTest, ExportVersionCanceledInDialogWritesNothing) {
  StartLibrary();
  const auto version = AddVersion(alcedo::test::MakeExposureTransferPackage(0.5), "Look A");
  chosen_path_.clear();

  const auto result = controller_->ExportVersion(QString::fromStdString(version.ToString()));
  EXPECT_TRUE(result.value("success").toBool());
  EXPECT_TRUE(result.value("canceled").toBool());
  EXPECT_FALSE(library_->busy());
  EXPECT_TRUE(FolderListing(root_).empty());
}

TEST(VersionExportFileNameTest, PhotoNameThenVersionNameMadeValid) {
  EXPECT_EQ(VersionExportFileName(QStringLiteral("DSC_0001.ARW"), QStringLiteral("Look A")),
            QStringLiteral("DSC_0001_Look A.alcpreset"));
  EXPECT_EQ(VersionExportFileName(QStringLiteral("trip.day1.NEF"), QStringLiteral("Warm/Cool")),
            QStringLiteral("trip.day1_Warm-Cool.alcpreset"));
  EXPECT_EQ(VersionExportFileName(QString(), QStringLiteral("Look A")),
            QStringLiteral("Look A.alcpreset"));
}

TEST_F(PresetControllerTest, ListModelShowsFolderPresetsBeforeGroups) {
  WriteBytes(root_ / "Zed.alcpreset", PresetText(0.5));
  WriteBytes(root_ / "Film" / "amber.alcpreset", PresetText(1.0));
  WriteBytes(root_ / "Apple.alcpreset", PresetText(-0.5));
  StartLibrary();
  auto* model = controller_->presets();
  ASSERT_EQ(model->rowCount(), 3);

  QStringList ids;
  for (int row = 0; row < model->rowCount(); ++row) {
    ids.push_back(model->roleAt(row, PresetListModel::PresetIdRole).toString());
  }
  EXPECT_EQ(ids, (QStringList{QStringLiteral("Apple.alcpreset"), QStringLiteral("Zed.alcpreset"),
                              QStringLiteral("Film/amber.alcpreset")}));
  EXPECT_EQ(model->roleAt(2, PresetListModel::GroupRole).toString(), QStringLiteral("Film"));
  EXPECT_EQ(model->rowOfPreset(QStringLiteral("Zed.alcpreset")), 1);
}

TEST(PresetNameFromLabelTest, LabelBecomesValidPresetName) {
  EXPECT_EQ(PresetNameFromLabel("Look A"), "Look A");
  EXPECT_EQ(PresetNameFromLabel(" Warm/Cool: v2. "), "Warm-Cool- v2");
  EXPECT_EQ(PresetNameFromLabel("   "), "Preset");
  EXPECT_EQ(PresetNameFromLabel("CON"), "CON Preset");
  EXPECT_EQ(PresetNameFromLabel(std::string(200, 'a')).size(), kPresetMaxNameLength);
  for (const auto* label : {"a\tb", "<x>", "name.", "LPT1", "x|y?z*"}) {
    EXPECT_TRUE(PresetNameError(PresetNameFromLabel(label)).empty()) << label;
  }
}

}  // namespace
}  // namespace alcedo::ui::test
