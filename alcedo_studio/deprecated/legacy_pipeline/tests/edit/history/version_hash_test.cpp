//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/history/edit_history.hpp"
#include "edit/history/edit_transaction.hpp"
#include "edit/history/version.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "type/type.hpp"

namespace alcedo {
class VersionHashTests : public ::testing::Test {
 protected:
  void SetUp() override { RegisterAllOperators(); }
};

TEST_F(VersionHashTests, EmptyVersionHasZeroHash) {
  Version v(1);
  EXPECT_EQ(v.GetVersionHash().low64(), 0ULL);
  EXPECT_EQ(v.GetVersionHash().high64(), 0ULL);
}

TEST_F(VersionHashTests, NonEmptyVersionHasNonZeroHash) {
  Version v(1);
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  v.AppendEditTransaction(std::move(tx));
  Hash128 h = v.GetVersionHash();
  EXPECT_FALSE(h.low64() == 0 && h.high64() == 0);
}

TEST_F(VersionHashTests, SameTransactionsProduceSameHash) {
  auto make_version = []() {
    Version v(1);
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::EXPOSURE,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"exposure", 1.0f}},
                       false,
                       true};
    v.AppendEditTransaction(std::move(tx));
    return v;
  };

  Version v1 = make_version();
  Version v2 = make_version();
  EXPECT_EQ(v1.GetVersionHash(), v2.GetVersionHash());
}

TEST_F(VersionHashTests, DifferentTransactionsProduceDifferentHash) {
  Version v1(1);
  EditTransaction tx1{TransactionType::_ADD,
                      OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"exposure", 1.0f}},
                      false,
                      true};
  v1.AppendEditTransaction(std::move(tx1));

  Version v2(1);
  EditTransaction tx2{TransactionType::_ADD,
                      OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"exposure", 2.0f}},
                      false,
                      true};
  v2.AppendEditTransaction(std::move(tx2));

  EXPECT_NE(v1.GetVersionHash(), v2.GetVersionHash());
}

TEST_F(VersionHashTests, AppendTransactionChangesHash) {
  Version v(1);
  Hash128  before = v.GetVersionHash();

  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  v.AppendEditTransaction(std::move(tx));
  Hash128 after = v.GetVersionHash();

  EXPECT_NE(before, after);
}

TEST_F(VersionHashTests, RemoveTransactionChangesHash) {
  Version v(1);
  EditTransaction tx1{TransactionType::_ADD,
                      OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"exposure", 1.0f}},
                      false,
                      true};
  v.AppendEditTransaction(std::move(tx1));

  EditTransaction tx2{TransactionType::_ADD,
                      OperatorType::CONTRAST,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"contrast", 0.5f}},
                      false,
                      true};
  v.AppendEditTransaction(std::move(tx2));

  Hash128 before = v.GetVersionHash();
  v.RemoveLastEditTransaction();
  Hash128 after = v.GetVersionHash();

  EXPECT_NE(before, after);
}

TEST_F(VersionHashTests, CursorChangeChangesHash) {
  Version v(1);
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  v.AppendEditTransaction(std::move(tx));

  Hash128 before = v.GetVersionHash();

  // Simulate cursor change by reconstructing version with different cursor
  CPUPipelineExecutor exec;
  WorkingVersion working(1, v.GetVersionID(), exec.ExportPipelineParams(),
                         v.GetAllEditTransactions(), 0);
  nlohmann::json head_params = exec.ExportPipelineParams();
  v.UpdateFromWorkingVersion(working, head_params);

  Hash128 after = v.GetVersionHash();
  EXPECT_NE(before, after);
}

TEST_F(VersionHashTests, MultipleTransactionsChangeHash) {
  Version v(1);
  Hash128  prev = v.GetVersionHash();

  for (int i = 0; i < 10; ++i) {
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::EXPOSURE,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"exposure", static_cast<float>(i) * 0.1f}},
                       false,
                       true};
    v.AppendEditTransaction(std::move(tx));
    Hash128 curr = v.GetVersionHash();
    EXPECT_NE(prev, curr) << "Hash should change after appending transaction " << i;
    prev = curr;
  }
}

TEST_F(VersionHashTests, JSONRoundTripPreservesHash) {
  Version v(1);
  EditTransaction tx1{TransactionType::_ADD,
                      OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"exposure", 1.0f}},
                      false,
                      true};
  v.AppendEditTransaction(std::move(tx1));

  EditTransaction tx2{TransactionType::_ADD,
                      OperatorType::CONTRAST,
                      PipelineStageName::Basic_Adjustment,
                      nlohmann::json(nullptr),
                      nlohmann::json{{"contrast", 0.5f}},
                      false,
                      true};
  v.AppendEditTransaction(std::move(tx2));

  Hash128 original_hash = v.GetVersionHash();

  nlohmann::json j = v.ToJSON();
  Version        restored(j);
  EXPECT_EQ(restored.GetVersionHash(), original_hash);
}

TEST_F(VersionHashTests, TransactionHashIsSerialized) {
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  tx.GenerateTransactionHash();

  const nlohmann::json j = tx.ToJSON();
  ASSERT_TRUE(j.contains("transaction_hash"));
  ASSERT_TRUE(j.at("transaction_hash").is_string());
  EXPECT_EQ(j.at("transaction_hash").get<std::string>(), tx.GetTransactionHash().ToString());

  EditTransaction restored(j);
  EXPECT_TRUE(restored.HasTransactionHash());
  EXPECT_EQ(restored.GetTransactionHash(), tx.GetTransactionHash());
}

TEST_F(VersionHashTests, WorkingVersionCommitGeneratesTransactionHash) {
  CPUPipelineExecutor exec;
  WorkingVersion      working(1, Hash128{}, exec.ExportPipelineParams());
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  working.AppendEditTransaction(std::move(tx));

  const auto& transactions = working.GetAllEditTransactions();
  ASSERT_EQ(transactions.size(), 1U);
  EXPECT_TRUE(transactions.front().HasTransactionHash());
}

TEST_F(VersionHashTests, EditHistoryGetActiveVersionHash) {
  constexpr sl_element_id_t file_id = 99;
  EditHistory               history(file_id);

  Hash128 empty_hash = history.GetActiveVersionHash();
  EXPECT_EQ(empty_hash.low64(), 0ULL);
  EXPECT_EQ(empty_hash.high64(), 0ULL);

  auto default_id = history.GetDefaultVersionID();
  CPUPipelineExecutor exec;
  exec.ImportPipelineParams(history.GetImportPipelineParams());

  WorkingVersion working(file_id, default_id, exec.ExportPipelineParams());
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  tx.ApplyForward(exec);
  working.AppendEditTransaction(std::move(tx));
  working.SetHeadPipelineParams(exec.ExportPipelineParams());

  history.UpdateVersionFromWorkingVersion(default_id, working, exec.ExportPipelineParams());

  Hash128 after_edit = history.GetActiveVersionHash();
  EXPECT_FALSE(after_edit.low64() == 0 && after_edit.high64() == 0);
}

TEST_F(VersionHashTests, VersionHashRecomputedAfterFromJSON) {
  // Even if version_hash is missing or corrupt in JSON, FromJSON recomputes it.
  Version v(1);
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  v.AppendEditTransaction(std::move(tx));
  Hash128 expected = v.GetVersionHash();

  nlohmann::json j = v.ToJSON();
  // Remove persisted hashes from JSON to simulate old data.
  j.erase("version_hash");
  for (auto& tx_json : j["transactions"]) {
    tx_json.erase("transaction_hash");
  }
  Version restored(j);
  EXPECT_EQ(restored.GetVersionHash(), expected);
}

TEST_F(VersionHashTests, MerkleRootEmptyInputIsZero) {
  Version v(1);
  EXPECT_EQ(v.GetVersionHash(), Hash128{});
}

TEST_F(VersionHashTests, MerkleRootSingleLeafIsNotIdentity) {
  // With one transaction + cursor leaf, root differs from the single tx hash.
  Version v(1);
  EditTransaction tx{TransactionType::_ADD,
                     OperatorType::EXPOSURE,
                     PipelineStageName::Basic_Adjustment,
                     nlohmann::json(nullptr),
                     nlohmann::json{{"exposure", 1.0f}},
                     false,
                     true};
  Hash128 tx_hash = tx.Hash();
  v.AppendEditTransaction(std::move(tx));
  Hash128 root = v.GetVersionHash();
  // The merkle root of [tx_hash, cursor_hash] should differ from tx_hash alone.
  EXPECT_NE(root, tx_hash);
}

TEST_F(VersionHashTests, TransactionOrderMatters) {
  Version v1(1);
  {
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::EXPOSURE,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"exposure", 1.0f}},
                       false,
                       true};
    v1.AppendEditTransaction(std::move(tx));
  }
  {
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::CONTRAST,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"contrast", 0.5f}},
                       false,
                       true};
    v1.AppendEditTransaction(std::move(tx));
  }

  Version v2(1);
  {
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::CONTRAST,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"contrast", 0.5f}},
                       false,
                       true};
    v2.AppendEditTransaction(std::move(tx));
  }
  {
    EditTransaction tx{TransactionType::_ADD,
                       OperatorType::EXPOSURE,
                       PipelineStageName::Basic_Adjustment,
                       nlohmann::json(nullptr),
                       nlohmann::json{{"exposure", 1.0f}},
                       false,
                       true};
    v2.AppendEditTransaction(std::move(tx));
  }

  EXPECT_NE(v1.GetVersionHash(), v2.GetVersionHash());
}
}  // namespace alcedo
