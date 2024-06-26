/* Copyright 2019 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/all_reduce_simplifier.h"

#include <vector>

#include "absl/status/statusor.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/literal_util.h"
#include "xla/service/hlo_replication_analysis.h"
#include "xla/shape_util.h"

namespace xla {

absl::StatusOr<bool> AllReduceSimplifier::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  TF_ASSIGN_OR_RETURN(
      auto replication,
      HloReplicationAnalysis::Run(module, /*cross_partition_spmd=*/false));
  std::vector<std::pair<HloInstruction*, int64_t>> all_reduces_to_replace;

  // Returns the size of a replica group if all groups have the same size, or -1
  // if they have different sizes.
  auto get_replica_group_size =
      [this](const HloInstruction* all_reduce) -> int64_t {
    if (all_reduce->replica_groups().empty()) {
      return replica_count_;
    }
    int64_t replica_group_size = -1;
    for (const auto& group : all_reduce->replica_groups()) {
      if (replica_group_size == -1) {
        replica_group_size = group.replica_ids_size();
      } else if (replica_group_size != group.replica_ids_size()) {
        return -1;
      }
    }
    return replica_group_size;
  };

  bool changed = false;
  for (auto computation : module->computations(execution_threads)) {
    for (HloInstruction* inst : computation->MakeInstructionPostOrder()) {
      // AllGather and ReduceScatter with the same input and output shape
      if ((inst->opcode() == HloOpcode::kAllGather ||
           inst->opcode() == HloOpcode::kReduceScatter) &&
          ShapeUtil::Compatible(inst->shape(), inst->operand(0)->shape())) {
        changed = true;
        TF_RETURN_IF_ERROR(
            computation->ReplaceInstruction(inst, inst->mutable_operand(0)));
      }
    }
  }

  for (auto computation : module->computations(execution_threads)) {
    for (HloInstruction* inst : computation->MakeInstructionPostOrder()) {
      if (!inst->shape().IsArray()) {
        // We currently do not change tuple-shaped all-reduce.
        // Until XLA will support Token fed AllReduce(), the PyTorch client code
        // uses a fake data token (constant) which relies on this pass to not
        // optimize out (being fed within a tuple input).
        continue;
      }
      if (!inst->IsCrossReplicaAllReduce()) {
        continue;
      }
      int64_t group_size = get_replica_group_size(inst);
      if (group_size == -1) {
        continue;
      }
      if (replication->HloInstructionIsReplicatedAt(inst->operand(0), {}) ||
          group_size == 1) {
        all_reduces_to_replace.push_back({inst, group_size});
      }
    }
  }

  for (auto all_reduce_and_group_size : all_reduces_to_replace) {
    auto all_reduce = all_reduce_and_group_size.first;
    const int64_t replica_group_size = all_reduce_and_group_size.second;
    if (replica_group_size == 1) {
      TF_RETURN_IF_ERROR(all_reduce->parent()->ReplaceInstruction(
          all_reduce, all_reduce->mutable_operand(0)));
      changed = true;
      continue;
    }
    if (all_reduce->to_apply()->instruction_count() != 3 ||
        all_reduce->to_apply()->num_parameters() != 2) {
      continue;
    }
    HloInstruction* replacement;
    switch (all_reduce->to_apply()->root_instruction()->opcode()) {
      case HloOpcode::kAdd: {
        // Create the multiplier:
        //   broadcast(convert_to_matching_type(s32 group size))
        auto multiplier =
            all_reduce->parent()->AddInstruction(HloInstruction::CreateConstant(
                LiteralUtil::CreateR0<int32_t>(replica_group_size)));
        if (all_reduce->shape().element_type() != S32) {
          multiplier = all_reduce->parent()->AddInstruction(
              HloInstruction::CreateConvert(
                  ShapeUtil::ChangeElementType(
                      multiplier->shape(), all_reduce->shape().element_type()),
                  multiplier));
        }
        if (all_reduce->shape().rank() > 0) {
          multiplier = all_reduce->parent()->AddInstruction(
              HloInstruction::CreateBroadcast(all_reduce->shape(), multiplier,
                                              {}));
        }
        replacement =
            all_reduce->parent()->AddInstruction(HloInstruction::CreateBinary(
                all_reduce->shape(), HloOpcode::kMultiply,
                all_reduce->mutable_operand(0), multiplier));
        break;
      }
      case HloOpcode::kMinimum:
      case HloOpcode::kMaximum:
      case HloOpcode::kOr:
      case HloOpcode::kAnd:
        replacement = all_reduce->mutable_operand(0);
        break;
      default:
        continue;
    }
    VLOG(2) << "Replacing " << all_reduce->ToString() << " with "
            << replacement->ToString();
    TF_RETURN_IF_ERROR(all_reduce->ReplaceAllUsesWith(replacement));
    changed = true;
  }
  return changed;
}

}  // namespace xla
