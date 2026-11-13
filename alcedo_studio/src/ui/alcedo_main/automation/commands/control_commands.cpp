//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <functional>
#include <optional>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_control_commands.hpp"

namespace alcedo::automation {
namespace {

using CommandHandler = std::function<void(const QJsonObject& params, AutomationReply reply)>;

auto ControlStateSchema() -> QJsonObject {
  return AutomationObjectSchema(
      QJsonObject{{"held", QJsonObject{{"type", "boolean"}}},
                  {"revoked_by_user",
                   QJsonObject{{"type", "boolean"},
                               {"description",
                                "True after the user stopped the agent, until an agent calls "
                                "control.acquire again."}}},
                  {"holder_name", QJsonObject{{"type", "string"}}}},
      QJsonArray{"held", "revoked_by_user", "holder_name"});
}

/// The answer to a state-changing command after the user stopped the agent. The agent must not
/// continue on its own: the user tells it when to continue, and then it calls control.acquire.
auto StoppedRefusal() -> AutomationCommandRefusal {
  const QString message = QStringLiteral(
      "The user stopped the agent. Stop working and ask the user what to do. Call "
      "control.acquire only after the user tells you to continue.");
  return AutomationCommandRefusal{AutomationErrorCode::ControlRevokedByUser, message,
                                  QJsonObject{{"reason", message}}};
}

constexpr auto kDefaultHolderName = "agent";

/// Answers one acquire with @p refusal.
void           AnswerAcquire(AutomationCommandWait* wait, const ui::AgentControlOwner* control,
                             ui::AgentControlRefusal refusal) {
  const QString holder = control != nullptr ? control->holder_name() : QString();
  switch (refusal) {
    case ui::AgentControlRefusal::None:
      wait->SendResult(AutomationControlState(control));
      return;
    case ui::AgentControlRefusal::HeldByOtherAgent:
      wait->SendError(AutomationErrorCode::Busy,
                                QStringLiteral("Another agent holds control: %1").arg(holder),
                                QJsonObject{{"holder_name", holder}});
      return;
    case ui::AgentControlRefusal::UnsealedEditorInput:
      // RequestAcquire waits in this case and never reports it.
      wait->SendOwnerError(AutomationErrorCode::Failed,
                                     QStringLiteral("The editor input of the user is not sealed."));
      return;
  }
}

auto AcquireHandler(QPointer<ui::AgentControlOwner> control, bool headless) -> CommandHandler {
  return [control, headless](const QJsonObject& params, AutomationReply reply) {
    if (headless) {
      reply.SendResult(AutomationControlState(nullptr));
      return;
    }
    if (control == nullptr) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("Agent control is not available."));
      return;
    }
    QString holder_name = params.value("holder_name").toString().trimmed();
    if (holder_name.isEmpty()) {
      holder_name = QLatin1String(kDefaultHolderName);
    }
    // The wait object lives with the owner, so an acquire that waits past the timeout response
    // does not outlive it.
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), control);
    control->RequestAcquire(
        holder_name, [wait]() { return wait != nullptr && !wait->IsAnswered(); },
        [wait, control](ui::AgentControlRefusal refusal) {
          if (wait != nullptr) {
            AnswerAcquire(wait, control, refusal);
          }
        });
  };
}

auto ReleaseHandler(QPointer<ui::AgentControlOwner> control, bool headless) -> CommandHandler {
  return [control, headless](const QJsonObject&, AutomationReply reply) {
    if (!headless && control != nullptr) {
      (void)control->Release();
    }
    reply.SendResult(AutomationControlState(headless ? nullptr : control.data()));
  };
}

auto GetHandler(QPointer<ui::AgentControlOwner> control, bool headless) -> CommandHandler {
  return [control, headless](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(AutomationControlState(headless ? nullptr : control.data()));
  };
}

}  // namespace

auto AutomationControlState(const ui::AgentControlOwner* owner) -> QJsonObject {
  if (owner == nullptr) {
    return QJsonObject{{"held", true}, {"revoked_by_user", false}, {"holder_name", ""}};
  }
  return QJsonObject{{"held", owner->held()},
                     {"revoked_by_user", owner->stopped_by_user()},
                     {"holder_name", owner->holder_name()}};
}

auto MakeAgentStateChangeControl(ui::AgentControlOwner* owner) -> AutomationStateChangeControl {
  QPointer<ui::AgentControlOwner> control = owner;
  AutomationStateChangeControl    rules;
  rules.admit = [control]() -> std::optional<AutomationCommandRefusal> {
    if (control != nullptr && control->held()) {
      return std::nullopt;
    }
    if (control != nullptr && control->stopped_by_user()) {
      return StoppedRefusal();
    }
    const QString message = QStringLiteral(
        "This command changes the session state and needs agent control. Call control.acquire "
        "first.");
    return AutomationCommandRefusal{AutomationErrorCode::ControlNotHeld, message,
                                    QJsonObject{{"reason", message}}};
  };
  rules.begin_command = [control](const QString& summary, const QString& note) -> quint64 {
    return control != nullptr ? control->BeginCommand(summary, note) : 0;
  };
  rules.end_command = [control](quint64 token, bool succeeded) {
    if (control != nullptr) {
      control->EndCommand(token, succeeded);
    }
  };
  return rules;
}

auto RegisterAutomationControlCommands(AutomationCommandRegistry& registry,
                                       ui::AgentControlOwner* owner, QString* error) -> bool {
  const QPointer<ui::AgentControlOwner> control  = owner;
  const bool                            headless = owner == nullptr;

  // The control commands are not state-changing commands: they change only the control itself,
  // so they run without control.
  AutomationCommandSpec                 acquire;
  acquire.method      = QStringLiteral("control.acquire");
  acquire.description = QStringLiteral(
      "Takes control of a GUI session so that state-changing commands run. While the agent holds "
      "control, the user cannot change the session state; the task island shows the agent task "
      "with each state-changing command and its note. Waits while the user drags an editor "
      "control. After the user stopped the agent, state-changing commands fail with -32004; "
      "call control.acquire again only after the user tells you to continue, and the agent "
      "task continues. Fails with -32005 while another holder holds control. The headless host "
      "always reports held.");
  acquire.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"holder_name",
       QJsonObject{
           {"type", "string"},
           {"default", kDefaultHolderName},
           {"description", "The name that the task island shows, for example claude-code."}}}});
  acquire.result_schema = ControlStateSchema();
  acquire.handler       = AcquireHandler(control, headless);
  if (!registry.Register(std::move(acquire), error)) {
    return false;
  }

  AutomationCommandSpec release;
  release.method      = QStringLiteral("control.release");
  release.description = QStringLiteral(
      "Gives control of a GUI session back to the user. The agent task finishes as succeeded. "
      "Without control the state does not change.");
  release.params_schema = AutomationClosedParamsSchema();
  release.result_schema = ControlStateSchema();
  release.handler       = ReleaseHandler(control, headless);
  if (!registry.Register(std::move(release), error)) {
    return false;
  }

  AutomationCommandSpec get;
  get.method        = QStringLiteral("control.get");
  get.description   = QStringLiteral("Returns the agent control state of the session.");
  get.params_schema = AutomationClosedParamsSchema();
  get.result_schema = ControlStateSchema();
  get.handler       = GetHandler(control, headless);
  return registry.Register(std::move(get), error);
}

}  // namespace alcedo::automation
