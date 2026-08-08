//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/image_loader.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

class ScopedTempPath {
 public:
  explicit ScopedTempPath(std::filesystem::path path) : path_(std::move(path)) {}

  ~ScopedTempPath() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  auto path() const -> const std::filesystem::path& { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(ImagePathEncodingTest, RestoredUtf8ImagePathRemainsReadable) {
  using namespace alcedo;

  const auto temp_root =
      std::filesystem::temp_directory_path() / std::filesystem::path(L"alcedo_中文路径_roundtrip");
  ScopedTempPath cleanup(temp_root);

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  ASSERT_FALSE(ec);

  const auto file_path = temp_root / std::filesystem::path(L"重新加载样片.raw");
  const std::vector<uint8_t> expected_bytes = {0x10, 0x20, 0x30, 0x40, 0x50};

  {
    std::ofstream file(file_path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.write(reinterpret_cast<const char*>(expected_bytes.data()),
               static_cast<std::streamsize>(expected_bytes.size()));
    ASSERT_TRUE(file.good());
  }

  ImageMapperParams params{
      42,
      std::make_unique<std::string>(conv::ToBytes(file_path.wstring())),
      std::make_unique<std::string>(conv::ToBytes(file_path.filename().wstring())),
      static_cast<uint32_t>(ImageType::DEFAULT),
      std::make_unique<std::string>("{}"),
  };

  auto restored = ImageMapper::FromParams(std::move(params));
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->image_path_.wstring(), file_path.wstring());
  EXPECT_EQ(restored->image_name_, file_path.filename().wstring());

  EXPECT_EQ(ByteBufferLoader::LoadByteBufferFromImage(restored), expected_bytes);

  auto shared_bytes = ByteBufferLoader::LoadFromImage(restored);
  ASSERT_NE(shared_bytes, nullptr);
  EXPECT_EQ(*shared_bytes, expected_bytes);
}
