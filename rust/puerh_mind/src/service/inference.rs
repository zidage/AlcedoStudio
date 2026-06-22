use std::sync::Arc;

use crate::config::AppConfig;
use crate::service::embedding::{EmbeddingEngine, EngineModelInfo, UnavailableEmbeddingEngine};
use crate::service::model_assets::{
    InferenceBackend, REQUIRED_EMBEDDING_DIMENSION, ensure_profile_supported, find_profile,
};
use crate::service::ort_clip::OrtClipEngine;
use tracing::warn;

#[cfg(target_os = "macos")]
use crate::service::coreml_clip::CoreMlClipEngine;

pub fn build_semantic_engine(config: &AppConfig) -> Arc<dyn EmbeddingEngine> {
    match build_available_semantic_engine(config) {
        Ok(engine) => engine,
        Err(err) => {
            warn!(
                "semantic inference model is unavailable; model-manager RPCs remain available: {err}"
            );
            Arc::new(unavailable_engine(config, err.to_string()))
        }
    }
}

fn build_available_semantic_engine(config: &AppConfig) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    let profile = find_profile(&config.semantic.model_id)?;
    ensure_profile_supported(profile)?;
    match profile.inference_backend {
        InferenceBackend::NativeCoreMl => build_coreml_semantic_engine(config),
        InferenceBackend::OnnxRuntime => Ok(Arc::new(OrtClipEngine::new(&config.semantic)?)),
    }
}

fn unavailable_engine(config: &AppConfig, reason: String) -> UnavailableEmbeddingEngine {
    let profile = find_profile(&config.semantic.model_id).ok();
    UnavailableEmbeddingEngine::new(
        EngineModelInfo {
            profile_id: profile
                .map(|profile| profile.profile_id.to_string())
                .unwrap_or_else(|| config.semantic.model_id.clone()),
            model_id: profile
                .map(|profile| profile.model_id.to_string())
                .unwrap_or_else(|| config.semantic.model_id.clone()),
            revision: profile
                .map(|profile| profile.revision.to_string())
                .unwrap_or_else(|| config.semantic.revision.clone()),
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
            provider: "unavailable".to_string(),
            model_root: config.semantic.model_root.clone(),
            prototype_config_hash: String::new(),
        },
        reason,
    )
}

#[cfg(target_os = "macos")]
fn build_coreml_semantic_engine(config: &AppConfig) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    Ok(Arc::new(CoreMlClipEngine::new(&config.semantic)?))
}

#[cfg(not(target_os = "macos"))]
fn build_coreml_semantic_engine(config: &AppConfig) -> anyhow::Result<Arc<dyn EmbeddingEngine>> {
    anyhow::bail!(
        "semantic model profile {} uses native CoreML inference, which is only supported on macOS",
        config.semantic.model_id
    )
}
