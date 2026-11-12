//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Rows and selections of the Camera Profile combo (dcp_camera_profile_plan.md, phase C5). The
// model reads a CameraProfileLibraryService that scans a scratch folder of synthetic DCP files;
// the selection tests scan the folder into DcpProfileLibrary::Shared(), the library that the
// parse of a selection reads, and clear it again.

#include "ui/alcedo_main/album_backend/editor_camera_profile_list_model.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <variant>

#include "app/camera_profile_library_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "image/dcp_profile_library.hpp"
#include "image/dcp_test_file_writer.hpp"
#include "support/recording_adjustment_submitter.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs = std::filesystem;
using alcedo::test::MinimalDcpProfile;

struct StoredValue {
  bool        raster           = false;
  bool        embedded_default = false;
  bool        selected         = false;
  bool        resolved         = false;
  bool        has_tone_curve   = false;
  bool        profile_tone     = false;
  std::string camera_model;
  std::string profile_name;
};

/// Adjustment snapshot with the `camera_profile_file` entry in the panel presentation form.
auto Snapshot(const StoredValue& value) -> QVariantMap {
  QVariantMap reference;
  reference.insert("raster", value.raster);
  reference.insert("embedded_default", value.embedded_default);
  reference.insert("selected", value.selected);
  reference.insert("resolved", value.resolved);
  reference.insert("has_tone_curve", value.has_tone_curve);
  reference.insert("profile_tone", value.profile_tone);
  reference.insert("camera_model", QString::fromStdString(value.camera_model));
  reference.insert("profile_name", QString::fromStdString(value.profile_name));
  QVariantMap field;
  field.insert("camera_profile_file", reference);
  QVariantMap snapshot;
  snapshot.insert("camera_profile_file", field);
  return snapshot;
}

/// Adjustment snapshot with only the `odt` entry, in the panel presentation form.
auto OdtSnapshot(const QString& method) -> QVariantMap {
  QVariantMap odt;
  odt.insert("method", method);
  QVariantMap field;
  field.insert("odt", odt);
  QVariantMap snapshot;
  snapshot.insert("odt", field);
  return snapshot;
}

auto Labels(const EditorCameraProfileListModel& model) -> QStringList {
  QStringList labels;
  for (const auto& entry : model.entries()) {
    labels << entry.toMap().value("label").toString();
  }
  return labels;
}

auto Row(const EditorCameraProfileListModel& model, int row) -> QVariantMap {
  return model.entries().value(row).toMap();
}

class EditorCameraProfileListModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device device;
    temp_ = fs::temp_directory_path() /
            ("alcedo_camera_profile_list_model_test_" + std::to_string(device()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    folder_ = temp_ / "Camera";
    fs::create_directories(folder_);
    Write("Canon EOS R6/standard.dcp", "Canon EOS R6", "Camera Standard");
    Write("Canon EOS R6/neutral.dcp", "Canon EOS R6", "Camera Neutral");
    Write("Canon EOS R6/adobe.dcp", "Canon EOS R6", "Adobe Color");
    Write("Canon EOS R6 Mark II/standard.dcp", "Canon EOS R6 Mark II", "Camera Standard");
    Write("Sony ILCE-7M4/standard.dcp", "Sony ILCE-7M4", "Camera ST");
  }

  void TearDown() override {
    if (uses_shared_library_) {
      DcpProfileLibrary::Shared().Scan({});
    }
    service_.reset();
    std::error_code error;
    fs::remove_all(temp_, error);
  }

  void Write(const fs::path& relative, const std::string& camera, const std::string& name) {
    const auto path = folder_ / relative;
    fs::create_directories(path.parent_path());
    MinimalDcpProfile(camera, name).Write(path);
  }

  /// A service over @p library (null: DcpProfileLibrary::Shared()) that has scanned the folder.
  auto ScannedService(DcpProfileLibrary* library) -> CameraProfileLibraryService& {
    uses_shared_library_ = library == nullptr;
    CameraProfileLibraryServiceOptions options;
    options.library       = library;
    options.settings_file = QString::fromStdWString((temp_ / "settings.ini").wstring());
    service_              = std::make_unique<CameraProfileLibraryService>(std::move(options));
    QSignalSpy finished(service_.get(), &CameraProfileLibraryService::ScanFinished);
    service_->setRootFolder(QUrl::fromLocalFile(QString::fromStdWString(folder_.wstring())));
    EXPECT_TRUE(finished.wait(10000));
    return *service_;
  }

  /// A model for a Canon EOS R6 RAW file without a stored reference.
  auto CanonModel(CameraProfileLibraryService& service)
      -> std::unique_ptr<EditorCameraProfileListModel> {
    auto model = std::make_unique<EditorCameraProfileListModel>();
    model->setLibrary(&service);
    model->setCameraMake("Canon");
    model->setCameraModel("Canon EOS R6");
    model->loadFromSnapshot(Snapshot({}));
    return model;
  }

  fs::path                                     temp_;
  fs::path                                     folder_;
  std::unique_ptr<CameraProfileLibraryService> service_;
  bool                                         uses_shared_library_ = false;
};

TEST_F(EditorCameraProfileListModelTest, RowsAreDefaultThenMatchedProfilesInNameOrder) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));

  // "Canon EOS R6 Mark II" and the Sony profile do not match the camera (plan section 2.4).
  EXPECT_EQ(Labels(*model),
            (QStringList{"Built-in", "Adobe Color", "Camera Neutral", "Camera Standard"}));
  EXPECT_TRUE(Row(*model, 0).value("isDefault").toBool());
  EXPECT_TRUE(Row(*model, 0).value("profileName").toString().isEmpty());
  for (int row = 1; row < 4; ++row) {
    EXPECT_FALSE(Row(*model, row).value("isDefault").toBool()) << row;
    EXPECT_FALSE(Row(*model, row).value("isMissing").toBool()) << row;
    EXPECT_EQ(Row(*model, row).value("cameraModel").toString(), "Canon EOS R6") << row;
  }
  EXPECT_EQ(Row(*model, 3).value("profileName").toString(), "Camera Standard");
  EXPECT_EQ(model->currentIndex(), 0);
  EXPECT_FALSE(model->currentUnresolved());
}

TEST_F(EditorCameraProfileListModelTest, ExifMakeSpellingMatchesTheProfileCameraName) {
  DcpProfileLibrary            library;
  auto&                        service = ScannedService(&library);
  EditorCameraProfileListModel model;
  model.setLibrary(&service);
  model.setCameraMake("SONY");
  model.setCameraModel("ILCE-7M4");
  model.loadFromSnapshot(Snapshot({}));

  EXPECT_EQ(Labels(model), (QStringList{"Built-in", "Camera ST"}));
}

TEST_F(EditorCameraProfileListModelTest, DngSourceWithEmbeddedProfileNamesDefaultRowEmbedded) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));
  model->loadFromSnapshot(Snapshot({.embedded_default = true}));

  ASSERT_EQ(model->entries().size(), 4);
  EXPECT_EQ(Row(*model, 0).value("label").toString(), "Embedded");
  EXPECT_TRUE(Row(*model, 0).value("isDefault").toBool());
}

TEST_F(EditorCameraProfileListModelTest, StoredMatchedReferenceSelectsItsRow) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));
  model->loadFromSnapshot(Snapshot({.selected     = true,
                                    .resolved     = true,
                                    .camera_model = "Canon EOS R6",
                                    .profile_name = "Camera Neutral"}));

  EXPECT_EQ(model->entries().size(), 4);
  EXPECT_EQ(model->currentIndex(), 2);
  EXPECT_FALSE(model->currentUnresolved());

  model->loadFromSnapshot(Snapshot({}));
  EXPECT_EQ(model->currentIndex(), 0);
}

TEST_F(EditorCameraProfileListModelTest, UnresolvedStoredReferenceAddsMissingRow) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));
  QSignalSpy        stored(model.get(), &EditorCameraProfileListModel::storedReferenceChanged);
  model->loadFromSnapshot(Snapshot({.selected     = true,
                                    .resolved     = false,
                                    .camera_model = "Canon EOS R6",
                                    .profile_name = "Camera Vivid"}));

  ASSERT_EQ(model->entries().size(), 5);
  int missing_rows = 0;
  for (const auto& entry : model->entries()) {
    missing_rows += entry.toMap().value("isMissing").toBool() ? 1 : 0;
  }
  EXPECT_EQ(missing_rows, 1);
  EXPECT_TRUE(Row(*model, 4).value("isMissing").toBool());
  EXPECT_EQ(Row(*model, 4).value("label").toString(), "Camera Vivid");
  EXPECT_EQ(Row(*model, 4).value("cameraModel").toString(), "Canon EOS R6");
  EXPECT_EQ(model->currentIndex(), 4);
  EXPECT_TRUE(model->currentUnresolved());
  EXPECT_EQ(stored.count(), 1);

  // Clearing the reference (the default entry after Undo) removes the missing row.
  model->loadFromSnapshot(Snapshot({}));
  EXPECT_EQ(model->entries().size(), 4);
  EXPECT_EQ(model->currentIndex(), 0);
  EXPECT_FALSE(model->currentUnresolved());
}

TEST_F(EditorCameraProfileListModelTest, RowsRefreshAfterLibraryRescan) {
  DcpProfileLibrary library;
  auto&             service = ScannedService(&library);
  auto              model   = CanonModel(service);
  ASSERT_EQ(model->entries().size(), 4);

  Write("Canon EOS R6/portrait.dcp", "Canon EOS R6", "Camera Portrait");
  QSignalSpy finished(&service, &CameraProfileLibraryService::ScanFinished);
  service.rescan();
  ASSERT_TRUE(finished.wait(10000));

  EXPECT_EQ(Labels(*model), (QStringList{"Built-in", "Adobe Color", "Camera Neutral",
                                         "Camera Portrait", "Camera Standard"}));
}

TEST_F(EditorCameraProfileListModelTest, RasterDocumentHasNoRows) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));
  model->loadFromSnapshot(Snapshot({.raster = true}));

  EXPECT_EQ(model->entries().size(), 0);
  EXPECT_TRUE(model->raster());
}

TEST_F(EditorCameraProfileListModelTest, SelectionSubmitsOneSettledWriteOfTheProfile) {
  auto               model = CanonModel(ScannedService(nullptr));
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);

  model->selectIndex(3);

  ASSERT_EQ(submitter.calls.size(), 1u);
  EXPECT_EQ(submitter.calls[0].fieldKey, "camera_profile_file");
  EXPECT_TRUE(submitter.calls[0].settled);
  const auto* update = std::get_if<DevelopCameraProfileFileUpdate>(&submitter.calls[0].write);
  ASSERT_NE(update, nullptr);
  ASSERT_NE(update->profile, nullptr);
  EXPECT_EQ(update->profile->profile_name, "Camera Standard");
  EXPECT_EQ(update->profile->unique_camera_model, "Canon EOS R6");
  EXPECT_FALSE(update->unresolved.has_value());
  EXPECT_EQ(model->currentIndex(), 3);
}

TEST_F(EditorCameraProfileListModelTest, DefaultEntryClearsTheStoredReference) {
  auto model = CanonModel(ScannedService(nullptr));
  model->loadFromSnapshot(Snapshot({.selected     = true,
                                    .resolved     = true,
                                    .camera_model = "Canon EOS R6",
                                    .profile_name = "Adobe Color"}));
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);

  model->selectIndex(0);

  ASSERT_EQ(submitter.calls.size(), 1u);
  const auto* update = std::get_if<DevelopCameraProfileFileUpdate>(&submitter.calls[0].write);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(update->profile, nullptr);
  EXPECT_FALSE(update->unresolved.has_value());
}

TEST_F(EditorCameraProfileListModelTest, RejectedSelectionReturnsToStoredRow) {
  auto model = CanonModel(ScannedService(nullptr));
  model->loadFromSnapshot(Snapshot({.selected     = true,
                                    .resolved     = true,
                                    .camera_model = "Canon EOS R6",
                                    .profile_name = "Adobe Color"}));
  ASSERT_EQ(model->currentIndex(), 1);
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);
  // The file of "Camera Neutral" disappears after the scan: the library read fails.
  std::error_code error;
  fs::remove(folder_ / "Canon EOS R6/neutral.dcp", error);
  ASSERT_FALSE(error);

  model->selectIndex(2);

  EXPECT_TRUE(submitter.calls.empty());
  EXPECT_EQ(model->currentIndex(), 1);
}

TEST_F(EditorCameraProfileListModelTest, SelectionIsNotSubmittedWhenTheSessionRejectsEdits) {
  auto               model = CanonModel(ScannedService(nullptr));
  RecordingSubmitter submitter;
  submitter.canEditState = false;
  model->setSubmitter(&submitter);

  model->selectIndex(2);

  EXPECT_TRUE(submitter.calls.empty());
  EXPECT_EQ(model->currentIndex(), 0);
}

// T3 (dcp_profile_tone_curve_study.md, section 6.4): the Tone Curve switch reads the stored
// reference, and the DRT method of the odt entry.
TEST_F(EditorCameraProfileListModelTest, ToneCurveSwitchStateFollowsTheStoredReference) {
  DcpProfileLibrary library;
  auto              model = CanonModel(ScannedService(&library));
  EXPECT_FALSE(model->toneCurveAvailable());
  EXPECT_FALSE(model->profileTone());
  EXPECT_FALSE(model->filmPrintMethod());

  QSignalSpy changed(model.get(), &EditorCameraProfileListModel::storedReferenceChanged);
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = true,
                                    .has_tone_curve = true,
                                    .profile_tone   = true,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  EXPECT_TRUE(model->toneCurveAvailable());
  EXPECT_TRUE(model->profileTone());
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = true,
                                    .has_tone_curve = true,
                                    .profile_tone   = false,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  EXPECT_FALSE(model->profileTone());
  EXPECT_EQ(changed.count(), 2);

  // A profile without a curve, or an unresolved reference, has no switch.
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = true,
                                    .has_tone_curve = false,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  EXPECT_FALSE(model->toneCurveAvailable());
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = false,
                                    .has_tone_curve = true,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  EXPECT_FALSE(model->toneCurveAvailable());

  QSignalSpy method(model.get(), &EditorCameraProfileListModel::filmPrintMethodChanged);
  model->loadFromSnapshot(OdtSnapshot("film_print"));
  EXPECT_TRUE(model->filmPrintMethod());
  // The odt entry alone leaves the stored reference.
  EXPECT_EQ(model->currentIndex(), 1);
  model->loadFromSnapshot(OdtSnapshot("open_drt"));
  EXPECT_FALSE(model->filmPrintMethod());
  EXPECT_EQ(method.count(), 2);
}

TEST_F(EditorCameraProfileListModelTest, ToneCurveSwitchSubmitsOneSettledWriteOfTheReference) {
  auto model = CanonModel(ScannedService(nullptr));
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = true,
                                    .has_tone_curve = true,
                                    .profile_tone   = true,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);

  EXPECT_FALSE(model->submitProfileTone(true));
  EXPECT_TRUE(submitter.calls.empty());
  ASSERT_TRUE(model->submitProfileTone(false));

  ASSERT_EQ(submitter.calls.size(), 1u);
  EXPECT_EQ(submitter.calls[0].fieldKey, "camera_profile_file");
  EXPECT_TRUE(submitter.calls[0].settled);
  const auto* update = std::get_if<DevelopCameraProfileFileUpdate>(&submitter.calls[0].write);
  ASSERT_NE(update, nullptr);
  ASSERT_NE(update->profile, nullptr);
  EXPECT_EQ(update->profile->profile_name, "Adobe Color");
  EXPECT_FALSE(update->profile_tone);
  // The model keeps no switch value of its own: the echo sets it.
  EXPECT_TRUE(model->profileTone());
}

TEST_F(EditorCameraProfileListModelTest, ToneCurveSwitchWithoutReferenceSubmitsNothing) {
  auto               model = CanonModel(ScannedService(nullptr));
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);

  EXPECT_FALSE(model->submitProfileTone(true));
  EXPECT_FALSE(model->submitProfileTone(false));
  EXPECT_TRUE(submitter.calls.empty());
}

TEST_F(EditorCameraProfileListModelTest, SelectionKeepsTheToneCurveSwitchOfTheStoredReference) {
  auto model = CanonModel(ScannedService(nullptr));
  model->loadFromSnapshot(Snapshot({.selected       = true,
                                    .resolved       = true,
                                    .has_tone_curve = true,
                                    .profile_tone   = false,
                                    .camera_model   = "Canon EOS R6",
                                    .profile_name   = "Adobe Color"}));
  RecordingSubmitter submitter;
  model->setSubmitter(&submitter);

  model->selectIndex(3);

  ASSERT_EQ(submitter.calls.size(), 1u);
  const auto* update = std::get_if<DevelopCameraProfileFileUpdate>(&submitter.calls[0].write);
  ASSERT_NE(update, nullptr);
  ASSERT_NE(update->profile, nullptr);
  EXPECT_EQ(update->profile->profile_name, "Camera Standard");
  EXPECT_FALSE(update->profile_tone);

  // From the default entry, the selection turns the stage on.
  model->loadFromSnapshot(Snapshot({}));
  model->selectIndex(3);
  ASSERT_EQ(submitter.calls.size(), 2u);
  update = std::get_if<DevelopCameraProfileFileUpdate>(&submitter.calls[1].write);
  ASSERT_NE(update, nullptr);
  EXPECT_TRUE(update->profile_tone);
}

}  // namespace
}  // namespace alcedo::ui::test
