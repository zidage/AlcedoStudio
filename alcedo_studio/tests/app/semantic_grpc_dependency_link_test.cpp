//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <google/protobuf/stubs/common.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

namespace alcedo {

TEST(SemanticGrpcDependencyLinkTest, BundledGrpcAndProtobufAreConsumable) {
  auto channel = grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials());
  ASSERT_NE(channel, nullptr);
  EXPECT_GT(GOOGLE_PROTOBUF_VERSION, 0);
}

}  // namespace alcedo
