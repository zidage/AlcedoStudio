//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_should_exit = 0;

void SignalHandler(int signal) {
  (void)signal;
  g_should_exit = 1;
}

auto ArgValue(const std::vector<std::string>& args, const std::string& name,
              const std::string& fallback = {}) -> std::string {
  for (size_t index = 0; index + 1 < args.size(); ++index) {
    if (args[index] == name) {
      return args[index + 1];
    }
  }
  return fallback;
}

auto HasArg(const std::vector<std::string>& args, const std::string& name) -> bool {
  for (const auto& arg : args) {
    if (arg == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  const auto record_path = ArgValue(args, "--record-args");
  if (!record_path.empty()) {
    std::ofstream out(record_path, std::ios::binary);
    for (const auto& arg : args) {
      out << arg << '\n';
    }
  }

  if (HasArg(args, "--exit-now")) {
    return std::stoi(ArgValue(args, "--exit-code", "0"));
  }

  if (HasArg(args, "--ignore-terminate")) {
    std::signal(SIGTERM, SIG_IGN);
    std::signal(SIGINT, SIG_IGN);
  } else {
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGINT, SignalHandler);
  }

  std::cout << "semantic fake runtime started" << std::endl;

  // Lines in the stdout form of the sidecar `tracing` output, in one write.
  if (HasArg(args, "--print-tracing-lines")) {
    std::cout << "2026-11-11T10:00:00.000001Z  INFO fake info line marker-info\n"
                 "2026-11-11T10:00:00.000002Z  WARN fake warn line marker-warn\n"
                 "2026-11-11T10:00:00.000003Z ERROR fake error line marker-error\n"
              << std::flush;
  }
  // One WARN line in two writes, with a pause between them, followed by its continuation line.
  if (HasArg(args, "--print-split-warn-line")) {
    std::cout << "2026-11-11T10:00:00.000004Z  WARN fake split line first-half" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << " second-half\n"
                 "  continuation of the warn message marker-continuation\n"
                 "2026-11-11T10:00:00.000005Z  INFO fake info after warn marker-end\n"
              << std::flush;
  }

  const auto exit_after_ms = std::stoi(ArgValue(args, "--exit-after-ms", "0"));
  const auto sleep_ms = std::stoi(ArgValue(args, "--sleep-ms", "30000"));
  const auto start = std::chrono::steady_clock::now();
  while (!g_should_exit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count();
    if (exit_after_ms > 0 && elapsed >= exit_after_ms) {
      std::cerr << "semantic fake runtime self-exit" << std::endl;
      return std::stoi(ArgValue(args, "--exit-code", "0"));
    }
    if (sleep_ms > 0 && elapsed >= sleep_ms) {
      break;
    }
  }

  return 0;
}
