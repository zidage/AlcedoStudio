use std::sync::Arc;
use std::time::Instant;

use tokio::sync::{mpsc, oneshot};
use tokio::time::{Duration, Instant as TokioInstant};
use tracing::info;

use tonic::{Request, Response, Status};

use crate::proto::semantic::{
    EmbedImageBatchRequest, EmbedImageRequest, EmbedTextBatchRequest, EmbedTextRequest,
    EmbeddingBatchItem, EmbeddingBatchResponse, EmbeddingResponse, GetModelInfoRequest,
    GetModelInfoResponse, GetRuntimeStatusRequest, GetRuntimeStatusResponse, PingRequest,
    PingResponse, semantic_service_server::SemanticService,
};
use crate::service::embedding::EmbeddingEngine;

const IMAGE_BATCH_QUEUE_CAPACITY: usize = 256;

struct PendingImageRequest {
    request_id: String,
    model_name: String,
    rgb: image::RgbImage,
    started_at: Instant,
    response_tx: oneshot::Sender<Result<EmbeddingResponse, Status>>,
}

pub struct SemanticServiceImpl {
    engine: Arc<dyn EmbeddingEngine>,
    image_batch_tx: mpsc::Sender<PendingImageRequest>,
    image_batch_cap: usize,
    image_batch_wait: Duration,
    started_at: Instant,
}

impl SemanticServiceImpl {
    pub fn new(
        engine: Arc<dyn EmbeddingEngine>,
        image_batch_cap: usize,
        image_batch_wait: Duration,
    ) -> Self {
        let image_batch_tx =
            Self::spawn_image_batch_worker(engine.clone(), image_batch_cap, image_batch_wait);

        Self {
            engine,
            image_batch_tx,
            image_batch_cap,
            image_batch_wait,
            started_at: Instant::now(),
        }
    }

    fn validate_text_request(&self, req: &EmbedTextRequest) -> Result<(), Status> {
        if req.text.trim().is_empty() {
            return Err(Status::invalid_argument("text must not be empty"));
        }
        Ok(())
    }

    fn decode_rgb8_image(
        &self,
        image_bytes: &[u8],
        image_format_hint: &str,
    ) -> Result<image::RgbImage, Status> {
        if image_bytes.is_empty() {
            return Err(Status::invalid_argument("image_bytes must not be empty"));
        }

        let dimensions = image_format_hint
            .strip_prefix("rgba8:")
            .ok_or_else(|| Status::invalid_argument("image_format_hint must be rgba8:WxH"))?;
        let (width_text, height_text) = dimensions
            .split_once('x')
            .ok_or_else(|| Status::invalid_argument("image_format_hint must be rgba8:WxH"))?;
        let width = width_text
            .parse::<u32>()
            .map_err(|_| Status::invalid_argument("rgba8 width must be a positive integer"))?;
        let height = height_text
            .parse::<u32>()
            .map_err(|_| Status::invalid_argument("rgba8 height must be a positive integer"))?;
        if width == 0 || height == 0 {
            return Err(Status::invalid_argument(
                "rgba8 width and height must be positive",
            ));
        }

        let expected_len = width as usize * height as usize * 4;
        if image_bytes.len() != expected_len {
            return Err(Status::invalid_argument(format!(
                "rgba8 byte length mismatch: expected {expected_len}, got {}",
                image_bytes.len()
            )));
        }

        let mut rgb = image::RgbImage::new(width, height);
        for (src, dst) in image_bytes.chunks_exact(4).zip(rgb.pixels_mut()) {
            *dst = image::Rgb([src[0], src[1], src[2]]);
        }
        Ok(rgb)
    }

    fn spawn_image_batch_worker(
        engine: Arc<dyn EmbeddingEngine>,
        image_batch_cap: usize,
        image_batch_wait: Duration,
    ) -> mpsc::Sender<PendingImageRequest> {
        let (tx, mut rx) = mpsc::channel::<PendingImageRequest>(IMAGE_BATCH_QUEUE_CAPACITY);

        tokio::spawn(async move {
            while let Some(first) = rx.recv().await {
                let mut batch = vec![first];
                let deadline = TokioInstant::now() + image_batch_wait;

                while batch.len() < image_batch_cap {
                    let now = TokioInstant::now();
                    if now >= deadline {
                        break;
                    }

                    let remaining = deadline.saturating_duration_since(now);
                    match tokio::time::timeout(remaining, rx.recv()).await {
                        Ok(Some(next)) => batch.push(next),
                        Ok(None) | Err(_) => break,
                    }
                }

                let engine = engine.clone();
                tokio::task::spawn_blocking(move || Self::process_image_batch(engine, batch));
            }
        });

        tx
    }

    fn validate_embedding(embedding: &[f32]) -> Result<(), Status> {
        if embedding.is_empty() {
            return Err(Status::internal("embedding must not be empty"));
        }
        if embedding.iter().any(|value| !value.is_finite()) {
            return Err(Status::internal("embedding contains non-finite values"));
        }
        let norm_sq = embedding
            .iter()
            .map(|value| (*value as f64) * (*value as f64))
            .sum::<f64>();
        if norm_sq == 0.0 {
            return Err(Status::internal("embedding norm is zero"));
        }
        Ok(())
    }

    fn embedding_batch_item_ok(
        request_id: String,
        model_name: String,
        embedding: Vec<f32>,
        elapsed_ms: u64,
    ) -> EmbeddingBatchItem {
        EmbeddingBatchItem {
            request_id,
            dimension: embedding.len() as u32,
            embedding,
            model_name,
            elapsed_ms,
            ok: true,
            error: String::new(),
        }
    }

    fn embedding_batch_item_err(
        request_id: String,
        model_name: String,
        error: impl Into<String>,
        elapsed_ms: u64,
    ) -> EmbeddingBatchItem {
        EmbeddingBatchItem {
            request_id,
            embedding: Vec::new(),
            dimension: 0,
            model_name,
            elapsed_ms,
            ok: false,
            error: error.into(),
        }
    }

    fn process_image_batch(engine: Arc<dyn EmbeddingEngine>, batch: Vec<PendingImageRequest>) {
        let batch_len = batch.len();
        let mut requests = Vec::with_capacity(batch_len);
        let mut images = Vec::with_capacity(batch_len);

        for PendingImageRequest {
            request_id,
            model_name,
            rgb,
            started_at,
            response_tx,
        } in batch
        {
            requests.push((request_id, model_name, started_at, response_tx));
            images.push(rgb);
        }

        info!("[SemanticService]: processing image batch size={batch_len}");

        match engine.embed_images(&images) {
            Ok(embeddings) => {
                if embeddings.len() != batch_len {
                    let status = Status::internal(format!(
                        "batched image embedding count mismatch: expected {batch_len}, got {}",
                        embeddings.len()
                    ));
                    for (_, _, _, response_tx) in requests {
                        let _ = response_tx.send(Err(status.clone()));
                    }
                    return;
                }

                for ((request_id, model_name, started_at, response_tx), embedding) in
                    requests.into_iter().zip(embeddings)
                {
                    let dimension = embedding.len() as u32;
                    if let Err(status) = Self::validate_embedding(&embedding) {
                        let _ = response_tx.send(Err(status));
                        continue;
                    }
                    let response = EmbeddingResponse {
                        request_id,
                        embedding,
                        dimension,
                        model_name: if model_name.is_empty() {
                            engine.default_image_model_name().to_string()
                        } else {
                            model_name
                        },
                        elapsed_ms: started_at.elapsed().as_millis() as u64,
                    };

                    let _ = response_tx.send(Ok(response));
                }
            }
            Err(err) => {
                let status = Status::internal(format!("failed to embed image batch: {err}"));
                for (_, _, _, response_tx) in requests {
                    let _ = response_tx.send(Err(status.clone()));
                }
            }
        }
    }
}

#[tonic::async_trait]
impl SemanticService for SemanticServiceImpl {
    async fn ping(&self, request: Request<PingRequest>) -> Result<Response<PingResponse>, Status> {
        info!("[SemanticService]: received Ping request");

        let start = std::time::Instant::now();

        let inner = request.into_inner();
        let request_id = inner.request_id;

        let response = PingResponse {
            request_id,
            message: "pong".to_string(),
            elapsed_ms: start.elapsed().as_millis() as u64,
        };

        Ok(Response::new(response))
    }

    async fn embed_text(
        &self,
        request: Request<EmbedTextRequest>,
    ) -> Result<Response<EmbeddingResponse>, Status> {
        info!("[SemanticService]: received EmbedText request");
        let start = Instant::now();
        let req = request.into_inner();

        self.validate_text_request(&req)?;

        let engine = self.engine.clone();
        let text = req.text;
        let embedding = tokio::task::spawn_blocking(move || engine.embed_text(&text))
            .await
            .map_err(|err| Status::internal(format!("text embedding task failed: {err}")))?
            .map_err(|e| Status::internal(format!("failed to embed text: {e}")))?;
        Self::validate_embedding(&embedding)?;
        let dimension = embedding.len() as u32;

        let response = EmbeddingResponse {
            request_id: req.request_id,
            embedding,
            dimension,
            model_name: if req.model_name.is_empty() {
                self.engine.default_text_model_name().to_string()
            } else {
                req.model_name
            },
            elapsed_ms: start.elapsed().as_millis() as u64,
        };

        Ok(Response::new(response))
    }

    async fn embed_text_batch(
        &self,
        request: Request<EmbedTextBatchRequest>,
    ) -> Result<Response<EmbeddingBatchResponse>, Status> {
        info!("[SemanticService]: received EmbedTextBatch request");
        let batch_start = Instant::now();
        let req = request.into_inner();
        if req.items.is_empty() {
            return Err(Status::invalid_argument("text batch must not be empty"));
        }

        let mut items = Vec::with_capacity(req.items.len());
        let mut valid_requests = Vec::new();
        let mut valid_texts = Vec::new();

        for item in req.items {
            let item_start = Instant::now();
            let model_name = if item.model_name.is_empty() {
                self.engine.default_text_model_name().to_string()
            } else {
                item.model_name.clone()
            };
            if let Err(status) = self.validate_text_request(&item) {
                items.push(Self::embedding_batch_item_err(
                    item.request_id,
                    model_name,
                    status.message().to_string(),
                    item_start.elapsed().as_millis() as u64,
                ));
                continue;
            }

            valid_texts.push(item.text);
            valid_requests.push((items.len(), item.request_id, model_name, item_start));
            items.push(EmbeddingBatchItem::default());
        }

        if !valid_texts.is_empty() {
            let engine = self.engine.clone();
            let embedding_result = tokio::task::spawn_blocking(move || {
                let text_refs = valid_texts.iter().map(String::as_str).collect::<Vec<_>>();
                engine.embed_texts(&text_refs)
            })
            .await
            .map_err(|err| Status::internal(format!("text batch embedding task failed: {err}")))?;

            match embedding_result {
                Ok(embeddings) if embeddings.len() == valid_requests.len() => {
                    for ((slot, request_id, model_name, item_start), embedding) in
                        valid_requests.into_iter().zip(embeddings)
                    {
                        items[slot] = match Self::validate_embedding(&embedding) {
                            Ok(()) => Self::embedding_batch_item_ok(
                                request_id,
                                model_name,
                                embedding,
                                item_start.elapsed().as_millis() as u64,
                            ),
                            Err(status) => Self::embedding_batch_item_err(
                                request_id,
                                model_name,
                                status.message().to_string(),
                                item_start.elapsed().as_millis() as u64,
                            ),
                        };
                    }
                }
                Ok(embeddings) => {
                    let error = format!(
                        "text embedding count mismatch: expected {}, got {}",
                        valid_requests.len(),
                        embeddings.len()
                    );
                    for (slot, request_id, model_name, item_start) in valid_requests {
                        items[slot] = Self::embedding_batch_item_err(
                            request_id,
                            model_name,
                            error.clone(),
                            item_start.elapsed().as_millis() as u64,
                        );
                    }
                }
                Err(err) => {
                    let error = format!("failed to embed text batch: {err}");
                    for (slot, request_id, model_name, item_start) in valid_requests {
                        items[slot] = Self::embedding_batch_item_err(
                            request_id,
                            model_name,
                            error.clone(),
                            item_start.elapsed().as_millis() as u64,
                        );
                    }
                }
            }
        }

        Ok(Response::new(EmbeddingBatchResponse {
            items,
            elapsed_ms: batch_start.elapsed().as_millis() as u64,
        }))
    }

    async fn embed_image(
        &self,
        request: Request<EmbedImageRequest>,
    ) -> Result<Response<EmbeddingResponse>, Status> {
        info!("[SemanticService]: received EmbedImg request");
        let start = Instant::now();
        let req = request.into_inner();

        let rgb = self.decode_rgb8_image(&req.image_bytes, &req.image_format_hint)?;
        let (response_tx, response_rx) = oneshot::channel();
        let pending = PendingImageRequest {
            request_id: req.request_id,
            model_name: req.model_name,
            rgb,
            started_at: start,
            response_tx,
        };

        self.image_batch_tx
            .send(pending)
            .await
            .map_err(|_| Status::unavailable("image batch worker unavailable"))?;

        let response = response_rx
            .await
            .map_err(|_| Status::internal("image batch worker dropped response"))??;

        Ok(Response::new(response))
    }

    async fn embed_image_batch(
        &self,
        request: Request<EmbedImageBatchRequest>,
    ) -> Result<Response<EmbeddingBatchResponse>, Status> {
        info!("[SemanticService]: received EmbedImageBatch request");
        let batch_start = Instant::now();
        let req = request.into_inner();
        if req.items.is_empty() {
            return Err(Status::invalid_argument("image batch must not be empty"));
        }

        let mut items = Vec::with_capacity(req.items.len());
        let mut valid_requests = Vec::new();
        let mut images = Vec::new();

        for item in req.items {
            let item_start = Instant::now();
            let model_name = if item.model_name.is_empty() {
                self.engine.default_image_model_name().to_string()
            } else {
                item.model_name.clone()
            };

            match self.decode_rgb8_image(&item.image_bytes, &item.image_format_hint) {
                Ok(rgb) => {
                    images.push(rgb);
                    valid_requests.push((items.len(), item.request_id, model_name, item_start));
                    items.push(EmbeddingBatchItem::default());
                }
                Err(status) => items.push(Self::embedding_batch_item_err(
                    item.request_id,
                    model_name,
                    status.message().to_string(),
                    item_start.elapsed().as_millis() as u64,
                )),
            }
        }

        if !images.is_empty() {
            let engine = self.engine.clone();
            let embedding_result =
                tokio::task::spawn_blocking(move || engine.embed_images(&images))
                    .await
                    .map_err(|err| {
                        Status::internal(format!("image batch embedding task failed: {err}"))
                    })?;

            match embedding_result {
                Ok(embeddings) if embeddings.len() == valid_requests.len() => {
                    for ((slot, request_id, model_name, item_start), embedding) in
                        valid_requests.into_iter().zip(embeddings)
                    {
                        items[slot] = match Self::validate_embedding(&embedding) {
                            Ok(()) => Self::embedding_batch_item_ok(
                                request_id,
                                model_name,
                                embedding,
                                item_start.elapsed().as_millis() as u64,
                            ),
                            Err(status) => Self::embedding_batch_item_err(
                                request_id,
                                model_name,
                                status.message().to_string(),
                                item_start.elapsed().as_millis() as u64,
                            ),
                        };
                    }
                }
                Ok(embeddings) => {
                    let error = format!(
                        "image embedding count mismatch: expected {}, got {}",
                        valid_requests.len(),
                        embeddings.len()
                    );
                    for (slot, request_id, model_name, item_start) in valid_requests {
                        items[slot] = Self::embedding_batch_item_err(
                            request_id,
                            model_name,
                            error.clone(),
                            item_start.elapsed().as_millis() as u64,
                        );
                    }
                }
                Err(err) => {
                    let error = format!("failed to embed image batch: {err}");
                    for (slot, request_id, model_name, item_start) in valid_requests {
                        items[slot] = Self::embedding_batch_item_err(
                            request_id,
                            model_name,
                            error.clone(),
                            item_start.elapsed().as_millis() as u64,
                        );
                    }
                }
            }
        }

        Ok(Response::new(EmbeddingBatchResponse {
            items,
            elapsed_ms: batch_start.elapsed().as_millis() as u64,
        }))
    }

    async fn get_model_info(
        &self,
        _request: Request<GetModelInfoRequest>,
    ) -> Result<Response<GetModelInfoResponse>, Status> {
        if !self.engine.is_ready() {
            return Err(Status::failed_precondition(
                self.engine
                    .unavailable_reason()
                    .unwrap_or("semantic model is unavailable")
                    .to_string(),
            ));
        }
        let info = self.engine.model_info();
        Ok(Response::new(GetModelInfoResponse {
            model_id: info.model_id,
            revision: info.revision,
            embedding_dimension: info.embedding_dim,
            image_size: info.image_size,
            provider: info.provider,
            model_root: info.model_root,
            prototype_config_hash: info.prototype_config_hash,
            profile_id: info.profile_id,
            engine_profile_id: info.engine_profile_id,
            language: info.language,
            native_embedding_dimension: info.native_embedding_dim,
            embedding_transform: info.embedding_transform,
        }))
    }

    async fn get_runtime_status(
        &self,
        _request: Request<GetRuntimeStatusRequest>,
    ) -> Result<Response<GetRuntimeStatusResponse>, Status> {
        let info = self.engine.model_info();
        Ok(Response::new(GetRuntimeStatusResponse {
            state: if self.engine.is_ready() {
                "ready".to_string()
            } else {
                "model_unavailable".to_string()
            },
            provider: info.provider,
            image_batch_cap: self.image_batch_cap as u32,
            image_batch_wait_ms: self.image_batch_wait.as_millis() as u32,
            uptime_ms: self.started_at.elapsed().as_millis() as u64,
        }))
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use anyhow::Result as AnyResult;
    use tonic::Request;

    use super::*;
    use crate::config::SemanticConfig;
    use crate::service::embedding::{EmbeddingEngine, EngineModelInfo, MockEmbeddingEngine};
    use crate::service::ort_clip::OrtClipEngine;

    fn test_model_root() -> String {
        std::env::var("ALCEDO_MIND_TEST_MODEL_ROOT").unwrap_or_else(|_| {
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("models")
                .join("mobileclip2-s2-openclip")
                .to_string_lossy()
                .into_owned()
        })
    }

    fn test_allow_download() -> bool {
        std::env::var("ALCEDO_MIND_TEST_ALLOW_DOWNLOAD")
            .ok()
            .is_some_and(|value| {
                matches!(
                    value.to_ascii_lowercase().as_str(),
                    "1" | "true" | "yes" | "on"
                )
            })
    }

    fn has_test_model_assets() -> bool {
        crate::service::model_assets::ClipModelPaths::from_root(test_model_root())
            .validate()
            .is_ok()
    }

    fn test_semantic_config() -> SemanticConfig {
        SemanticConfig {
            model_id: "plhery/mobileclip2-onnx:s2".to_string(),
            revision: crate::service::model_assets::MOBILECLIP2_ONNX_REVISION.to_string(),
            model_root: test_model_root(),
            hf_endpoint: "https://hf-mirror.com".to_string(),
            device: "cpu".to_string(),
            allow_download: test_allow_download(),
            batch_cap: 512,
            batch_wait_ms: 25,
        }
    }

    fn raw_rgba8_image(width: u32, height: u32, rgba: [u8; 4]) -> (Vec<u8>, String) {
        let mut bytes = Vec::with_capacity(width as usize * height as usize * 4);
        for _ in 0..(width * height) {
            bytes.extend_from_slice(&rgba);
        }
        (bytes, format!("rgba8:{width}x{height}"))
    }

    #[test]
    fn uses_configured_model_id_as_default_name() {
        let config = SemanticConfig {
            model_id: "plhery/mobileclip2-onnx:s2".to_string(),
            revision: crate::service::model_assets::MOBILECLIP2_ONNX_REVISION.to_string(),
            model_root: "./models/mobileclip2-s2-openclip".to_string(),
            hf_endpoint: "https://hf-mirror.com".to_string(),
            device: "cpu".to_string(),
            allow_download: false,
            batch_cap: 512,
            batch_wait_ms: 25,
        };

        struct NamedEngine {
            config: SemanticConfig,
        }

        impl EmbeddingEngine for NamedEngine {
            fn embed_text(&self, text: &str) -> AnyResult<Vec<f32>> {
                MockEmbeddingEngine.embed_text(text)
            }

            fn embed_image(&self, rgb: &image::RgbImage) -> AnyResult<Vec<f32>> {
                MockEmbeddingEngine.embed_image(rgb)
            }

            fn default_text_model_name(&self) -> &str {
                &self.config.model_id
            }

            fn default_image_model_name(&self) -> &str {
                &self.config.model_id
            }

            fn model_info(&self) -> EngineModelInfo {
                EngineModelInfo {
                    model_id: self.config.model_id.clone(),
                    revision: self.config.revision.clone(),
                    model_root: self.config.model_root.clone(),
                    ..MockEmbeddingEngine.model_info()
                }
            }
        }

        let engine = NamedEngine { config };
        assert_eq!(
            engine.default_text_model_name(),
            "plhery/mobileclip2-onnx:s2"
        );
        assert_eq!(
            engine.default_image_model_name(),
            "plhery/mobileclip2-onnx:s2"
        );
    }

    #[tokio::test]
    async fn embeds_text_request_with_ort_engine() {
        if !test_allow_download() && !has_test_model_assets() {
            eprintln!(
                "skipping ORT service test; set ALCEDO_MIND_TEST_MODEL_ROOT or ALCEDO_MIND_TEST_ALLOW_DOWNLOAD=1"
            );
            return;
        }

        let config = test_semantic_config();
        let engine = Arc::new(OrtClipEngine::new(&config).expect("engine should load"));
        let service = SemanticServiceImpl::new(engine, 512, Duration::from_millis(25));

        let request = Request::new(EmbedTextRequest {
            request_id: "test-1".to_string(),
            text: "a red tea cake".to_string(),
            model_name: String::new(),
        });

        let response = service
            .embed_text(request)
            .await
            .expect("embed text should succeed")
            .into_inner();

        assert_eq!(response.request_id, "test-1");
        assert_eq!(response.dimension as usize, response.embedding.len());
        assert!(!response.embedding.is_empty());
        assert_eq!(response.model_name, "plhery/mobileclip2-onnx:s2");
        assert!(response.elapsed_ms <= u64::MAX);
    }

    #[tokio::test]
    async fn batches_image_requests_up_to_configured_batch_size_and_preserves_request_ids() {
        struct RecordingEngine {
            batches: Mutex<Vec<usize>>,
        }

        impl EmbeddingEngine for RecordingEngine {
            fn embed_text(&self, text: &str) -> AnyResult<Vec<f32>> {
                MockEmbeddingEngine.embed_text(text)
            }

            fn embed_image(&self, rgb: &image::RgbImage) -> AnyResult<Vec<f32>> {
                MockEmbeddingEngine.embed_image(rgb)
            }

            fn embed_images(&self, rgbs: &[image::RgbImage]) -> AnyResult<Vec<Vec<f32>>> {
                self.batches.lock().unwrap().push(rgbs.len());
                MockEmbeddingEngine.embed_images(rgbs)
            }

            fn default_text_model_name(&self) -> &str {
                "mock-text-v1"
            }

            fn default_image_model_name(&self) -> &str {
                "mock-image-v1"
            }

            fn model_info(&self) -> EngineModelInfo {
                MockEmbeddingEngine.model_info()
            }
        }

        let engine = Arc::new(RecordingEngine {
            batches: Mutex::new(Vec::new()),
        });
        let service = Arc::new(SemanticServiceImpl::new(
            engine.clone(),
            512,
            Duration::from_millis(25),
        ));
        let (rgba8, format_hint) = raw_rgba8_image(16, 12, [64, 128, 192, 255]);

        let mut tasks = Vec::new();
        const TEST_REQUEST_COUNT: usize = 64;

        for index in 0..TEST_REQUEST_COUNT {
            let service = service.clone();
            let rgba8 = rgba8.clone();
            let format_hint = format_hint.clone();
            tasks.push(tokio::spawn(async move {
                let request_id = format!("img-{index}");
                let response = service
                    .embed_image(Request::new(EmbedImageRequest {
                        request_id: request_id.clone(),
                        image_bytes: rgba8,
                        image_format_hint: format_hint,
                        model_name: String::new(),
                    }))
                    .await
                    .expect("embed image should succeed")
                    .into_inner();

                (request_id, response)
            }));
        }

        for task in tasks {
            let (request_id, response) = task.await.expect("task should join");
            assert_eq!(response.request_id, request_id);
            assert_eq!(response.dimension as usize, response.embedding.len());
            assert_eq!(response.model_name, "mock-image-v1");
        }

        assert_eq!(
            engine.batches.lock().unwrap().as_slice(),
            &[TEST_REQUEST_COUNT]
        );
    }

    #[tokio::test]
    async fn text_batch_preserves_request_order_and_item_errors() {
        let service = SemanticServiceImpl::new(
            Arc::new(MockEmbeddingEngine),
            512,
            Duration::from_millis(25),
        );

        let response = service
            .embed_text_batch(Request::new(EmbedTextBatchRequest {
                items: vec![
                    EmbedTextRequest {
                        request_id: "ok-1".to_string(),
                        text: "tea".to_string(),
                        model_name: String::new(),
                    },
                    EmbedTextRequest {
                        request_id: "bad-1".to_string(),
                        text: "   ".to_string(),
                        model_name: String::new(),
                    },
                    EmbedTextRequest {
                        request_id: "ok-2".to_string(),
                        text: "cake".to_string(),
                        model_name: "custom".to_string(),
                    },
                ],
            }))
            .await
            .expect("batch should return")
            .into_inner();

        assert_eq!(response.items.len(), 3);
        assert_eq!(response.items[0].request_id, "ok-1");
        assert!(response.items[0].ok);
        assert_eq!(response.items[1].request_id, "bad-1");
        assert!(!response.items[1].ok);
        assert!(response.items[1].error.contains("text must not be empty"));
        assert_eq!(response.items[2].request_id, "ok-2");
        assert!(response.items[2].ok);
        assert_eq!(response.items[2].model_name, "custom");
    }

    #[tokio::test]
    async fn image_batch_preserves_request_order_and_item_errors() {
        let service = SemanticServiceImpl::new(
            Arc::new(MockEmbeddingEngine),
            512,
            Duration::from_millis(25),
        );
        let (rgba8, format_hint) = raw_rgba8_image(16, 12, [64, 128, 192, 255]);

        let response = service
            .embed_image_batch(Request::new(EmbedImageBatchRequest {
                items: vec![
                    EmbedImageRequest {
                        request_id: "img-1".to_string(),
                        image_bytes: rgba8,
                        image_format_hint: format_hint,
                        model_name: String::new(),
                    },
                    EmbedImageRequest {
                        request_id: "img-bad".to_string(),
                        image_bytes: b"not rgba8".to_vec(),
                        image_format_hint: "rgba8:16x12".to_string(),
                        model_name: String::new(),
                    },
                ],
            }))
            .await
            .expect("batch should return")
            .into_inner();

        assert_eq!(response.items.len(), 2);
        assert_eq!(response.items[0].request_id, "img-1");
        assert!(response.items[0].ok);
        assert_eq!(response.items[1].request_id, "img-bad");
        assert!(!response.items[1].ok);
        assert!(
            response.items[1]
                .error
                .contains("rgba8 byte length mismatch")
        );
    }

    #[tokio::test]
    async fn rejects_non_finite_batch_embedding_as_item_error() {
        struct NonFiniteEngine;

        impl EmbeddingEngine for NonFiniteEngine {
            fn embed_text(&self, _text: &str) -> AnyResult<Vec<f32>> {
                Ok(vec![1.0, f32::NAN])
            }

            fn embed_image(&self, rgb: &image::RgbImage) -> AnyResult<Vec<f32>> {
                MockEmbeddingEngine.embed_image(rgb)
            }

            fn default_text_model_name(&self) -> &str {
                "non-finite"
            }

            fn default_image_model_name(&self) -> &str {
                "mock-image-v1"
            }

            fn model_info(&self) -> EngineModelInfo {
                MockEmbeddingEngine.model_info()
            }
        }

        let service =
            SemanticServiceImpl::new(Arc::new(NonFiniteEngine), 512, Duration::from_millis(25));
        let response = service
            .embed_text_batch(Request::new(EmbedTextBatchRequest {
                items: vec![EmbedTextRequest {
                    request_id: "bad-embedding".to_string(),
                    text: "tea".to_string(),
                    model_name: String::new(),
                }],
            }))
            .await
            .expect("batch should return")
            .into_inner();

        assert_eq!(response.items.len(), 1);
        assert_eq!(response.items[0].request_id, "bad-embedding");
        assert!(!response.items[0].ok);
        assert!(response.items[0].error.contains("non-finite"));
    }

    #[tokio::test]
    async fn reports_model_info_and_runtime_status() {
        let service =
            SemanticServiceImpl::new(Arc::new(MockEmbeddingEngine), 7, Duration::from_millis(11));

        let model_info = service
            .get_model_info(Request::new(GetModelInfoRequest {}))
            .await
            .expect("model info should return")
            .into_inner();
        assert_eq!(model_info.model_id, "mock-model-v1");
        assert_eq!(model_info.profile_id, "mock-profile-v1");
        assert_eq!(model_info.engine_profile_id, "mock-engine");
        assert_eq!(model_info.language, "en");
        assert_eq!(model_info.embedding_dimension, 8);
        assert_eq!(model_info.native_embedding_dimension, 8);
        assert_eq!(model_info.embedding_transform, "l2_normalize");

        let status = service
            .get_runtime_status(Request::new(GetRuntimeStatusRequest {}))
            .await
            .expect("runtime status should return")
            .into_inner();
        assert_eq!(status.state, "ready");
        assert_eq!(status.image_batch_cap, 7);
        assert_eq!(status.image_batch_wait_ms, 11);
    }
}
