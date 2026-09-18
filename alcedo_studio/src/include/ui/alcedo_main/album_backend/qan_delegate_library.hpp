//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <memory>

#include "app/editor_node_graph_projection.hpp"
#include "qanGraph.h"

namespace alcedo::ui {

/**
 * @brief Owns the QML delegates used by one Alcedo QuickQanava adapter.
 *
 * The library owns delegate URLs and components that are tied to a
 * QQmlEngine. QuickQanava owns components installed on a qan::Graph, so the
 * library retains only per-graph installation markers for those components.
 * It has no controller, adapter, or shared application context ownership.
 *
 * Threading: GUI thread only. @c EnsureLoaded must run on the thread that owns
 * the supplied engine and graph. Pointer lifetime: the engine and graph must
 * outlive the call; the library clears markers when either changes or when
 * Reset is called. Failure returns the actual delegate-load or installation
 * error and never selects an upstream visual delegate as a substitute.
 */
class QanDelegateLibrary final {
 public:
  QanDelegateLibrary()                                     = default;
  ~QanDelegateLibrary()                                    = default;

  QanDelegateLibrary(const QanDelegateLibrary&)            = delete;
  QanDelegateLibrary& operator=(const QanDelegateLibrary&) = delete;
  QanDelegateLibrary(QanDelegateLibrary&&)                 = delete;
  QanDelegateLibrary& operator=(QanDelegateLibrary&&)      = delete;

  /**
   * @brief Configure all delegate URLs and discard cached components on change.
   */
  void Configure(QUrl color_grade_delegate_url, QUrl endpoint_delegate_url, QUrl port_delegate_url,
                 QUrl port_dock_delegate_url, QUrl edge_delegate_url);

  /**
   * @brief Load delegates for @p engine and install graph-owned delegates.
   * @return Empty on success, or the exact load/installation failure.
   */
  [[nodiscard]] auto EnsureLoaded(QQmlEngine& engine, qan::Graph& graph) -> QString;

  /**
   * @brief Return the cached node delegate for @p kind.
   * @return A cached component, or nullptr before a successful EnsureLoaded.
   */
  [[nodiscard]] auto ComponentFor(EditorNodeKind kind) const -> QQmlComponent*;

  /**
   * @brief Return the cached edge delegate after a successful EnsureLoaded.
   */
  [[nodiscard]] auto EdgeComponent() const -> QQmlComponent* { return edge_component_.get(); }

  /**
   * @brief Drop engine-owned components and all graph-installation markers.
   *
   * Components already transferred to a graph remain owned by that graph.
   */
  void               Reset();

  [[nodiscard]] auto ColorGradeDelegateUrl() const -> const QUrl& {
    return color_grade_delegate_url_;
  }
  [[nodiscard]] auto EndpointDelegateUrl() const -> const QUrl& { return endpoint_delegate_url_; }
  [[nodiscard]] auto PortDelegateUrl() const -> const QUrl& { return port_delegate_url_; }
  [[nodiscard]] auto PortDockDelegateUrl() const -> const QUrl& { return port_dock_delegate_url_; }
  [[nodiscard]] auto EdgeDelegateUrl() const -> const QUrl& { return edge_delegate_url_; }

 private:
  struct LoadedComponent {
    std::unique_ptr<QQmlComponent> component;
    QString                        error;
  };

  [[nodiscard]] auto LoadComponent(QQmlEngine& engine, const QUrl& url, const QString& role)
      -> LoadedComponent;
  [[nodiscard]] auto InstallPortDelegate(QQmlEngine& engine, qan::Graph& graph) -> QString;
  [[nodiscard]] auto InstallPortDockDelegate(QQmlEngine& engine, qan::Graph& graph) -> QString;
  [[nodiscard]] auto InstallInvisibleSelectionDelegate(QQmlEngine& engine, qan::Graph& graph)
      -> QString;
  void                           ResetGraphInstallations();

  QUrl                           color_grade_delegate_url_;
  QUrl                           endpoint_delegate_url_;
  QUrl                           port_delegate_url_;
  QUrl                           port_dock_delegate_url_;
  QUrl                           edge_delegate_url_;

  QPointer<QQmlEngine>           engine_;
  QPointer<qan::Graph>           port_delegate_graph_;
  QPointer<qan::Graph>           port_dock_delegate_graph_;
  QPointer<qan::Graph>           selection_delegate_graph_;
  std::unique_ptr<QQmlComponent> color_grade_component_;
  std::unique_ptr<QQmlComponent> endpoint_component_;
  std::unique_ptr<QQmlComponent> edge_component_;
};

}  // namespace alcedo::ui
