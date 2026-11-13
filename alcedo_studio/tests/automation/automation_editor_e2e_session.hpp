//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Fixture for end-to-end tests of the editor commands: a headless session with one CI RAW file.
// On Windows a build with CUDA uses the CUDA editor backend.

#pragma once

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <cmath>

#include "automation_e2e_session.hpp"
#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::automation::test {

class AutomationEditorE2ETestBase : public AutomationE2ETestBase {
 protected:
  void SetUp() override {
    AutomationE2ETestBase::SetUp();
#ifdef _WIN32
    if (editor_rhi::IsBackendAvailableInThisBuild(editor_rhi::EditorBackend::Cuda)) {
      extra_host_options_ = QStringList{"--editor-backend", "cuda"};
    }
#endif
  }

  /// Starts a session with a new project that holds one CI RAW file and returns the project
  /// path. Sets element_ to the photo.
  auto StartWithOnePhoto(const QString& name) -> QString {
    const auto raw_files = CollectCiRawFiles();
    if (raw_files.empty()) {
      ADD_FAILURE() << "the CI RAW files are missing under " << TEST_IMG_PATH;
      return {};
    }
    const QString project_path = StartWithNewProject(name);
    if (project_path.isEmpty()) {
      return {};
    }
    EXPECT_EQ(ImportAndWait({raw_files.front()}).value("state").toString(),
              QStringLiteral("succeeded"));
    const QJsonArray items = ListItems();
    if (items.size() != 1) {
      ADD_FAILURE() << "expected one photo, listed " << items.size();
      return {};
    }
    element_ = items.first().toObject().value("element_id");
    return project_path;
  }

  /// Opens the photo in the editor and returns the editor.open result.
  auto OpenPhoto() -> QJsonObject {
    const QJsonObject opened =
        Result(QStringLiteral("editor.open"), QJsonObject{{"element_id", element_}});
    EXPECT_EQ(opened.value("state").toString(), QStringLiteral("Interactive"));
    return opened;
  }

  auto Value(const QString& field) -> double {
    return Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{field}}})
        .value("values")
        .toObject()
        .value(field)
        .toDouble(std::nan(""));
  }

  auto CommitCount() -> qsizetype {
    return Result(QStringLiteral("editor.history")).value("commits").toArray().size();
  }

  auto Head() -> QJsonValue {
    return Result(QStringLiteral("editor.history")).value("head_commit");
  }

  auto Set(const QString& field, double value) -> QJsonObject {
    return Result(QStringLiteral("editor.set"), QJsonObject{{"field", field}, {"value", value}});
  }

  QJsonValue element_;
};

}  // namespace alcedo::automation::test
