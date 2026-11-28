//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The photo timing tool of AI Denoise on macOS (integration plan, DN10): the time of the tile loop
// (DenoiseBayerMosaic) with UtNet2CoreMlSession on full photos, the creation of the sessions of
// the tile shapes, and the physical footprint of the process.
//
// Input: the folder of alcedo-denoise-lab scripts/export/write_raw_inputs.py (`manifest.txt` and
// the packed photos in float32, input preparation 1) and the FP16 graph.
//
//   UtNet2CoreMlPhotoTimingTool --graph <fp16 onnx> --inputs <folder> [--photos 24mp 36mp 45mp]
//       [--repeats 3] [--tile auto|768|1024|1536] [--model-cache <folder>]
//       [--release-after-each-photo] [--rounds 1] [--no-slow-device-check]
//       [--hold-after-release <seconds>]
//
// For each photo: one run that creates the sessions of its tile shapes (not measured), then
// `--repeats` runs with 1 s between them. Each run prints the tile size, the tile count, the time
// of the tile loop, the sum of the `Run` times, and the physical footprint of the process (Mach
// `phys_footprint`, which counts the memory of the Neural Engine) and its growth since the photo
// and its output image were in memory and no session existed. `--rounds` repeats the list of
// photos (a batch); `--release-after-each-photo` releases the sessions after each photo. After
// the last photo the tool releases the sessions and prints the footprint; `--hold-after-release`
// then keeps the process for that many seconds (for vmmap and footprint of the process).

#include <mach/mach.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "decoders/processor/nn/denoise_errors.hpp"
#include "decoders/processor/nn/utnet2_coreml_session.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"

namespace {

using alcedo::nn::DenoiseBayerMosaic;
using alcedo::nn::DenoiseDeviceTooSlowError;
using alcedo::nn::PackedMosaicView;
using alcedo::nn::RgbPlanesView;
using alcedo::nn::UtNet2CoreMlSession;
using alcedo::nn::UtNet2TileLoopOptions;

struct Arguments {
  std::filesystem::path    graph;
  std::filesystem::path    inputs;
  std::filesystem::path    model_cache;
  std::vector<std::string> photos                   = {"24mp", "36mp", "45mp"};
  int                      repeats                  = 3;
  int                      rounds                   = 1;
  int                      tile                     = 0;
  int                      hold_after_release       = 0;
  bool                     slow_device_check        = true;
  bool                     release_after_each_photo = false;
};

auto ParseArguments(int argc, char** argv) -> Arguments {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string name  = argv[index];
    const auto        value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(name + " needs a value");
      }
      return argv[++index];
    };
    if (name == "--graph") {
      arguments.graph = value();
    } else if (name == "--inputs") {
      arguments.inputs = value();
    } else if (name == "--model-cache") {
      arguments.model_cache = value();
    } else if (name == "--photos") {
      arguments.photos.clear();
      while (index + 1 < argc && std::string(argv[index + 1]).rfind("--", 0) != 0) {
        arguments.photos.emplace_back(argv[++index]);
      }
    } else if (name == "--repeats") {
      arguments.repeats = std::stoi(value());
    } else if (name == "--rounds") {
      arguments.rounds = std::stoi(value());
    } else if (name == "--tile") {
      const auto text = value();
      arguments.tile  = text == "auto" ? 0 : std::stoi(text);
    } else if (name == "--hold-after-release") {
      arguments.hold_after_release = std::stoi(value());
    } else if (name == "--release-after-each-photo") {
      arguments.release_after_each_photo = true;
    } else if (name == "--no-slow-device-check") {
      arguments.slow_device_check = false;
    } else {
      throw std::invalid_argument("unknown argument " + name);
    }
  }
  if (arguments.graph.empty() || arguments.inputs.empty()) {
    throw std::invalid_argument("--graph and --inputs are required");
  }
  return arguments;
}

struct PackedPhoto {
  int                width  = 0;
  int                height = 0;
  std::vector<float> data;
};

auto LoadPhoto(const std::filesystem::path& folder, const std::string& name) -> PackedPhoto {
  std::ifstream manifest(folder / "manifest.txt");
  std::string   line;
  while (std::getline(manifest, line)) {
    std::istringstream fields(line);
    std::string        entry, kind, file;
    int                channels = 0, height = 0, width = 0;
    fields >> entry >> kind >> channels >> height >> width >> file;
    if (entry != name || kind != "photo") {
      continue;
    }
    PackedPhoto   photo{width, height, std::vector<float>(std::size_t{4} * width * height)};
    std::ifstream stream(folder / file, std::ios::binary);
    stream.read(reinterpret_cast<char*>(photo.data.data()),
                static_cast<std::streamsize>(photo.data.size() * sizeof(float)));
    if (channels != 4 || !stream) {
      throw std::runtime_error("photo " + name + ": " + (folder / file).string() +
                               " is not a (4, h, w) float32 file");
    }
    return photo;
  }
  throw std::runtime_error("photo " + name + " is not in " + (folder / "manifest.txt").string());
}

struct Footprint {
  std::uint64_t current = 0;
  std::uint64_t peak    = 0;
};

auto ReadFootprint() -> Footprint {
  task_vm_info_data_t    info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return {};
  }
  return {info.phys_footprint, static_cast<std::uint64_t>(info.ledger_phys_footprint_peak)};
}

auto Seconds(std::chrono::nanoseconds time) -> double {
  return std::chrono::duration<double>(time).count();
}

auto Mebibytes(std::uint64_t bytes) -> double { return static_cast<double>(bytes) / (1 << 20); }

auto SignedMebibytes(std::uint64_t after, std::uint64_t before) -> double {
  return Mebibytes(after) - Mebibytes(before);
}

void RunPhoto(UtNet2CoreMlSession& session, const Arguments& arguments, const std::string& name,
              int round) {
  const auto             photo = LoadPhoto(arguments.inputs, name);
  std::vector<float>     output(std::size_t{3} * 4 * photo.width * photo.height, 0.0f);
  const PackedMosaicView mosaic{photo.data, photo.width, photo.height};
  const RgbPlanesView    image{output, 2 * photo.width, 2 * photo.height};
  UtNet2TileLoopOptions  options;
  options.tile_size = arguments.tile;
  if (!arguments.slow_device_check) {
    options.slow_device_limit_seconds_per_megapixel = std::numeric_limits<double>::infinity();
  }
  const double megapixels = 4.0 * photo.width * photo.height / 1e6;
  const auto   baseline   = ReadFootprint();

  for (int repeat = -1; repeat < arguments.repeats; ++repeat) {
    if (repeat >= 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    const auto creation_before = session.SessionCreationTime();
    const auto started         = std::chrono::steady_clock::now();
    std::cout.precision(4);
    std::cout << "{\"photo\": \"" << name << "\", \"round\": " << round
              << ", \"megapixels\": " << megapixels << ", \"repeat\": " << repeat;
    try {
      const auto record = DenoiseBayerMosaic(
          session, mosaic, image, {}, {}, [] { return std::chrono::steady_clock::now(); }, options);
      std::cout << ", \"tile\": " << record.tile_size << ", \"tiles\": " << record.tile_count
                << ", \"loop_s\": " << Seconds(record.loop_time)
                << ", \"run_s\": " << Seconds(record.run_time)
                << ", \"measured_tiles\": " << record.measured_tiles;
    } catch (const DenoiseDeviceTooSlowError& error) {
      std::cout << ", \"slow_device_error\": \"" << error.what() << "\", \"time_to_stop_s\": "
                << Seconds(std::chrono::steady_clock::now() - started);
    }
    const auto footprint = ReadFootprint();
    std::cout << ", \"session_creation_s\": "
              << Seconds(session.SessionCreationTime() - creation_before)
              << ", \"sessions\": " << session.SessionCount()
              << ", \"footprint_mib\": " << Mebibytes(footprint.current)
              << ", \"footprint_peak_mib\": " << Mebibytes(footprint.peak)
              << ", \"growth_since_photo_mib\": "
              << SignedMebibytes(footprint.current, baseline.current) << "}" << std::endl;
  }
  if (arguments.release_after_each_photo) {
    session.Release();
    std::cout << "{\"released_after\": \"" << name
              << "\", \"footprint_mib\": " << Mebibytes(ReadFootprint().current) << "}"
              << std::endl;
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    const auto          arguments = ParseArguments(argc, argv);
    const auto          start     = ReadFootprint();
    UtNet2CoreMlSession session(arguments.graph, arguments.model_cache);
    std::cout << "{\"footprint_start_mib\": " << Mebibytes(start.current)
              << ", \"footprint_after_graph_check_mib\": " << Mebibytes(ReadFootprint().current)
              << ", \"model_cache\": \"" << arguments.model_cache.string() << "\"}" << std::endl;
    for (int round = 0; round < arguments.rounds; ++round) {
      for (const auto& photo : arguments.photos) {
        RunPhoto(session, arguments, photo, round);
      }
    }
    session.Release();
    const auto end = ReadFootprint();
    std::cout << "{\"released\": true, \"footprint_mib\": " << Mebibytes(end.current)
              << ", \"footprint_peak_mib\": " << Mebibytes(end.peak)
              << ", \"growth_since_start_mib\": " << SignedMebibytes(end.current, start.current)
              << ", \"pid\": " << getpid() << "}" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(arguments.hold_after_release));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << std::endl;
    return 1;
  }
}
