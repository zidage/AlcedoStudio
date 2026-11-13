//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Issue #401: the PNG outputs of library.thumbnail, library.thumbnails, and render.preview are
// created with an exclusive open, so an existing file keeps its bytes.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "ui/alcedo_main/automation/automation_command_support.hpp"

namespace alcedo::automation {
namespace {

const QByteArray kUserData("USER DATA MUST SURVIVE");

auto             ReadBytes(const QString& path) -> QByteArray {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

auto WriteBytes(const QString& path, const QByteArray& bytes) -> bool {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}

auto TestImage() -> QImage {
  QImage image(40, 24, QImage::Format_RGBA8888);
  image.fill(Qt::darkCyan);
  return image;
}

TEST(AutomationNewOutputFileTest, CreateFailsOnExistingFileOfAnyExtensionAndKeepsItsBytes) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  for (const QString& name : {QStringLiteral("important-document.txt"),
                              QStringLiteral("source.png"), QStringLiteral("no_extension")}) {
    const QString path = folder.filePath(name);
    ASSERT_TRUE(WriteBytes(path, kUserData));
    {
      AutomationNewOutputFile out(path);
      const QString           failure = out.Create();
      EXPECT_TRUE(failure.contains(path)) << failure.toStdString();
      EXPECT_FALSE(out.WritePng(TestImage()).isEmpty()) << "a file that was not created";
      out.Keep();
    }
    EXPECT_EQ(ReadBytes(path), kUserData) << name.toStdString();
  }
}

TEST(AutomationNewOutputFileTest, CreateFailsOnFileThatAppearsAfterTheExistenceCheck) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  const QString path = folder.filePath(QStringLiteral("late.png"));
  ASSERT_TRUE(AutomationExistingOutputReason(path).isEmpty());
  AutomationNewOutputFile out(path);
  // Another program creates the file between the check and the open.
  ASSERT_TRUE(WriteBytes(path, kUserData));
  EXPECT_FALSE(out.Create().isEmpty());
  EXPECT_FALSE(AutomationExistingOutputReason(path).isEmpty());
  EXPECT_EQ(ReadBytes(path), kUserData);
}

TEST(AutomationNewOutputFileTest, CreateFailsOnExistingFolder) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  const QString path = folder.filePath(QStringLiteral("folder.png"));
  ASSERT_TRUE(QDir().mkpath(path));
  {
    AutomationNewOutputFile out(path);
    EXPECT_FALSE(out.Create().isEmpty());
  }
  EXPECT_TRUE(QDir(path).exists());
}

TEST(AutomationNewOutputFileTest, KeptFileIsNewPngWithImageDimensions) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  const QString path = folder.filePath(QStringLiteral("thumbnail.png"));
  {
    AutomationNewOutputFile out(path);
    ASSERT_TRUE(out.Create().isEmpty());
    ASSERT_TRUE(out.WritePng(TestImage()).isEmpty());
    out.Keep();
  }
  EXPECT_TRUE(ReadBytes(path).startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)));
  const QImage written(path);
  ASSERT_FALSE(written.isNull());
  EXPECT_EQ(written.size(), QSize(40, 24));
}

TEST(AutomationNewOutputFileTest, CreatedFileWithoutKeepIsRemoved) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  const QString empty_path   = folder.filePath(QStringLiteral("empty.png"));
  const QString written_path = folder.filePath(QStringLiteral("written.png"));
  {
    AutomationNewOutputFile empty(empty_path);
    ASSERT_TRUE(empty.Create().isEmpty());
    AutomationNewOutputFile written(written_path);
    ASSERT_TRUE(written.Create().isEmpty());
    ASSERT_TRUE(written.WritePng(TestImage()).isEmpty());
    EXPECT_TRUE(QFile::exists(empty_path));
    EXPECT_TRUE(QFile::exists(written_path));
  }
  EXPECT_FALSE(QFile::exists(empty_path));
  EXPECT_FALSE(QFile::exists(written_path));
}

TEST(AutomationNewOutputFileTest, DiscardRemovesCreatedFileAtOnceButNotKeptFile) {
  QTemporaryDir folder;
  ASSERT_TRUE(folder.isValid());
  const QString           discarded_path = folder.filePath(QStringLiteral("discarded.png"));
  const QString           kept_path      = folder.filePath(QStringLiteral("kept.png"));
  AutomationNewOutputFile discarded(discarded_path);
  ASSERT_TRUE(discarded.Create().isEmpty());
  ASSERT_TRUE(discarded.WritePng(TestImage()).isEmpty());
  discarded.Discard();
  EXPECT_FALSE(QFile::exists(discarded_path)) << "before the object ends";

  AutomationNewOutputFile kept(kept_path);
  ASSERT_TRUE(kept.Create().isEmpty());
  ASSERT_TRUE(kept.WritePng(TestImage()).isEmpty());
  kept.Keep();
  kept.Discard();
  EXPECT_TRUE(QFile::exists(kept_path));
}

}  // namespace
}  // namespace alcedo::automation
