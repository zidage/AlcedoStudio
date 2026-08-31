//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/execution_plan.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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
}

}  // namespace alcedo
