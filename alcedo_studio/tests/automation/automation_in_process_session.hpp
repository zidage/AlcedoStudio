//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <memory>
#include <optional>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"
#include "ui/alcedo_main/automation/automation_host_commands.hpp"
#include "ui/alcedo_main/automation/automation_library_commands.hpp"
#include "ui/alcedo_main/automation/automation_project_commands.hpp"
#include "ui/alcedo_main/automation/automation_task_commands.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation::test {

/// An application module host and the command registry of a headless session in the test
/// process. Commands go through AutomationCommandRegistry::Dispatch, the path that the server
/// uses for every socket request.
class InProcessAutomationSession {
 public:
  /// @p host_mode is `headless` or `gui`; the library selection commands need `gui`.
  explicit InProcessAutomationSession(const QString& host_mode = QStringLiteral("headless")) {
    AutomationHostCommandContext context;
    context.host      = &host_;
    context.host_mode = host_mode;
    QString    error;
    const bool registered =
        RegisterAutomationHostCommands(registry_, std::move(context), &error) &&
        RegisterAutomationProjectCommands(registry_, &host_, &error) &&
        RegisterAutomationLibraryCommands(registry_, &host_, &tracker_, host_mode, &error) &&
        RegisterAutomationTaskCommands(registry_, &host_, &tracker_, &error) &&
        RegisterAutomationEditorCommands(registry_, &error);
    EXPECT_TRUE(registered) << error.toStdString();
  }

  [[nodiscard]] auto host() -> ui::ApplicationModuleHost& { return host_; }
  [[nodiscard]] auto registry() -> AutomationCommandRegistry& { return registry_; }

  /// Dispatches one request and runs the event loop until its response arrives or
  /// @p wait_ms passes. Returns nothing when no response arrived.
  auto               Call(const QString& method, QJsonObject params = {}, int wait_ms = 180000)
      -> std::optional<AutomationResponse> {
    auto              response = std::make_shared<std::optional<AutomationResponse>>();
    AutomationRequest request{QJsonValue(++next_id_), method, std::move(params)};
    registry_.Dispatch(request, AutomationReply(request.id, [response](AutomationResponse value) {
                         *response = std::move(value);
                       }));
    const QDeadlineTimer deadline(wait_ms);
    while (!response->has_value() && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    }
    return *response;
  }

  /// Calls @p method and returns its result object. Adds a test failure for an error response or
  /// a missing response.
  auto CallResult(const QString& method, QJsonObject params = {}, int wait_ms = 180000)
      -> QJsonObject {
    const auto response = Call(method, std::move(params), wait_ms);
    if (!response.has_value()) {
      ADD_FAILURE() << method.toStdString() << " did not answer";
      return {};
    }
    if (response->error.has_value()) {
      ADD_FAILURE() << method.toStdString()
                    << " failed: " << response->error->message.toStdString();
      return {};
    }
    return response->result.toObject();
  }

 private:
  ui::ApplicationModuleHost host_;
  AutomationTaskTracker     tracker_{&host_};
  AutomationCommandRegistry registry_;
  int                       next_id_ = 0;
};

}  // namespace alcedo::automation::test
