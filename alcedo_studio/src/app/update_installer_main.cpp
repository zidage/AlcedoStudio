//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <string>

#ifdef Q_OS_WIN
#include "app/update_installer_windows.hpp"
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {

auto ArgumentValue(const QStringList& arguments, const QString& name) -> QString {
  const qsizetype index = arguments.indexOf(name);
  return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString{};
}

auto WriteFailure(const QString& package_path, const QString& message) -> int {
  const QString error_directory =
      package_path.isEmpty() ? QDir::tempPath() : QFileInfo(package_path).dir().path();
  QSaveFile output(QDir(error_directory).filePath(QStringLiteral("install-error.txt")));
  if (output.open(QIODevice::WriteOnly | QIODevice::Text)) {
    output.write(message.toUtf8());
    output.write("\n");
    output.commit();
  }
  return 1;
}

auto WaitForProcess(qint64 pid) -> bool {
  if (pid <= 0) {
    return false;
  }
#ifdef Q_OS_WIN
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return GetLastError() == ERROR_INVALID_PARAMETER;
  }
  const DWORD result = WaitForSingleObject(process, 120000);
  CloseHandle(process);
  return result == WAIT_OBJECT_0;
#else
  for (int attempt = 0; attempt < 1200; ++attempt) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
      return true;
    }
    QThread::msleep(100);
  }
  return false;
#endif
}

auto VerifyPackage(const QString& path, const QByteArray& expected_sha256) -> bool {
  QFile package(path);
  if (expected_sha256.size() != 32 || !package.open(QIODevice::ReadOnly)) {
    return false;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&package) && hash.result() == expected_sha256;
}

auto StartApplication(const QString& path) -> bool {
#ifdef Q_OS_MACOS
  return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {path});
#else
  return QProcess::startDetached(path, {});
#endif
}

#ifdef Q_OS_WIN
auto InstallWindows(const QString& package_path, const QString& install_path,
                    const QString& app_executable) -> int {
  const std::wstring package = QDir::toNativeSeparators(package_path).toStdWString();
  const std::wstring parameters = alcedo::BuildSilentNsisArguments(install_path).toStdWString();
  SHELLEXECUTEINFOW  launch{};
  launch.cbSize       = sizeof(launch);
  launch.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  launch.lpVerb       = L"runas";
  launch.lpFile       = package.c_str();
  launch.lpParameters = parameters.c_str();
  launch.nShow        = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&launch) || launch.hProcess == nullptr) {
    StartApplication(app_executable);
    return WriteFailure(package_path, QStringLiteral("The NSIS installer could not start."));
  }
  WaitForSingleObject(launch.hProcess, INFINITE);
  DWORD exit_code = ERROR_INSTALL_FAILURE;
  GetExitCodeProcess(launch.hProcess, &exit_code);
  CloseHandle(launch.hProcess);
  if (exit_code != ERROR_SUCCESS) {
    StartApplication(app_executable);
    return WriteFailure(package_path, QStringLiteral("The NSIS installer did not complete."));
  }
  if (!StartApplication(app_executable)) {
    return WriteFailure(package_path, QStringLiteral("The updated application could not start."));
  }
  return 0;
}
#endif

#ifdef Q_OS_MACOS
auto FindStagedBundle(const QString& directory) -> QString {
  const QFileInfoList entries =
      QDir(directory).entryInfoList({QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
  return entries.size() == 1 ? entries.constFirst().absoluteFilePath() : QString{};
}

auto RunDitto(const QStringList& arguments) -> bool {
  QProcess process;
  process.setProgram(QStringLiteral("/usr/bin/ditto"));
  process.setArguments(arguments);
  process.start();
  return process.waitForStarted(10000) && process.waitForFinished(120000) &&
         process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

auto InstallMac(const QString& package_path, const QString& bundle_path) -> int {
  QTemporaryDir staging(QDir(QFileInfo(package_path).dir().absolutePath())
                            .filePath(QStringLiteral("alcedo-update-stage-XXXXXX")));
  if (!staging.isValid() ||
      !RunDitto({QStringLiteral("-x"), QStringLiteral("-k"), QStringLiteral("--sequesterRsrc"),
                 package_path, staging.path()})) {
    StartApplication(bundle_path);
    return WriteFailure(package_path,
                        QStringLiteral("The macOS update ZIP could not be extracted."));
  }
  const QString staged_bundle = FindStagedBundle(staging.path());
  if (staged_bundle.isEmpty() ||
      QFileInfo(staged_bundle).fileName() != QFileInfo(bundle_path).fileName()) {
    StartApplication(bundle_path);
    return WriteFailure(package_path,
                        QStringLiteral("The update ZIP has an unexpected app bundle."));
  }

  const QFileInfo current_info(bundle_path);
  QDir            parent = current_info.dir();
  const QString   backup_name =
      current_info.fileName() + QStringLiteral(".update-backup-") +
      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss"));
  if (!parent.rename(current_info.fileName(), backup_name)) {
    StartApplication(bundle_path);
    return WriteFailure(package_path,
                        QStringLiteral("The app location is not writable. Move Alcedo Studio to a "
                                       "user-writable folder or install the update manually."));
  }

  if (!RunDitto({staged_bundle, bundle_path})) {
    QDir(bundle_path).removeRecursively();
    parent.rename(backup_name, current_info.fileName());
    StartApplication(bundle_path);
    return WriteFailure(
        package_path,
        QStringLiteral("The new app bundle could not be copied. The old app was restored."));
  }

  if (!StartApplication(bundle_path)) {
    QDir(bundle_path).removeRecursively();
    parent.rename(backup_name, current_info.fileName());
    StartApplication(bundle_path);
    return WriteFailure(package_path,
                        QStringLiteral("The new app did not start. The old app was restored."));
  }
  // Keep the backup. A later maintenance action can remove it after the user
  // confirms that the new version works.
  return 0;
}
#endif

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication  application(argc, argv);
  const QStringList arguments = application.arguments();
  const qint64      pid       = ArgumentValue(arguments, QStringLiteral("--wait-pid")).toLongLong();
  const QString     package_path = ArgumentValue(arguments, QStringLiteral("--package"));
  const QByteArray  expected_sha256 =
      QByteArray::fromHex(ArgumentValue(arguments, QStringLiteral("--sha256")).toLatin1());
  const QString install_path   = ArgumentValue(arguments, QStringLiteral("--install-path"));
  const QString app_executable = ArgumentValue(arguments, QStringLiteral("--app-executable"));

  if (package_path.isEmpty() || install_path.isEmpty() || app_executable.isEmpty() ||
      !VerifyPackage(package_path, expected_sha256)) {
    return WriteFailure(package_path, QStringLiteral("The update helper rejected its input."));
  }
  if (!WaitForProcess(pid)) {
    return WriteFailure(package_path,
                        QStringLiteral("The application did not close before the update timeout."));
  }

#ifdef Q_OS_WIN
  return InstallWindows(package_path, install_path, app_executable);
#elif defined(Q_OS_MACOS)
  return InstallMac(package_path, install_path);
#else
  return WriteFailure(package_path, QStringLiteral("This platform is not supported."));
#endif
}
