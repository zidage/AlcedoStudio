//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/execution_plan.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace alcedo {
namespace {

auto DescribeValue(const GraphValueId& id) -> std::string {
  return std::string{id.producer.Value()} + "." + std::string{id.output_port.Value()};
}

auto DescribePass(const GpuPassDesc& pass) -> std::string {
  return std::string{GpuPassKindName(pass.kind)} + " owner=" + std::string{pass.owner.Value()} +
         " ordinal=" + std::to_string(pass.instance.ordinal);
}

}  // namespace

void ValidateExecutionPlan(const ExecutionPlan& plan) {
  std::set<PassInstanceId> seen_instances;
  std::map<GraphValueId, std::pair<std::size_t, CompiledValueKind>> defined;

  for (std::size_t index = 0; index < plan.passes.size(); ++index) {
    const auto& pass = plan.passes[index];
    if (pass.kind != pass.instance.kind) {
      throw std::runtime_error("ExecutionPlan: pass kind does not match instance kind for " +
                               DescribePass(pass));
    }
    if (pass.owner != pass.instance.owner) {
      throw std::runtime_error("ExecutionPlan: pass owner does not match instance owner for " +
                               DescribePass(pass));
    }
    if (!seen_instances.insert(pass.instance).second) {
      throw std::runtime_error("ExecutionPlan: duplicate pass instance for " + DescribePass(pass));
    }

    for (const auto& input : pass.inputs) {
      const auto found = defined.find(input.source);
      if (found == defined.end()) {
        throw std::runtime_error("ExecutionPlan: missing producer for " + DescribeValue(input.source) +
                                 " consumed by " + DescribePass(pass));
      }
      if (found->second.first >= index) {
        throw std::runtime_error("ExecutionPlan: consumer does not follow producer for " +
                                 DescribeValue(input.source) + " in " + DescribePass(pass));
      }
      if (found->second.second != input.expected_kind) {
        throw std::runtime_error("ExecutionPlan: invalid input type for " +
                                 DescribeValue(input.source) + " in " + DescribePass(pass));
      }
    }

    for (const auto& output : pass.outputs) {
      const auto existing = defined.find(output.value);
      if (existing != defined.end()) {
        bool in_place = false;
        for (const auto& input : pass.inputs) {
          if (input.source == output.value) {
            in_place = true;
            break;
          }
        }
        if (!in_place) {
          throw std::runtime_error("ExecutionPlan: duplicate output definition for " +
                                   DescribeValue(output.value) + " in " + DescribePass(pass));
        }
        if (existing->second.second != output.kind) {
          throw std::runtime_error("ExecutionPlan: in-place output kind mismatch for " +
                                   DescribeValue(output.value) + " in " + DescribePass(pass));
        }
      }
      defined[output.value] = {index, output.kind};
    }
  }

  for (const auto& grade : plan.grade_nodes) {
    if (!grade.mask_stack.has_value()) {
      continue;
    }
    const auto& stack = *grade.mask_stack;
    if (stack.owner_node_id != grade.node_id) {
      throw std::runtime_error("ExecutionPlan: Mask stack owner does not match Color Grade " +
                               std::string{grade.node_id.Value()});
    }
    if (stack.sources.empty()) {
      throw std::runtime_error("ExecutionPlan: compiled Mask stack has no sources for " +
                               std::string{grade.node_id.Value()});
    }
    if (stack.union_output != grade.mask_output) {
      throw std::runtime_error("ExecutionPlan: Mask Union output does not match Grade mask output");
    }
    const auto union_def = defined.find(stack.union_output);
    if (union_def == defined.end() || union_def->second.second != CompiledValueKind::Mask) {
      throw std::runtime_error("ExecutionPlan: missing Mask Union producer for " +
                               DescribeValue(stack.union_output));
    }
    for (std::size_t index = 0; index < stack.sources.size(); ++index) {
      const auto& source = stack.sources[index];
      if (source.owner_node_id != grade.node_id) {
        throw std::runtime_error("ExecutionPlan: Mask source owner is not the Color Grade");
      }
      if (source.mask_id.Empty()) {
        throw std::runtime_error("ExecutionPlan: compiled Mask source has an empty MaskId");
      }
      if (index > 0 && !(stack.sources[index - 1].mask_id < source.mask_id)) {
        throw std::runtime_error("ExecutionPlan: Mask sources are not sorted by MaskId");
      }
      if (source.range_input != grade.scene_input) {
        throw std::runtime_error("ExecutionPlan: Mask range input is not the owning Grade scene input");
      }
      const auto source_def = defined.find(source.effective_output);
      if (source_def == defined.end() || source_def->second.second != CompiledValueKind::Mask) {
        throw std::runtime_error("ExecutionPlan: missing Mask source producer for " +
                                 DescribeValue(source.effective_output));
      }
    }
  }
}

auto CollectParameterSlotKeys(const ExecutionPlan& plan) -> std::vector<ParameterSlotKey> {
  std::vector<ParameterSlotKey> keys;
  for (const auto& grade : plan.grade_nodes) {
    for (const auto& adjustment : grade.adjustments) {
      keys.push_back(ParameterSlotKey{grade.node_id, adjustment.instance_id});
    }
  }
  for (const auto& adjustment : plan.drt.post_adjustments) {
    keys.push_back(ParameterSlotKey{plan.drt.node_id, adjustment.instance_id});
  }
  return keys;
}

RemainingValueConsumers::RemainingValueConsumers(const ExecutionPlan& plan) {
  for (const auto& pass : plan.passes) {
    for (const auto& input : pass.inputs) {
      ++remaining_[input.source];
    }
  }
}

void RemainingValueConsumers::Consume(const GraphValueId& id) {
  const auto it = remaining_.find(id);
  if (it == remaining_.end() || it->second == 0) {
    throw std::runtime_error("RemainingValueConsumers: no remaining consumer for " +
                             std::string{id.producer.Value()} + "." +
                             std::string{id.output_port.Value()});
  }
  --it->second;
}

auto RemainingValueConsumers::Remaining(const GraphValueId& id) const -> std::uint32_t {
  const auto it = remaining_.find(id);
  return it == remaining_.end() ? 0 : it->second;
}

auto RemainingValueConsumers::Exhausted(const GraphValueId& id) const -> bool {
  const auto it = remaining_.find(id);
  return it != remaining_.end() && it->second == 0;
}

}  // namespace alcedo
