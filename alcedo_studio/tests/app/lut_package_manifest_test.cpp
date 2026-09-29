//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QUrl>

extern "C" {
#include <ed25519.h>
}

#include "app/lut_package_manifest.hpp"

namespace alcedo::test {
namespace {

struct SignedFeed {
  QByteArray json;
  QByteArray signature;
  QByteArray public_key;
};

auto PackageJson(const QString& id, const QString& url = {}) -> QString {
  const QString artifact_url =
      url.isEmpty()
          ? QStringLiteral("https://static.aoraw.org/luts/v1/packages/%1/r1/%1-r1.7z").arg(id)
          : url;
  return QStringLiteral(
             R"({"id":"%1","revision":"2026.09.1","file_count":42,"inventory_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","unpacked_bytes":7000000,"artifact":{"url":"%2","size":123456,"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"}})")
      .arg(id, artifact_url);
}

auto SignFeed(const QString& packages, quint64 sequence = 20260101000000ULL,
              const QString& extra = {}) -> SignedFeed {
  const QByteArray json =
      QStringLiteral(R"({"schema":1,"kind":"alcedo-lut-packages","sequence":%1%3,"packages":[%2]})")
          .arg(sequence)
          .arg(packages, extra)
          .toUtf8();
  unsigned char seed[32] = {};
  for (size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index + 7);
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

const QUrl kFeedUrl(QStringLiteral("https://static.aoraw.org/luts/v1/manifest.json"));

auto Verify(const SignedFeed& feed, quint64 minimum_sequence = 1) -> LutPackageManifestResult {
  return VerifyLutPackageManifest(feed.json, feed.signature, feed.public_key, kFeedUrl,
                                  minimum_sequence);
}

TEST(LutPackageManifestTest, LutManifestAcceptsOldSignedFeedWithoutExpiry) {
  // The feed was signed long ago and has no expiry; only its trusted sequence matters.
  const SignedFeed feed = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")) + "," +
                                   PackageJson(QStringLiteral("spektrafilm_lut")));
  const LutPackageManifestResult result = Verify(feed, 20260101000000ULL);
  ASSERT_TRUE(result) << result.error.toStdString();
  EXPECT_EQ(result.manifest->sequence, 20260101000000ULL);
  ASSERT_EQ(result.manifest->packages.size(), 2);
  const LutPackageDescriptor& first = result.manifest->packages.front();
  EXPECT_EQ(first.id, QStringLiteral("spectral_film_lut"));
  EXPECT_EQ(first.revision, QStringLiteral("2026.09.1"));
  EXPECT_EQ(first.file_count, 42u);
  EXPECT_EQ(first.unpacked_bytes, 7000000u);
  EXPECT_EQ(first.inventory_sha256.size(), 32);
  EXPECT_EQ(first.artifact.size, 123456);
  EXPECT_EQ(first.artifact.sha256.size(), 32);

  // A stale expiry-like field is not part of the schema and is not enforced.
  const SignedFeed stale_field =
      SignFeed(PackageJson(QStringLiteral("spectral_film_lut")), 20260101000000ULL,
               QStringLiteral(R"(,"expiresAt":"2000-01-01T00:00:00Z")"));
  EXPECT_TRUE(Verify(stale_field));
}

TEST(LutPackageManifestTest, LutManifestRejectsBadSignatureAndDuplicatePackage) {
  SignedFeed tampered = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")));
  tampered.json.replace("\"file_count\":42", "\"file_count\":43");
  const LutPackageManifestResult tampered_result = Verify(tampered);
  EXPECT_FALSE(tampered_result);
  EXPECT_TRUE(tampered_result.error.contains(QStringLiteral("signature")));

  SignedFeed wrong_key    = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")));
  wrong_key.public_key[0] = static_cast<char>(wrong_key.public_key[0] ^ 0x01);
  EXPECT_FALSE(Verify(wrong_key));

  SignedFeed malformed_signature = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")));
  malformed_signature.signature  = "not base64!";
  EXPECT_FALSE(Verify(malformed_signature));

  const SignedFeed duplicate = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")) + "," +
                                        PackageJson(QStringLiteral("spectral_film_lut")));
  const LutPackageManifestResult duplicate_result = Verify(duplicate);
  EXPECT_FALSE(duplicate_result);
  EXPECT_TRUE(duplicate_result.error.contains(QStringLiteral("twice")));
}

TEST(LutPackageManifestTest, LutManifestRejectsOlderSequence) {
  const SignedFeed feed = SignFeed(PackageJson(QStringLiteral("spectral_film_lut")), 5);
  EXPECT_TRUE(Verify(feed, 5));
  EXPECT_FALSE(Verify(feed, 6));
}

TEST(LutPackageManifestTest, LutManifestRejectsForeignOrNonArchiveUrls) {
  for (const QString& url : {QStringLiteral("http://static.aoraw.org/luts/v1/p.7z"),
                             QStringLiteral("https://example.com/luts/v1/p.7z"),
                             QStringLiteral("https://user@static.aoraw.org/luts/v1/p.7z"),
                             QStringLiteral("https://static.aoraw.org/luts/v1/p.zip")}) {
    const SignedFeed feed = SignFeed(PackageJson(QStringLiteral("spectral_film_lut"), url));
    EXPECT_FALSE(Verify(feed)) << url.toStdString();
  }
}

TEST(LutPackageManifestTest, LutManifestRejectsInvalidPackageFields) {
  const QString     valid   = PackageJson(QStringLiteral("spectral_film_lut"));
  const QStringList invalid = {
      QString(valid).replace(QStringLiteral("\"spectral_film_lut\""), QStringLiteral("\"Bad Id\"")),
      QString(valid).replace(QStringLiteral("\"file_count\":42"),
                             QStringLiteral("\"file_count\":0")),
      QString(valid).replace(QStringLiteral("0123456789abcdef0123"),
                             QStringLiteral("0123456789ABCDEF0123")),
      QString(valid).replace(QStringLiteral("\"size\":123456"), QStringLiteral("\"size\":-1")),
      QString(valid).replace(QStringLiteral("\"revision\":\"2026.09.1\""),
                             QStringLiteral("\"revision\":\"\"")),
  };
  for (const QString& package : invalid) {
    EXPECT_FALSE(Verify(SignFeed(package))) << package.toStdString();
  }
  EXPECT_FALSE(Verify(SignFeed({})));
}

}  // namespace
}  // namespace alcedo::test
