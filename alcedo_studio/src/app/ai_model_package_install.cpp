//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_model_package_install.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <system_error>
#include <utility>

#include "app/package_archive.hpp"

namespace alcedo {
namespace {

namespace fs                                      = std::filesystem;

/// Upper bounds of the small JSON files of a package; they are not part of `unpacked_bytes`.
constexpr std::uint64_t    kMaximumInventoryBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t    kMaximumSmallJsonBytes = 64ULL * 1024ULL;
constexpr std::string_view kContentDirectoryName  = "content";

auto                       ToQString(const fs::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                                 static_cast<qsizetype>(text.size()));
}

auto ParseJsonObject(std::string_view bytes, QJsonObject* object) -> bool {
  QJsonParseError     error;
  const QJsonDocument document = QJsonDocument::fromJson(
      QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  *object = document.object();
  return true;
}

auto ReadUnsigned(const QJsonObject& object, const char* name, std::uint64_t* output) -> bool {
  const QJsonValue value = object.value(QLatin1String(name));
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number ||
      number > 9007199254740991.0) {
    return false;
  }
  *output = static_cast<std::uint64_t>(number);
  return true;
}

auto ReadText(const QJsonObject& object, const char* name, std::string* output) -> bool {
  const QJsonValue value = object.value(QLatin1String(name));
  if (!value.isString()) return false;
  *output = value.toString().toStdString();
  return true;
}

auto HasSchema(const QJsonObject& object, std::string_view kind) -> bool {
  return object.value(QStringLiteral("schema")).toDouble() == 1.0 &&
         object.value(QStringLiteral("kind")).toString().toStdString() == kind;
}

auto IsLowerHexDigest(std::string_view text) -> bool {
  return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

/// Parsed `package-inventory.json` of an AI model package.
struct AiModelPackageInventory {
  std::string                       package_id;
  std::uint64_t                     model_set_version = 0;
  std::string                       revision;
  std::string                       platform;
  std::vector<PackageInventoryFile> files;
};

auto ParseInventory(std::string_view bytes) -> std::optional<AiModelPackageInventory> {
  QJsonObject object;
  if (!ParseJsonObject(bytes, &object) || !HasSchema(object, "alcedo-ai-model-package-inventory")) {
    return std::nullopt;
  }
  AiModelPackageInventory inventory;
  if (!ReadText(object, "id", &inventory.package_id) ||
      !ReadUnsigned(object, "model_set_version", &inventory.model_set_version) ||
      !ReadText(object, "revision", &inventory.revision) ||
      !ReadText(object, "platform", &inventory.platform)) {
    return std::nullopt;
  }
  const QJsonValue files = object.value(QStringLiteral("files"));
  if (!files.isArray()) return std::nullopt;
  for (const QJsonValue& value : files.toArray()) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject    item = value.toObject();
    PackageInventoryFile file;
    if (!ReadText(item, "path", &file.path) || !IsSafePackageRelativePath(file.path) ||
        !ReadUnsigned(item, "size", &file.size) || !ReadText(item, "sha256", &file.sha256) ||
        !IsLowerHexDigest(file.sha256)) {
      return std::nullopt;
    }
    inventory.files.push_back(std::move(file));
  }
  return inventory;
}

/// `model_set.json`: the identity of the model set that the sidecar reads (AM5).
struct AiModelSetIdentity {
  std::string   package_id;
  std::uint64_t model_set_version = 0;
  std::string   revision;
};

auto ParseModelSet(std::string_view bytes) -> std::optional<AiModelSetIdentity> {
  QJsonObject        object;
  AiModelSetIdentity identity;
  if (!ParseJsonObject(bytes, &object) || !HasSchema(object, "alcedo-ai-model-set") ||
      !ReadText(object, "id", &identity.package_id) ||
      !ReadUnsigned(object, "model_set_version", &identity.model_set_version) ||
      !ReadText(object, "revision", &identity.revision)) {
    return std::nullopt;
  }
  return identity;
}

/// Parse `<package id>-v<N>` (N without a leading zero).
auto ParseDirectoryVersion(std::string_view name, std::string_view package_id)
    -> std::optional<std::uint64_t> {
  const std::string prefix = std::string(package_id) + "-v";
  if (!name.starts_with(prefix)) return std::nullopt;
  const std::string_view digits = name.substr(prefix.size());
  if (digits.empty() || digits.size() > 7 || digits.front() == '0') return std::nullopt;
  std::uint64_t version   = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), version);
  if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
  return version;
}

/// Read one installed version; the problem text is empty when it is complete.
auto ReadInstalledVersion(const fs::path& directory, std::string_view package_id,
                          std::uint64_t version) -> AiModelSetInstallation {
  AiModelSetInstallation installation{directory, version, std::nullopt, {}};
  std::string            bytes;
  if (!ReadSmallPackageFile(directory / PackagePathFromUtf8(kAiModelPackageReceiptFileName),
                            kMaximumSmallJsonBytes, &bytes)) {
    installation.problem = "installed.json is missing";
    return installation;
  }
  std::optional<AiModelPackageReceipt> receipt = ParseAiModelPackageReceipt(bytes);
  if (!receipt || receipt->package_id != package_id || receipt->model_set_version != version) {
    installation.problem = "installed.json is not valid for this directory";
    return installation;
  }
  installation.receipt = std::move(receipt);

  if (!ReadSmallPackageFile(directory / PackagePathFromUtf8(kAiModelPackageInventoryFileName),
                            kMaximumInventoryBytes, &bytes)) {
    installation.problem = "package-inventory.json is missing";
    return installation;
  }
  const std::optional<AiModelPackageInventory> inventory = ParseInventory(bytes);
  if (!inventory || inventory->package_id != package_id ||
      inventory->model_set_version != version ||
      inventory->revision != installation.receipt->revision ||
      inventory->files.size() != installation.receipt->file_count) {
    installation.problem = "package-inventory.json does not match installed.json";
    return installation;
  }
  for (const PackageInventoryFile& file : inventory->files) {
    std::error_code error;
    const fs::path  path = directory / PackagePathFromUtf8(file.path);
    if (!fs::is_regular_file(path, error) || fs::file_size(path, error) != file.size || error) {
      installation.problem = "a model file is missing or changed: " + file.path;
      return installation;
    }
  }
  if (!ReadSmallPackageFile(directory / PackagePathFromUtf8(kAiModelSetFileName),
                            kMaximumSmallJsonBytes, &bytes)) {
    installation.problem = "model_set.json is missing";
    return installation;
  }
  const std::optional<AiModelSetIdentity> model_set = ParseModelSet(bytes);
  if (!model_set || model_set->package_id != package_id ||
      model_set->model_set_version != version ||
      model_set->revision != installation.receipt->revision) {
    installation.problem = "model_set.json does not match installed.json";
  }
  return installation;
}

auto WriteNewFile(const fs::path& path, std::string_view bytes) -> std::string {
  QFile output(ToQString(path));
  if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
          static_cast<qint64>(bytes.size()) ||
      !output.flush()) {
    return "cannot write " + PackagePathToUtf8(path) + ": " + output.errorString().toStdString();
  }
  return {};
}

auto HiddenSibling(const fs::path& model_directory, std::string_view directory_name,
                   std::string_view suffix) -> fs::path {
  return model_directory /
         PackagePathFromUtf8("." + std::string(directory_name) + "." + std::string(suffix));
}

/// Verify the extracted content against the signed fields; empty on success.
auto VerifyExtractedAiModelPackage(const fs::path&                                  content,
                                   const AiModelPackageInstallRequest&              request,
                                   const std::map<std::string, PackageArchiveFile>& files)
    -> std::string {
  const AiModelPackageReceipt& expected = request.expected;
  std::string                  bytes;
  if (!files.contains(std::string(kAiModelPackageInventoryFileName)) ||
      !ReadSmallPackageFile(content / PackagePathFromUtf8(kAiModelPackageInventoryFileName),
                            kMaximumInventoryBytes, &bytes)) {
    return "the package archive has no readable package-inventory.json";
  }
  const std::string digest =
      QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                               QCryptographicHash::Sha256)
          .toHex()
          .toStdString();
  if (digest != expected.inventory_sha256) {
    return "the package inventory differs from the signed package descriptor";
  }
  const std::optional<AiModelPackageInventory> inventory = ParseInventory(bytes);
  if (!inventory) return "the package inventory is invalid";
  std::uint64_t listed_bytes = 0;
  for (const PackageInventoryFile& file : inventory->files) listed_bytes += file.size;
  if (inventory->package_id != expected.package_id ||
      inventory->model_set_version != expected.model_set_version ||
      inventory->revision != expected.revision || inventory->platform != expected.platform ||
      inventory->files.size() != expected.file_count || listed_bytes != expected.unpacked_bytes) {
    return "the archive inventory differs from the signed package descriptor";
  }
  if (std::string mismatch =
          VerifyExtractedPackageFiles(inventory->files, files, kAiModelPackageInventoryFileName);
      !mismatch.empty()) {
    return mismatch;
  }
  if (files.contains(std::string(kAiModelPackageReceiptFileName))) {
    return "the package archive contains installed.json";
  }
  if (!ReadSmallPackageFile(content / PackagePathFromUtf8(kAiModelSetFileName),
                            kMaximumSmallJsonBytes, &bytes)) {
    return "the package archive has no readable model_set.json";
  }
  const std::optional<AiModelSetIdentity> model_set = ParseModelSet(bytes);
  if (!model_set || model_set->package_id != expected.package_id ||
      model_set->model_set_version != expected.model_set_version ||
      model_set->revision != expected.revision) {
    return "model_set.json differs from the signed package descriptor";
  }
  for (const std::string& license : request.license_files) {
    if (!files.contains(license)) return "the package archive does not contain " + license;
  }
  return {};
}

}  // namespace

auto AiModelPackageDirectoryName(std::string_view package_id, std::uint64_t model_set_version)
    -> std::string {
  return std::string(package_id) + "-v" + std::to_string(model_set_version);
}

auto AiModelPackageStagingDirectory(const fs::path& model_directory, std::string_view package_id,
                                    std::uint64_t model_set_version) -> fs::path {
  return HiddenSibling(model_directory, AiModelPackageDirectoryName(package_id, model_set_version),
                       "download");
}

auto SerializeAiModelPackageReceipt(const AiModelPackageReceipt& receipt) -> std::string {
  const auto  text   = [](const std::string& value) { return QString::fromStdString(value); };
  const auto  number = [](std::uint64_t value) { return static_cast<double>(value); };
  QJsonObject object{{QStringLiteral("schema"), 1},
                     {QStringLiteral("kind"), QStringLiteral("alcedo-ai-model-package-receipt")},
                     {QStringLiteral("id"), text(receipt.package_id)},
                     {QStringLiteral("model_set_version"), number(receipt.model_set_version)},
                     {QStringLiteral("revision"), text(receipt.revision)},
                     {QStringLiteral("platform"), text(receipt.platform)},
                     {QStringLiteral("file_count"), number(receipt.file_count)},
                     {QStringLiteral("unpacked_bytes"), number(receipt.unpacked_bytes)},
                     {QStringLiteral("inventory_sha256"), text(receipt.inventory_sha256)},
                     {QStringLiteral("artifact_url"), text(receipt.artifact_url)},
                     {QStringLiteral("artifact_size"), number(receipt.artifact_size)},
                     {QStringLiteral("artifact_sha256"), text(receipt.artifact_sha256)},
                     {QStringLiteral("feed_sequence"), number(receipt.feed_sequence)}};
  return QJsonDocument(object).toJson(QJsonDocument::Indented).toStdString();
}

auto ParseAiModelPackageReceipt(std::string_view bytes) -> std::optional<AiModelPackageReceipt> {
  QJsonObject           object;
  AiModelPackageReceipt receipt;
  if (!ParseJsonObject(bytes, &object) || !HasSchema(object, "alcedo-ai-model-package-receipt") ||
      !ReadText(object, "id", &receipt.package_id) ||
      !ReadUnsigned(object, "model_set_version", &receipt.model_set_version) ||
      !ReadText(object, "revision", &receipt.revision) ||
      !ReadText(object, "platform", &receipt.platform) ||
      !ReadUnsigned(object, "file_count", &receipt.file_count) ||
      !ReadUnsigned(object, "unpacked_bytes", &receipt.unpacked_bytes) ||
      !ReadText(object, "inventory_sha256", &receipt.inventory_sha256) ||
      !ReadText(object, "artifact_url", &receipt.artifact_url) ||
      !ReadUnsigned(object, "artifact_size", &receipt.artifact_size) ||
      !ReadText(object, "artifact_sha256", &receipt.artifact_sha256) ||
      !ReadUnsigned(object, "feed_sequence", &receipt.feed_sequence) ||
      receipt.package_id.empty() || receipt.model_set_version == 0 || receipt.revision.empty() ||
      !IsLowerHexDigest(receipt.inventory_sha256) || !IsLowerHexDigest(receipt.artifact_sha256)) {
    return std::nullopt;
  }
  return receipt;
}

auto ReadAiModelInstallation(const fs::path& model_directory, std::string_view package_id,
                             std::uint64_t minimum_version, std::uint64_t maximum_version)
    -> AiModelInstallation {
  AiModelInstallation installation;
  std::error_code     error;
  if (model_directory.empty() || !fs::is_directory(model_directory, error)) return installation;
  for (fs::directory_iterator item(model_directory, error), end; !error && item != end;
       item.increment(error)) {
    std::error_code status_error;
    if (item->is_symlink(status_error) || !item->is_directory(status_error)) continue;
    const std::optional<std::uint64_t> version =
        ParseDirectoryVersion(PackagePathToUtf8(item->path().filename()), package_id);
    if (!version) continue;
    if (*version < minimum_version) {
      installation.below_minimum_version =
          std::max(installation.below_minimum_version.value_or(0), *version);
    } else if (*version > maximum_version) {
      installation.above_maximum_version =
          std::max(installation.above_maximum_version.value_or(0), *version);
    } else if (!installation.supported || *version > installation.supported->model_set_version) {
      installation.supported = ReadInstalledVersion(item->path(), package_id, *version);
    }
  }
  return installation;
}

auto ReadAiMaskModelInstallation(const fs::path& model_directory) -> AiModelInstallation {
  return ReadAiModelInstallation(model_directory, kAiMaskModelPackageId,
                                 kAiMaskModelSetMinimumVersion, kAiMaskModelSetMaximumVersion);
}

auto RecoverAiModelPackageDirectory(const fs::path& model_directory, std::string_view package_id,
                                    std::uint64_t model_set_version) -> std::vector<std::string> {
  std::vector<std::string> problems;
  const std::string        name     = AiModelPackageDirectoryName(package_id, model_set_version);
  const fs::path           target   = model_directory / PackagePathFromUtf8(name);
  const fs::path           previous = HiddenSibling(model_directory, name, "previous");
  const fs::path           removing = HiddenSibling(model_directory, name, "removing");
  const fs::path           content =
      AiModelPackageStagingDirectory(model_directory, package_id, model_set_version) /
      PackagePathFromUtf8(kContentDirectoryName);
  std::error_code error;
  if (fs::exists(previous, error)) {
    if (!fs::exists(target, error)) {
      // The process stopped between moving the installed set aside and moving the new
      // content into place: the commit did not happen, so the installed set returns.
      fs::rename(previous, target, error);
      if (error) {
        problems.push_back("cannot restore " + PackagePathToUtf8(target) + ": " + error.message());
      }
    } else {
      // The commit happened; only the old set was not removed.
      fs::remove_all(previous, error);
      if (error) {
        problems.push_back("cannot remove " + PackagePathToUtf8(previous) + ": " + error.message());
      }
    }
  }
  for (const fs::path& leftover : {removing, content}) {
    error.clear();
    if (!fs::exists(leftover, error)) continue;
    fs::remove_all(leftover, error);
    if (error) {
      problems.push_back("cannot remove " + PackagePathToUtf8(leftover) + ": " + error.message());
    }
  }
  return problems;
}

auto InstallAiModelPackageArchive(const AiModelPackageInstallRequest&                    request,
                                  const std::atomic<bool>&                               stop,
                                  const std::function<void(AiModelPackageInstallStage)>& on_stage)
    -> AiModelPackageInstallOutcome {
  AiModelPackageInstallOutcome outcome;
  const AiModelPackageReceipt& expected = request.expected;
  const auto                   stage    = [&](AiModelPackageInstallStage value) {
    if (on_stage) on_stage(value);
  };
  if (expected.package_id.empty() || expected.model_set_version == 0 ||
      !IsLowerHexDigest(expected.inventory_sha256) || !IsLowerHexDigest(expected.artifact_sha256) ||
      request.model_directory.empty()) {
    outcome.error = "the package descriptor is not valid";
    return outcome;
  }
  const std::string name =
      AiModelPackageDirectoryName(expected.package_id, expected.model_set_version);
  const fs::path target  = request.model_directory / PackagePathFromUtf8(name);
  const fs::path staging = AiModelPackageStagingDirectory(
      request.model_directory, expected.package_id, expected.model_set_version);
  const fs::path content  = staging / PackagePathFromUtf8(kContentDirectoryName);
  const fs::path previous = HiddenSibling(request.model_directory, name, "previous");

  // 1. The exact archive bytes must match the signed size and SHA-256.
  stage(AiModelPackageInstallStage::kVerifying);
  if (std::string mismatch = VerifyPackageArchiveFile(request.archive_path, expected.artifact_size,
                                                      expected.artifact_sha256);
      !mismatch.empty()) {
    std::error_code ignored;
    fs::remove(request.archive_path, ignored);
    outcome.error = std::move(mismatch);
    return outcome;
  }
  if (stop.load()) {
    outcome.canceled = true;
    outcome.error    = "the installation was canceled";
    return outcome;
  }

  // 2. Finish or undo an earlier interrupted operation before the new content is written.
  if (const std::vector<std::string> problems = RecoverAiModelPackageDirectory(
          request.model_directory, expected.package_id, expected.model_set_version);
      !problems.empty()) {
    outcome.error = problems.front();
    return outcome;
  }

  // 3. Extract and verify into the staging content.
  stage(AiModelPackageInstallStage::kInstalling);
  const auto abandon = [&](std::string error, bool canceled) {
    std::error_code ignored;
    fs::remove_all(content, ignored);
    // A canceled transfer keeps its verified archive for a retry; a rejected one is removed.
    if (!canceled) fs::remove(request.archive_path, ignored);
    outcome.error    = std::move(error);
    outcome.canceled = canceled;
    return outcome;
  };
  const PackageArchiveLimits     limits{expected.unpacked_bytes + kMaximumInventoryBytes,
                                    expected.file_count + 1};
  const PackageArchiveExtraction extraction =
      ExtractPackageArchive(request.archive_path, content, limits, stop);
  if (!extraction.error.empty()) return abandon(extraction.error, extraction.canceled);
  if (std::string mismatch = VerifyExtractedAiModelPackage(content, request, extraction.files);
      !mismatch.empty()) {
    return abandon(std::move(mismatch), false);
  }
  if (std::string error =
          WriteNewFile(content / PackagePathFromUtf8(kAiModelPackageReceiptFileName),
                       SerializeAiModelPackageReceipt(expected));
      !error.empty()) {
    return abandon(std::move(error), false);
  }
  // Last cancellation point. The move into place completes as one operation once started.
  if (stop.load()) return abandon("the installation was canceled", true);

  // 4. Moving the verified content into place is the commit point.
  std::error_code error;
  const bool      replaces = fs::exists(target, error);
  if (replaces) {
    fs::rename(target, previous, error);
    if (error) {
      return abandon(
          "the installed models cannot be replaced (they may be in use): " + error.message(),
          false);
    }
  }
  fs::rename(content, target, error);
  if (error) {
    std::error_code restore_error;
    if (replaces) fs::rename(previous, target, restore_error);
    return abandon("the new models cannot be moved into place: " + error.message(), false);
  }
  outcome.committed = true;

  // 5. Remove the replaced set and the staging directory with the archive.
  std::error_code ignored;
  if (replaces) fs::remove_all(previous, ignored);
  fs::remove_all(staging, ignored);
  return outcome;
}

auto RemoveAiModelPackage(const fs::path& model_directory, std::string_view package_id,
                          std::uint64_t model_set_version) -> std::string {
  if (const std::vector<std::string> problems =
          RecoverAiModelPackageDirectory(model_directory, package_id, model_set_version);
      !problems.empty()) {
    return problems.front();
  }
  const std::string name     = AiModelPackageDirectoryName(package_id, model_set_version);
  const fs::path    target   = model_directory / PackagePathFromUtf8(name);
  const fs::path    removing = HiddenSibling(model_directory, name, "removing");
  std::error_code   error;
  if (!fs::exists(target, error)) return "the models are not installed";
  fs::rename(target, removing, error);
  if (error) {
    return "the installed models cannot be removed (they may be in use): " + error.message();
  }
  fs::remove_all(removing, error);
  if (error) {
    return "some model files cannot be removed from " + PackagePathToUtf8(removing) + ": " +
           error.message();
  }
  fs::remove_all(AiModelPackageStagingDirectory(model_directory, package_id, model_set_version),
                 error);
  return {};
}

}  // namespace alcedo
