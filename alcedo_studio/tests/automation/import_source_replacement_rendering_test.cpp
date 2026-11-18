// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "automation_in_process_session.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "ui/alcedo_main/automation/headless_host.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::automation {
namespace {
constexpr int kWidth  = 128;
constexpr int kHeight = 96;

/// Write a valid RGB TIFF with different dimensions and pixels for the replacement.
void          WriteRaster(const std::filesystem::path& path, bool replacement) {
  const int            width = replacement ? kWidth : kWidth - 16;
  std::vector<uint8_t> pixels(width * kHeight * 3);
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<uint8_t>((replacement ? 35 : 95) + i % 50);
  auto output = OIIO::ImageOutput::create(path.string());
  if (!output ||
      !output->open(path.string(), OIIO::ImageSpec(width, kHeight, 3, OIIO::TypeDesc::UINT8)) ||
      !output->write_image(OIIO::TypeDesc::UINT8, pixels.data()) || !output->close())
    throw std::runtime_error("Cannot write replacement raster fixture: " + OIIO::geterror());
}

/// Write an uncompressed RGGB DNG. Both files share camera/color metadata, so a fresh import
/// of B provides an independent pixel comparison with A's preserved exposure adjustment.
void WriteBayerDng(const std::filesystem::path& path, bool replacement) {
  struct Tag {
    uint16_t id, type;
    uint32_t count, value;
  };
  const std::string    model = "Alcedo test Bayer";
  std::vector<Tag>     tags{{256, 4, 1, kWidth},
                            {257, 4, 1, kHeight},
                            {258, 3, 1, 16},
                            {259, 3, 1, 1},
                            {262, 3, 1, 32803},
                            {273, 4, 1, 0},
                            {277, 3, 1, 1},
                            {278, 4, 1, kHeight},
                            {279, 4, 1, kWidth * kHeight * 2},
                            {284, 3, 1, 1},
                            {33421, 3, 2, 0x00020002},
                            {33422, 1, 4, 0x02010100},
                            {50706, 1, 4, 0x00000401},
                            {50707, 1, 4, 0x00000101},
                            {50708, 2, static_cast<uint32_t>(model.size() + 1), 0},
                            {50714, 3, 1, 0},
                            {50717, 4, 1, 4095},
                            {50721, 10, 9, 0},
                            {50728, 5, 3, 0},
                            {50778, 3, 1, 21}};
  std::vector<uint8_t> bytes(8 + 2 + tags.size() * 12 + 4, 0);
  const auto           append = [&](uint32_t value, int size) {
    for (int i = 0; i < size; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
  };
  const auto offset = [&](uint16_t id) {
    for (auto& tag : tags)
      if (tag.id == id) tag.value = static_cast<uint32_t>(bytes.size());
  };
  offset(50708);
  bytes.insert(bytes.end(), model.begin(), model.end());
  bytes.push_back(0);
  if (bytes.size() % 2) bytes.push_back(0);
  offset(50721);
  for (int i = 0; i < 9; ++i) {
    append(i % 4 == 0 ? 10000 : 0, 4);
    append(10000, 4);
  }
  offset(50728);
  for (int i = 0; i < 3; ++i) {
    append(1, 4);
    append(1, 4);
  }
  offset(273);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) append((replacement ? 450 : 1200) + y + x * 2, 2);
  if (replacement) bytes.insert(bytes.end(), 16, 0);  // Accepted size-only replacement signal.
  const auto write = [&](size_t position, uint32_t value, int size) {
    for (int i = 0; i < size; ++i) bytes[position + i] = static_cast<uint8_t>(value >> (8 * i));
  };
  write(0, 0x4949, 2);
  write(2, 42, 2);
  write(4, 8, 4);
  write(8, static_cast<uint32_t>(tags.size()), 2);
  for (size_t i = 0; i < tags.size(); ++i) {
    const auto& tag = tags[i];
    write(10 + i * 12, tag.id, 2);
    write(12 + i * 12, tag.type, 2);
    write(14 + i * 12, tag.count, 4);
    write(18 + i * 12, tag.value, 4);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("Cannot write replacement Bayer DNG fixture");
}

/// Read RGB8 output for a per-channel comparison; reject missing or malformed image files.
auto ReadPixels(const QString& path) -> std::vector<uint8_t> {
  auto input = OIIO::ImageInput::open(path.toStdString());
  if (!input || input->spec().nchannels < 3)
    throw std::runtime_error("Cannot read rendered fixture");
  const auto&          spec = input->spec();
  std::vector<uint8_t> pixels(spec.width * spec.height * 3);
  if (!input->read_image(0, 0, 0, 3, OIIO::TypeDesc::UINT8, pixels.data()) || !input->close())
    throw std::runtime_error("Cannot read rendered pixels");
  return pixels;
}

/// Maximum RGB8 channel error. Different pixel counts fail rather than comparing prefixes.
auto PixelDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) -> int {
  if (a.size() != b.size()) return 255;
  int difference = 0;
  for (size_t i = 0; i < a.size(); ++i)
    difference = std::max(difference, std::abs(static_cast<int>(a[i]) - b[i]));
  return difference;
}

/// Real application owners, GPU presentation, automation import, preview and export. Test
/// files stay under build/tmp and disappear only after the host and workers have shut down.
class ImportSourceReplacementRenderingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
        "build/tmp/source_replacement";
    std::filesystem::create_directories(root);
    directory_ =
        std::make_unique<QTemporaryDir>(QString::fromStdWString((root / "case-XXXXXX").wstring()));
    ASSERT_TRUE(directory_->isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_->path());
    TimeProvider::Refresh();
#ifdef _WIN32
    constexpr auto backend = editor_rhi::EditorBackend::Cuda;
#elif defined(__APPLE__)
    constexpr auto backend = editor_rhi::EditorBackend::Metal;
#else
    constexpr auto backend = editor_rhi::EditorBackend::OpenCl;
#endif
    const auto error = StartHeadlessEditorBackend(backend);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    session_ = std::make_unique<test::InProcessAutomationSession>();
    session_->host().project()->SetRuntimeAcceleratorPreference(
        HeadlessAcceleratorPreference(backend));
    ASSERT_TRUE(
        session_->host().editor_session()->BindHeadlessPresentationSink(&sink_, kWidth, kHeight));
  }
  void TearDown() override {
    if (session_) session_->host().Shutdown();
    session_.reset();
  }
  /// Wait for an owner condition while delivering real asynchronous render/import completions.
  template <class Predicate>
  auto WaitUntil(Predicate predicate) -> bool {
    const QDeadlineTimer deadline(120000);
    while (!predicate() && !deadline.hasExpired())
      QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    return predicate();
  }
  /// Import through the normal handler and return terminal task counts.
  auto Import(const std::filesystem::path& path) -> QJsonObject {
    const auto task = session_->CallResult(
        "library.import", {{"paths", QJsonArray{QString::fromStdWString(path.wstring())}},
                           {"skip_duplicates", true}});
    const auto result = session_->CallResult("tasks.wait", {{"task_id", task.value("task_id")}});
    EXPECT_EQ(result.value("state"), "succeeded");
    EXPECT_EQ(result.value("counts").toObject().value("failed"), 0);
    return result.value("counts").toObject();
  }
  /// Capture actual editor preview pixels with the current persisted/working adjustments.
  auto Preview(const QString& name) -> std::vector<uint8_t> {
    const auto result = session_->CallResult("render.preview", {{"out_dir", directory_->path()},
                                                                {"long_edge", kWidth},
                                                                {"file_stem", name},
                                                                {"compare", "none"}});
    return ReadPixels(result.value("current").toObject().value("path").toString());
  }
  /// Export lossless RGB pixels through the production queue, then decode the written file.
  auto Export(QJsonValue element, const QString& name) -> std::vector<uint8_t> {
    const auto output   = QDir(directory_->path()).filePath(name);
    const auto task     = session_->CallResult("export.run", {{"element_ids", QJsonArray{element}},
                                                              {"out_dir", output},
                                                              {"format", "PNG"},
                                                              {"bit_depth", 8},
                                                              {"file_name_pattern", "result"}});
    const auto finished = session_->CallResult("tasks.wait", {{"task_id", task.value("task_id")}});
    EXPECT_EQ(finished.value("counts").toObject().value("succeeded"), 1);
    EXPECT_EQ(finished.value("counts").toObject().value("failed"), 0);
    return ReadPixels(QDir(output).filePath("result.png"));
  }
  /// Replace A while it is open; prove refreshed and reopened outputs match independently
  /// imported B with the same edit, and prove the rating, identity and commit stay unchanged.
  void VerifyReplacement(bool raw) {
    const auto root        = std::filesystem::path(directory_->path().toStdWString());
    const auto source      = root / (raw ? "source.dng" : "source.tif");
    const auto replacement = root / (raw ? "replacement.dng" : "replacement.tif");
    const auto write       = raw ? WriteBayerDng : WriteRaster;
    write(source, false);
    write(replacement, true);
    ASSERT_NE(std::filesystem::file_size(source), std::filesystem::file_size(replacement));
    const auto created = session_->CallResult(
        "project.create", {{"folder", directory_->path()}, {"name", "replacement"}});
    ASSERT_EQ(Import(source).value("completed"), 1);
    const auto element = session_->CallResult("library.list")
                             .value("items")
                             .toArray()
                             .first()
                             .toObject()
                             .value("element_id");
    session_->CallResult("library.rate", {{"element_ids", QJsonArray{element}}, {"rating", 4}});
    session_->CallResult("editor.open", {{"element_id", element}});
    const auto image_id = session_->host().editor_session()->image_id();
    const auto edited = session_->CallResult("editor.set", {{"field", "exposure"}, {"value", 0.5}});
    const auto before = Preview("before");
    const auto before_export = Export(element, "export_before");
    const auto frames        = sink_.ready_frame_count();
    std::filesystem::copy_file(replacement, source,
                               std::filesystem::copy_options::overwrite_existing);
    const auto counts = Import(source);
    EXPECT_EQ(counts.value("updated"), 1);
    EXPECT_EQ(counts.value("duplicates"), 0);
    ASSERT_TRUE(WaitUntil([&] {
      return sink_.ready_frame_count() > frames && session_->host().editor_session()->can_edit();
    }));
    EXPECT_EQ(session_->host().editor_session()->image_id(), image_id);
    EXPECT_EQ(session_->CallResult("editor.history").value("head_commit"),
              edited.value("head_commit"));
    const auto after        = Preview("after");
    const auto after_export = Export(element, "export_after");
    EXPECT_GT(PixelDifference(before, after), 10);
    EXPECT_GT(PixelDifference(before_export, after_export), 10);
    ASSERT_EQ(Import(replacement).value("completed"), 1);
    const auto items = session_->CallResult("library.list").value("items").toArray();
    ASSERT_EQ(items.size(), 2);
    QJsonValue reference;
    for (const auto& item : items)
      if (item.toObject().value("element_id") != element)
        reference = item.toObject().value("element_id");
    ASSERT_FALSE(reference.isNull());
    session_->CallResult("editor.open", {{"element_id", reference}});
    const auto unedited        = Preview("reference_unedited");
    const auto unedited_export = Export(reference, "export_unedited");
    session_->CallResult("editor.set", {{"field", "exposure"}, {"value", 0.5}});
    const auto expected        = Preview("reference_edited");
    const auto expected_export = Export(reference, "export_expected");
    EXPECT_GT(PixelDifference(expected, unedited), 5);
    EXPECT_GT(PixelDifference(expected_export, unedited_export), 5);
    EXPECT_LE(PixelDifference(after, expected), 1);
    EXPECT_LE(PixelDifference(after_export, expected_export), 1);
    session_->CallResult("project.open", {{"path", created.value("path")}});
    session_->CallResult("editor.open", {{"element_id", element}});
    EXPECT_EQ(session_->host().editor_session()->image_id(), image_id);
    EXPECT_EQ(session_->CallResult("editor.history").value("head_commit"),
              edited.value("head_commit"));
    EXPECT_EQ(session_->CallResult("editor.get", {{"fields", QJsonArray{"exposure"}}})
                  .value("values")
                  .toObject()
                  .value("exposure"),
              0.5);
    const auto info = session_->CallResult("library.info", {{"element_ids", QJsonArray{element}}});
    ASSERT_EQ(info.value("items").toArray().size(), 1);
    EXPECT_EQ(info.value("items").toArray().first().toObject().value("rating"), 4);
    EXPECT_LE(PixelDifference(Preview("reopened"), expected), 1);
    EXPECT_LE(PixelDifference(Export(element, "export_reopened"), expected_export), 1);
  }
  std::unique_ptr<QTemporaryDir>                    directory_;
  HeadlessFrameSink                                 sink_;
  std::unique_ptr<test::InProcessAutomationSession> session_;
};

TEST_F(ImportSourceReplacementRenderingTest,
       RasterReplacementRefreshesPreviewAndExportAndPreservesEditsAfterReopen) {
  VerifyReplacement(false);
}
TEST_F(ImportSourceReplacementRenderingTest,
       BayerRawReplacementRefreshesPreviewAndExportAndPreservesEditsAfterReopen) {
  VerifyReplacement(true);
}
}  // namespace
}  // namespace alcedo::automation
