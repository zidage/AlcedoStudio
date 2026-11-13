//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <iostream>
#include <thread>

#include "edit/history/edit_history.hpp"
#include "history_test_fixation.hpp"
#include "type/type.hpp"

using namespace alcedo;

TEST_F(EditHistoryTests, CreateEditHistory) {
  sl_element_id_t image_id = 12345;
  EditHistory     history(image_id);
  EXPECT_EQ(history.GetBoundImage(), image_id);
  EXPECT_EQ(history.GetVersions().size(), 1U);
  EXPECT_EQ(history.GetDefaultVersion().GetDisplayName(), "Default");
  EXPECT_EQ(history.GetActiveVersionID(), history.GetDefaultVersionID());
  // EXPECT_NE(history.GetHistoryId(), 0);
  EXPECT_NE(history.GetAddTime(), 0);
  EXPECT_EQ(history.GetAddTime(), history.GetLastModifiedTime());
}

TEST_F(EditHistoryTests, CommitVersion) {
  sl_element_id_t image_id = 12345;
  EditHistory     history(image_id);
  Version         ver1 = Version::Empty(image_id, "Version 1");
  Version         ver2 = Version::Empty(image_id, "Version 2");

  // It is unlikely that we will commit two versions at the exact same time.
  // Therefore, the history ID should be different.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ver2.SetLastModifiedTime();
  ver2.CalculateVersionID();
  auto ver1_id = history.CommitVersion(std::move(ver1));
  auto ver2_id = history.CommitVersion(std::move(ver2));
  EXPECT_NE(ver1_id, ver2_id);
  EXPECT_NO_THROW(history.GetVersion(ver1_id));
  EXPECT_NO_THROW(history.GetVersion(ver2_id));
}

TEST_F(EditHistoryTests, SerializeDeserialize) {
  sl_element_id_t image_id = 12345;
  EditHistory     history(image_id);
  Version         ver1 = Version::Empty(image_id, "Version 1");
  Version         ver2 = Version::Empty(image_id, "Version 2");

  std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Ensure different timestamps
  ver2.SetLastModifiedTime();
  ver2.CalculateVersionID();
  auto        ver1_id = history.CommitVersion(std::move(ver1));
  auto        ver2_id = history.CommitVersion(std::move(ver2));

  auto        j2      = history.ToJSON();
  EditHistory history2(image_id + 1);  // Different bound image to ensure it's overwritten
  history2.FromJSON(j2);
  auto val1 = j2.dump(2);
  auto val2 = history2.ToJSON().dump(2);
  std::cout << "Before serialize: \n" << val1 << std::endl;
  std::cout << "After serialize: \n" << val2 << std::endl;
  // Expect the deserialized history to match the original
  EXPECT_EQ(val1, val2);
  EXPECT_EQ(history2.GetBoundImage(), image_id);
  EXPECT_EQ(history2.GetAddTime(), history.GetAddTime());
  EXPECT_EQ(history2.GetLastModifiedTime(), history.GetLastModifiedTime());
  EXPECT_EQ(history2.GetHistoryId(), history.GetHistoryId());
  EXPECT_NO_THROW(history2.GetVersion(ver1_id));
  EXPECT_NO_THROW(history2.GetVersion(ver2_id));
}
