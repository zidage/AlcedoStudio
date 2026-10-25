//! A semantic engine that prepares the CLIP model profile on the first request that needs it.
//!
//! The sidecar also serves AI Mask segmentation, which uses no CLIP model. Preparing the
//! profile checks the SHA-256 of the model files, initializes the ORT environment, and parses
//! the tokenizer (about 2 s for SigLIP2). Before issue #518 this ran before the gRPC server
//! listened and delayed every sidecar start; now the first semantic request runs it.

use std::sync::{Arc, OnceLock};
use std::time::Instant;

use anyhow::Result;
use image::RgbImage;
use tracing::info;

use crate::config::{EncoderResidencyConfig, SemanticConfig};
use crate::service::embedding::{EmbeddingEngine, EngineModelInfo};
use crate::service::encoder_residency::EncoderStatus;
use crate::service::inference::{build_semantic_engine, profile_model_info};

/// `EngineModelInfo::provider` before the profile is prepared.
pub const NOT_PREPARED_PROVIDER: &str = "not prepared";

/// Makes the engine of the model profile; runs once, on the first request that needs it.
pub type SemanticEngineBuilder = Box<dyn Fn() -> Arc<dyn EmbeddingEngine> + Send + Sync>;

pub struct LazySemanticEngine {
    /// Model information of the configured profile from the built-in profile table.
    profile_info: EngineModelInfo,
    build: SemanticEngineBuilder,
    engine: OnceLock<Arc<dyn EmbeddingEngine>>,
}

impl LazySemanticEngine {
    /// An engine that prepares the profile of @p semantic with `build_semantic_engine`.
    pub fn new(semantic: &SemanticConfig, residency: &EncoderResidencyConfig) -> Self {
        let semantic_config = semantic.clone();
        let residency_config = residency.clone();
        Self::with_builder(
            profile_model_info(semantic, NOT_PREPARED_PROVIDER),
            Box::new(move || build_semantic_engine(&semantic_config, &residency_config)),
        )
    }

    pub fn with_builder(profile_info: EngineModelInfo, build: SemanticEngineBuilder) -> Self {
        Self {
            profile_info,
            build,
            engine: OnceLock::new(),
        }
    }

    /// The prepared engine. The first call prepares it on the calling thread; a concurrent
    /// call waits for that preparation. Call it only on a thread that may block.
    pub fn engine(&self) -> &Arc<dyn EmbeddingEngine> {
        self.engine.get_or_init(|| {
            let start = Instant::now();
            let engine = (self.build)();
            info!(
                "prepared semantic model profile {} on first use in {} ms",
                self.profile_info.profile_id,
                start.elapsed().as_millis()
            );
            engine
        })
    }

    fn prepared_engine(&self) -> Option<&Arc<dyn EmbeddingEngine>> {
        self.engine.get()
    }
}

impl EmbeddingEngine for LazySemanticEngine {
    fn is_prepared(&self) -> bool {
        self.prepared_engine().is_some()
    }

    fn is_ready(&self) -> bool {
        self.engine().is_ready()
    }

    fn unavailable_reason(&self) -> Option<&str> {
        self.engine().unavailable_reason()
    }

    fn embed_text(&self, text: &str) -> Result<Vec<f32>> {
        self.engine().embed_text(text)
    }

    fn embed_texts(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>> {
        self.engine().embed_texts(texts)
    }

    fn embed_image(&self, rgb: &RgbImage) -> Result<Vec<f32>> {
        self.engine().embed_image(rgb)
    }

    fn embed_images(&self, rgbs: &[RgbImage]) -> Result<Vec<Vec<f32>>> {
        self.engine().embed_images(rgbs)
    }

    fn default_text_model_name(&self) -> &str {
        match self.prepared_engine() {
            Some(engine) => engine.default_text_model_name(),
            None => &self.profile_info.model_id,
        }
    }

    fn default_image_model_name(&self) -> &str {
        match self.prepared_engine() {
            Some(engine) => engine.default_image_model_name(),
            None => &self.profile_info.model_id,
        }
    }

    fn model_info(&self) -> EngineModelInfo {
        match self.prepared_engine() {
            Some(engine) => engine.model_info(),
            None => self.profile_info.clone(),
        }
    }

    fn unload_idle_encoders(&self) {
        if let Some(engine) = self.prepared_engine() {
            engine.unload_idle_encoders();
        }
    }

    fn encoder_status(&self) -> Vec<EncoderStatus> {
        self.prepared_engine()
            .map(|engine| engine.encoder_status())
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::service::embedding::{MockEmbeddingEngine, UnavailableEmbeddingEngine};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::time::Duration;

    fn profile_info() -> EngineModelInfo {
        EngineModelInfo {
            provider: NOT_PREPARED_PROVIDER.to_string(),
            ..MockEmbeddingEngine.model_info()
        }
    }

    fn counting_engine(
        builds: Arc<AtomicUsize>,
        engine: impl Fn() -> Arc<dyn EmbeddingEngine> + Send + Sync + 'static,
    ) -> LazySemanticEngine {
        LazySemanticEngine::with_builder(
            profile_info(),
            Box::new(move || {
                builds.fetch_add(1, Ordering::SeqCst);
                engine()
            }),
        )
    }

    #[test]
    fn status_reads_do_not_prepare_the_profile() {
        let builds = Arc::new(AtomicUsize::new(0));
        let engine = counting_engine(builds.clone(), || Arc::new(MockEmbeddingEngine));

        assert!(!engine.is_prepared());
        assert_eq!(engine.model_info(), profile_info());
        assert_eq!(engine.default_text_model_name(), "mock-model-v1");
        assert_eq!(engine.default_image_model_name(), "mock-model-v1");
        assert!(engine.encoder_status().is_empty());
        engine.unload_idle_encoders();

        assert_eq!(builds.load(Ordering::SeqCst), 0);
        assert!(!engine.is_prepared());
    }

    #[test]
    fn first_embed_prepares_the_profile_once() {
        let builds = Arc::new(AtomicUsize::new(0));
        let engine = counting_engine(builds.clone(), || Arc::new(MockEmbeddingEngine));

        assert_eq!(engine.embed_text("abc").unwrap()[0], 3.0);
        assert!(engine.is_prepared());
        assert!(engine.is_ready());
        assert_eq!(engine.embed_texts(&["ab"]).unwrap()[0][0], 2.0);
        let image = RgbImage::new(4, 2);
        assert_eq!(engine.embed_image(&image).unwrap()[0], 4.0);
        assert_eq!(engine.embed_images(&[image]).unwrap().len(), 1);

        assert_eq!(builds.load(Ordering::SeqCst), 1);
        assert_eq!(engine.model_info(), MockEmbeddingEngine.model_info());
        assert_eq!(engine.default_text_model_name(), "mock-text-v1");
        assert_eq!(engine.default_image_model_name(), "mock-image-v1");
    }

    #[test]
    fn readiness_query_prepares_the_profile_and_reports_its_failure() {
        let builds = Arc::new(AtomicUsize::new(0));
        let engine = counting_engine(builds.clone(), || {
            Arc::new(UnavailableEmbeddingEngine::new(
                EngineModelInfo {
                    provider: "unavailable".to_string(),
                    ..profile_info()
                },
                "missing image file",
            ))
        });

        assert!(!engine.is_ready());
        assert_eq!(engine.unavailable_reason(), Some("missing image file"));
        assert_eq!(engine.model_info().provider, "unavailable");
        assert!(engine.embed_text("abc").is_err());
        assert_eq!(builds.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn concurrent_first_requests_share_one_preparation() {
        let builds = Arc::new(AtomicUsize::new(0));
        let engine = Arc::new(counting_engine(builds.clone(), || {
            std::thread::sleep(Duration::from_millis(50));
            Arc::new(MockEmbeddingEngine)
        }));

        let threads: Vec<_> = (0..4)
            .map(|_| {
                let engine = engine.clone();
                std::thread::spawn(move || engine.embed_text("abcd").unwrap()[0])
            })
            .collect();
        for thread in threads {
            assert_eq!(thread.join().unwrap(), 4.0);
        }

        assert_eq!(builds.load(Ordering::SeqCst), 1);
    }
}
