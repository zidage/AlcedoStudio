//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/qan_delegate_library.hpp"

#include <QByteArray>
#include <QVariant>
#include <utility>

#include "qanGraph.h"

namespace alcedo::ui {

void QanDelegateLibrary::Configure(QUrl color_grade_delegate_url, QUrl endpoint_delegate_url,
                                   QUrl port_delegate_url, QUrl port_dock_delegate_url,
                                   QUrl edge_delegate_url) {
  if (color_grade_delegate_url_ == color_grade_delegate_url &&
      endpoint_delegate_url_ == endpoint_delegate_url && port_delegate_url_ == port_delegate_url &&
      port_dock_delegate_url_ == port_dock_delegate_url &&
      edge_delegate_url_ == edge_delegate_url) {
    return;
  }
  color_grade_delegate_url_ = std::move(color_grade_delegate_url);
  endpoint_delegate_url_    = std::move(endpoint_delegate_url);
  port_delegate_url_        = std::move(port_delegate_url);
  port_dock_delegate_url_   = std::move(port_dock_delegate_url);
  edge_delegate_url_        = std::move(edge_delegate_url);
  Reset();
}

auto QanDelegateLibrary::EnsureLoaded(QQmlEngine& engine, qan::Graph& graph) -> QString {
  if (engine_.data() != &engine) {
    Reset();
    engine_ = &engine;
  }
  if (port_delegate_graph_.data() != &graph) {
    port_delegate_graph_.clear();
  }
  if (port_dock_delegate_graph_.data() != &graph) {
    port_dock_delegate_graph_.clear();
  }
  if (selection_delegate_graph_.data() != &graph) {
    selection_delegate_graph_.clear();
  }

  if (!color_grade_component_) {
    auto loaded = LoadComponent(engine, color_grade_delegate_url_,
                                QStringLiteral("Alcedo color-grade node delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    color_grade_component_ = std::move(loaded.component);
  }
  if (!endpoint_component_) {
    auto loaded = LoadComponent(engine, endpoint_delegate_url_,
                                QStringLiteral("Alcedo endpoint node delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    endpoint_component_ = std::move(loaded.component);
  }
  if (!edge_component_) {
    auto loaded = LoadComponent(engine, edge_delegate_url_, QStringLiteral("Alcedo edge delegate"));
    if (!loaded.component) {
      return loaded.error;
    }
    edge_component_ = std::move(loaded.component);
  }

  auto error = InstallPortDelegate(engine, graph);
  if (!error.isEmpty()) {
    return error;
  }
  error = InstallPortDockDelegate(engine, graph);
  if (!error.isEmpty()) {
    return error;
  }
  return InstallInvisibleSelectionDelegate(engine, graph);
}

auto QanDelegateLibrary::ComponentFor(EditorNodeKind kind) const -> QQmlComponent* {
  return kind == EditorNodeKind::ColorGrade ? color_grade_component_.get()
                                            : endpoint_component_.get();
}

void QanDelegateLibrary::Reset() {
  color_grade_component_.reset();
  endpoint_component_.reset();
  edge_component_.reset();
  engine_.clear();
  ResetGraphInstallations();
}

void QanDelegateLibrary::ResetGraphInstallations() {
  port_delegate_graph_.clear();
  port_dock_delegate_graph_.clear();
  selection_delegate_graph_.clear();
}

auto QanDelegateLibrary::LoadComponent(QQmlEngine& engine, const QUrl& url, const QString& role)
    -> LoadedComponent {
  LoadedComponent loaded;
  if (url.isEmpty()) {
    loaded.error = role + QStringLiteral(" URL is empty");
    return loaded;
  }
  auto component = std::make_unique<QQmlComponent>(&engine, url, QQmlComponent::PreferSynchronous);
  if (component->isError() || !component->isReady()) {
    loaded.error = role + QStringLiteral(" failed to load");
    if (!component->errorString().isEmpty()) {
      loaded.error += QStringLiteral(": ") + component->errorString().trimmed();
    }
    return loaded;
  }
  loaded.component = std::move(component);
  return loaded;
}

auto QanDelegateLibrary::InstallPortDelegate(QQmlEngine& engine, qan::Graph& graph) -> QString {
  if (port_delegate_graph_.data() == &graph) {
    return {};
  }
  auto loaded = LoadComponent(engine, port_delegate_url_, QStringLiteral("Alcedo port delegate"));
  if (!loaded.component) {
    return loaded.error;
  }
  auto* component = loaded.component.get();
  if (!graph.setProperty("portDelegate", QVariant::fromValue(component))) {
    return QStringLiteral("Alcedo port delegate installation failed");
  }
  loaded.component.release();
  port_delegate_graph_ = &graph;
  return {};
}

auto QanDelegateLibrary::InstallPortDockDelegate(QQmlEngine& engine, qan::Graph& graph) -> QString {
  if (port_dock_delegate_graph_.data() == &graph) {
    return {};
  }
  auto loaded =
      LoadComponent(engine, port_dock_delegate_url_, QStringLiteral("Alcedo port dock delegate"));
  if (!loaded.component) {
    return loaded.error;
  }
  auto* component = loaded.component.get();
  if (!graph.setProperty("horizontalDockDelegate", QVariant::fromValue(component))) {
    return QStringLiteral("Alcedo port dock delegate installation failed");
  }
  loaded.component.release();
  port_dock_delegate_graph_ = &graph;
  return {};
}

auto QanDelegateLibrary::InstallInvisibleSelectionDelegate(QQmlEngine& engine, qan::Graph& graph)
    -> QString {
  if (selection_delegate_graph_.data() == &graph) {
    return {};
  }
  auto component = std::make_unique<QQmlComponent>(&engine);
  component->setData(QByteArrayLiteral("import QtQuick\nItem {}\n"), QUrl());
  if (component->isError() || !component->isReady()) {
    QString error = QStringLiteral("Alcedo selection delegate failed to load");
    if (!component->errorString().isEmpty()) {
      error += QStringLiteral(": ") + component->errorString().trimmed();
    }
    return error;
  }
  auto* raw_component = component.get();
  if (!graph.setProperty("selectionDelegate", QVariant::fromValue(raw_component))) {
    return QStringLiteral("Alcedo selection delegate installation failed");
  }
  component.release();
  selection_delegate_graph_ = &graph;
  return {};
}

}  // namespace alcedo::ui
