//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <QSignalSpy>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostI18nTests = ApplicationModuleHostTestFixture;

TEST_F(ApplicationModuleHostI18nTests, InitialLocalizedStrings_AreAvailable) {
  ApplicationModuleHost backend;

  EXPECT_FALSE(backend.project()->ServiceMessage().isEmpty());
  EXPECT_FALSE(backend.project()->TaskStatus().isEmpty());
  EXPECT_FALSE(backend.import_export()->ExportStatus().isEmpty());
  EXPECT_FALSE(backend.editor()->EditorStatus().isEmpty());
}

TEST_F(ApplicationModuleHostI18nTests, TranslationNotifier_RefreshesObservableStateSignals) {
  ApplicationModuleHost backend;

  QSignalSpy service_spy(backend.project(), &ProjectModule::ServiceStateChanged);
  QSignalSpy task_spy(backend.project(), &ProjectModule::TaskStateChanged);
  QSignalSpy import_spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  QSignalSpy export_spy(backend.import_export(), &ImportExportHandler::ExportStateChanged);
  QSignalSpy editor_spy(backend.editor(), &EditorController::EditorStateChanged);
  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectLoadStateChanged);

  i18n::TranslationNotifier::Instance().NotifyLanguageChanged();
  ProcessEvents(100);

  EXPECT_GE(service_spy.count(), 1);
  EXPECT_GE(task_spy.count(), 1);
  EXPECT_GE(import_spy.count(), 1);
  EXPECT_GE(export_spy.count(), 1);
  EXPECT_GE(editor_spy.count(), 1);
  EXPECT_GE(project_spy.count(), 1);
}

}  // namespace
}  // namespace alcedo::ui::test
