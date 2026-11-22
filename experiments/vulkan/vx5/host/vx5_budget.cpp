//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: allocation above the VK_EXT_memory_budget value. Each step allocates one block,
// fills it, and runs a touch pass over every block; the speed of each block shows if Windows keeps
// it in video memory or moved it to shared system memory. Experiment code: it does not merge.
#include "vx5_budget.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "vx2_common.hpp"

namespace vx5 {

using vx2::Log;
using vx2::NowNs;

namespace {

constexpr VkDeviceSize kMiB = 1024ull * 1024ull;

auto Mib(VkDeviceSize bytes) -> unsigned long long { return static_cast<unsigned long long>(bytes / kMiB); }

/// The device-local heap (the first heap with DEVICE_LOCAL).
auto LocalHeap(const std::vector<HeapBudget>& heaps) -> std::size_t {
  for (std::size_t i = 0; i < heaps.size(); ++i) {
    if (heaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) return i;
  }
  return 0;
}

void LogBudget(const char* label, VulkanContext& ctx) {
  const auto heaps = QueryBudget(ctx.physical);
  std::string text;
  for (std::size_t i = 0; i < heaps.size(); ++i) {
    char part[160];
    std::snprintf(part, sizeof(part), " heap%zu(local=%d size=%llu budget=%llu usage=%llu)", i,
                  (heaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 1 : 0, Mib(heaps[i].size),
                  Mib(heaps[i].budget), Mib(heaps[i].usage));
    text += part;
  }
  const auto dxgi = QueryDxgi(ctx.luid);
  std::printf("RESULT budget=%s mib%s dxgi(valid=%d local_budget=%llu local_usage=%llu "
              "nonlocal_budget=%llu nonlocal_usage=%llu) free_physical=%llu\n",
              label, text.c_str(), dxgi.valid ? 1 : 0, Mib(dxgi.local_budget), Mib(dxgi.local_usage),
              Mib(dxgi.nonlocal_budget), Mib(dxgi.nonlocal_usage),
              static_cast<unsigned long long>(FreePhysicalMiB()));
}

struct PassResult {
  VkResult            result = VK_SUCCESS;
  std::vector<double> gbps;
  double              total_ms = 0;
};

auto Pass(Compute& compute, const std::vector<Block>& blocks, std::size_t first = 0) -> PassResult {
  PassResult               p;
  std::vector<const Block*> list;
  for (std::size_t i = first; i < blocks.size(); ++i) list.push_back(&blocks[i]);
  std::vector<double> ms;
  p.result = compute.Touch(list, ms);
  for (std::size_t i = 0; i < list.size(); ++i) {
    p.gbps.push_back(Gbps(list[i]->size, ms[i]));
    p.total_ms += ms[i];
  }
  return p;
}

/// Blocks below half of @p fast: their indices as a compact list ("3-7,9").
auto SlowList(const std::vector<double>& gbps, double fast, std::size_t first, int& count) -> std::string {
  std::string text;
  count = 0;
  std::size_t i = 0;
  while (i < gbps.size()) {
    if (gbps[i] >= 0.5 * fast) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j + 1 < gbps.size() && gbps[j + 1] < 0.5 * fast) ++j;
    count += static_cast<int>(j - i + 1);
    if (!text.empty()) text += ",";
    text += std::to_string(first + i);
    if (j > i) text += "-" + std::to_string(first + j);
    i = j + 1;
  }
  return text.empty() ? "none" : text;
}

void ReportPass(const char* label, int step, const PassResult& p, double fast, std::size_t first = 0) {
  int        slow = 0;
  const auto list = SlowList(p.gbps, fast, first, slow);
  double     lo = 1e30, hi = 0;
  for (double g : p.gbps) {
    lo = std::min(lo, g);
    hi = std::max(hi, g);
  }
  std::printf("RESULT pass=%s step=%d blocks=%zu total_ms=%.1f gbps_min=%.1f gbps_max=%.1f slow=%d "
              "slow_blocks=%s vk=%d\n",
              label, step, p.gbps.size(), p.total_ms, p.gbps.empty() ? 0.0 : lo, hi, slow,
              list.c_str(), static_cast<int>(p.result));
}

void ReportBlocks(const char* label, const PassResult& p) {
  std::string text;
  for (double g : p.gbps) {
    char part[16];
    std::snprintf(part, sizeof(part), "%.0f ", g);
    text += part;
  }
  std::printf("RESULT blocks=%s gbps=[ %s]\n", label, text.c_str());
}

/// Passes back to back (20 ms apart) until @p end: a line for the first and the last pass and for
/// each change of the slow set, then the pass times.
void Repeat(const char* label, Compute& compute, const std::vector<Block>& blocks, std::size_t first,
            std::int64_t end, double fast) {
  std::vector<double> totals;
  std::string         last;
  int                 n = 0;
  PassResult          p;
  while (NowNs() < end) {
    p = Pass(compute, blocks, first);
    if (p.result != VK_SUCCESS) {
      ReportPass(label, n, p, fast, blocks[first].index);
      break;
    }
    int        slow = 0;
    const auto list = SlowList(p.gbps, fast, blocks[first].index, slow);
    if (n == 0 || list != last) ReportPass(label, n, p, fast, blocks[first].index);
    last = list;
    totals.push_back(p.total_ms);
    ++n;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (n > 0) ReportPass(label, n - 1, p, fast, blocks[first].index);
  std::printf("RESULT repeat=%s passes=%d %s\n", label, n,
              vx2::Format("pass_ms", vx2::Summarize(totals)).c_str());
}

}  // namespace

auto RunBudget(VulkanContext& ctx, Compute& compute, const BudgetArgs& args) -> int {
  const VkDeviceSize block_bytes = static_cast<VkDeviceSize>(args.block_mib) * kMiB;
  LogBudget("start", ctx);
  auto       heaps     = QueryBudget(ctx.physical);
  const auto local     = LocalHeap(heaps);
  const auto budget0   = heaps[local].budget;
  const auto usage0    = heaps[local].usage;
  const VkDeviceSize target =
      args.max_mib > 0 ? static_cast<VkDeviceSize>(args.max_mib) * kMiB
                       : budget0 - std::min(budget0, usage0) + static_cast<VkDeviceSize>(args.over_mib) * kMiB;
  std::printf("RESULT plan block_mib=%d target_mib=%llu budget_mib=%llu usage_mib=%llu priority=%s "
              "memory_priority=%d pageable=%d min_free_physical_mib=%d\n",
              args.block_mib, Mib(target), Mib(budget0), Mib(usage0), args.priority.c_str(),
              ctx.memory_priority_enabled ? 1 : 0, ctx.pageable_enabled ? 1 : 0, args.min_free_mib);

  // Usage of the other device of the process: one block on the display device.
  if (args.display_probe && ctx.split) {
    Compute display(ctx, ctx.device, args.spv_path);
    Block   b;
    double  ms = 0;
    const auto before = QueryBudget(ctx.physical)[local].usage;
    if (display.Allocate(256 * kMiB, -1.0f, b, ms) == VK_SUCCESS) {
      display.Fill(b);
      const auto after = QueryBudget(ctx.physical)[local].usage;
      std::printf("RESULT display_device_block mib=256 usage_delta_mib=%lld\n",
                  static_cast<long long>(after - before) / static_cast<long long>(kMiB));
      display.Free(b);
    }
  }

  std::vector<Block> blocks;
  double             fast = 0;  // speed of the first block, alone, in video memory
  int                step = 0;
  bool               stop = false;
  while (!stop) {
    VkDeviceSize total = 0;
    for (const auto& b : blocks) total += b.size;
    if (total + block_bytes > target) break;
    if (FreePhysicalMiB() < static_cast<std::uint64_t>(args.min_free_mib)) {
      std::printf("RESULT stop reason=free_physical_below_limit free_mib=%llu\n",
                  static_cast<unsigned long long>(FreePhysicalMiB()));
      break;
    }
    // Priority: split gives the blocks inside the start budget 0.2 (cache) and the blocks above
    // it 0.8 (work); same gives 0.5 to each; none sets no priority.
    float priority = -1.0f;
    if (args.priority == "same") priority = 0.5f;
    if (args.priority == "split") priority = total + block_bytes <= budget0 - std::min(budget0, usage0) ? 0.2f : 0.8f;
    Block      b;
    double     alloc_ms = 0;
    const auto r        = compute.Allocate(block_bytes, priority, b, alloc_ms);
    if (r != VK_SUCCESS) {
      std::printf("RESULT allocate step=%d vk=%d total_mib=%llu alloc_ms=%.2f\n", step, static_cast<int>(r),
                  Mib(total), alloc_ms);
      break;
    }
    b.index = step;
    const std::int64_t f0 = NowNs();
    const auto         fr = compute.Fill(b);
    const double       fill_ms = (NowNs() - f0) * 1e-6;
    blocks.push_back(b);
    // The new block alone, best of 3.
    double best = 0;
    for (int k = 0; k < 3; ++k) {
      const auto p = Pass(compute, blocks, blocks.size() - 1);
      if (p.result != VK_SUCCESS) {
        stop = true;
        break;
      }
      best = std::max(best, p.gbps[0]);
    }
    if (step == 0) fast = best;
    heaps            = QueryBudget(ctx.physical);
    const auto dxgi  = QueryDxgi(ctx.luid);
    std::printf("RESULT step=%d total_mib=%llu priority=%.1f alloc_ms=%.2f fill_ms=%.1f fill_vk=%d "
                "new_block_gbps=%.1f budget_mib=%llu usage_mib=%llu dxgi_local_usage=%llu "
                "dxgi_nonlocal_usage=%llu\n",
                step, Mib(total + block_bytes), priority, alloc_ms, fill_ms, static_cast<int>(fr), best,
                Mib(heaps[local].budget), Mib(heaps[local].usage), Mib(dxgi.local_usage),
                Mib(dxgi.nonlocal_usage));
    const auto all = Pass(compute, blocks);
    ReportPass("all", step, all, fast);
    if (all.result != VK_SUCCESS) break;
    std::fflush(stdout);
    ++step;
  }
  LogBudget("full", ctx);
  ReportBlocks("full", Pass(compute, blocks));

  // Hold: passes over every block back to back. A change of the slow set means that Windows
  // moves memory between the segments while the program works.
  const auto hold_end = NowNs() + static_cast<std::int64_t>(args.hold_seconds * 1e9);
  Repeat("hold", compute, blocks, 0, hold_end, fast);
  ReportBlocks("after_hold", Pass(compute, blocks));

  // Work set: only the newest blocks (args.work_mib) for the hold time. Windows can bring a used
  // block back to video memory and move an unused one out.
  const std::size_t work_blocks = std::min<std::size_t>(blocks.size(), std::max(1, args.work_mib / args.block_mib));
  const std::size_t first       = blocks.size() - work_blocks;
  const auto        work_end    = NowNs() + static_cast<std::int64_t>(args.hold_seconds * 1e9);
  Repeat("work", compute, blocks, first, work_end, fast);
  ReportBlocks("after_work", Pass(compute, blocks));
  LogBudget("after_work", ctx);

  // Free the oldest blocks until the total is 512 MiB below the start budget; then the remaining
  // blocks for the hold time.
  std::size_t freed = 0;
  while (!blocks.empty()) {
    VkDeviceSize total = 0;
    for (const auto& b : blocks) total += b.size;
    if (total + 512 * kMiB <= budget0 - std::min(budget0, usage0)) break;
    compute.Free(blocks.front());
    blocks.erase(blocks.begin());
    ++freed;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  LogBudget("after_free", ctx);
  std::printf("RESULT freed_blocks=%zu remaining=%zu\n", freed, blocks.size());
  const auto free_end = NowNs() + static_cast<std::int64_t>(args.hold_seconds * 1e9);
  if (!blocks.empty()) Repeat("after_free", compute, blocks, 0, free_end, fast);
  if (!blocks.empty()) ReportBlocks("end", Pass(compute, blocks));
  for (auto& b : blocks) compute.Free(b);
  LogBudget("end", ctx);
  return 0;
}

auto RunHold(VulkanContext& ctx, Compute& compute, int mib, double seconds) -> int {
  LogBudget("hold_start", ctx);
  std::vector<Block> blocks;
  for (int i = 0; i < mib / 256; ++i) {
    Block  b;
    double ms = 0;
    if (compute.Allocate(256 * kMiB, -1.0f, b, ms) != VK_SUCCESS) break;
    compute.Fill(b);
    blocks.push_back(b);
  }
  LogBudget("hold_full", ctx);
  std::printf("RESULT hold_ready blocks=%zu\n", blocks.size());
  std::fflush(stdout);
  // A pass each 100 ms keeps the blocks in use, as a program that renders does.
  const auto end = NowNs() + static_cast<std::int64_t>(seconds * 1e9);
  while (NowNs() < end) {
    Pass(compute, blocks);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  for (auto& b : blocks) compute.Free(b);
  return 0;
}

}  // namespace vx5
