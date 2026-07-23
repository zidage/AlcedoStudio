//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

namespace alcedo::ui {
namespace {

TEST(EditorSessionThumbnailPortTest, InvokesCallbackOnInvalidate) {
  std::atomic<sl_element_id_t> last_id{0};
  std::atomic<int>             call_count{0};
  EditorSessionThumbnailPort   port([&](sl_element_id_t element_id) {
    last_id.store(element_id);
    call_count.fetch_add(1);
  });

  port.Invalidate(42);
  EXPECT_EQ(last_id.load(), 42u);
  EXPECT_EQ(call_count.load(), 1);
  port.Invalidate(99);
  EXPECT_EQ(last_id.load(), 99u);
  EXPECT_EQ(call_count.load(), 2);
}

TEST(EditorSessionThumbnailPortTest, NullCallbackIsNoOp) {
  EditorSessionThumbnailPort port(nullptr);
  EXPECT_NO_THROW(port.Invalidate(1));
}

TEST(EditorSessionThumbnailPortTest, CallbackFailureDoesNotEscape) {
  EditorSessionThumbnailPort port(
      [&](sl_element_id_t) { throw std::runtime_error("thumbnail failure"); });
  EXPECT_NO_THROW(port.Invalidate(1));
}

}  // namespace
}  // namespace alcedo::ui
