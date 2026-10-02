//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_comparison_inputs.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/version_ref.hpp"

namespace alcedo {
namespace {

/// A comparison side resolved against the held history.
struct ResolvedComparisonSource {
  std::string        label;
  /// True when the side shows the captured working values.
  bool               uses_current = false;
  head_commit_hash_t head;
};

auto ResolveSource(const CommitGraph& graph, sl_element_id_t element_id,
                   const EditorComparisonSource& source) -> ResolvedComparisonSource {
  switch (source.kind) {
    case EditorComparisonSourceKind::Root:
      return {"Root", false, std::nullopt};
    case EditorComparisonSourceKind::Current:
      return {"Current working state", true, std::nullopt};
    case EditorComparisonSourceKind::Version: {
      const auto& refs  = graph.GetAllVersionRefs();
      const auto  found = refs.find(source.version_id);
      if (found == refs.end()) {
        throw std::invalid_argument("The selected Version does not exist in this image's history");
      }
      const auto* ref = &found->second;
      if (ref->element_id != element_id) {
        throw std::invalid_argument("Version \"" + ref->display_name +
                                    "\" belongs to another image");
      }
      if (graph.GetActiveVersionId() == source.version_id) {
        // The active Version shows the working values captured on entry.
        return {"Version \"" + ref->display_name + "\"", true, std::nullopt};
      }
      return {"Version \"" + ref->display_name + "\"", false, ref->head_commit_hash};
    }
  }
  throw std::invalid_argument("Unknown comparison source");
}

void RejectHdr(const PipelineDocument& document, const std::string& label) {
  const auto* drt = document.Drt();
  if (drt != nullptr && IsHdrExportEncoding(*drt)) {
    throw std::invalid_argument(label +
                                " uses an HDR encoding (ST 2084 or HLG); HDR comparison is "
                                "unavailable");
  }
}

/// Throws the real error when the camera profile references a DNG profile that was not loaded.
void RequireBoundCameraProfile(const PipelineDocument& document) {
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::invalid_argument("The document has no Develop node");
  }
  (void)develop->Params().DngProfile().RequireBound();
}

auto BuildInput(const CommitGraph& graph, const LoadedRootState& root,
                const std::shared_ptr<const PipelineGraphSnapshot>& current,
                sl_element_id_t element_id, const EditorComparisonSource& source)
    -> EditorComparisonInput {
  const auto resolved = ResolveSource(graph, element_id, source);
  if (resolved.uses_current) {
    // The captured preview already has the current sensor settings; no derivative is needed.
    return {source, std::nullopt, current};
  }
  std::string replay_error;
  auto        document = BuildDocumentFromRoot(graph, root, resolved.head, &replay_error);
  if (!document) {
    throw std::runtime_error(resolved.label + " cannot be rebuilt: " + replay_error);
  }
  try {
    ValidateProductDocument(*document, element_id);
    RejectHdr(*document, resolved.label);
    document->UseSensorSettingsFrom(current->Document());
    RequireBoundCameraProfile(*document);
  } catch (const std::exception& ex) {
    throw std::runtime_error(resolved.label + ": " + ex.what());
  }
  // The private document is frozen and dropped here; only the frozen copy leaves this call.
  auto snapshot = PipelineGraphSnapshot::Preview(document->Freeze(), current->ElementId(),
                                                 current->Lineage(), transaction_chain_hash_t{});
  return {source, resolved.head, std::move(snapshot)};
}

}  // namespace

auto BuildEditorComparisonInputs(const CommitGraph& graph, const LoadedRootState& root,
                                 const std::shared_ptr<const PipelineGraphSnapshot>& current,
                                 sl_element_id_t element_id, const EditorComparisonSource& a,
                                 const EditorComparisonSource& b, std::string* error)
    -> std::optional<EditorComparisonInputPair> {
  try {
    if (!current) {
      throw std::invalid_argument("The captured working state is missing");
    }
    if (current->ElementId() != element_id) {
      throw std::invalid_argument("The captured working state belongs to another image");
    }
    RejectHdr(current->Document(), "The current working state");
    RequireBoundCameraProfile(current->Document());
    EditorComparisonInputPair pair{BuildInput(graph, root, current, element_id, a),
                                   BuildInput(graph, root, current, element_id, b)};
    return pair;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
  } catch (...) {
    if (error != nullptr) {
      *error = "Comparison inputs failed with an unknown error";
    }
  }
  return std::nullopt;
}

}  // namespace alcedo
