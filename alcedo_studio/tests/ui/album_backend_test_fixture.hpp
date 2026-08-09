//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_test_fixture.hpp
/// @brief Shared test fixture for ApplicationModuleHost UI unit tests.
///
/// Provides temp project creation, Qt event-loop helpers, and image collection
/// utilities.  Designed for GoogleTest + QSignalSpy; a custom main() must
/// create a QCoreApplication before RUN_ALL_TESTS().

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <string>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "type/supported_file_type.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/profiler/profiler.hpp"

namespace alcedo::ui::test {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Process the Qt event loop for up to @p ms milliseconds.
/// This allows queued signals (QMetaObject::invokeMethod with
/// Qt::QueuedConnection) to be delivered.
inline void ProcessEvents(int ms = 200) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

/// Wait for a QSignalSpy to receive at least one signal, processing the event
/// loop.  Returns true if the signal arrived within @p timeoutMs.
inline bool WaitForSignal(QSignalSpy& spy, int timeoutMs = 5000) {
  if (!spy.isEmpty()) return true;
  return spy.wait(timeoutMs);
}

/// Collect RAW test images from a subdirectory under TEST_IMG_PATH.
inline auto CollectRawTestImages(const std::string& subdir, size_t maxCount = 0)
    -> std::vector<std::filesystem::path> {
  const auto collect_from_root = [maxCount](const std::filesystem::path& root) {
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::exists(root)) return paths;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
        if (maxCount > 0 && paths.size() >= maxCount) {
          break;
        }
      }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  };

  const std::filesystem::path root{std::string(TEST_IMG_PATH) + "/raw/" + subdir};
  auto                        paths = collect_from_root(root);
  if (!paths.empty()) {
    return paths;
  }

  const auto fallback_root = std::filesystem::path("/Users/zidage/Photos");
  return collect_from_root(fallback_root);
}

/// Collect RAW test images only from a specific fixture subdirectory.
inline auto CollectRawFixtureImages(const std::string& subdir, size_t maxCount = 0)
    -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/raw/" + subdir};
  std::vector<std::filesystem::path> paths;
  if (!std::filesystem::exists(root)) return paths;

  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file() && is_supported_file(entry.path())) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  if (maxCount > 0 && paths.size() > maxCount) {
    paths.resize(maxCount);
  }
  return paths;
}

inline auto HasRawTestImages(const std::string& subdir) -> bool {
  return !CollectRawTestImages(subdir, 1).empty();
}

/// Convert a filesystem path to a QString suitable for ApplicationModuleHost methods.
inline auto PathToQString(const std::filesystem::path& p) -> QString {
#ifdef _WIN32
  return QString::fromStdWString(p.wstring());
#else
  return QString::fromStdString(p.string());
#endif
}

/// Convert a vector of filesystem paths to a QStringList.
inline auto PathsToQStringList(const std::vector<std::filesystem::path>& paths) -> QStringList {
  QStringList list;
  list.reserve(static_cast<int>(paths.size()));
  for (const auto& p : paths) {
    list.append(PathToQString(p));
  }
  return list;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/// Base fixture for all ApplicationModuleHost tests.
///
/// Creates an isolated temp directory per test, initialises global singletons
/// (operator registry, Exiv2, clock), and cleans up on tear-down.
class ApplicationModuleHostTestFixture : public ::testing::Test {
 protected:
  std::filesystem::path temp_dir_;   ///< Per-test temp directory.
  std::filesystem::path db_path_;    ///< Temp DB file path.
  std::filesystem::path meta_path_;  ///< Temp metadata JSON path.

  void                  SetUp() override {
    alcedo::TimeProvider::Refresh();
    alcedo::RegisterAllOperators();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);

    // Create a unique temp directory for each test.
    temp_dir_ = std::filesystem::temp_directory_path() /
                ("alcedo_ui_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir_);

    db_path_   = temp_dir_ / "test.db";
    meta_path_ = temp_dir_ / "meta.json";

#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_ENABLE;
#endif
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);

#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_DISABLE;
#endif
  }

  /// Create a new packed project inside the temp directory, wait for
  /// ProjectChanged, and return true if the backend became serviceReady.
  bool CreateTestProject(ApplicationModuleHost& backend, const QString& name = "ui_test_project") {
    QSignalSpy spy(backend.project(), &ProjectModule::ProjectChanged);
    const bool ok = backend.project()->CreateProjectInFolderNamed(PathToQString(temp_dir_), name);
    if (!ok) return false;

    // The project loading is async — wait for LoadProject + pipeline init
    // to finish (ProjectChanged signal).
    WaitForSignal(spy, 15000);
    // Process remaining queued events (stats rebuild, etc.)
    ProcessEvents(500);
    return backend.project()->ServiceReady();
  }
};

}  // namespace alcedo::ui::test
