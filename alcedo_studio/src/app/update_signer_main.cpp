//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <array>

extern "C" {
#include <ed25519.h>
}

namespace {

auto Value(const QStringList& arguments, const QString& name) -> QString {
  const qsizetype index = arguments.indexOf(name);
  return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString{};
}

auto WriteBase64File(const QString& path, const unsigned char* data, qsizetype size,
                     QFileDevice::Permissions permissions) -> bool {
  if (QFileInfo::exists(path)) {
    return false;
  }
  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return false;
  }
  output.write(QByteArray(reinterpret_cast<const char*>(data), size).toBase64());
  output.write("\n");
  if (!output.commit()) {
    return false;
  }
  return QFile::setPermissions(path, permissions);
}

auto ReadBase64File(const QString& path, qsizetype expected_size) -> QByteArray {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray value =
      QByteArray::fromBase64(input.readAll().trimmed(), QByteArray::AbortOnBase64DecodingErrors);
  return value.size() == expected_size ? value : QByteArray{};
}

auto Generate(const QStringList& arguments) -> int {
  const QString private_path = Value(arguments, QStringLiteral("--private-key"));
  const QString public_path  = Value(arguments, QStringLiteral("--public-key"));
  if (private_path.isEmpty() || public_path.isEmpty() || QFileInfo::exists(private_path) ||
      QFileInfo::exists(public_path)) {
    qCritical("Key paths are missing or already exist.");
    return 2;
  }

  std::array<unsigned char, 32> seed{};
  std::array<unsigned char, 32> public_key{};
  std::array<unsigned char, 64> private_key{};
  if (ed25519_create_seed(seed.data()) != 0) {
    qCritical("The operating system random source failed.");
    return 1;
  }
  ed25519_create_keypair(public_key.data(), private_key.data(), seed.data());

  const QFileDevice::Permissions private_permissions =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner;
  const QFileDevice::Permissions public_permissions =
      private_permissions | QFileDevice::ReadGroup | QFileDevice::ReadOther;
  if (!WriteBase64File(private_path, seed.data(), static_cast<qsizetype>(seed.size()),
                       private_permissions) ||
      !WriteBase64File(public_path, public_key.data(), static_cast<qsizetype>(public_key.size()),
                       public_permissions)) {
    qCritical("A key file could not be written. Remove any partial output and try again.");
    return 1;
  }
  qInfo().noquote() << QString::fromLatin1(
      QByteArray(reinterpret_cast<const char*>(public_key.data()), public_key.size()).toBase64());
  return 0;
}

auto Sign(const QStringList& arguments) -> int {
  const QString private_path   = Value(arguments, QStringLiteral("--private-key"));
  const QString manifest_path  = Value(arguments, QStringLiteral("--manifest"));
  const QString signature_path = Value(arguments, QStringLiteral("--signature"));
  if (private_path.isEmpty() || manifest_path.isEmpty() || signature_path.isEmpty() ||
      QFileInfo::exists(signature_path)) {
    qCritical("Signing paths are missing or the signature output already exists.");
    return 2;
  }
  const QByteArray seed = ReadBase64File(private_path, 32);
  QFile            manifest_file(manifest_path);
  if (seed.size() != 32 || !manifest_file.open(QIODevice::ReadOnly)) {
    qCritical("The private key or manifest could not be read.");
    return 1;
  }
  const QByteArray              manifest = manifest_file.readAll();
  std::array<unsigned char, 32> public_key{};
  std::array<unsigned char, 64> private_key{};
  std::array<unsigned char, 64> signature{};
  ed25519_create_keypair(public_key.data(), private_key.data(),
                         reinterpret_cast<const unsigned char*>(seed.constData()));
  ed25519_sign(signature.data(), reinterpret_cast<const unsigned char*>(manifest.constData()),
               static_cast<size_t>(manifest.size()), public_key.data(), private_key.data());
  if (ed25519_verify(signature.data(), reinterpret_cast<const unsigned char*>(manifest.constData()),
                     static_cast<size_t>(manifest.size()), public_key.data()) != 1 ||
      !WriteBase64File(signature_path, signature.data(), static_cast<qsizetype>(signature.size()),
                       QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
                           QFileDevice::ReadOther)) {
    qCritical("The signature could not be created or verified.");
    return 1;
  }
  qInfo("The manifest signature is valid.");
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication  application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() >= 2 && arguments.at(1) == QStringLiteral("generate")) {
    return Generate(arguments);
  }
  if (arguments.size() >= 2 && arguments.at(1) == QStringLiteral("sign")) {
    return Sign(arguments);
  }
  qCritical(
      "Usage:\n  alcedo_update_signer generate --private-key PATH --public-key PATH\n  "
      "alcedo_update_signer sign --private-key PATH --manifest PATH --signature PATH");
  return 2;
}
