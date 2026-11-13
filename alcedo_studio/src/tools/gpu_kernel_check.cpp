// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef HAVE_OPENCL
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#endif
#ifdef HAVE_METAL
#include <alcedo/metal/Metal.hpp>

#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"
#endif

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// The installer writes these hashes from the build inputs, not from the copied files.
// This also detects stale libraries and changes in CPack staging or archive extraction.
auto VerifyAssets(const QString& root) -> QStringList {
  QFile manifest(root + "/runtime-assets.sha256");
  Require(manifest.open(QIODevice::ReadOnly), "Cannot read " + manifest.fileName().toStdString());
  QStringList paths;
  while (!manifest.atEnd()) {
    const auto line = manifest.readLine().trimmed();
    Require(line.size() > 66 && line.mid(64, 2) == "  ", "Invalid GPU asset checksum entry");
    const auto relative = QString::fromUtf8(line.mid(66));
    Require(!QDir::isAbsolutePath(relative) && !relative.split('/').contains(".."),
            "Invalid GPU asset path: " + relative.toStdString());
    Require(!paths.contains(relative), "Duplicate GPU asset: " + relative.toStdString());
    QFile file(QDir(root).filePath(relative));
    Require(file.open(QIODevice::ReadOnly), "Missing GPU asset: " + file.fileName().toStdString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    Require(hash.addData(&file), "Cannot hash GPU asset: " + file.fileName().toStdString());
    Require(hash.result().toHex() == line.left(64),
            "GPU asset differs from build input: " + file.fileName().toStdString());
    paths.push_back(relative);
  }
  Require(!paths.empty(), "GPU asset checksum list is empty");
  return paths;
}

#ifdef HAVE_OPENCL
void VerifyOpenCl(const QString& bin) {
  const auto root   = bin + "/opencl";
  const auto assets = VerifyAssets(root);
  alcedo::RegisterOpenClBackendPrograms();
  auto&      library = alcedo::OpenClProgramLibrary::Instance();
  const auto names   = library.RegisteredProgramNames();
  Require(!names.empty(), "No OpenCL programs are registered");
  const auto source_root = std::filesystem::path(ALCEDO_OPENCL_SHADER_SOURCE_ROOT);
  for (const auto& name : names) {
    const auto sources = library.RegisteredSourcePaths(name);
    Require(!sources.empty(), "OpenCL program has no sources: " + name);
    for (const auto& source : sources) {
      const auto relative = source.lexically_relative(source_root);
      Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
              "OpenCL source is outside the source root: " + name);
      Require(assets.contains(QString::fromStdString(relative.generic_string())),
              "OpenCL source is absent from the package inventory: " + relative.generic_string());
    }
  }
  library.SetBuildTraceSink([](std::string_view message) { std::cout << message << std::endl; });
  auto& context = alcedo::OpenClContext::Instance();
  context.Initialize();
  const auto& device = context.Capabilities();
  std::cout << "OpenCL device=" << device.name << " driver=" << device.driver_version << std::endl;
  std::size_t kernel_count = 0;
  for (const auto& name : names) {
    const auto program = library.GetProgram(name);
    cl_uint    count   = 0;
    auto       error   = clCreateKernelsInProgram(program, 0, nullptr, &count);
    Require(error == CL_SUCCESS && count > 0,
            "Cannot enumerate kernels in " + name + ": OpenCL error " + std::to_string(error));
    std::vector<cl_kernel> kernels(count, nullptr);
    error = clCreateKernelsInProgram(program, count, kernels.data(), nullptr);
    for (auto kernel : kernels) {
      if (kernel != nullptr) {
        clReleaseKernel(kernel);
      }
    }
    Require(error == CL_SUCCESS,
            "Cannot create kernels in " + name + ": OpenCL error " + std::to_string(error));
    kernel_count += count;
    std::cout << "OpenCL program=" << name << " kernels=" << count << std::endl;
  }
  std::cout << "gpu.kernels.ok=opencl programs=" << names.size() << " kernels=" << kernel_count
            << std::endl;
}
#endif

#ifdef HAVE_METAL
void VerifyMetal(const QString& bin) {
  auto       pool   = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  const auto root   = QDir(bin).filePath("../Resources/metallib");
  const auto assets = VerifyAssets(root);
  auto*      device = alcedo::MetalContext::Instance().Device();
  Require(device != nullptr, "Metal device is unavailable");
  std::cout << "Metal device=" << device->name()->utf8String() << std::endl;
  std::size_t kernel_count = 0;
  for (const auto& asset : assets) {
    Require(asset.endsWith(".metallib"), "Unexpected Metal asset: " + asset.toStdString());
    const auto path    = QDir(root).filePath(asset).toStdString();
    NS::Error* error   = nullptr;
    auto       library = NS::TransferPtr(
        device->newLibrary(NS::String::string(path.c_str(), NS::UTF8StringEncoding), &error));
    Require(static_cast<bool>(library),
            "Cannot load " + path +
                (error ? ": " + std::string(error->localizedDescription()->utf8String()) : ""));
    auto* functions = library->functionNames();
    Require(functions != nullptr && functions->count() > 0, "No Metal functions in " + path);
    for (NS::UInteger i = 0; i < functions->count(); ++i) {
      const auto* name = functions->object<NS::String>(i);
      std::cout << "Metal library=" << asset.toStdString() << " function=" << name->utf8String()
                << std::endl;
      (void)alcedo::metal::ComputePipelineCache::Instance().GetPipelineState(
          path.c_str(), name->utf8String(), name->utf8String());
      ++kernel_count;
    }
  }
  std::cout << "gpu.kernels.ok=metal libraries=" << assets.size() << " kernels=" << kernel_count
            << std::endl;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    const auto args = app.arguments();
    Require(args.size() == 2, "Usage: alcedo-gpu-check --opencl|--metal");
#ifdef HAVE_OPENCL
    if (args[1] == "--opencl") {
      VerifyOpenCl(app.applicationDirPath());
      return 0;
    }
#endif
#ifdef HAVE_METAL
    if (args[1] == "--metal") {
      VerifyMetal(app.applicationDirPath());
      return 0;
    }
#endif
    throw std::runtime_error("Requested GPU backend is not built: " + args[1].toStdString());
  } catch (const std::exception& error) {
    std::cerr << "GPU kernel verification failed: " << error.what() << std::endl;
    return 1;
  }
}
