//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "app/editor_adjustment_types.hpp"

namespace alcedo {

class CPUPipelineExecutor;

/// Applies one render request's adjustment state to an executor. The caller
/// must hold executor.GetRenderLock() so the state and resulting frame belong
/// to the same render generation.
auto ApplyEditorAdjustmentSnapshot(CPUPipelineExecutor&                  executor,
                                   const EditorRenderAdjustmentSnapshot& snapshot,
                                   std::string*                          error) -> bool;

}  // namespace alcedo
