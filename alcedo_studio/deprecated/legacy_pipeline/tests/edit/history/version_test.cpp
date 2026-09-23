//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/version.hpp"

#include <gtest/gtest.h>

#include <iostream>

#include "edit/history/edit_transaction.hpp"
#include "edit/pipeline/pipeline.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "history_test_fixation.hpp"
#include "type/type.hpp"

namespace alcedo {
TEST_F(EditHistoryTests, DISABLED_VersionIDGenerationTest) {
  {
    Version v{0};
    v.SetBasePipelineExecutor(std::make_shared<CPUPipelineExecutor>());
    EditTransaction tx1{TransactionType::_ADD,
                        OperatorType::EXPOSURE,
                        PipelineStageName::Basic_Adjustment,
                        {{"exposure", 1.0f}}};
    v.AppendEditTransaction(std::move(tx1));

    auto            id1 = v.GetVersionID().ToString();

    EditTransaction tx2{TransactionType::_ADD,
                        OperatorType::CONTRAST,
                        PipelineStageName::Basic_Adjustment,
                        {{"contrast", 2.2f}},
                        tx1.GetTransactionID()};
    v.AppendEditTransaction(std::move(tx2));
    auto id2 = v.GetVersionID().ToString();

    std::cout << "Version ID after first transaction: 0x" << id1 << std::endl;
    std::cout << "Version ID after second transaction: 0x" << id2 << std::endl;
    EXPECT_NE(id1, id2) << "Version IDs (hashes) should differ after adding a new transaction.";
  }
}

TEST_F(EditHistoryTests, DISABLED_RemoveTransactionTest) {
  {
    Version v{0};
    v.SetBasePipelineExecutor(std::make_shared<CPUPipelineExecutor>());

    EditTransaction tx1{TransactionType::_ADD,
                        OperatorType::EXPOSURE,
                        PipelineStageName::Basic_Adjustment,
                        {{"exposure", 1.0f}}};
    v.AppendEditTransaction(std::move(tx1));

    EditTransaction tx2{TransactionType::_ADD,
                        OperatorType::CONTRAST,
                        PipelineStageName::Basic_Adjustment,
                        {{"contrast", 2.2f}},
                        tx1.GetTransactionID()};
    v.AppendEditTransaction(std::move(tx2));

    auto all_txs = v.GetAllEditTransactions();
    EXPECT_EQ(all_txs.size(), 2) << "There should be 2 transactions before removal.";

    auto id_before_removal = v.GetVersionID().ToString();
    std::cout << "Version ID before removing transaction: 0x" << id_before_removal << std::endl;

    EditTransaction removed_tx       = v.RemoveLastEditTransaction();

    auto            id_after_removal = v.GetVersionID().ToString();
    std::cout << "Version ID after removing transaction: 0x" << id_after_removal << std::endl;

    EXPECT_NE(id_before_removal, id_after_removal)
        << "Version IDs (hashes) should differ after removing a transaction.";

    all_txs = v.GetAllEditTransactions();
    EXPECT_EQ(all_txs.size(), 1) << "There should be 1 transaction after removal.";
  }
}

TEST_F(EditHistoryTests, DISABLED_FuzzTest) {
  {
    Version v{0};
    v.SetBasePipelineExecutor(std::make_shared<CPUPipelineExecutor>());

    const int num_transactions = 10000;
    auto      id               = v.GetVersionID();
    for (int i = 0; i < num_transactions; ++i) {
      EditTransaction tx{TransactionType::_ADD,
                         OperatorType::EXPOSURE,
                         PipelineStageName::Basic_Adjustment,
                         {{"exposure", static_cast<float>(i) * 0.1f}}};
      v.AppendEditTransaction(std::move(tx));
      EXPECT_NE(id, v.GetVersionID()) << "Version ID should change after adding transaction " << i;
      id = v.GetVersionID();
    }

    EXPECT_EQ(2048, v.GetAllEditTransactions().size())
        << "Version should only keep the last 2048 transactions.";
  }
}

TEST_F(EditHistoryTests, JSONSerializationTest) {
  Version version(1);
  version.SetBoundImage(1001);
  auto pipeline_executor = std::make_shared<CPUPipelineExecutor>();
  version.SetBasePipelineExecutor(pipeline_executor);

  EditTransaction tx1(TransactionType::_ADD, OperatorType::EXPOSURE,
                      PipelineStageName::Basic_Adjustment, {{"exposure", 0.5}});
  version.AppendEditTransaction(std::move(tx1));
  // The id for tx1 will be assigned when appended to version
  nlohmann::json  j_tx1 = version.GetTransactionByID(1).ToJSON();

  EditTransaction tx2(TransactionType::_ADD, OperatorType::CONTRAST,
                      PipelineStageName::Basic_Adjustment, {{"contrast", 50}},
                      tx1.GetTransactionID());
  version.AppendEditTransaction(std::move(tx2));
  // The id for tx2 will be assigned when appended to version
  nlohmann::json j_tx2 = version.GetTransactionByID(2).ToJSON();

  nlohmann::json j     = version.ToJSON();

  // std::cout << j.dump(2) << std::endl;

  Version        version2{j};

  nlohmann::json j2 = version2.ToJSON();
  // std::cout << j2.dump(2) << std::endl;

  EXPECT_EQ(j["version_id"].dump(), j2["version_id"].dump());

  // Check parent-child relationships
  auto txs = version2.GetAllEditTransactions();
  EXPECT_EQ(txs.size(), 2);

  auto& tx1_reloaded = version2.GetTransactionByID(1);
  auto& tx2_reloaded = version2.GetTransactionByID(2);

  EXPECT_EQ(tx1_reloaded.ToJSON().dump(), j_tx1.dump());
  EXPECT_EQ(tx2_reloaded.ToJSON().dump(), j_tx2.dump());
}
}  // namespace alcedo
