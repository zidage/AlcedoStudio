//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QString>

#include "ui/alcedo_main/album_backend/agent_control_owner.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// The control state that `control.*`, `state.get`, and `control.changed` report: `held`,
/// `revoked_by_user`, and `holder_name`. A null @p owner is the headless host, where the agent
/// always holds control.
auto AutomationControlState(const ui::AgentControlOwner* owner) -> QJsonObject;

/// The agent control rules of a GUI session over @p owner: a state-changing command needs
/// control (-32003 without it, -32004 after the user took control), and its summary shows in the
/// agent task while it runs. @p owner must outlive every dispatch.
auto MakeAgentStateChangeControl(ui::AgentControlOwner* owner) -> AutomationStateChangeControl;

/// Registers `control.acquire`, `control.release`, and `control.get`. With a null @p owner
/// (headless host) the agent always holds control and the commands report `held: true`. Returns
/// false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationControlCommands(AutomationCommandRegistry& registry,
                                                     ui::AgentControlOwner*     owner,
                                                     QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
