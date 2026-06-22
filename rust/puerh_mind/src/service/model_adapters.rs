use anyhow::{Result, bail};

use crate::service::model_assets::ModelProfileSpec;

pub(crate) const TEXT_SEQUENCE_LENGTH: usize = 77;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ImageResizeMode {
    ShortestEdgeCenterCrop,
    Squash,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum TextInputElementType {
    Int32,
    Int64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EngineProfileAdapter {
    MobileClipOpenClip,
    JinaClipV2OnnxInt8,
    Siglip2OpenClip,
    Siglip2CoreMlNative,
}

impl EngineProfileAdapter {
    pub(crate) fn from_profile(profile: &ModelProfileSpec) -> Result<Self> {
        match profile.engine_profile_id {
            "mobileclip2-openclip" => Ok(Self::MobileClipOpenClip),
            "jina-clip-v2-onnx-int8" => Ok(Self::JinaClipV2OnnxInt8),
            "siglip2-openclip" => Ok(Self::Siglip2OpenClip),
            "siglip2-coreml-native" => Ok(Self::Siglip2CoreMlNative),
            other => bail!(
                "semantic model profile {} requests unsupported engine adapter {other:?}",
                profile.profile_id
            ),
        }
    }

    pub(crate) fn supports_current_onnx_loader(self) -> bool {
        matches!(
            self,
            Self::MobileClipOpenClip | Self::JinaClipV2OnnxInt8 | Self::Siglip2OpenClip
        )
    }

    pub(crate) fn text_sequence_length(self) -> usize {
        match self {
            Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => 64,
            Self::MobileClipOpenClip | Self::JinaClipV2OnnxInt8 => TEXT_SEQUENCE_LENGTH,
        }
    }

    pub(crate) fn requires_attention_mask(self) -> bool {
        false
    }

    pub(crate) fn text_input_element_type(self) -> TextInputElementType {
        match self {
            Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => TextInputElementType::Int32,
            Self::MobileClipOpenClip | Self::JinaClipV2OnnxInt8 => TextInputElementType::Int64,
        }
    }

    pub(crate) fn image_mean_std(self) -> ([f32; 3], [f32; 3]) {
        match self {
            Self::MobileClipOpenClip => ([0.0, 0.0, 0.0], [1.0, 1.0, 1.0]),
            Self::JinaClipV2OnnxInt8 => (
                [0.48145466, 0.4578275, 0.40821073],
                [0.26862954, 0.26130258, 0.27577711],
            ),
            Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => ([0.5, 0.5, 0.5], [0.5, 0.5, 0.5]),
        }
    }

    pub(crate) fn image_resize_mode(self) -> ImageResizeMode {
        match self {
            Self::MobileClipOpenClip | Self::JinaClipV2OnnxInt8 => {
                ImageResizeMode::ShortestEdgeCenterCrop
            }
            Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => ImageResizeMode::Squash,
        }
    }

    pub(crate) fn uses_multimodal_session(self) -> bool {
        matches!(self, Self::JinaClipV2OnnxInt8)
    }

    pub(crate) fn preferred_text_output_name(self) -> &'static str {
        match self {
            Self::JinaClipV2OnnxInt8 => "l2norm_text_embeddings",
            Self::MobileClipOpenClip | Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => "",
        }
    }

    pub(crate) fn preferred_image_output_name(self) -> &'static str {
        match self {
            Self::JinaClipV2OnnxInt8 => "l2norm_image_embeddings",
            Self::MobileClipOpenClip | Self::Siglip2OpenClip | Self::Siglip2CoreMlNative => "",
        }
    }
}

pub(crate) fn l2_normalize(mut embedding: Vec<f32>) -> Result<Vec<f32>> {
    if embedding.iter().any(|value| !value.is_finite()) {
        bail!("embedding contains non-finite values");
    }

    let norm = embedding
        .iter()
        .map(|value| (*value as f64) * (*value as f64))
        .sum::<f64>()
        .sqrt();

    if norm == 0.0 {
        bail!("embedding norm is zero");
    }

    for value in &mut embedding {
        *value = (*value as f64 / norm) as f32;
    }
    Ok(embedding)
}

pub(crate) fn apply_embedding_transform(
    profile_id: &str,
    embedding_dim: usize,
    native_embedding_dim: usize,
    transform: &str,
    mut embedding: Vec<f32>,
) -> Result<Vec<f32>> {
    if embedding.len() != native_embedding_dim {
        bail!(
            "unexpected native embedding length {}, expected {} for profile {}",
            embedding.len(),
            native_embedding_dim,
            profile_id
        );
    }

    match transform {
        "l2_normalize" => {
            if embedding_dim != native_embedding_dim {
                bail!(
                    "profile {} uses l2_normalize but output dim {} differs from native dim {}",
                    profile_id,
                    embedding_dim,
                    native_embedding_dim
                );
            }
        }
        "matryoshka_truncate_then_l2_normalize" => {
            if embedding_dim > native_embedding_dim {
                bail!(
                    "profile {} cannot truncate native dim {} to larger output dim {}",
                    profile_id,
                    native_embedding_dim,
                    embedding_dim
                );
            }
            embedding.truncate(embedding_dim);
        }
        "identity" => {}
        other => bail!(
            "profile {} requests unsupported embedding transform {other:?}",
            profile_id
        ),
    }

    if embedding.len() != embedding_dim {
        bail!(
            "transformed embedding length {}, expected {} for profile {}",
            embedding.len(),
            embedding_dim,
            profile_id
        );
    }
    l2_normalize(embedding)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn siglip2_uses_int32_text_inputs() {
        assert_eq!(
            EngineProfileAdapter::Siglip2OpenClip.text_input_element_type(),
            TextInputElementType::Int32
        );
        assert_eq!(
            EngineProfileAdapter::Siglip2CoreMlNative.text_input_element_type(),
            TextInputElementType::Int32
        );
        assert_eq!(
            EngineProfileAdapter::MobileClipOpenClip.text_input_element_type(),
            TextInputElementType::Int64
        );
        assert_eq!(
            EngineProfileAdapter::JinaClipV2OnnxInt8.text_input_element_type(),
            TextInputElementType::Int64
        );
    }
}
