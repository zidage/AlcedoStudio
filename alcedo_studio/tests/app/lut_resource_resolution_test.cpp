//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Resolution of LUT references through the library's published state (plan L4).

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTest>
#include <filesystem>
#include <future>
#include <string>
#include <string_view>
#include <thread>

#include "app/lut_library_service.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "lut_library_test_support.hpp"

namespace alcedo::test {
namespace {

constexpr std::string_view kPackageId = "spectral_film_lut";
constexpr std::string_view kLutId     = "spectral_film_lut:kodak_vision3_250d_5207";

auto                       Official() -> LutReference {
  return OfficialLutReference{std::string(kPackageId), std::string(kLutId)};
}

class LutResourceResolutionTest : public LutLibraryRootFixture {
 protected:
  /// Write package content @p name with the official LUT and make it the active content.
  auto ActivateContent(std::string_view name, std::string_view table = kNumericTable) -> fs::path {
    const std::string directory = "packages/spectral_film_lut/content/" + std::string(name);
    const fs::path    file = library_root_ / fs::path(directory) / "kodak_vision3_250d_5207.cube";
    WriteBytes(file, std::string(kOfficialComment) + "\n" + std::string(table));
    WriteBytes(
        library_root_ / "packages/spectral_film_lut/installed.json",
        R"({"schema":1,"kind":"alcedo-lut-package-receipt","package_id":"spectral_film_lut",)"
        R"("content_directory":")" +
            directory + R"("})");
    return file;
  }

  static auto Refresh(Service& service) -> bool {
    return service.RefreshInventory() == Service::Status::kOk && WaitUntilIdle(service);
  }
};

TEST_F(LutResourceResolutionTest, OfficialReferenceResolvesToActivePackageContent) {
  const fs::path active = ActivateContent("a");
  // A loose file declaring the same official ID is not package content.
  WriteBytes(library_root_ / "loose" / "kodak_vision3_250d_5207.cube", OfficialCube());
  const auto service    = StartService();

  const auto resolution = service->Resources()->Resolve(Official());
  EXPECT_EQ(resolution.status, LutResourceStatus::kAvailable);
  EXPECT_EQ(resolution.path, active);
  EXPECT_EQ(resolution.content_sha256.size(), 64U);

  const auto other = service->Resources()->Resolve(
      OfficialLutReference{std::string(kPackageId), "spectral_film_lut:unknown"});
  EXPECT_EQ(other.status, LutResourceStatus::kMissing);
}

TEST_F(LutResourceResolutionTest, OfficialUpdateResolvesSameIdToNewContent) {
  ActivateContent("a");
  const auto service = StartService();
  const auto before  = service->Resources()->Resolve(Official());
  ASSERT_EQ(before.status, LutResourceStatus::kAvailable);

  const fs::path updated = ActivateContent(
      "b", "LUT_3D_SIZE 2\n1 1 1\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n0 0 0\n");
  ASSERT_TRUE(Refresh(*service));
  const auto after = service->Resources()->Resolve(Official());
  EXPECT_EQ(after.status, LutResourceStatus::kAvailable);
  EXPECT_EQ(after.path, updated);
  EXPECT_NE(after.content_sha256, before.content_sha256);
  EXPECT_NE(after.ContentIdentity(), before.ContentIdentity());
}

TEST_F(LutResourceResolutionTest, RootMigrationPreservesRenderedLutSelection) {
  const fs::path original = library_root_ / "user" / "films" / "look.cube";
  WriteBytes(original, UserCube("look"));
  const auto         service = StartService();
  const LutReference library = LibraryLutReference{"user/films/look.cube"};
  const LutReference legacy  = FileLutReference{LutPathToUtf8(original)};
  ASSERT_EQ(service->Resources()->Resolve(library).path, original);
  ASSERT_EQ(service->Resources()->Resolve(legacy).path, original);

  const fs::path destination = base_ / "moved library";
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  ASSERT_FALSE(fs::exists(original)) << "unchanged migrated sources are cleaned up";

  const fs::path moved      = destination / "user" / "films" / "look.cube";
  const auto     by_library = service->Resources()->Resolve(library);
  EXPECT_EQ(by_library.status, LutResourceStatus::kAvailable);
  EXPECT_EQ(by_library.path, moved);
  // A project that stored the old absolute path follows the recorded root mapping.
  const auto by_path = service->Resources()->Resolve(legacy);
  EXPECT_EQ(by_path.status, LutResourceStatus::kAvailable);
  EXPECT_EQ(by_path.path, moved);
  EXPECT_EQ(ReadBytes(moved), UserCube("look"));
  // A path outside every known root is not matched by file name.
  EXPECT_EQ(
      service->Resources()->Resolve(FileLutReference{LutPathToUtf8(base_ / "look.cube")}).status,
      LutResourceStatus::kMissing);
}

TEST_F(LutResourceResolutionTest, MissingReferenceRequestsOneRefreshWithoutRetries) {
  const auto service = StartService();
  QSignalSpy finished(service.get(), &Service::OperationFinished);
  const auto refreshes = [&finished] {
    int count = 0;
    for (const auto& arguments : finished) {
      count += arguments.at(0).value<Service::Operation>() == Service::Operation::kRefresh ? 1 : 0;
    }
    return count;
  };
  const LutReference absent = LibraryLutReference{"user/absent.cube"};
  for (int frame = 0; frame < 3; ++frame) {
    EXPECT_EQ(service->Resources()->Resolve(absent).status, LutResourceStatus::kMissing);
    QTest::qWait(20);
    ASSERT_TRUE(WaitUntilIdle(*service));
  }
  EXPECT_EQ(refreshes(), 1);

  // A user-requested refresh allows the unresolved reference to request again.
  ASSERT_TRUE(Refresh(*service));
  (void)service->Resources()->Resolve(absent);
  QTest::qWait(20);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(refreshes(), 3);
}

TEST_F(LutResourceResolutionTest, RetiredPackageContentWaitsForActiveResourceRead) {
  const fs::path     old_file = ActivateContent("a");
  const auto         service  = StartService();

  // A render resolved the official LUT and is still reading the old content.
  std::promise<void> entered;
  std::promise<void> release;
  auto               released = release.get_future().share();
  fs::path           read_path;
  std::thread        render([&] {
    service->Resources()->ReadResource(Official(), [&](const LutResourceResolution& resolution) {
      read_path = resolution.path;
      entered.set_value();
      released.wait();
      EXPECT_TRUE(fs::exists(resolution.path)) << "content removed during a resource read";
    });
  });
  entered.get_future().wait();

  // Another installation committed; reloading the root retires the old content.
  const fs::path new_file = ActivateContent("b");
  ASSERT_EQ(service->UseRoot(library_root_), Service::Status::kOk);
  QTest::qWait(300);
  EXPECT_TRUE(service->busy()) << "retirement must wait for the running read";
  EXPECT_TRUE(fs::exists(old_file));

  release.set_value();
  render.join();
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(read_path, old_file);
  EXPECT_FALSE(fs::exists(old_file));
  EXPECT_EQ(service->Resources()->Resolve(Official()).path, new_file);
}

TEST_F(LutResourceResolutionTest, ResolverOutlivesTheLibraryService) {
  const fs::path active    = ActivateContent("a");
  auto           service   = StartService();
  const auto     resources = service->Resources();
  service.reset();
  // Render executors may hold the resolver while the application shuts down.
  const auto resolution = resources->Resolve(Official());
  EXPECT_EQ(resolution.status, LutResourceStatus::kAvailable);
  EXPECT_EQ(resolution.path, active);
  EXPECT_EQ(resources->Resolve(LibraryLutReference{"user/absent.cube"}).status,
            LutResourceStatus::kMissing);
}

}  // namespace
}  // namespace alcedo::test
