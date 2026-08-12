//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "app/download_service.hpp"

#ifndef ALCEDO_FAKE_ARIA2_PATH
#define ALCEDO_FAKE_ARIA2_PATH ""
#endif

namespace alcedo::test {
namespace {

class Aria2Environment final {
 public:
  Aria2Environment() : previous_(qgetenv("ALCEDO_ARIA2C_BINARY")) {}
  ~Aria2Environment() {
    if (previous_.isNull()) {
      qunsetenv("ALCEDO_ARIA2C_BINARY");
    } else {
      qputenv("ALCEDO_ARIA2C_BINARY", previous_);
    }
  }

 private:
  QByteArray previous_;
};

auto OneByteRequest(const QString& id, const QString& destination) -> DownloadRequest {
  DownloadRequest request;
  request.id = id;
  request.items.push_back(
      {QUrl(QStringLiteral("https://example.invalid/file")), destination, 1, {}});
  return request;
}

TEST(DownloadServiceTests, UserCancellationDoesNotPoisonTheNextDownload) {
  Aria2Environment environment;
  QTemporaryDir    temporary_dir;
  ASSERT_TRUE(temporary_dir.isValid());
  qputenv("ALCEDO_ARIA2C_BINARY", QByteArrayLiteral(ALCEDO_FAKE_ARIA2_PATH));

  DownloadService service;
  QSignalSpy      finished(&service, &DownloadService::Finished);
  const auto first = OneByteRequest(
      QStringLiteral("first"), QDir(temporary_dir.path()).filePath(QStringLiteral("first.bin")));
  ASSERT_TRUE(service.Start(first));
  QTimer::singleShot(100, &service, [&service] { service.Cancel(QStringLiteral("first")); });
  ASSERT_TRUE(finished.wait(10000));
  ASSERT_EQ(finished.count(), 1);
  EXPECT_EQ(finished.at(0).at(0).toString(), QStringLiteral("first"));
  EXPECT_FALSE(finished.at(0).at(1).toBool());
  EXPECT_TRUE(finished.at(0).at(2).toBool());
  EXPECT_NE(finished.at(0).at(3).toString().indexOf(QStringLiteral("canceled"), 0,
                                                    Qt::CaseInsensitive),
            -1);

  finished.clear();
  qputenv("ALCEDO_ARIA2C_BINARY",
          QDir(temporary_dir.path()).filePath(QStringLiteral("missing-aria2c.exe")).toUtf8());
  const auto second = OneByteRequest(
      QStringLiteral("second"), QDir(temporary_dir.path()).filePath(QStringLiteral("second.bin")));
  ASSERT_TRUE(service.Start(second));
  ASSERT_TRUE(finished.wait(5000));
  ASSERT_EQ(finished.count(), 1);
  EXPECT_EQ(finished.at(0).at(0).toString(), QStringLiteral("second"));
  EXPECT_FALSE(finished.at(0).at(1).toBool());
  EXPECT_FALSE(finished.at(0).at(2).toBool());
  EXPECT_NE(finished.at(0).at(3).toString().indexOf(QStringLiteral("not found"), 0,
                                                    Qt::CaseInsensitive),
            -1);
}

TEST(DownloadServiceTests, RejectsMalformedRequestsWithoutEnteringRunningState) {
  DownloadService service;
  EXPECT_FALSE(service.Start({}));

  DownloadRequest request;
  request.id = QStringLiteral("invalid");
  request.items.push_back({QUrl(QStringLiteral("ftp://example.invalid/file")),
                           QStringLiteral("file.bin"), 1, {}});
  EXPECT_FALSE(service.Start(request));
  EXPECT_FALSE(service.IsRunning());
  EXPECT_TRUE(service.ActiveRequestId().isEmpty());
}

}  // namespace
}  // namespace alcedo::test
