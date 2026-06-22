use std::{
    fs,
    fs::File,
    io,
    path::{Path, PathBuf},
    sync::Mutex,
};

use anyhow::{Context, Result, bail};
use apple_cf::cv::CVPixelBuffer;
use coreml::{
    BatchProvider, ComputeUnits, FeatureProvider, Model, ModelConfiguration, MultiArray,
    MultiArrayScalar,
};
use image::RgbImage;
use tokenizers::Tokenizer;
use tracing::info;
use zip::ZipArchive;

use crate::config::SemanticConfig;
use crate::service::embedding::{EmbeddingEngine, EngineModelInfo};
use crate::service::model_adapters::{EngineProfileAdapter, ImageResizeMode};
use crate::service::model_assets::{
    AssetRole, ModelProfileSpec, find_profile, profile_asset_path, validate_model_profile,
};

const COREML_ENGINE_PROFILE_ID: &str = "siglip2-coreml-native";
const COREML_CACHE_DIR: &str = ".coreml";
const IMAGE_INPUT_NAME: &str = "image";
const TEXT_INPUT_NAME: &str = "tokens";
const OUTPUT_NAME: &str = "embedding";
const BGRA_PIXEL_FORMAT: u32 = 0x4247_5241;

struct CoreMlModel {
    inner: Mutex<Model>,
}

// MLModel supports concurrent prediction, but we still serialize access through
// a Mutex because the Rust wrapper does not declare Send/Sync for the raw handle.
unsafe impl Send for CoreMlModel {}
unsafe impl Sync for CoreMlModel {}

pub struct CoreMlClipEngine {
    profile_id: String,
    model_id: String,
    revision: String,
    engine_profile_id: String,
    language: String,
    embedding_dim: usize,
    native_embedding_dim: usize,
    image_size: usize,
    embedding_transform: String,
    provider: String,
    model_root: PathBuf,
    tokenizer: Tokenizer,
    text_model: CoreMlModel,
    image_model: CoreMlModel,
    adapter: EngineProfileAdapter,
}

impl CoreMlClipEngine {
    pub fn new(config: &SemanticConfig) -> Result<Self> {
        let manifest = validate_model_profile(&config.model_id, &config.model_root)?;
        if manifest.revision != config.revision {
            bail!(
                "configured semantic model revision {} does not match resolved profile revision {} for {}",
                config.revision,
                manifest.revision,
                manifest.profile_id
            );
        }
        if manifest.engine_profile_id != COREML_ENGINE_PROFILE_ID {
            bail!(
                "semantic model profile {} uses engine {}, expected {}",
                manifest.profile_id,
                manifest.engine_profile_id,
                COREML_ENGINE_PROFILE_ID
            );
        }

        let profile = find_profile(&manifest.profile_id)?;
        let adapter = EngineProfileAdapter::from_profile(profile)?;
        if adapter != EngineProfileAdapter::Siglip2CoreMlNative {
            bail!(
                "semantic model profile {} is not a native CoreML SigLIP2 profile",
                profile.profile_id
            );
        }

        let model_root = PathBuf::from(&config.model_root);
        let cache_root = model_root.join(COREML_CACHE_DIR);
        fs::create_dir_all(&cache_root)
            .with_context(|| format!("failed to create {}", cache_root.display()))?;

        let image_package = ensure_unpacked_package(
            profile,
            &model_root,
            &cache_root,
            AssetRole::CoreMlVisionModel,
            "ImageEncoder.mlmodelc",
        )?;
        let text_package = ensure_unpacked_package(
            profile,
            &model_root,
            &cache_root,
            AssetRole::CoreMlTextModel,
            "TextEncoder.mlmodelc",
        )?;
        let tokenizer_json = ensure_unpacked_tokenizer(profile, &model_root, &cache_root)?;

        let tokenizer = Tokenizer::from_file(&tokenizer_json)
            .map_err(|e| anyhow::anyhow!("failed to load tokenizer: {e}"))?;
        let (configuration, provider) = model_configuration_for_device(&config.device)?;

        info!(
            "loading native CoreML clip profile {} from {} on {}",
            profile.profile_id,
            model_root.display(),
            provider
        );

        let image_model = load_compiled_model(&image_package, &configuration)?;
        let text_model = load_compiled_model(&text_package, &configuration)?;

        Ok(Self {
            profile_id: manifest.profile_id,
            model_id: manifest.model_id,
            revision: manifest.revision,
            engine_profile_id: manifest.engine_profile_id,
            language: manifest.language,
            embedding_dim: manifest.embedding_dimension as usize,
            native_embedding_dim: manifest.native_embedding_dimension as usize,
            image_size: manifest.image_size as usize,
            embedding_transform: manifest.embedding_transform,
            provider,
            model_root: PathBuf::from(manifest.model_root),
            tokenizer,
            text_model,
            image_model,
            adapter,
        })
    }

    fn encode_text_ids(&self, text: &str) -> Result<Vec<i32>> {
        if text.trim().is_empty() {
            bail!("text must not be empty");
        }

        let seq_len = self.adapter.text_sequence_length();
        let encoding = self
            .tokenizer
            .encode(text, true)
            .map_err(|e| anyhow::anyhow!("failed to tokenize text: {e}"))?;
        let mut ids = encoding
            .get_ids()
            .iter()
            .copied()
            .map(|id| {
                i32::try_from(id).map_err(|_| anyhow::anyhow!("token id {id} exceeds Int32 range"))
            })
            .collect::<Result<Vec<_>>>()?;
        ids.truncate(seq_len);
        ids.resize(seq_len, 0);
        Ok(ids)
    }

    fn forward_text_embedding(&self, text: &str) -> Result<Vec<f32>> {
        let mut embeddings = self.forward_text_embeddings(&[text])?;
        if embeddings.len() != 1 {
            bail!("expected one text embedding row, got {}", embeddings.len());
        }
        Ok(embeddings.remove(0))
    }

    fn forward_text_embeddings(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>> {
        if texts.is_empty() {
            bail!("text batch must not be empty");
        }

        let mut providers = Vec::with_capacity(texts.len());
        for text in texts {
            providers.push(self.build_text_inputs(text)?);
        }
        let batch = BatchProvider::from_feature_providers(providers);
        let outputs = self
            .text_model
            .inner
            .lock()
            .map_err(|err| anyhow::anyhow!("text CoreML model lock poisoned: {err}"))?
            .predict_batch(&batch)
            .map_err(|e| anyhow::anyhow!("failed to run text CoreML batch model: {e}"))?;
        self.extract_batch_embeddings(&outputs, texts.len(), "text")
    }

    fn build_text_inputs(&self, text: &str) -> Result<FeatureProvider> {
        let ids = self.encode_text_ids(text)?;
        let mut tokens = MultiArray::new_i32(&[1, ids.len()])
            .map_err(|e| anyhow::anyhow!("failed to allocate CoreML token tensor: {e}"))?;
        tokens
            .copy_from_i32_slice(&ids)
            .map_err(|e| anyhow::anyhow!("failed to fill CoreML token tensor: {e}"))?;

        let mut inputs = FeatureProvider::new();
        inputs
            .try_insert_multi_array(TEXT_INPUT_NAME, tokens)
            .map_err(|e| anyhow::anyhow!("failed to build CoreML text inputs: {e}"))?;
        Ok(inputs)
    }

    fn forward_image_embedding(&self, rgb: &RgbImage) -> Result<Vec<f32>> {
        let mut embeddings = self.forward_image_embeddings(std::slice::from_ref(rgb))?;
        if embeddings.len() != 1 {
            bail!("expected one image embedding row, got {}", embeddings.len());
        }
        Ok(embeddings.remove(0))
    }

    fn forward_image_embeddings(&self, rgbs: &[RgbImage]) -> Result<Vec<Vec<f32>>> {
        if rgbs.is_empty() {
            bail!("image batch must not be empty");
        }

        rgbs.iter()
            .map(|rgb| self.forward_single_image_embedding(rgb))
            .collect()
    }

    fn forward_single_image_embedding(&self, rgb: &RgbImage) -> Result<Vec<f32>> {
        let mut bgra = self.prepare_bgra_image(rgb)?;
        let pixel_buffer = unsafe {
            CVPixelBuffer::create_with_bytes(
                self.image_size,
                self.image_size,
                BGRA_PIXEL_FORMAT,
                bgra.as_mut_ptr().cast(),
                self.image_size * 4,
            )
        }
        .map_err(|status| anyhow::anyhow!("failed to create CoreML pixel buffer: {status}"))?;

        let mut inputs = FeatureProvider::new();
        inputs
            .try_insert_cv_pixel_buffer(IMAGE_INPUT_NAME, &pixel_buffer)
            .map_err(|e| anyhow::anyhow!("failed to build CoreML image inputs: {e}"))?;

        let outputs = self
            .image_model
            .inner
            .lock()
            .map_err(|err| anyhow::anyhow!("image CoreML model lock poisoned: {err}"))?
            .predict(&inputs)
            .map_err(|e| anyhow::anyhow!("failed to run image CoreML model: {e}"))?;
        self.extract_embedding(&outputs, "image")
    }

    fn prepare_bgra_image(&self, rgb: &RgbImage) -> Result<Vec<u8>> {
        let (src_w, src_h) = rgb.dimensions();
        if src_w == 0 || src_h == 0 {
            bail!("image must not be empty");
        }

        let target = self.image_size as u32;
        let prepared = match self.adapter.image_resize_mode() {
            ImageResizeMode::Squash => {
                image::imageops::resize(rgb, target, target, image::imageops::FilterType::Triangle)
            }
            ImageResizeMode::ShortestEdgeCenterCrop => {
                let scale = if src_w < src_h {
                    target as f32 / src_w as f32
                } else {
                    target as f32 / src_h as f32
                };
                let resized_w = ((src_w as f32) * scale).round().max(target as f32) as u32;
                let resized_h = ((src_h as f32) * scale).round().max(target as f32) as u32;
                let resized = image::imageops::resize(
                    rgb,
                    resized_w,
                    resized_h,
                    image::imageops::FilterType::Triangle,
                );
                let crop_x = (resized_w - target) / 2;
                let crop_y = (resized_h - target) / 2;
                image::imageops::crop_imm(&resized, crop_x, crop_y, target, target).to_image()
            }
        };

        let mut bgra = Vec::with_capacity(self.image_size * self.image_size * 4);
        for pixel in prepared.pixels() {
            bgra.push(pixel[2]);
            bgra.push(pixel[1]);
            bgra.push(pixel[0]);
            bgra.push(255);
        }
        Ok(bgra)
    }

    fn extract_embedding(&self, outputs: &FeatureProvider, modality: &str) -> Result<Vec<f32>> {
        let embedding = outputs.get_multi_array(OUTPUT_NAME).ok_or_else(|| {
            anyhow::anyhow!(
                "missing {modality} CoreML output {OUTPUT_NAME:?}; available outputs: {:?}",
                outputs.keys()
            )
        })?;
        let shape = embedding.shape();
        let mut values = Vec::with_capacity(self.native_embedding_dim);
        match shape.as_slice() {
            [dim] if *dim == self.native_embedding_dim => {
                for index in 0..self.native_embedding_dim {
                    values.push(read_multi_array_f32(&embedding, &[index]).ok_or_else(|| {
                        anyhow::anyhow!("failed to read {modality} embedding element {index}")
                    })?);
                }
            }
            [batch, dim] if *batch == 1 && *dim == self.native_embedding_dim => {
                for index in 0..self.native_embedding_dim {
                    values.push(read_multi_array_f32(&embedding, &[0, index]).ok_or_else(
                        || anyhow::anyhow!("failed to read {modality} embedding element {index}"),
                    )?);
                }
            }
            _ => bail!(
                "unexpected {modality} CoreML embedding shape {:?}, expected [{}] or [1, {}]",
                shape,
                self.native_embedding_dim,
                self.native_embedding_dim
            ),
        }

        crate::service::model_adapters::apply_embedding_transform(
            &self.profile_id,
            self.embedding_dim,
            self.native_embedding_dim,
            &self.embedding_transform,
            values,
        )
    }

    fn extract_batch_embeddings(
        &self,
        outputs: &BatchProvider,
        expected_len: usize,
        modality: &str,
    ) -> Result<Vec<Vec<f32>>> {
        if outputs.len() != expected_len {
            bail!(
                "unexpected {modality} CoreML batch output count {}, expected {}",
                outputs.len(),
                expected_len
            );
        }
        (0..expected_len)
            .map(|index| {
                let provider = outputs.get(index).ok_or_else(|| {
                    anyhow::anyhow!("missing {modality} CoreML batch output at index {index}")
                })?;
                self.extract_embedding(&provider, modality)
            })
            .collect()
    }
}

impl EmbeddingEngine for CoreMlClipEngine {
    fn embed_text(&self, text: &str) -> Result<Vec<f32>> {
        self.forward_text_embedding(text)
    }

    fn embed_texts(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>> {
        self.forward_text_embeddings(texts)
    }

    fn embed_image(&self, rgb: &RgbImage) -> Result<Vec<f32>> {
        self.forward_image_embedding(rgb)
    }

    fn embed_images(&self, rgbs: &[RgbImage]) -> Result<Vec<Vec<f32>>> {
        self.forward_image_embeddings(rgbs)
    }

    fn default_text_model_name(&self) -> &str {
        &self.model_id
    }

    fn default_image_model_name(&self) -> &str {
        &self.model_id
    }

    fn model_info(&self) -> EngineModelInfo {
        EngineModelInfo {
            profile_id: self.profile_id.clone(),
            model_id: self.model_id.clone(),
            revision: self.revision.clone(),
            engine_profile_id: self.engine_profile_id.clone(),
            language: self.language.clone(),
            embedding_dim: self.embedding_dim as u32,
            native_embedding_dim: self.native_embedding_dim as u32,
            image_size: self.image_size as u32,
            embedding_transform: self.embedding_transform.clone(),
            provider: self.provider.clone(),
            model_root: self.model_root.to_string_lossy().into_owned(),
            prototype_config_hash: String::new(),
        }
    }
}

fn read_multi_array_f32(array: &MultiArray, indices: &[usize]) -> Option<f32> {
    match array.number_at_indices(indices)? {
        MultiArrayScalar::Float32(value) => Some(value),
        MultiArrayScalar::Float64(value) => Some(value as f32),
        MultiArrayScalar::Float16(value) => Some(value.to_f32()),
        MultiArrayScalar::Int32(value) => Some(value as f32),
    }
}

fn model_configuration_for_device(device: &str) -> Result<(ModelConfiguration, String)> {
    let normalized = device.trim().to_ascii_lowercase();
    let units = match normalized.as_str() {
        "" | "auto" | "coreml" | "coreml:all" => ComputeUnits::All,
        "cpu" | "coreml:cpuonly" => ComputeUnits::CpuOnly,
        "coreml:cpuandgpu" => ComputeUnits::CpuAndGpu,
        "coreml:cpuandneuralengine" => ComputeUnits::CpuAndNeuralEngine,
        other => bail!(
            "device {other:?} is not supported by native CoreML inference; use auto, cpu, coreml:all, coreml:cpuandgpu, coreml:cpuandneuralengine, or coreml:cpuonly"
        ),
    };
    let provider = match units {
        ComputeUnits::All => "coreml-native:all",
        ComputeUnits::CpuOnly => "coreml-native:cpuonly",
        ComputeUnits::CpuAndGpu => "coreml-native:cpuandgpu",
        ComputeUnits::CpuAndNeuralEngine => "coreml-native:cpuandneuralengine",
    }
    .to_string();
    Ok((
        ModelConfiguration::new().with_compute_units(units),
        provider,
    ))
}

fn ensure_unpacked_package(
    profile: &ModelProfileSpec,
    model_root: &Path,
    cache_root: &Path,
    role: AssetRole,
    package_name: &str,
) -> Result<PathBuf> {
    let archive_path = profile_asset_path(profile, model_root, role)?;
    ensure_unpacked_archive(&archive_path, cache_root, package_name)
}

fn ensure_unpacked_tokenizer(
    profile: &ModelProfileSpec,
    model_root: &Path,
    cache_root: &Path,
) -> Result<PathBuf> {
    let archive_path = profile_asset_path(profile, model_root, AssetRole::TokenizerArchive)?;
    let tokenizer_dir = ensure_unpacked_archive(&archive_path, cache_root, "tokenizer")?;
    let tokenizer_json = tokenizer_dir.join("tokenizer.json");
    if !tokenizer_json.exists() {
        bail!("missing tokenizer.json in {}", tokenizer_dir.display());
    }
    Ok(tokenizer_json)
}

fn ensure_unpacked_archive(
    archive_path: &Path,
    cache_root: &Path,
    output_name: &str,
) -> Result<PathBuf> {
    let output_path = cache_root.join(output_name);
    let marker_path = cache_root.join(format!("{output_name}.unpacked"));
    if output_path.exists() && marker_path.exists() {
        return Ok(output_path);
    }

    let staging_path = cache_root.join(format!(".{output_name}.tmp"));
    if staging_path.exists() {
        fs::remove_dir_all(&staging_path)
            .with_context(|| format!("failed to remove {}", staging_path.display()))?;
    }
    fs::create_dir_all(&staging_path)
        .with_context(|| format!("failed to create {}", staging_path.display()))?;
    unzip_archive(archive_path, &staging_path)?;

    let unpacked_path = staging_path.join(output_name);
    if !unpacked_path.exists() {
        bail!(
            "archive {} did not contain {}",
            archive_path.display(),
            output_name
        );
    }
    if output_path.exists() {
        fs::remove_dir_all(&output_path)
            .with_context(|| format!("failed to remove {}", output_path.display()))?;
    }
    fs::rename(&unpacked_path, &output_path).with_context(|| {
        format!(
            "failed to move {} to {}",
            unpacked_path.display(),
            output_path.display()
        )
    })?;
    fs::remove_dir_all(&staging_path)
        .with_context(|| format!("failed to remove {}", staging_path.display()))?;
    fs::write(&marker_path, archive_path.display().to_string())
        .with_context(|| format!("failed to write {}", marker_path.display()))?;

    Ok(output_path)
}

fn unzip_archive(archive_path: &Path, destination: &Path) -> Result<()> {
    let file = File::open(archive_path)
        .with_context(|| format!("failed to open {}", archive_path.display()))?;
    let mut archive = ZipArchive::new(file)
        .with_context(|| format!("failed to read zip {}", archive_path.display()))?;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).with_context(|| {
            format!(
                "failed to read entry {index} from {}",
                archive_path.display()
            )
        })?;
        let enclosed = entry.enclosed_name().ok_or_else(|| {
            anyhow::anyhow!(
                "zip entry {:?} in {} escapes destination",
                entry.name(),
                archive_path.display()
            )
        })?;
        let output_path = destination.join(enclosed);
        if entry.is_dir() {
            fs::create_dir_all(&output_path)
                .with_context(|| format!("failed to create {}", output_path.display()))?;
        } else {
            if let Some(parent) = output_path.parent() {
                fs::create_dir_all(parent)
                    .with_context(|| format!("failed to create {}", parent.display()))?;
            }
            let mut output = File::create(&output_path)
                .with_context(|| format!("failed to create {}", output_path.display()))?;
            io::copy(&mut entry, &mut output)
                .with_context(|| format!("failed to extract {}", output_path.display()))?;
        }
    }
    Ok(())
}

fn load_compiled_model(
    compiled_path: &Path,
    configuration: &ModelConfiguration,
) -> Result<CoreMlModel> {
    let model = Model::load_from_url(compiled_path, configuration).map_err(|e| {
        anyhow::anyhow!(
            "failed to load precompiled CoreML model {}: {e}",
            compiled_path.display()
        )
    })?;
    Ok(CoreMlModel {
        inner: Mutex::new(model),
    })
}
