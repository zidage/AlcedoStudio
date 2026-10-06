//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Remembered LUT encodings in the library user state (lut_color_encoding_plan.md, Phase L5):
// `lut-library.json` serialization, item-level validation, and the LutLibraryService owner
// operation with its persistence order, busy rule, package update, and root migration.

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "app/lut_library_service.hpp"
#include "lut_library_test_support.hpp"

namespace alcedo::test {
namespace {

using LutRememberedEncodingsTest = LutLibraryRootFixture;

const LutRememberedEncodings kSlog3ToRec709{"sony_slog3_sgamut3cine", "rec709_bt1886"};

TEST(LutRememberedEncodingsStateTest, StateWithoutRememberedEncodingsKeepsItsSerializedBytes) {
  LutLibraryUserState state;
  state.favorite_entry_ids = {"library:films/portra.cube"};
  state.previous_roots     = {"D:/old"};

  // The bytes written before Phase L5 for the same state.
  EXPECT_EQ(SerializeLutLibraryUserState(state),
            "{\n"
            " \"favorite_entries\": [\n"
            "  \"library:films/portra.cube\"\n"
            " ],\n"
            " \"kind\": \"alcedo-lut-library-state\",\n"
            " \"previous_roots\": [\n"
            "  \"D:/old\"\n"
            " ],\n"
            " \"schema\": 1\n"
            "}");
}

TEST(LutRememberedEncodingsStateTest, RememberedEncodingsRoundTripThroughTheSerializedState) {
  LutLibraryUserState state;
  state.favorite_entry_ids   = {"library:films/portra.cube"};
  state.remembered_encodings = {
      {"library:films/portra.cube", kSlog3ToRec709},
      {"official:spectral_film_lut/spectral_film_lut:kodak_vision3_250d_5207",
       {"acescc", "rec2100_pq1000"}}};

  const std::string serialized = SerializeLutLibraryUserState(state);
  EXPECT_NE(serialized.find("\"remembered_encodings\""), std::string::npos);
  const LutLibraryUserStateReadResult read = ParseLutLibraryUserState(serialized);
  ASSERT_TRUE(read.state.has_value()) << read.error;
  EXPECT_TRUE(read.dropped_items.empty());
  EXPECT_EQ(read.state->remembered_encodings, state.remembered_encodings);
  EXPECT_EQ(read.state->favorite_entry_ids, state.favorite_entry_ids);
}

// An invalid remembered item is left out on its own. Rejecting the file would load an empty
// state, and the next write would then lose the favorites too.
TEST(LutRememberedEncodingsStateTest, InvalidRememberedItemsAreDroppedAndFavoritesAreKept) {
  const LutLibraryUserStateReadResult read = ParseLutLibraryUserState(R"({
    "schema": 1,
    "kind": "alcedo-lut-library-state",
    "favorite_entries": ["library:a.cube"],
    "previous_roots": [],
    "remembered_encodings": {
      "library:a.cube": {"input": "sony_slog3_sgamut3cine", "output": "rec709_bt1886"},
      "library:../outside.cube": {"input": "acescc", "output": "acescc"},
      "library:b.cube": {"input": "no_such_encoding", "output": "acescc"},
      "library:c.cube": {"input": "acescc"},
      "library:d.cube": "acescc"
    }
  })");

  ASSERT_TRUE(read.state.has_value()) << read.error;
  EXPECT_EQ(read.state->favorite_entry_ids, (std::vector<std::string>{"library:a.cube"}));
  ASSERT_EQ(read.state->remembered_encodings.size(), 1u);
  EXPECT_EQ(read.state->remembered_encodings.at("library:a.cube"), kSlog3ToRec709);
  EXPECT_EQ(read.dropped_items.size(), 4u);

  const LutLibraryUserStateReadResult not_an_object = ParseLutLibraryUserState(
      R"({"schema": 1, "kind": "alcedo-lut-library-state", "favorite_entries": ["library:a.cube"],
          "remembered_encodings": ["library:a.cube"]})");
  ASSERT_TRUE(not_an_object.state.has_value()) << not_an_object.error;
  EXPECT_EQ(not_an_object.state->favorite_entry_ids, (std::vector<std::string>{"library:a.cube"}));
  EXPECT_TRUE(not_an_object.state->remembered_encodings.empty());
  EXPECT_EQ(not_an_object.dropped_items.size(), 1u);
}

TEST_F(LutRememberedEncodingsTest, SetRememberedEncodingsPersistsBeforePublishingAndForgets) {
  WriteBytes(library_root_ / "films" / "portra.cube", UserCube("portra"));
  const std::string        entry_id   = "library:films/portra.cube";
  bool                     fail_write = false;
  LutLibraryFileOperations io         = LutLibraryFileOperations::Default();
  io.write_user_state                 = [&fail_write](const fs::path&            root,
                                      const LutLibraryUserState& state) -> std::string {
    if (fail_write) return "disk is full";
    return WriteLutLibraryUserStateFile(root, state);
  };
  const auto service = StartService(io);
  QSignalSpy changed(service.get(), &Service::RememberedEncodingsChanged);

  // A failed write publishes nothing.
  fail_write = true;
  EXPECT_EQ(service->SetRememberedEncodings(entry_id, kSlog3ToRec709),
            Service::Status::kPersistenceError);
  EXPECT_FALSE(service->RememberedEncodings(entry_id).has_value());
  EXPECT_EQ(changed.count(), 0);
  EXPECT_FALSE(service->last_error().isEmpty());

  fail_write = false;
  EXPECT_EQ(service->SetRememberedEncodings(entry_id, kSlog3ToRec709), Service::Status::kOk);
  EXPECT_EQ(service->RememberedEncodings(entry_id), kSlog3ToRec709);
  ASSERT_EQ(changed.count(), 1);
  EXPECT_EQ(changed.takeFirst().at(0).toStringList(),
            QStringList{QString::fromStdString(entry_id)});
  const LutLibraryUserStateReadResult persisted = ReadLutLibraryUserStateFile(library_root_);
  ASSERT_TRUE(persisted.state.has_value());
  EXPECT_EQ(persisted.state->remembered_encodings.at(entry_id), kSlog3ToRec709);

  // The same pair again writes nothing and announces nothing.
  EXPECT_EQ(service->SetRememberedEncodings(entry_id, kSlog3ToRec709), Service::Status::kOk);
  EXPECT_EQ(changed.count(), 0);

  // A restarted service reads the pair.
  EXPECT_EQ(StartService()->RememberedEncodings(entry_id), kSlog3ToRec709);

  EXPECT_EQ(service->SetRememberedEncodings(entry_id, std::nullopt), Service::Status::kOk);
  EXPECT_FALSE(service->RememberedEncodings(entry_id).has_value());
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(ReadLutLibraryUserStateFile(library_root_).state->remembered_encodings.empty());
}

TEST_F(LutRememberedEncodingsTest, SetRememberedEncodingsRejectsInvalidIdsWithoutChange) {
  WriteBytes(library_root_ / "films" / "portra.cube", UserCube("portra"));
  const auto service = StartService();
  QSignalSpy changed(service.get(), &Service::RememberedEncodingsChanged);

  EXPECT_EQ(service->SetRememberedEncodings("films/portra.cube", kSlog3ToRec709),
            Service::Status::kInvalidRequest);
  EXPECT_EQ(service->SetRememberedEncodings("library:films/portra.cube",
                                            LutRememberedEncodings{"acescc", "no_such_encoding"}),
            Service::Status::kInvalidRequest);
  EXPECT_FALSE(service->RememberedEncodings("library:films/portra.cube").has_value());
  EXPECT_EQ(changed.count(), 0);
  EXPECT_TRUE(ReadLutLibraryUserStateFile(library_root_).state->remembered_encodings.empty());
}

// The root migration completes on the owner thread, so it is still running until events are
// processed.
TEST_F(LutRememberedEncodingsTest, SetRememberedEncodingsIsRejectedWhileRootOperationRuns) {
  WriteBytes(library_root_ / "films" / "portra.cube", UserCube("portra"));
  const auto service = StartService();
  ASSERT_EQ(service->MigrateRoot(base_ / "moved"), Service::Status::kOk);

  EXPECT_EQ(service->SetRememberedEncodings("library:films/portra.cube", kSlog3ToRec709),
            Service::Status::kBusy);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_FALSE(service->RememberedEncodings("library:films/portra.cube").has_value());
  EXPECT_EQ(service->SetRememberedEncodings("library:films/portra.cube", kSlog3ToRec709),
            Service::Status::kOk);
}

TEST_F(LutRememberedEncodingsTest, RememberedEncodingsFollowPackageUpdateAndRootMigration) {
  const std::string file_name = "kodak_vision3_250d_5207.cube";
  const std::string content_a = "packages/spectral_film_lut/content/a";
  WriteBytes(library_root_ / fs::path(content_a) / file_name, OfficialCube());
  WriteBytes(library_root_ / "user" / "look.cube", UserCube("look"));
  LutPackageReceipt receipt;
  receipt.package_id        = "spectral_film_lut";
  receipt.content_directory = content_a;
  ASSERT_TRUE(WriteLutPackageReceiptFile(library_root_, receipt).empty());
  const auto        service = StartService();
  const std::string official =
      "official:spectral_film_lut/spectral_film_lut:kodak_vision3_250d_5207";
  const LutRememberedEncodings official_pair{"acescc", "rec709_bt1886"};
  ASSERT_EQ(service->SetRememberedEncodings(official, official_pair), Service::Status::kOk);
  ASSERT_EQ(service->SetRememberedEncodings("library:user/look.cube", kSlog3ToRec709),
            Service::Status::kOk);

  // A package update activates the same LUT in another content directory.
  const std::string content_b = "packages/spectral_film_lut/content/b";
  WriteBytes(library_root_ / fs::path(content_b) / file_name, OfficialCube());
  receipt.content_directory = content_b;
  ASSERT_TRUE(WriteLutPackageReceiptFile(library_root_, receipt).empty());
  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  std::string listed_path;
  ASSERT_TRUE(service->ReadEntryById(
      official, [&](const LutLibraryEntry& entry) { listed_path = entry.relative_path; }));
  EXPECT_EQ(listed_path, content_b + "/" + file_name);
  EXPECT_EQ(service->RememberedEncodings(official), official_pair);

  const fs::path destination = base_ / "migrated";
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  ASSERT_EQ(service->Root(), destination);
  EXPECT_EQ(service->RememberedEncodings(official), official_pair);
  EXPECT_EQ(service->RememberedEncodings("library:user/look.cube"), kSlog3ToRec709);

  const auto restarted = StartService();
  EXPECT_EQ(restarted->Root(), destination);
  EXPECT_EQ(restarted->RememberedEncodings(official), official_pair);
  EXPECT_EQ(restarted->RememberedEncodings("library:user/look.cube"), kSlog3ToRec709);
}

// Only a user-requested refresh writes ACEScc to ACEScc, and only for official package LUTs that
// have no remembered pair. User LUTs get nothing, and a pair the user chose is kept.
TEST_F(LutRememberedEncodingsTest, UserRefreshRemembersDefaultEncodingsForOfficialLutsOnly) {
  const std::string content = "packages/spectral_film_lut/content/a";
  WriteBytes(library_root_ / fs::path(content) / "kodak_vision3_250d_5207.cube", OfficialCube());
  WriteBytes(library_root_ / "user" / "look.cube", UserCube("look"));
  LutPackageReceipt receipt;
  receipt.package_id        = "spectral_film_lut";
  receipt.content_directory = content;
  ASSERT_TRUE(WriteLutPackageReceiptFile(library_root_, receipt).empty());
  const std::string official =
      "official:spectral_film_lut/spectral_film_lut:kodak_vision3_250d_5207";
  const LutRememberedEncodings defaults{"acescc", "acescc"};

  // Loading the library is not a user refresh.
  const auto                   service = StartService();
  EXPECT_FALSE(service->RememberedEncodings(official).has_value());
  QSignalSpy changed(service.get(), &Service::RememberedEncodingsChanged);

  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->RememberedEncodings(official), defaults);
  EXPECT_FALSE(service->RememberedEncodings("library:user/look.cube").has_value());
  ASSERT_EQ(changed.count(), 1);
  EXPECT_EQ(changed.takeFirst().at(0).toStringList(),
            QStringList{QString::fromStdString(official)});
  EXPECT_EQ(ReadLutLibraryUserStateFile(library_root_).state->remembered_encodings.at(official),
            defaults);

  // A pair the user chose is not replaced by the next refresh.
  ASSERT_EQ(service->SetRememberedEncodings(official, kSlog3ToRec709), Service::Status::kOk);
  changed.clear();
  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->RememberedEncodings(official), kSlog3ToRec709);
  EXPECT_EQ(changed.count(), 0);

  // A forgotten official pair gets the defaults again on the next user refresh.
  ASSERT_EQ(service->SetRememberedEncodings(official, std::nullopt), Service::Status::kOk);
  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->RememberedEncodings(official), defaults);
}

// A user refresh requested while an automatic refresh runs is coalesced into it, and that
// refresh then writes the official defaults.
TEST_F(LutRememberedEncodingsTest, UserRefreshCoalescedIntoRunningRefreshWritesDefaults) {
  const std::string content = "packages/spectral_film_lut/content/a";
  WriteBytes(library_root_ / fs::path(content) / "kodak_vision3_250d_5207.cube", OfficialCube());
  LutPackageReceipt receipt;
  receipt.package_id        = "spectral_film_lut";
  receipt.content_directory = content;
  ASSERT_TRUE(WriteLutPackageReceiptFile(library_root_, receipt).empty());
  const std::string official =
      "official:spectral_film_lut/spectral_film_lut:kodak_vision3_250d_5207";
  const auto service = StartService();

  // A lookup of an unlisted path starts an automatic refresh, which writes no defaults.
  EXPECT_EQ(service->LocateEntry("user/absent.cube").status,
            Service::LocateStatus::kNotInInventory);
  ASSERT_EQ(service->CurrentOperation(), Service::Operation::kRefresh);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_FALSE(service->RememberedEncodings(official).has_value());

  EXPECT_EQ(service->LocateEntry("user/other_absent.cube").status,
            Service::LocateStatus::kNotInInventory);
  ASSERT_EQ(service->CurrentOperation(), Service::Operation::kRefresh);
  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->RememberedEncodings(official), (LutRememberedEncodings{"acescc", "acescc"}));
}

}  // namespace
}  // namespace alcedo::test
