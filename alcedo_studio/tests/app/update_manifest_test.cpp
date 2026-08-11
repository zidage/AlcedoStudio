//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDateTime>
#include <QUrl>

extern "C" {
#include <ed25519.h>
}

#include "app/update_manifest.hpp"

namespace alcedo::test {
namespace {

struct SignedManifest {
  QByteArray json;
  QByteArray signature;
  QByteArray public_key;
};

auto MakeSignedManifest(
    QString artifact_url = QStringLiteral("https://static.aoraw.org/releases/0.2.9/update.exe"),
    quint64 sequence     = 42) -> SignedManifest {
  const QByteArray json =
      QStringLiteral(
          R"({"schema":1,"sequence":%1,"version":"0.2.9","build":2009,"publishedAt":"2026-08-10T00:00:00Z","expiresAt":"2026-09-10T00:00:00Z","notesUrl":"https://alcedo.studio/releases/0.2.9","artifacts":{"windows-x86_64":{"url":"%2","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","size":123456}}})")
          .arg(sequence)
          .arg(artifact_url)
          .toUtf8();
  unsigned char seed[32] = {};
  for (size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index + 1);
  }
  unsigned char public_key[32]  = {};
  unsigned char private_key[64] = {};
  unsigned char signature[64]   = {};
  ed25519_create_keypair(public_key, private_key, seed);
  ed25519_sign(signature, reinterpret_cast<const unsigned char*>(json.constData()),
               static_cast<size_t>(json.size()), public_key, private_key);
  return {json, QByteArray(reinterpret_cast<const char*>(signature), sizeof(signature)).toBase64(),
          QByteArray(reinterpret_cast<const char*>(public_key), sizeof(public_key))};
}

const QDateTime kValidationTime =
    QDateTime::fromString(QStringLiteral("2026-08-11T00:00:00Z"), Qt::ISODate);
const QUrl kFeedUrl(QStringLiteral("https://static.aoraw.org/updates/v1/stable/manifest.json"));

TEST(UpdateManifestTest, AcceptsSignedArtifactForCurrentPlatform) {
  const SignedManifest input = MakeSignedManifest();
  const auto           result =
      VerifyUpdateManifest(input.json, input.signature, input.public_key,
                           QStringLiteral("windows-x86_64"), kFeedUrl, 40, kValidationTime);
  ASSERT_TRUE(result) << result.error.toStdString();
  EXPECT_EQ(result.manifest->sequence, 42u);
  EXPECT_EQ(result.manifest->version, QStringLiteral("0.2.9"));
  EXPECT_EQ(result.manifest->artifact.size, 123456);
  EXPECT_EQ(result.manifest->artifact.sha256.size(), 32);
}

TEST(UpdateManifestTest, RejectsAnyChangeAfterSigning) {
  SignedManifest input = MakeSignedManifest();
  input.json.replace("123456", "123457");
  const auto result =
      VerifyUpdateManifest(input.json, input.signature, input.public_key,
                           QStringLiteral("windows-x86_64"), kFeedUrl, 1, kValidationTime);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error.contains(QStringLiteral("signature"), Qt::CaseInsensitive));
}

TEST(UpdateManifestTest, RejectsSignedRollbackSequence) {
  const SignedManifest input = MakeSignedManifest({}, 41);
  const auto           result =
      VerifyUpdateManifest(input.json, input.signature, input.public_key,
                           QStringLiteral("windows-x86_64"), kFeedUrl, 42, kValidationTime);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error.contains(QStringLiteral("sequence"), Qt::CaseInsensitive));
}

TEST(UpdateManifestTest, RejectsArtifactOnDifferentHost) {
  const SignedManifest input =
      MakeSignedManifest(QStringLiteral("https://downloads.example.test/update.exe"));
  const auto result =
      VerifyUpdateManifest(input.json, input.signature, input.public_key,
                           QStringLiteral("windows-x86_64"), kFeedUrl, 1, kValidationTime);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error.contains(QStringLiteral("URL"), Qt::CaseInsensitive));
}

TEST(UpdateManifestTest, RejectsScriptArtifactForWindows) {
  const SignedManifest input =
      MakeSignedManifest(QStringLiteral("https://static.aoraw.org/releases/0.2.9/update.ps1"));
  const auto result =
      VerifyUpdateManifest(input.json, input.signature, input.public_key,
                           QStringLiteral("windows-x86_64"), kFeedUrl, 1, kValidationTime);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error.contains(QStringLiteral("type"), Qt::CaseInsensitive));
}

TEST(UpdateManifestTest, RejectsExpiredSignedManifest) {
  const SignedManifest input = MakeSignedManifest();
  const QDateTime      later =
      QDateTime::fromString(QStringLiteral("2026-10-01T00:00:00Z"), Qt::ISODate);
  const auto result = VerifyUpdateManifest(input.json, input.signature, input.public_key,
                                           QStringLiteral("windows-x86_64"), kFeedUrl, 1, later);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.error.contains(QStringLiteral("expired"), Qt::CaseInsensitive));
}

}  // namespace
}  // namespace alcedo::test
