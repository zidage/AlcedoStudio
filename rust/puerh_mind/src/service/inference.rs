use std::sync::Arc;

use crate::config::{EncoderResidencyConfig, SemanticConfig};
use crate::service::embedding::{EmbeddingEngine, EngineModelInfo, UnavailableEmbeddingEngine};
use crate::service::model_assets::{
    InferenceBackend, REQUIRED_EMBEDDING_DIMENSION, ensure_profile_supported, find_profile,
};
use crate::service::ort_clip::OrtClipEngine;
use tracing::warn;

#[cfg(target_os = "macos")]
use crate::service::coreml_clip::CoreMlClipEngine;

/// The semantic engine of the configured model profile. It validates the manifest, the model
/// files, and the device values; no encoder loads here (each loads on its first request).
pub fn build_semantic_engine(
    semantic: &SemanticConfig,
    residency: &EncoderResidencyConfig,
) -> Arc<dyn EmbeddingEngine> {
    match build_available_semantic_engine(semantic, residency) {
        Ok(engine) => engine,
        Err(err) => {
            warn!(
                "semantic inference model is unavailable; model-manager RPCs remain available: {err}"
            );
            Arc::new(UnavailableEmbeddingEngine::new(
                profile_model_info(semantic, "unavailable"),
                err.to_string(),
            ))
        }
    }
}

fn build_available_semantic_engine(
    semantic: &SemanticConfig,
    residency: &EncoderResidencyConfig,
) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    let profile = find_profile(&semantic.model_id)?;
    ensure_profile_supported(profile)?;
    match profile.inference_backend {
        InferenceBackend::NativeCoreMl => build_coreml_semantic_engine(semantic, residency),
        InferenceBackend::OnnxRuntime => Ok(Arc::new(OrtClipEngine::with_residency(
            semantic, residency,
        )?)),
    }
}

/// The model information of the configured profile from the built-in profile table, without
/// reading the model files. @p provider names the state of the engine that reports it.
pub fn profile_model_info(semantic: &SemanticConfig, provider: &str) -> EngineModelInfo {
    let profile = find_profile(&semantic.model_id).ok();
    EngineModelInfo {
        profile_id: profile
            .map(|profile| profile.profile_id.to_string())
            .unwrap_or_else(|| semantic.model_id.clone()),
        model_id: profile
            .map(|profile| profile.model_id.to_string())
            .unwrap_or_else(|| semantic.model_id.clone()),
        revision: profile
            .map(|profile| profile.revision.to_string())
            .unwrap_or_else(|| semantic.revision.clone()),
        engine_profile_id: profile
            .map(|profile| profile.engine_profile_id.to_string())
            .unwrap_or_default(),
        language: profile
            .map(|profile| profile.language.as_str().to_string())
            .unwrap_or_default(),
        embedding_dim: profile
            .map(|profile| profile.embedding_dimension)
            .unwrap_or(REQUIRED_EMBEDDING_DIMENSION),
        native_embedding_dim: profile
            .map(|profile| profile.native_embedding_dimension)
            .unwrap_or(REQUIRED_EMBEDDING_DIMENSION),
        image_size: profile
            .map(|profile| profile.image_size)
            .unwrap_or_default(),
        embedding_transform: profile
            .map(|profile| profile.embedding_transform.to_string())
            .unwrap_or_default(),
        provider: provider.to_string(),
        model_root: semantic.model_root.clone(),
        prototype_config_hash: String::new(),
    }
}

#[cfg(target_os = "macos")]
fn build_coreml_semantic_engine(
    semantic: &SemanticConfig,
    residency: &EncoderResidencyConfig,
) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    Ok(Arc::new(CoreMlClipEngine::with_residency(
        semantic, residency,
    )?))
}

#[cfg(not(target_os = "macos"))]
fn build_coreml_semantic_engine(
    semantic: &SemanticConfig,
    _residency: &EncoderResidencyConfig,
) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    anyhow::bail!(
        "semantic model profile {} uses native CoreML inference, which is only supported on macOS",
        semantic.model_id
    )
}
