//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Apply Preset... for many photos (edit_presets_plan.md, PS4A): PresetController applies a preset
// to library photos through the multi-target Paste of AdjustmentTransferController, against a
// real packed project of the application module host; the dialog previews of a library photo
// render on the thumbnail executors of that project.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/pipeline_service.hpp"
#include "app/preset_library_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "support/preset_test_support.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/preset_controller.hpp"
#include "ui/alcedo_main/album_backend/preset_preview_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/saved_project_invariants.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs = std::filesystem;

/// Names of the Versions of the stored history of @p element_id.
auto StoredVersionNames(PipelineMgmtService& pipelines, sl_element_id_t element_id)
    -> std::vector<std::string> {
  const auto               history = pipelines.LoadHistorySnapshot(element_id);
  std::vector<std::string> names;
  for (const auto& [id, version] : history.graph_->GetAllVersionRefs()) {
    names.push_back(version.display_name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

auto CountOf(const std::vector<std::string>& names, const std::string& name) -> long long {
  return std::count(names.begin(), names.end(), name);
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

class PresetApplyTargetsTest : public ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    preset_root_ = temp_dir_ / "presets";
    fs::create_directories(preset_root_);
    alcedo::test::WritePresetBytes(preset_root_ / "Warm.alcpreset",
                                   alcedo::test::ExposurePresetText(0.5));
  }

  /// Closes the host, then checks the storage invariants of each saved project package (#805).
  void TearDown() override {
    host_.reset();
    ExpectSavedProjectInvariants(temp_dir_);
    ApplicationModuleHostTestFixture::TearDown();
  }

  /// A host whose preset folder is preset_root_, with a packed project of @p image_count photos
  /// without files, or of the files @p sources.
  auto StartHost(std::size_t image_count, const std::vector<fs::path>& sources = {}) -> bool {
    ApplicationModuleHost::PackageServiceFactories factories;
    factories.preset_library_options = [root = preset_root_] {
      return alcedo::test::PresetLibraryOptionsOn(root);
    };
    host_ = std::make_unique<ApplicationModuleHost>(
        nullptr, ApplicationModuleHost::LifecycleObserver{}, std::move(factories));
    auto seeded = CreatePopulatedPackedProject(temp_dir_, sources, image_count);
    if (!seeded.has_value() || !LoadPackedProject(*host_, seeded->packed_path_)) {
      return false;
    }
    project_ = std::move(*seeded);
    return QTest::qWaitFor([this] { return !host_->preset_library()->busy(); }, 20000) &&
           host_->preset_library()->FindEntry("Warm.alcpreset") != nullptr;
  }

  /// `{elementId, imageId}` maps of the photos @p first to @p first + @p count - 1.
  auto Targets(std::size_t first, std::size_t count) const -> QVariantList {
    QVariantList targets;
    for (std::size_t i = first; i < first + count; ++i) {
      const auto& image = project_.images_.at(i);
      targets.push_back(QVariantMap{{"elementId", static_cast<uint>(image.file_id_)},
                                    {"imageId", static_cast<uint>(image.image_id_)}});
    }
    return targets;
  }

  /// Starts the apply with @p start and returns the PasteFinished result.
  template <typename Start>
  auto RunApply(Start start) -> std::optional<QVariantMap> {
    QSignalSpy finished(host_->adjustment_transfer(), &AdjustmentTransferController::PasteFinished);
    const QVariantMap started = start();
    EXPECT_TRUE(started.value("success").toBool())
        << started.value("message").toString().toStdString();
    if (!started.value("success").toBool() || !started.value("pending").toBool()) {
      return std::nullopt;
    }
    if (!finished.wait(120000)) {
      ADD_FAILURE() << "the background Paste did not finish";
      return std::nullopt;
    }
    return finished.front().front().toMap();
  }

  auto Pipelines() -> std::shared_ptr<PipelineMgmtService> {
    return host_->project()->handler().pipeline_service();
  }

  fs::path                               preset_root_;
  PopulatedProject                       project_;
  std::unique_ptr<ApplicationModuleHost> host_;
};

TEST_F(PresetApplyTargetsTest, ApplyToTargetsCreatesOneVersionPerTargetNamedAfterPreset) {
  ASSERT_TRUE(StartHost(3));
  const auto result = RunApply([this] {
    return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), Targets(0, 3));
  });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->value("appliedCount").toInt(), 3);
  EXPECT_EQ(result->value("failureCount").toInt(), 0);
  for (const auto& image : project_.images_) {
    const auto history = Pipelines()->LoadHistorySnapshot(image.file_id_);
    EXPECT_EQ(history.graph_->GetActiveVersionRef().display_name, "Warm") << image.file_id_;
    EXPECT_EQ(CountOf(StoredVersionNames(*Pipelines(), image.file_id_), "Warm"), 1);
  }
}

TEST_F(PresetApplyTargetsTest, ApplyToTargetsReadsPresetFileOnce) {
  ASSERT_TRUE(StartHost(20));
  auto*      library      = host_->preset_library();
  const auto reads_before = library->PackageFileReadCount();
  const auto result       = RunApply([this] {
    return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), Targets(0, 20));
  });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->value("appliedCount").toInt(), 20);
  EXPECT_EQ(library->PackageFileReadCount(), reads_before + 1);
}

TEST_F(PresetApplyTargetsTest, ApplyToTargetsRoutesOpenPhotoThroughEditorSession) {
  ASSERT_TRUE(StartHost(3));
  const auto                          open_id = project_.images_.at(0).file_id_;
  alcedo::test::HistorySessionBackend backend(open_id);
  EditorSessionController             session(&backend);
  session.Open(open_id, project_.images_.at(0).image_id_);
  host_->adjustment_transfer()->SetEditorSession(&session);
  const auto stored_before = StoredVersionNames(*Pipelines(), open_id);

  const auto result        = RunApply([this] {
    return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), Targets(0, 3));
  });

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(backend.paste_count(), 1);
  EXPECT_EQ(backend.graph().GetActiveVersionRef().display_name, "Warm");
  EXPECT_EQ(result->value("appliedCount").toInt(), 2);
  EXPECT_FALSE(result->value("appliedIds").toList().contains(static_cast<uint>(open_id)));
  EXPECT_EQ(StoredVersionNames(*Pipelines(), open_id), stored_before)
      << "the coordinator must not load the photo that the editor holds";
  host_->adjustment_transfer()->SetEditorSession(host_->editor_session());
}

// Issue #1111: the targets are read and written in chunks of several photos. An apply larger
// than one chunk, with a failing target inside a chunk, applies every other photo once and
// reports the results in the order of the targets.
TEST_F(PresetApplyTargetsTest, ApplyToManyTargetsAppliesEachPhotoOnceInTargetOrder) {
  constexpr std::size_t kPhotos = 70;
  ASSERT_TRUE(StartHost(kPhotos));
  constexpr uint kMissingId = 999999;
  QVariantList   targets    = Targets(0, 35);
  targets.push_back(QVariantMap{{"elementId", kMissingId}, {"imageId", kMissingId}});
  targets.append(Targets(35, kPhotos - 35));

  const auto result = RunApply([this, &targets] {
    return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), targets);
  });

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->value("appliedCount").toInt(), static_cast<int>(kPhotos));
  const auto failures = result->value("failures").toList();
  ASSERT_EQ(failures.size(), 1);
  EXPECT_EQ(failures.front().toMap().value("elementId").toUInt(), kMissingId);
  const auto applied = result->value("appliedIds").toList();
  ASSERT_EQ(applied.size(), static_cast<qsizetype>(kPhotos));
  for (std::size_t i = 0; i < kPhotos; ++i) {
    const auto element_id = project_.images_.at(i).file_id_;
    EXPECT_EQ(applied.at(static_cast<qsizetype>(i)).toUInt(), static_cast<uint>(element_id));
    const auto history = Pipelines()->LoadHistorySnapshot(element_id);
    EXPECT_EQ(history.graph_->GetActiveVersionRef().display_name, "Warm") << element_id;
    EXPECT_EQ(CountOf(StoredVersionNames(*Pipelines(), element_id), "Warm"), 1) << element_id;
  }
}

TEST_F(PresetApplyTargetsTest, OneFailedTargetIsReportedAndOtherTargetsApply) {
  ASSERT_TRUE(StartHost(2));
  constexpr uint kMissingId = 999999;
  QVariantList   targets    = Targets(0, 2);
  targets.push_back(QVariantMap{{"elementId", kMissingId}, {"imageId", kMissingId}});

  const auto result = RunApply([this, &targets] {
    return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), targets);
  });

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->value("success").toBool());
  EXPECT_EQ(result->value("appliedCount").toInt(), 2);
  const auto failures = result->value("failures").toList();
  ASSERT_EQ(failures.size(), 1);
  EXPECT_EQ(failures.front().toMap().value("elementId").toUInt(), kMissingId);
  EXPECT_FALSE(failures.front().toMap().value("message").toString().isEmpty());
  QVariantList applied = result->value("appliedIds").toList();
  std::sort(applied.begin(), applied.end(),
            [](const QVariant& a, const QVariant& b) { return a.toUInt() < b.toUInt(); });
  QVariantList expected{static_cast<uint>(project_.images_.at(0).file_id_),
                        static_cast<uint>(project_.images_.at(1).file_id_)};
  std::sort(expected.begin(), expected.end(),
            [](const QVariant& a, const QVariant& b) { return a.toUInt() < b.toUInt(); });
  EXPECT_EQ(applied, expected);
}

TEST_F(PresetApplyTargetsTest, ApplyFileToTargetsLeavesLibraryUnchanged) {
  const fs::path outside = temp_dir_ / "downloads" / "Cool.alcpreset";
  alcedo::test::WritePresetBytes(outside, alcedo::test::ExposurePresetText(-1.0));
  ASSERT_TRUE(StartHost(2));
  auto*      library        = host_->preset_library();
  const auto listing_before = FolderListing(preset_root_);
  const auto entries_before = library->entry_count();
  const auto reads_before   = library->PackageFileReadCount();

  const auto result         = RunApply([this, &outside] {
    return host_->presets()->ApplyFileToTargets(
        QUrl::fromLocalFile(QString::fromStdWString(outside.wstring())).toString(), Targets(0, 2));
  });

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->value("appliedCount").toInt(), 2);
  for (const auto& image : project_.images_) {
    EXPECT_EQ(CountOf(StoredVersionNames(*Pipelines(), image.file_id_), "Cool"), 1);
  }
  EXPECT_FALSE(library->busy());
  EXPECT_EQ(FolderListing(preset_root_), listing_before);
  EXPECT_EQ(library->entry_count(), entries_before);
  EXPECT_EQ(library->FindEntry("Cool.alcpreset"), nullptr);
  EXPECT_EQ(library->PackageFileReadCount(), reads_before);
}

TEST_F(PresetApplyTargetsTest, ApplyOfUnavailablePresetStartsNoPaste) {
  alcedo::test::WritePresetBytes(preset_root_ / "Broken.alcpreset", "{\"schema\":1}");
  ASSERT_TRUE(StartHost(1));
  const auto before = StoredVersionNames(*Pipelines(), project_.images_.at(0).file_id_);

  const auto broken =
      host_->presets()->ApplyToTargets(QStringLiteral("Broken.alcpreset"), Targets(0, 1));
  const auto missing =
      host_->presets()->ApplyToTargets(QStringLiteral("Absent.alcpreset"), Targets(0, 1));

  for (const auto& result : {broken, missing}) {
    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_FALSE(result.value("message").toString().isEmpty());
  }
  EXPECT_FALSE(host_->adjustment_transfer()->paste_in_progress());
  EXPECT_EQ(StoredVersionNames(*Pipelines(), project_.images_.at(0).file_id_), before);
}

// Time record of plan PS4 (exit criterion): Apply Preset... on 200 photos against Paste
// Adjustments of the same package on the same photos. Each run adds one Version to each photo, and
// a run takes longer with more Versions, so the runs go in the order Paste, Preset, Preset, Paste:
// the two paths then have the same mean number of Versions. A record, not a check: run it on
// demand in the release build with --gtest_also_run_disabled_tests.
TEST_F(PresetApplyTargetsTest, DISABLED_RecordApplyPresetTimeAgainstPasteFor200Photos) {
  constexpr std::size_t kPhotos = 200;
  ASSERT_TRUE(StartHost(kPhotos));
  const auto package = host_->preset_library()->PackageFor("Warm.alcpreset");
  ASSERT_TRUE(package.Ok()) << package.message;
  const QVariantList targets = Targets(0, kPhotos);

  const auto timed = [this](auto start) -> double {
    QElapsedTimer timer;
    timer.start();
    const auto   result = RunApply(start);
    const double ms     = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    EXPECT_TRUE(result.has_value());
    if (result.has_value()) {
      EXPECT_EQ(result->value("appliedCount").toInt(), static_cast<int>(kPhotos));
    }
    return ms;
  };
  const auto paste = [&] {
    return timed([&] {
      return host_->adjustment_transfer()->PastePackageToTargets(
          *package.package, targets, QStringLiteral("Pasted Adjustments"));
    });
  };
  const auto preset = [&] {
    return timed([&] {
      return host_->presets()->ApplyToTargets(QStringLiteral("Warm.alcpreset"), targets);
    });
  };
  const double paste_first  = paste();
  const double preset_first = preset();
  const double preset_last  = preset();
  const double paste_last   = paste();
  const double paste_mean   = (paste_first + paste_last) / 2.0;
  const double preset_mean  = (preset_first + preset_last) / 2.0;
  std::cout << "[PS4 time] " << kPhotos << " photos, runs Paste " << paste_first << " ms, Preset "
            << preset_first << " ms, Preset " << preset_last << " ms, Paste " << paste_last
            << " ms\n[PS4 time] mean Paste Adjustments " << paste_mean << " ms, Apply Preset "
            << preset_mean << " ms, ratio " << preset_mean / paste_mean << '\n';
}

// Plan PS4 step 3: the Apply Preset dialog shows the previews of its first library photo, rendered
// on the thumbnail executors of the project and served by the dialog image provider.
TEST_F(PresetApplyTargetsTest, DialogPreviewRendersPresetOnLibraryPhoto) {
  // A render reads the RAW file, so the photo is the sample linear DNG.
  const fs::path dng = fs::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
  if (!fs::is_regular_file(dng)) {
    GTEST_SKIP() << "Sample DNG file is missing: " << dng.string();
  }
  ASSERT_TRUE(StartHost(1, {dng}));
  auto*       previews = host_->preset_dialog_preview();
  const auto& photo    = project_.images_.at(0);
  ASSERT_TRUE(previews->showLibraryPhoto(static_cast<uint>(photo.file_id_),
                                         static_cast<uint>(photo.image_id_)));
  const QString id = QStringLiteral("Warm.alcpreset");
  previews->watch(id);
  ASSERT_TRUE(QTest::qWaitFor(
      [&] { return !previews->previewUrl(id).isEmpty() || previews->previewFailed(id); }, 120000));
  EXPECT_FALSE(previews->previewFailed(id));
  EXPECT_TRUE(
      previews->previewUrl(id).startsWith(QStringLiteral("image://alcedo-preset-dialog-preview/")))
      << previews->previewUrl(id).toStdString();
  EXPECT_EQ(previews->render_count(), 1u);
  // The previews of the editor do not follow the dialog photo.
  EXPECT_EQ(host_->preset_preview()->render_count(), 0u);
  previews->unwatch(id);
}

}  // namespace
}  // namespace alcedo::ui::test
