//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

#include "edit/operators/op_base.hpp"
#include "edit/operators/operator_factory.hpp"
#include "edit/pipeline/pipeline.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
using tx_id_t = uint32_t;
enum class TransactionType : int { _ADD, _DELETE, _EDIT };
/**
 * @brief Represents a single edit transaction in the pipeline. Each transaction can be an addition,
 * deletion, or modification of an operator. Once created, transactions are immutable.
 *
 */
class EditTransaction {
 private:
  tx_id_t                       tx_id_ = 0;
  TransactionType               type_;

  OperatorType                  operator_type_;
  PipelineStageName             stage_name_;
  nlohmann::json                before_params_;
  nlohmann::json                after_params_;
  bool                          before_enabled_ = false;
  bool                          after_enabled_  = true;
  std::time_t                   created_time_   = 0;
  std::optional<Hash128>         transaction_hash_{};

 public:
  EditTransaction(TransactionType type, OperatorType operator_type, PipelineStageName stage_name,
                  nlohmann::json         operator_params,
                  std::optional<tx_id_t> parent_tx_id = std::nullopt)
      : type_(type),
        operator_type_(operator_type),
        stage_name_(stage_name),
        before_params_(nlohmann::json(nullptr)),
        after_params_(std::move(operator_params)),
        before_enabled_(false),
        after_enabled_(true) {
    (void)parent_tx_id;
    SetCreateTime();
  }

  EditTransaction(TransactionType type, OperatorType operator_type, PipelineStageName stage_name,
                  nlohmann::json before_params, nlohmann::json after_params,
                  bool before_enabled, bool after_enabled)
      : type_(type),
        operator_type_(operator_type),
        stage_name_(stage_name),
        before_params_(std::move(before_params)),
        after_params_(std::move(after_params)),
        before_enabled_(before_enabled),
        after_enabled_(after_enabled) {
    SetCreateTime();
  }

  EditTransaction(const nlohmann::json& j) { FromJSON(j); }

  void SetTransactionID(tx_id_t id) { tx_id_ = id; }

  auto GetTransactionID() const -> tx_id_t { return tx_id_; }

  auto GetTransactionType() const -> TransactionType { return type_; }
  auto GetOperatorParams() const -> const nlohmann::json& { return after_params_; }
  auto GetBeforeParams() const -> const nlohmann::json& { return before_params_; }
  auto GetAfterParams() const -> const nlohmann::json& { return after_params_; }
  auto GetBeforeEnabled() const -> bool { return before_enabled_; }
  auto GetAfterEnabled() const -> bool { return after_enabled_; }
  auto GetCreateTime() const -> std::time_t { return created_time_; }

  auto ApplyForward(PipelineExecutor& pipeline) const -> bool;
  auto ApplyBackward(PipelineExecutor& pipeline) const -> bool;
  auto ApplyTransaction(PipelineExecutor& pipeline) const -> bool { return ApplyForward(pipeline); }

  auto ToJSON() const -> nlohmann::json;
  void FromJSON(const nlohmann::json& j);

  auto GetTxOpStageName() const -> PipelineStageName { return stage_name_; }
  auto GetTxOperatorType() const -> OperatorType { return operator_type_; }
  auto HasTransactionHash() const -> bool { return transaction_hash_.has_value(); }
  auto GetTransactionHash() const -> Hash128 {
    return transaction_hash_.value_or(ComputeTransactionHash());
  }
  void GenerateTransactionHash() { transaction_hash_ = ComputeTransactionHash(); }

  void SetLastOperatorParams(const nlohmann::json& params) {
    before_params_   = params;
    before_enabled_  = true;
  }
  auto GetLastOperatorParams() const -> std::optional<nlohmann::json> {
    if (before_params_.is_null()) {
      return std::nullopt;
    }
    return before_params_;
  }

  auto UndoTransaction() const -> EditTransaction {
    return EditTransaction(TransactionType::_EDIT, operator_type_, stage_name_, after_params_,
                           before_params_, after_enabled_, before_enabled_);
  }

  auto Hash() const -> Hash128 { return GetTransactionHash(); }

 private:
  auto ComputeTransactionHash() const -> Hash128 {
    std::string before_params_str = before_params_.dump();
    std::string after_params_str  = after_params_.dump();
    Hash128     result =
        Hash128::Blend(Hash128::Blend(Hash128::Compute(&type_, sizeof(type_)),
                                      Hash128::Compute(&operator_type_, sizeof(operator_type_))),
                       Hash128::Blend(Hash128::Compute(&stage_name_, sizeof(stage_name_)),
                                      Hash128::Compute(after_params_str.data(),
                                                       after_params_str.size())));
    result = Hash128::Blend(result,
                            Hash128::Compute(before_params_str.data(), before_params_str.size()));
    result = Hash128::Blend(result, Hash128::Compute(&before_enabled_, sizeof(before_enabled_)));
    result = Hash128::Blend(result, Hash128::Compute(&after_enabled_, sizeof(after_enabled_)));
    return result;
  }

 public:
  static auto TransactionTypeToString(TransactionType type) -> const char*;
  static auto OperatorTypeToString(OperatorType type) -> const char*;
  static auto StageNameToString(PipelineStageName stage) -> const char*;
  auto Describe(bool include_params = true, std::size_t max_params_chars = 160) const
      -> std::string;

 private:
  void SetCreateTime();
};
};  // namespace alcedo
