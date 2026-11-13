//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QUrl>

extern "C" {
#include <ed25519.h>
}

#include "app/ai_model_package_manifest.hpp"

namespace alcedo::test {
namespace {

const QUrl kFeedUrl(QStringLiteral("https://static.aoraw.org/ai-models/v1/manifest.json"));
constexpr const char* kDigest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct SignedFeed {
  QByteArray json;
  QByteArray signature;
  QByteArray public_key;
};

auto ArtifactJson(const QString& platform, quint64 version, const QString& url = {}) -> QString {
  const QString artifact_url =
      url.isEmpty() ? QStringLiteral(
                          "https://static.aoraw.org/ai-models/v1/packages/alcedo-ai-mask/"
                          "v%1/r1/alcedo-ai-mask-v%1-r1-%2.7z")
                          .arg(version)
                          .arg(platform)
                    : url;
  return QStringLiteral(
             R"("%1":{"url":"%2","size":356815751,"sha256":"%3","file_count":9,"unpacked_bytes":356900000,"inventory_sha256":"%3"})")
      .arg(platform, artifact_url, QString::fromLatin1(kDigest));
}

auto EntryJson(quint64 version, const QString& artifacts = {},
               const QString& revision = QStringLiteral("r1")) -> QString {
  const QString artifact_list = artifacts.isEmpty()
                                    ? ArtifactJson(QStringLiteral("windows-x86_64"), version) +
                                          "," + ArtifactJson(QStringLiteral("macos-arm64"), version)
                                    : artifacts;
  return QStringLiteral(
             R"({"id":"alcedo-ai-mask","name":"AI Mask","model_set_version":%1,"revision":"%2","licenses":[{"name":"Apache-2.0","file":"licenses/Apache-2.0.txt"},{"name":"GPL-3.0","file":"licenses/GPL-3.0.txt"}],"artifacts":{%3}})")
      .arg(version)
      .arg(revision, artifact_list);
}

auto SignFeed(const QStringList& entries, quint64 sequence = 20261027000000ULL) -> SignedFeed {
  const QByteArray json =
      QStringLiteral(
          R"({"schema":1,"kind":"alcedo-ai-model-packages","sequence":%1,"packages":[%2]})")
          .arg(sequence)
          .arg(entries.join(QLatin1Char(',')))
          .toUtf8();
  unsigned char seed[32] = {};
  for (size_t index = 0; index < sizeof(seed); ++index) {
    seed[index] = static_cast<unsigned char>(index + 11);
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

auto Verify(const SignedFeed& feed, quint64 minimum_sequence = 1) -> AiModelPackageManifestResult {
  return VerifyAiModelPackageManifest(feed.json, feed.signature, feed.public_key, kFeedUrl,
                                      minimum_sequence);
}

TEST(AiModelPackageManifestTest, SignedFeedListsVersionsLicensesAndPlatformArtifacts) {
  const SignedFeed                   feed   = SignFeed({EntryJson(1), EntryJson(2)});
  const AiModelPackageManifestResult result = Verify(feed, 20261027000000ULL);
  ASSERT_TRUE(result) << result.error.toStdString();
  EXPECT_EQ(result.manifest->sequence, 20261027000000ULL);
  ASSERT_EQ(result.manifest->packages.size(), 2);
  const AiModelPackageDescriptor& first = result.manifest->packages.front();
  EXPECT_EQ(first.id, QStringLiteral("alcedo-ai-mask"));
  EXPECT_EQ(first.name, QStringLiteral("AI Mask"));
  EXPECT_EQ(first.model_set_version, 1u);
  EXPECT_EQ(first.revision, QStringLiteral("r1"));
  ASSERT_EQ(first.licenses.size(), 2);
  EXPECT_EQ(first.licenses.front().name, QStringLiteral("Apache-2.0"));
  EXPECT_EQ(first.licenses.front().file, QStringLiteral("licenses/Apache-2.0.txt"));
  const AiModelPackageArtifact* windows = first.Artifact(QStringLiteral("windows-x86_64"));
  ASSERT_NE(windows, nullptr);
  EXPECT_EQ(windows->size, 356815751);
  EXPECT_EQ(windows->file_count, 9u);
  EXPECT_EQ(windows->unpacked_bytes, 356900000u);
  EXPECT_EQ(windows->sha256.size(), 32);
  EXPECT_EQ(windows->inventory_sha256.size(), 32);
  EXPECT_NE(first.Artifact(QStringLiteral("macos-arm64")), nullptr);
  EXPECT_EQ(first.Artifact(QStringLiteral("linux-x86_64")), nullptr);
}

TEST(AiModelPackageManifestTest, ChangedManifestByteFailsSignature) {
  SignedFeed feed = SignFeed({EntryJson(1)});
  feed.json.replace("\"r1\"", "\"r2\"");
  const AiModelPackageManifestResult result = Verify(feed);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.indexOf(QStringLiteral("signature")), -1) << result.error.toStdString();
}

TEST(AiModelPackageManifestTest, OlderSequenceThanTrustedIsRejected) {
  const SignedFeed                   feed   = SignFeed({EntryJson(1)}, 20261027000000ULL);
  const AiModelPackageManifestResult result = Verify(feed, 20261027000001ULL);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.indexOf(QStringLiteral("sequence")), -1) << result.error.toStdString();
  EXPECT_TRUE(Verify(feed, 20261027000000ULL));  // The same sequence stays valid.
}

TEST(AiModelPackageManifestTest, ArchiveOnForeignHostIsRejected) {
  const SignedFeed                   feed   = SignFeed({EntryJson(
      1, ArtifactJson(QStringLiteral("windows-x86_64"), 1,
                                          QStringLiteral("https://example.com/ai-models/alcedo-ai-mask.7z")))});
  const AiModelPackageManifestResult result = Verify(feed);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.indexOf(QStringLiteral("not allowed")), -1) << result.error.toStdString();
}

TEST(AiModelPackageManifestTest, ArchiveThatIsNot7zOrNotHttpsIsRejected) {
  for (const QString& url :
       {QStringLiteral("https://static.aoraw.org/ai-models/v1/alcedo-ai-mask.zip"),
        QStringLiteral("http://static.aoraw.org/ai-models/v1/alcedo-ai-mask.7z"),
        QStringLiteral("https://user@static.aoraw.org/ai-models/v1/alcedo-ai-mask.7z")}) {
    const SignedFeed feed =
        SignFeed({EntryJson(1, ArtifactJson(QStringLiteral("windows-x86_64"), 1, url))});
    const AiModelPackageManifestResult result = Verify(feed);
    EXPECT_FALSE(result) << url.toStdString();
    EXPECT_NE(result.error.indexOf(QStringLiteral("not allowed")), -1)
        << url.toStdString() << ": " << result.error.toStdString();
  }
}

TEST(AiModelPackageManifestTest, DuplicateIdAndModelSetVersionIsRejected) {
  const SignedFeed feed = SignFeed({EntryJson(1), EntryJson(1, {}, QStringLiteral("r2"))});
  const AiModelPackageManifestResult result = Verify(feed);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.indexOf(QStringLiteral("twice")), -1) << result.error.toStdString();
}

TEST(AiModelPackageManifestTest, InvalidRevisionLicensePathAndKindAreRejected) {
  EXPECT_FALSE(Verify(SignFeed({EntryJson(1, {}, QStringLiteral("2026.10"))})));
  QString traversal = EntryJson(1);
  traversal.replace(QStringLiteral("licenses/GPL-3.0.txt"), QStringLiteral("../GPL-3.0.txt"));
  EXPECT_FALSE(Verify(SignFeed({traversal})));
  SignedFeed other_kind = SignFeed({EntryJson(1)});
  other_kind.json.replace("alcedo-ai-model-packages", "alcedo-lut-packages");
  EXPECT_FALSE(VerifyAiModelPackageManifest(other_kind.json, other_kind.signature,
                                            other_kind.public_key, kFeedUrl, 1));
}

TEST(AiModelPackageManifestTest, SelectionPicksHighestSupportedVersionForPlatform) {
  const SignedFeed feed = SignFeed(
      {EntryJson(1), EntryJson(2), EntryJson(3, ArtifactJson(QStringLiteral("macos-arm64"), 3))});
  const AiModelPackageManifestResult result = Verify(feed);
  ASSERT_TRUE(result) << result.error.toStdString();
  const AiModelPackageManifest&   manifest = *result.manifest;
  const QString                   id       = QStringLiteral("alcedo-ai-mask");
  const QString                   windows  = QStringLiteral("windows-x86_64");

  const AiModelPackageDescriptor* only_v1  = SelectAiModelPackage(manifest, id, 1, 1, windows);
  ASSERT_NE(only_v1, nullptr);
  EXPECT_EQ(only_v1->model_set_version, 1u);

  // v3 has no Windows artifact, so the highest supported Windows entry is v2.
  const AiModelPackageDescriptor* up_to_v3 = SelectAiModelPackage(manifest, id, 1, 3, windows);
  ASSERT_NE(up_to_v3, nullptr);
  EXPECT_EQ(up_to_v3->model_set_version, 2u);
  EXPECT_EQ(
      SelectAiModelPackage(manifest, id, 1, 3, QStringLiteral("macos-arm64"))->model_set_version,
      3u);

  // Versions outside the range are ignored.
  EXPECT_EQ(SelectAiModelPackage(manifest, id, 4, 9, windows), nullptr);
  EXPECT_EQ(SelectAiModelPackage(manifest, QStringLiteral("other"), 1, 3, windows), nullptr);
  EXPECT_TRUE(ListsAiModelPackageAbove(manifest, id, 1, windows));
  EXPECT_FALSE(ListsAiModelPackageAbove(manifest, id, 2, windows));
}

auto ReadFixture(const char* name) -> QByteArray {
  QFile file(QDir(QStringLiteral(ALCEDO_AI_MODEL_PACKAGE_FIXTURE_DIR)).filePath(name));
  EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name;
  return file.readAll();
}

// scripts/ai_models/tests/test_ai_model_packages.py writes and checks this feed
// (AiModelPackageCppFixtureTest).
TEST(AiModelPackageManifestTest, PythonPreparedFeedVerifiesInCpp) {
  const QByteArray json      = ReadFixture("manifest.json");
  const QByteArray signature = ReadFixture("manifest.json.sig");
  const QByteArray public_key =
      QByteArray::fromBase64(ReadFixture("test_public_key.txt").trimmed());
  ASSERT_EQ(public_key.size(), 32);

  const AiModelPackageManifestResult result =
      VerifyAiModelPackageManifest(json, signature, public_key, kFeedUrl, 20261027120000ULL);
  ASSERT_TRUE(result) << result.error.toStdString();
  EXPECT_EQ(result.manifest->sequence, 20261027120000ULL);
  ASSERT_EQ(result.manifest->packages.size(), 1);
  const AiModelPackageDescriptor& entry = result.manifest->packages.front();
  EXPECT_EQ(entry.id, QStringLiteral("alcedo-ai-mask"));
  EXPECT_EQ(entry.name, QStringLiteral("AI Mask"));
  EXPECT_EQ(entry.model_set_version, 1u);
  EXPECT_EQ(entry.revision, QStringLiteral("r1"));
  QStringList license_names;
  QStringList license_files;
  for (const AiModelPackageLicense& license : entry.licenses) {
    license_names.push_back(license.name);
    license_files.push_back(license.file);
  }
  EXPECT_EQ(license_names,
            (QStringList{QStringLiteral("Apache-2.0"), QStringLiteral("MIT"),
                         QStringLiteral("DINOv3 License"), QStringLiteral("GPL-3.0")}));
  EXPECT_EQ(license_files.front(), QStringLiteral("licenses/SAM2-Apache-2.0.txt"));
  for (const QString platform : {QStringLiteral("windows-x86_64"), QStringLiteral("macos-arm64")}) {
    const AiModelPackageArtifact* artifact = entry.Artifact(platform);
    ASSERT_NE(artifact, nullptr) << platform.toStdString();
    EXPECT_EQ(artifact->url.toString(),
              QStringLiteral("https://static.aoraw.org/ai-models/v1/packages/alcedo-ai-mask/v1/r1/"
                             "alcedo-ai-mask-v1-r1-%1.7z")
                  .arg(platform));
    EXPECT_EQ(artifact->size, ReadFixture(qPrintable(artifact->url.fileName())).size());
    EXPECT_GT(artifact->file_count, 10u);
    EXPECT_EQ(artifact->inventory_sha256.size(), 32);
  }
  EXPECT_EQ(SelectAiModelPackage(*result.manifest, entry.id, 1, 1, QStringLiteral("macos-arm64")),
            &entry);

  // The Python feed is signed over its exact bytes.
  QByteArray changed = json;
  changed.replace("\"r1\"", "\"r2\"");
  EXPECT_FALSE(
      VerifyAiModelPackageManifest(changed, signature, public_key, kFeedUrl, 20261027120000ULL));
}

}  // namespace
}  // namespace alcedo::test
