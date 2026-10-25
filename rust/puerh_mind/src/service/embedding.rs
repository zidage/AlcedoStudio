use anyhow::Result;

use crate::service::encoder_residency::EncoderStatus;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EngineModelInfo {
    pub profile_id: String,
    pub model_id: String,
    pub revision: String,
    pub engine_profile_id: String,
    pub language: String,
    pub embedding_dim: u32,
    pub native_embedding_dim: u32,
    pub image_size: u32,
    pub embedding_transform: String,
    pub provider: String,
    pub model_root: String,
    pub prototype_config_hash: String,
}

pub trait EmbeddingEngine: Send + Sync {
    /// False while the engine has not prepared its model profile yet. Such an engine
    /// (`LazySemanticEngine`) prepares it in `is_ready`, `unavailable_reason`, and the embed
    /// methods; the other methods return without preparing it.
    fn is_prepared(&self) -> bool {
        true
    }
    fn is_ready(&self) -> bool {
        true
    }
    fn unavailable_reason(&self) -> Option<&str> {
        None
    }
    fn embed_text(&self, text: &str) -> Result<Vec<f32>>;
    fn embed_texts(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>> {
        texts.iter().map(|text| self.embed_text(text)).collect()
    }
    fn embed_image(&self, rgb: &image::RgbImage) -> Result<Vec<f32>>;
    fn embed_images(&self, rgbs: &[image::RgbImage]) -> Result<Vec<Vec<f32>>> {
        rgbs.iter().map(|rgb| self.embed_image(rgb)).collect()
    }
    fn default_text_model_name(&self) -> &str;
    fn default_image_model_name(&self) -> &str;
    fn model_info(&self) -> EngineModelInfo;
    /// Drops each loaded encoder that is idle for longer than its idle time. Called every
    /// 5 seconds by the server; blocks while an encoder runs a request.
    fn unload_idle_encoders(&self) {}
    /// Residency of each encoder; empty for engines without lazily loaded encoders.
    fn encoder_status(&self) -> Vec<EncoderStatus> {
        Vec::new()
    }
}

#[allow(dead_code)]
pub struct UnavailableEmbeddingEngine {
    model_info: EngineModelInfo,
    reason: String,
}

impl UnavailableEmbeddingEngine {
    pub fn new(model_info: EngineModelInfo, reason: impl Into<String>) -> Self {
        Self {
            model_info,
            reason: reason.into(),
        }
    }
}

impl EmbeddingEngine for UnavailableEmbeddingEngine {
    fn is_ready(&self) -> bool {
        false
    }

    fn unavailable_reason(&self) -> Option<&str> {
        Some(&self.reason)
    }

    fn embed_text(&self, _text: &str) -> Result<Vec<f32>> {
        anyhow::bail!("semantic model is unavailable: {}", self.reason)
    }

    fn embed_image(&self, _rgb: &image::RgbImage) -> Result<Vec<f32>> {
        anyhow::bail!("semantic model is unavailable: {}", self.reason)
    }

    fn default_text_model_name(&self) -> &str {
        &self.model_info.model_id
    }

    fn default_image_model_name(&self) -> &str {
        &self.model_info.model_id
    }

    fn model_info(&self) -> EngineModelInfo {
        self.model_info.clone()
    }
}

#[allow(dead_code)]
pub struct MockEmbeddingEngine;

impl EmbeddingEngine for MockEmbeddingEngine {
    fn embed_text(&self, text: &str) -> Result<Vec<f32>> {
        let len = text.len() as f32;

        Ok(vec![
            len,
            len + 1.0,
            len + 2.0,
            len + 3.0,
            len + 4.0,
            len + 5.0,
            len + 6.0,
            len + 7.0,
        ])
    }

    fn embed_image(&self, rgb: &image::RgbImage) -> Result<Vec<f32>> {
        let width = rgb.width() as f32;
        let height = rgb.height() as f32;

        Ok(vec![
            width,
            height,
            width / height.max(1.0),
            width * height,
            1.0,
            2.0,
            3.0,
            4.0,
        ])
    }

    fn embed_images(&self, rgbs: &[image::RgbImage]) -> Result<Vec<Vec<f32>>> {
        rgbs.iter().map(|rgb| self.embed_image(rgb)).collect()
    }

    fn default_text_model_name(&self) -> &'static str {
        "mock-text-v1"
    }

    fn default_image_model_name(&self) -> &'static str {
        "mock-image-v1"
    }

    fn model_info(&self) -> EngineModelInfo {
        EngineModelInfo {
            profile_id: "mock-profile-v1".to_string(),
            model_id: "mock-model-v1".to_string(),
            revision: "mock-revision".to_string(),
            engine_profile_id: "mock-engine".to_string(),
            language: "en".to_string(),
            embedding_dim: 8,
            native_embedding_dim: 8,
            image_size: 256,
            embedding_transform: "l2_normalize".to_string(),
            provider: "mock".to_string(),
            model_root: String::new(),
            prototype_config_hash: String::new(),
        }
    }
}
