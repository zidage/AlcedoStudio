//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/version_name_rules.hpp"

#include <gtest/gtest.h>

#include <string>

namespace alcedo {
namespace {

TEST(VersionNameRulesTest, TrimmedEmptyNameIsRejected) {
  for (const char* text : {"", "   ", "\t\n ", "\xE3\x80\x80", " \xC2\xA0 "}) {
    std::string error;
    EXPECT_FALSE(NormalizeVersionName(text, &error).has_value()) << text;
    EXPECT_EQ(error, kEmptyVersionNameMessage) << text;
  }
}

TEST(VersionNameRulesTest, NameIsTrimmedLikeThePanel) {
  EXPECT_EQ(NormalizeVersionName("  Warm  "), "Warm");
  EXPECT_EQ(NormalizeVersionName("\tWarm look\n"), "Warm look");
  // An ideographic space (U+3000) is white space for the JavaScript trim of the panel.
  EXPECT_EQ(NormalizeVersionName("\xE3\x80\x80\xE6\x9A\x96\xE3\x80\x80"), "\xE6\x9A\x96");
  // Inner white space stays.
  EXPECT_EQ(NormalizeVersionName(" a  b "), "a  b");
  // Bytes that are no valid UTF-8 stop the trim and stay in the name.
  EXPECT_EQ(NormalizeVersionName(" \xFF "), "\xFF");
}

TEST(VersionNameRulesTest, DefaultNameUsesTheNextNumber) {
  EXPECT_EQ(DefaultVersionName(0), "Version 1");
  EXPECT_EQ(DefaultVersionName(2), "Version 3");
  EXPECT_EQ(DefaultVersionName(4, "\xE7\x89\x88\xE6\x9C\xAC %1"), "\xE7\x89\x88\xE6\x9C\xAC 5");
  EXPECT_EQ(DefaultVersionName(1, "Look"), "Look 2");
}

TEST(VersionNameRulesTest, RenameToTheSameNameIsUnchanged) {
  EXPECT_TRUE(IsUnchangedVersionName("Warm", " Warm "));
  EXPECT_FALSE(IsUnchangedVersionName("Warm", "Cool"));
  EXPECT_FALSE(IsUnchangedVersionName("Warm", "   "));
}

}  // namespace
}  // namespace alcedo
