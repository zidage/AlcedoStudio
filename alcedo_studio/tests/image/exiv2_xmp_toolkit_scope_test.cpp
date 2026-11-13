// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/exiv2_xmp_toolkit_scope.hpp"

#include <gtest/gtest.h>

#include <exiv2/error.hpp>
#include <exiv2/properties.hpp>
#include <exiv2/xmp_exiv2.hpp>
#include <string>
#include <thread>
#include <vector>

namespace alcedo {
namespace {

constexpr int kDecodeThreads    = 16;
constexpr int kDecodesPerThread = 64;

/// An XMP packet with one property in a namespace that only this packet uses, so every decode
/// registers a new namespace.
auto          MakeXmpPacket(int thread_index, int decode_index) -> std::string {
  const std::string prefix =
      "t" + std::to_string(thread_index) + "_" + std::to_string(decode_index);
  std::string packet = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)";
  packet += R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)";
  packet += R"(<rdf:Description rdf:about="" xmlns:)" + prefix;
  packet += R"(="http://alcedo.app/test/xmp/)" + prefix + R"(/" )";
  packet += prefix + R"(:Label="value"/>)";
  packet += R"(</rdf:RDF></x:xmpmeta>)";
  return packet;
}

// Issue 389: thumbnail workers decoded DNG XMP in parallel and crashed in
// XMPMeta::RegisterNamespace. With the toolkit initialized on the main thread first, parallel
// decodes that each register new namespaces all succeed and read their property back.
TEST(Exiv2XmpToolkitScopeTest, ParallelXmpDecodesWithNewNamespacesSucceedAfterScopeStarts) {
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  const Exiv2XmpToolkitScope xmp_toolkit;
  ASSERT_TRUE(xmp_toolkit.initialized());

  std::vector<int>         failed_decodes(kDecodeThreads, 0);
  std::vector<std::thread> threads;
  threads.reserve(kDecodeThreads);
  for (int thread_index = 0; thread_index < kDecodeThreads; ++thread_index) {
    threads.emplace_back([thread_index, &failed_decodes] {
      for (int decode_index = 0; decode_index < kDecodesPerThread; ++decode_index) {
        Exiv2::XmpData xmp_data;
        const int      result =
            Exiv2::XmpParser::decode(xmp_data, MakeXmpPacket(thread_index, decode_index));
        if (result != 0 || xmp_data.count() != 1 ||
            xmp_data.begin()->toString() != std::string("value")) {
          ++failed_decodes[thread_index];
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  for (int thread_index = 0; thread_index < kDecodeThreads; ++thread_index) {
    EXPECT_EQ(failed_decodes[thread_index], 0) << "thread " << thread_index;
  }
}

}  // namespace
}  // namespace alcedo
