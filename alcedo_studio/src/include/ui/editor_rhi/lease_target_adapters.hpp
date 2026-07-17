//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QSize>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"

class QRhi;

namespace alcedo::editor_rhi {

// Native target creation is intentionally isolated behind this interface. It
// is called only by EditorViewportRenderer on the scene-graph render thread.
class ILeaseTargetAdapter {
 public:
  virtual ~ILeaseTargetAdapter() = default;

  [[nodiscard]] virtual auto backend() const -> EditorBackend = 0;
  [[nodiscard]] virtual auto CreateTarget(QRhi* rhi, const QSize& size,
                                          TargetGeneration generation,
                                          LeaseFrameLayer layer = LeaseFrameLayer::InteractivePrimary)
      -> std::optional<WritableTargetLease> = 0;
  // Destroy only after both producer and renderer have released the lease, or
  // after a cancelled target with no active write.
  virtual void DestroyTarget(const WritableTargetLease& lease) = 0;

  // Producer-side OpenCL GL acquire/release. No-ops for backends that do not
  // require explicit interop acquire. Returns false on failure.
  [[nodiscard]] virtual auto AcquireForProducerWrite(const WritableTargetLease& lease) -> bool {
    (void)lease;
    return true;
  }
  [[nodiscard]] virtual auto ReleaseAfterProducerWrite(const WritableTargetLease& lease) -> bool {
    (void)lease;
    return true;
  }

  // Wait until the producer GPU write is ordered before Qt Quick samples.
  // CUDA: stream/device sync. OpenCL: clFinish after release. Returns false on error.
  [[nodiscard]] virtual auto WaitProducerWriteComplete(const WritableTargetLease& lease) -> bool {
    (void)lease;
    return true;
  }

  [[nodiscard]] virtual auto lastError() const -> const std::string& = 0;
};

class CudaD3D11LeaseAdapter final : public ILeaseTargetAdapter {
 public:
  CudaD3D11LeaseAdapter();
  ~CudaD3D11LeaseAdapter() override;

  [[nodiscard]] auto backend() const -> EditorBackend override { return EditorBackend::Cuda; }
  [[nodiscard]] auto CreateTarget(QRhi* rhi, const QSize& size, TargetGeneration generation,
                                  LeaseFrameLayer layer) -> std::optional<WritableTargetLease> override;
  void DestroyTarget(const WritableTargetLease& lease) override;
  [[nodiscard]] auto WaitProducerWriteComplete(const WritableTargetLease& lease) -> bool override;
  [[nodiscard]] auto lastError() const -> const std::string& override { return last_error_; }

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::string last_error_;
};

class OpenClOpenGlLeaseAdapter final : public ILeaseTargetAdapter {
 public:
  OpenClOpenGlLeaseAdapter();
  ~OpenClOpenGlLeaseAdapter() override;

  [[nodiscard]] auto backend() const -> EditorBackend override { return EditorBackend::OpenCl; }
  [[nodiscard]] auto CreateTarget(QRhi* rhi, const QSize& size, TargetGeneration generation,
                                  LeaseFrameLayer layer) -> std::optional<WritableTargetLease> override;
  void DestroyTarget(const WritableTargetLease& lease) override;
  [[nodiscard]] auto AcquireForProducerWrite(const WritableTargetLease& lease) -> bool override;
  [[nodiscard]] auto ReleaseAfterProducerWrite(const WritableTargetLease& lease) -> bool override;
  [[nodiscard]] auto WaitProducerWriteComplete(const WritableTargetLease& lease) -> bool override;
  [[nodiscard]] auto lastError() const -> const std::string& override { return last_error_; }

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::string last_error_;
};

class UnsupportedLeaseTargetAdapter final : public ILeaseTargetAdapter {
 public:
  explicit UnsupportedLeaseTargetAdapter(EditorBackend backend) : backend_(backend) {}

  [[nodiscard]] auto backend() const -> EditorBackend override { return backend_; }
  [[nodiscard]] auto CreateTarget(QRhi*, const QSize&, TargetGeneration, LeaseFrameLayer)
      -> std::optional<WritableTargetLease> override {
    last_error_ = "native lease adapter is not implemented for this backend";
    return std::nullopt;
  }
  void DestroyTarget(const WritableTargetLease&) override {}
  [[nodiscard]] auto lastError() const -> const std::string& override { return last_error_; }

 private:
  EditorBackend backend_;
  std::string last_error_;
};

[[nodiscard]] auto MakeLeaseTargetAdapter(EditorBackend backend)
    -> std::unique_ptr<ILeaseTargetAdapter>;

// Shared helpers used by LeaseFrameSink (pipeline worker) without owning the
// adapter instance. These look up resources from lease fields only.
[[nodiscard]] auto ProducerAcquireWritable(const WritableTargetLease& lease) -> bool;
[[nodiscard]] auto ProducerReleaseWritable(const WritableTargetLease& lease) -> bool;
[[nodiscard]] auto ProducerWaitWritableComplete(const WritableTargetLease& lease) -> bool;

}  // namespace alcedo::editor_rhi
