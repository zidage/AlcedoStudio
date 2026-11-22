//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: the budget case and the hold case (a second process that holds video memory).
// Experiment code: it does not merge.
#pragma once

#include <string>

#include "vx5_compute.hpp"

namespace vx5 {

struct BudgetArgs {
  std::string spv_path;
  int         block_mib    = 256;
  /// The allocation goes this far above the free budget at the start (budget - usage).
  int         over_mib     = 2048;
  /// When > 0: the total of the allocation, in place of the budget rule.
  int         max_mib      = 0;
  /// none | same | split (see RunBudget).
  std::string priority     = "none";
  double      hold_seconds = 5.0;
  int         work_mib     = 1536;
  /// The allocation stops when the free physical memory of the computer falls below this value.
  int         min_free_mib = 6144;
  bool        display_probe = true;
};

auto RunBudget(VulkanContext& ctx, Compute& compute, const BudgetArgs& args) -> int;
auto RunHold(VulkanContext& ctx, Compute& compute, int mib, double seconds) -> int;

}  // namespace vx5
