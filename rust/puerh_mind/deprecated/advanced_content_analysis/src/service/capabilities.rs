//! Capability descriptors advertised by `AiRuntimeService::ListCapabilities`.
//!
//! Phase 5a: the remote image-analysis descriptors are now data-driven, derived
//! from loaded provider configs (`crate::service::provider_config`). Built-in
//! OpenRouter and Volcengine Ark configs advertise `image_understanding.describe`
//! and `image_rating.score` descriptors per vision+structured-output model, so
//! the C++ host can display remote-provider availability before a task starts.
//! The previous placeholder `provider_id="remote"` / `model_id="unconfigured"`
//! descriptor is replaced by these real, config-derived descriptors.
//!
//! `extra` carries descriptors from local in-process providers that are not in a
//! provider config — the Phase 5b mock provider advertises a no-credential
//! `image_understanding.describe` / `image_rating.score` capability this way.

use crate::proto::alcedo::ai::{AiCapability, AiInputKind, AiOutputKind};
use crate::service::embedding::EmbeddingEngine;
use crate::service::provider_config::ProviderRegistry;

/// Build the capability descriptor list from the embedding engine, the loaded
/// provider configs, and any local in-process providers.
pub fn build_capability_descriptors(
    engine: &dyn EmbeddingEngine,
    max_payload_bytes: usize,
    registry: &ProviderRegistry,
    extra: &[AiCapability],
) -> Vec<AiCapability> {
    let info = engine.model_info();

    let local_semantic = AiCapability {
        task_id: "semantic.embed_*".to_string(),
        provider_id: "local".to_string(),
        model_id: info.model_id.clone(),
        input_kinds: vec![
            AiInputKind::AiInputImage as i32,
            AiInputKind::AiInputThumbnail as i32,
        ],
        output_kinds: vec![AiOutputKind::AiOutputEmbedding as i32],
        supports_batch: true,
        supports_cancel: true,
        requires_credential: false,
        max_payload_bytes: max_payload_bytes as i64,
    };

    let mut caps = vec![local_semantic];
    caps.extend(crate::service::provider_config::build_provider_capability_descriptors(registry));
    caps.extend_from_slice(extra);
    caps
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::service::embedding::MockEmbeddingEngine;
    use crate::service::provider_config::load_provider_configs;

    fn builtin_registry() -> ProviderRegistry {
        load_provider_configs(None).expect("built-in configs load")
    }

    #[test]
    fn builds_local_semantic_descriptor() {
        let engine = MockEmbeddingEngine;
        let registry = builtin_registry();
        let caps = build_capability_descriptors(&engine, 4096, &registry, &[]);
        let local = caps
            .iter()
            .find(|c| c.task_id == "semantic.embed_*")
            .expect("local cap");
        assert_eq!(local.provider_id, "local");
        assert_eq!(local.model_id, "mock-model-v1");
        assert_eq!(
            local.input_kinds,
            vec![
                AiInputKind::AiInputImage as i32,
                AiInputKind::AiInputThumbnail as i32,
            ]
        );
        assert_eq!(
            local.output_kinds,
            vec![AiOutputKind::AiOutputEmbedding as i32]
        );
        assert!(local.supports_batch);
        assert!(local.supports_cancel);
        assert!(!local.requires_credential);
        assert_eq!(local.max_payload_bytes, 4096);
    }

    #[test]
    fn includes_config_derived_remote_descriptors() {
        let engine = MockEmbeddingEngine;
        let registry = builtin_registry();
        let caps = build_capability_descriptors(&engine, 4096, &registry, &[]);
        // Local + 4 config-derived (openrouter + volcengine, understanding+rating each).
        assert!(caps.len() >= 5);
        let understanding = caps
            .iter()
            .find(|c| c.task_id == "image_understanding.describe" && c.provider_id == "openrouter")
            .expect("openrouter understanding descriptor present");
        assert!(understanding.requires_credential);
        let rating = caps
            .iter()
            .find(|c| c.task_id == "image_rating.score" && c.provider_id == "volcengine_ark")
            .expect("volcengine rating descriptor present");
        assert!(rating.requires_credential);
    }

    #[test]
    fn extra_local_providers_are_appended() {
        let engine = MockEmbeddingEngine;
        let registry = builtin_registry();
        let mock = AiCapability {
            task_id: "image_understanding.describe".to_string(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            input_kinds: vec![AiInputKind::AiInputPreview as i32],
            output_kinds: vec![AiOutputKind::AiOutputCaption as i32],
            supports_batch: false,
            supports_cancel: true,
            requires_credential: false,
            max_payload_bytes: 0,
        };
        let caps = build_capability_descriptors(&engine, 4096, &registry, &[mock.clone()]);
        assert!(
            caps.iter()
                .any(|c| c.provider_id == "mock" && c.model_id == "alcedo-mock")
        );
    }
}
