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
                                          TargetGeneration generation)
      -> std::optional<WritableTargetLease> = 0;
  virtual void DestroyTarget(const WritableTargetLease& lease) = 0;
  [[nodiscard]] virtual auto lastError() const -> const std::string& = 0;
};

class CudaD3D11LeaseAdapter final : public ILeaseTargetAdapter {
 public:
  CudaD3D11LeaseAdapter();
  ~CudaD3D11LeaseAdapter() override;

  [[nodiscard]] auto backend() const -> EditorBackend override { return EditorBackend::Cuda; }
  [[nodiscard]] auto CreateTarget(QRhi* rhi, const QSize& size,
                                  TargetGeneration generation)
      -> std::optional<WritableTargetLease> override;
  void DestroyTarget(const WritableTargetLease& lease) override;
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
  [[nodiscard]] auto CreateTarget(QRhi* rhi, const QSize& size,
                                  TargetGeneration generation)
      -> std::optional<WritableTargetLease> override;
  void DestroyTarget(const WritableTargetLease& lease) override;
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
  [[nodiscard]] auto CreateTarget(QRhi*, const QSize&, TargetGeneration)
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

}  // namespace alcedo::editor_rhi
