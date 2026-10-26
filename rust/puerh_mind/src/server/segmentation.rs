//! `SegmentationService` server (master plan section 9.2, SM3.A, SM3.B, SM3.C, and SM3.D).
//!
//! Converts the proto messages to engine values, runs the engine on the blocking thread pool, and
//! reports each outcome in the `AiResponseHeader`: the gRPC status stays OK, and a failed
//! operation sets the header status, the error code, and the real error message. The service does
//! not keep any state of its own.

use std::sync::Arc;
use std::time::Instant;

use tonic::{Request, Response, Status};
use tracing::warn;

use crate::proto::alcedo::ai::{
    AiErrorCode, AiRequestHeader, AiResponseHeader, AiResponseStatus,
    DecodeSegmentationPromptRequest, DecodeSegmentationPromptResponse,
    EndSegmentationSessionRequest, EndSegmentationSessionResponse, GetSegmentationStatusRequest,
    GetSegmentationStatusResponse, LoadSegmentationModelsRequest, LoadSegmentationModelsResponse,
    RunSegmentationTaskRequest, RunSegmentationTaskResponse, SegmentationBox,
    SegmentationMaskLevel, SegmentationModelResidency, SegmentationPerson, SegmentationPointLabel,
    SegmentationTask as ProtoSegmentationTask, SegmentationTaskMaskKind,
    StartSegmentationSessionRequest, StartSegmentationSessionResponse,
    UnloadSegmentationModelsRequest, UnloadSegmentationModelsResponse,
    segmentation_service_server::SegmentationService,
};
use crate::service::segmentation::engine::{
    RgbImageView, SegmentationEngine, SegmentationError, SegmentationTask, StartSessionRequest,
    TaskMaskValues, TaskRequest,
};
use crate::service::segmentation::model_set::LOGIT_SIZE;
use crate::service::segmentation::sam2::{PointLabel, PromptBox, PromptPoint, SegmentationPrompt};

/// Provider name in the response headers.
const PROVIDER: &str = "local";

pub struct SegmentationServiceImpl {
    engine: Arc<SegmentationEngine>,
}

impl SegmentationServiceImpl {
    pub fn new(engine: Arc<SegmentationEngine>) -> Self {
        Self { engine }
    }

    /// Runs @p operation on the blocking pool with the engine.
    async fn run_blocking<T: Send + 'static>(
        &self,
        operation: impl FnOnce(&SegmentationEngine) -> Result<T, SegmentationError> + Send + 'static,
    ) -> Result<T, SegmentationError> {
        let engine = self.engine.clone();
        tokio::task::spawn_blocking(move || operation(&engine))
            .await
            .unwrap_or_else(|err| {
                Err(SegmentationError::Inference(format!(
                    "segmentation task stopped: {err}"
                )))
            })
    }
}

fn header_of(header: Option<AiRequestHeader>) -> AiRequestHeader {
    header.unwrap_or_default()
}

fn response_header(
    request: &AiRequestHeader,
    task_id: &str,
    started: Instant,
    error: Option<&SegmentationError>,
) -> Option<AiResponseHeader> {
    let (status, error_code, message) = match error {
        None => (
            AiResponseStatus::AiStatusOk,
            AiErrorCode::AiErrorNone,
            String::new(),
        ),
        Some(err) => {
            let (status, code) = match err {
                SegmentationError::ModelNotReady(_) => (
                    AiResponseStatus::AiStatusProviderUnavailable,
                    AiErrorCode::AiErrorModelNotReady,
                ),
                SegmentationError::InvalidInput(_) => (
                    AiResponseStatus::AiStatusInvalidArgument,
                    AiErrorCode::AiErrorInvalidInput,
                ),
                SegmentationError::SessionNotFound(_) => (
                    AiResponseStatus::AiStatusInvalidArgument,
                    AiErrorCode::AiErrorSessionNotFound,
                ),
                SegmentationError::ImageNotCached(_) => (
                    AiResponseStatus::AiStatusInvalidArgument,
                    AiErrorCode::AiErrorImageNotCached,
                ),
                SegmentationError::Inference(_) => (
                    AiResponseStatus::AiStatusInternal,
                    AiErrorCode::AiErrorInternal,
                ),
            };
            warn!(
                request_id = %request.request_id,
                task_id,
                error = %err,
                "segmentation request failed"
            );
            (status, code, err.to_string())
        }
    };
    Some(AiResponseHeader {
        request_id: request.request_id.clone(),
        task_id: task_id.to_string(),
        status: status as i32,
        error_code: error_code as i32,
        error_message: message,
        provider: PROVIDER.to_string(),
        model_id: crate::service::segmentation::model_set::MODEL_SET_ID.to_string(),
        elapsed_ms: started.elapsed().as_millis() as i64,
    })
}

/// Converts a decode request to an engine prompt. Rejects an unknown point label.
fn prompt_from_request(
    request: DecodeSegmentationPromptRequest,
) -> Result<(String, SegmentationPrompt), SegmentationError> {
    let points = request
        .points
        .iter()
        .enumerate()
        .map(|(index, point)| {
            let label = match SegmentationPointLabel::try_from(point.label) {
                Ok(SegmentationPointLabel::Add) => PointLabel::Add,
                Ok(SegmentationPointLabel::Subtract) => PointLabel::Subtract,
                Err(_) => {
                    return Err(SegmentationError::InvalidInput(format!(
                        "point {index} has the unknown label {}",
                        point.label
                    )));
                }
            };
            Ok(PromptPoint {
                x: point.x,
                y: point.y,
                label,
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    let prompt_box = request.r#box.map(|value| PromptBox {
        x0: value.x0,
        y0: value.y0,
        x1: value.x1,
        y1: value.y1,
    });
    Ok((
        request.session_id,
        SegmentationPrompt {
            points,
            prompt_box,
            mask_input: request.mask_input,
        },
    ))
}

/// Converts a task request to an engine task request. Rejects an unspecified or unknown task.
fn task_from_request(
    request: &RunSegmentationTaskRequest,
) -> Result<TaskRequest, SegmentationError> {
    let task = match ProtoSegmentationTask::try_from(request.task) {
        Ok(ProtoSegmentationTask::Subject) => SegmentationTask::Subject,
        Ok(ProtoSegmentationTask::Background) => SegmentationTask::Background,
        Ok(ProtoSegmentationTask::Sky) => SegmentationTask::Sky,
        Ok(ProtoSegmentationTask::People) => SegmentationTask::People,
        Ok(ProtoSegmentationTask::Person) => SegmentationTask::Person,
        Ok(ProtoSegmentationTask::Unspecified) | Err(_) => {
            return Err(SegmentationError::InvalidInput(format!(
                "segmentation task {} is not a task mask",
                request.task
            )));
        }
    };
    Ok(TaskRequest {
        task,
        person_index: request.person_index,
    })
}

#[tonic::async_trait]
impl SegmentationService for SegmentationServiceImpl {
    async fn load_segmentation_models(
        &self,
        request: Request<LoadSegmentationModelsRequest>,
    ) -> Result<Response<LoadSegmentationModelsResponse>, Status> {
        let started = Instant::now();
        let request = request.into_inner();
        let header = header_of(request.header);
        let directory = std::path::PathBuf::from(request.model_directory);
        let device = request.device;
        let result = self
            .run_blocking(move |engine| engine.load(&directory, &device))
            .await;
        Ok(Response::new(LoadSegmentationModelsResponse {
            header: response_header(
                &header,
                "segmentation.load_models",
                started,
                result.as_ref().err(),
            ),
        }))
    }

    async fn unload_segmentation_models(
        &self,
        request: Request<UnloadSegmentationModelsRequest>,
    ) -> Result<Response<UnloadSegmentationModelsResponse>, Status> {
        let started = Instant::now();
        let header = header_of(request.into_inner().header);
        let result = self.run_blocking(|engine| engine.unload()).await;
        Ok(Response::new(UnloadSegmentationModelsResponse {
            header: response_header(
                &header,
                "segmentation.unload_models",
                started,
                result.as_ref().err(),
            ),
        }))
    }

    async fn get_segmentation_status(
        &self,
        request: Request<GetSegmentationStatusRequest>,
    ) -> Result<Response<GetSegmentationStatusResponse>, Status> {
        let started = Instant::now();
        let header = header_of(request.into_inner().header);
        let result = self.engine.status();
        let mut response = GetSegmentationStatusResponse {
            header: response_header(
                &header,
                "segmentation.status",
                started,
                result.as_ref().err(),
            ),
            ..Default::default()
        };
        if let Ok(status) = result {
            response.loaded = status.loaded;
            response.model_set_id = status.model_set_id.to_string();
            response.model_directory = status.model_directory;
            response.device = status.device;
            response.backend = status.backend;
            response.models = status
                .models
                .into_iter()
                .map(|model| SegmentationModelResidency {
                    role: model.role.as_str().to_string(),
                    device: model.device,
                    load_ms: model.load_ms as i64,
                })
                .collect();
            response.session_count = status.session_count as u32;
            response.cached_image_count = status.cached_image_count as u32;
        }
        Ok(Response::new(response))
    }

    async fn start_segmentation_session(
        &self,
        request: Request<StartSegmentationSessionRequest>,
    ) -> Result<Response<StartSegmentationSessionResponse>, Status> {
        let started = Instant::now();
        let request = request.into_inner();
        let header = header_of(request.header);
        let (width, height, pixels) = (request.width, request.height, request.rgb8_pixels);
        let image_key = request.image_key;
        let result = self
            .run_blocking(move |engine| {
                // Width, height, and pixels all empty: the request starts from the cache only.
                // Any other combination is an image that the engine validates.
                let has_image = width != 0 || height != 0 || !pixels.is_empty();
                engine.start_session(StartSessionRequest {
                    image: has_image.then_some(RgbImageView {
                        width,
                        height,
                        rgb8: &pixels,
                    }),
                    image_key: &image_key,
                })
            })
            .await;
        let mut response = StartSegmentationSessionResponse {
            header: response_header(
                &header,
                "segmentation.start_session",
                started,
                result.as_ref().err(),
            ),
            ..Default::default()
        };
        if let Ok(session) = result {
            response.session_id = session.session_id;
            response.encoder_ms = session.encoder_ms as i64;
            response.from_cache = session.from_cache;
            response.image_width = session.image_width;
            response.image_height = session.image_height;
        }
        Ok(Response::new(response))
    }

    async fn decode_segmentation_prompt(
        &self,
        request: Request<DecodeSegmentationPromptRequest>,
    ) -> Result<Response<DecodeSegmentationPromptResponse>, Status> {
        let started = Instant::now();
        let mut request = request.into_inner();
        let header = header_of(request.header.take());
        let result = match prompt_from_request(request) {
            Ok((session_id, prompt)) => {
                self.run_blocking(move |engine| engine.decode(&session_id, prompt))
                    .await
            }
            Err(err) => Err(err),
        };
        let mut response = DecodeSegmentationPromptResponse {
            header: response_header(
                &header,
                "segmentation.decode_prompt",
                started,
                result.as_ref().err(),
            ),
            logit_size: LOGIT_SIZE as u32,
            ..Default::default()
        };
        if let Ok(decoded) = result {
            response.selected_index = decoded.masks.selected_index as u32;
            response.decoder_ms = decoded.decoder_ms as i64;
            response.masks = decoded
                .masks
                .masks
                .into_iter()
                .map(|level| SegmentationMaskLevel {
                    logits: level.logits,
                    score: level.score,
                    token: level.token,
                    area: level.area,
                })
                .collect();
        }
        Ok(Response::new(response))
    }

    async fn end_segmentation_session(
        &self,
        request: Request<EndSegmentationSessionRequest>,
    ) -> Result<Response<EndSegmentationSessionResponse>, Status> {
        let started = Instant::now();
        let request = request.into_inner();
        let header = header_of(request.header);
        let result = self.engine.end_session(&request.session_id);
        Ok(Response::new(EndSegmentationSessionResponse {
            header: response_header(
                &header,
                "segmentation.end_session",
                started,
                result.as_ref().err(),
            ),
            ended: result.unwrap_or(false),
        }))
    }

    async fn run_segmentation_task(
        &self,
        request: Request<RunSegmentationTaskRequest>,
    ) -> Result<Response<RunSegmentationTaskResponse>, Status> {
        let started = Instant::now();
        let mut request = request.into_inner();
        let header = header_of(request.header.take());
        let result = match task_from_request(&request) {
            Ok(task) => {
                let session_id = std::mem::take(&mut request.session_id);
                self.run_blocking(move |engine| engine.run_task(&session_id, task))
                    .await
            }
            Err(err) => Err(err),
        };
        let mut response = RunSegmentationTaskResponse {
            header: response_header(
                &header,
                "segmentation.run_task",
                started,
                result.as_ref().err(),
            ),
            ..Default::default()
        };
        if let Ok(result) = result {
            response.message = result.message;
            response.presence = result.presence;
            response.presence_threshold = result.presence_threshold;
            response.region_count = result.region_count;
            response.subject_score = result.subject_score;
            response.mask_area = result.mask_area;
            response.model_ms = result.model_ms as i64;
            response.people = result
                .people
                .into_iter()
                .enumerate()
                .map(|(index, person)| SegmentationPerson {
                    index: index as u32,
                    r#box: Some(SegmentationBox {
                        x0: person.x0,
                        y0: person.y0,
                        x1: person.x1,
                        y1: person.y1,
                    }),
                    score: person.score,
                    subject_fraction: person.subject_fraction,
                    in_subject: person.in_subject,
                })
                .collect();
            if let Some(mask) = result.mask {
                response.found = true;
                response.mask_width = mask.width;
                response.mask_height = mask.height;
                match mask.values {
                    TaskMaskValues::Binary(values) => {
                        response.mask_kind = SegmentationTaskMaskKind::Binary as i32;
                        response.mask_r8 = values;
                    }
                    TaskMaskValues::Alpha { values, mask_box } => {
                        response.mask_kind = SegmentationTaskMaskKind::Alpha as i32;
                        response.alpha_values = values;
                        response.mask_box = Some(SegmentationBox {
                            x0: mask_box.x0,
                            y0: mask_box.y0,
                            x1: mask_box.x1,
                            y1: mask_box.y1,
                        });
                    }
                }
            }
        }
        Ok(Response::new(response))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::alcedo::ai::SegmentationPoint;
    use crate::service::segmentation::test_support::{FakeFactory, model_directory};

    fn service() -> SegmentationServiceImpl {
        SegmentationServiceImpl::new(Arc::new(SegmentationEngine::new(Arc::new(
            FakeFactory::new(),
        ))))
    }

    fn header_status(header: &Option<AiResponseHeader>) -> (i32, i32) {
        let header = header.as_ref().expect("header");
        (header.status, header.error_code)
    }

    #[tokio::test]
    async fn server_maps_model_not_ready_to_provider_unavailable() {
        let service = service();
        let response = service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: Some(AiRequestHeader {
                    request_id: "r1".to_string(),
                    ..Default::default()
                }),
                width: 2,
                height: 2,
                rgb8_pixels: vec![0; 12],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header),
            (
                AiResponseStatus::AiStatusProviderUnavailable as i32,
                AiErrorCode::AiErrorModelNotReady as i32
            )
        );
        assert_eq!(response.header.as_ref().expect("header").request_id, "r1");
        assert!(response.session_id.is_empty());
    }

    async fn loaded_service() -> SegmentationServiceImpl {
        let service = service();
        let response = service
            .load_segmentation_models(Request::new(LoadSegmentationModelsRequest {
                header: None,
                model_directory: model_directory("server").display().to_string(),
                device: "fake".to_string(),
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        service
    }

    #[tokio::test]
    async fn server_maps_invalid_image_to_invalid_argument() {
        let service = loaded_service().await;
        let response = service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: None,
                width: 4,
                height: 4,
                rgb8_pixels: vec![0; 10],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header),
            (
                AiResponseStatus::AiStatusInvalidArgument as i32,
                AiErrorCode::AiErrorInvalidInput as i32
            )
        );
    }

    #[tokio::test]
    async fn server_rejects_unknown_point_label_and_unknown_session() {
        let service = loaded_service().await;
        let response = service
            .decode_segmentation_prompt(Request::new(DecodeSegmentationPromptRequest {
                session_id: "missing".to_string(),
                points: vec![SegmentationPoint {
                    x: 0.5,
                    y: 0.5,
                    label: 2,
                }],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header),
            (
                AiResponseStatus::AiStatusInvalidArgument as i32,
                AiErrorCode::AiErrorInvalidInput as i32
            )
        );
        let response = service
            .decode_segmentation_prompt(Request::new(DecodeSegmentationPromptRequest {
                session_id: "missing".to_string(),
                points: vec![SegmentationPoint {
                    x: 0.5,
                    y: 0.5,
                    label: SegmentationPointLabel::Add as i32,
                }],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header),
            (
                AiResponseStatus::AiStatusInvalidArgument as i32,
                AiErrorCode::AiErrorSessionNotFound as i32
            )
        );
    }

    #[tokio::test]
    async fn server_returns_click_levels_with_logit_size() {
        let service = loaded_service().await;
        let started = service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: None,
                width: 8,
                height: 4,
                rgb8_pixels: vec![50; 8 * 4 * 3],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert!(!started.session_id.is_empty());
        let response = service
            .decode_segmentation_prompt(Request::new(DecodeSegmentationPromptRequest {
                session_id: started.session_id.clone(),
                points: vec![SegmentationPoint {
                    x: 0.5,
                    y: 0.5,
                    label: SegmentationPointLabel::Add as i32,
                }],
                ..Default::default()
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(
            header_status(&response.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert_eq!(response.logit_size, LOGIT_SIZE as u32);
        assert_eq!(response.masks.len(), 3);
        assert!(
            response
                .masks
                .iter()
                .all(|level| level.logits.len() == LOGIT_SIZE * LOGIT_SIZE)
        );
        let ended = service
            .end_segmentation_session(Request::new(EndSegmentationSessionRequest {
                header: None,
                session_id: started.session_id,
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert!(ended.ended);
    }

    async fn start_with_key(
        service: &SegmentationServiceImpl,
        image_key: &str,
        with_pixels: bool,
    ) -> StartSegmentationSessionResponse {
        let (width, height) = if with_pixels { (8, 4) } else { (0, 0) };
        service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: None,
                width,
                height,
                rgb8_pixels: vec![50; (width * height * 3) as usize],
                image_key: image_key.to_string(),
            }))
            .await
            .expect("transport ok")
            .into_inner()
    }

    #[tokio::test]
    async fn server_maps_image_not_cached_to_invalid_argument() {
        let service = loaded_service().await;
        let response = start_with_key(&service, "photo-a", false).await;
        assert_eq!(
            header_status(&response.header),
            (
                AiResponseStatus::AiStatusInvalidArgument as i32,
                AiErrorCode::AiErrorImageNotCached as i32
            )
        );
        assert!(response.session_id.is_empty());
        assert!(!response.from_cache);
    }

    #[tokio::test]
    async fn start_response_carries_image_size() {
        let service = loaded_service().await;
        let with_pixels = start_with_key(&service, "photo-a", true).await;
        assert_eq!(
            header_status(&with_pixels.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(!with_pixels.from_cache);
        assert_eq!((with_pixels.image_width, with_pixels.image_height), (8, 4));
        let key_only = start_with_key(&service, "photo-a", false).await;
        assert_eq!(
            header_status(&key_only.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(key_only.from_cache);
        assert_eq!((key_only.image_width, key_only.image_height), (8, 4));
        let not_cached = start_with_key(&service, "photo-b", false).await;
        assert!(not_cached.session_id.is_empty());
        assert_eq!((not_cached.image_width, not_cached.image_height), (0, 0));
    }

    #[tokio::test]
    async fn server_reports_from_cache_and_cached_image_count() {
        let service = loaded_service().await;
        let first = start_with_key(&service, "photo-a", true).await;
        assert_eq!(
            header_status(&first.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(!first.from_cache);
        let second = start_with_key(&service, "photo-a", false).await;
        assert_eq!(
            header_status(&second.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(second.from_cache);
        assert_eq!(second.encoder_ms, 0);
        assert_ne!(first.session_id, second.session_id);
        let status = service
            .get_segmentation_status(Request::new(GetSegmentationStatusRequest::default()))
            .await
            .expect("transport ok")
            .into_inner();
        assert_eq!(status.cached_image_count, 1);
        assert_eq!(status.session_count, 2);
    }

    async fn run_task(
        service: &SegmentationServiceImpl,
        session_id: &str,
        task: ProtoSegmentationTask,
        person_index: Option<u32>,
    ) -> RunSegmentationTaskResponse {
        service
            .run_segmentation_task(Request::new(RunSegmentationTaskRequest {
                header: None,
                session_id: session_id.to_string(),
                task: task as i32,
                person_index,
            }))
            .await
            .expect("transport ok")
            .into_inner()
    }

    #[tokio::test]
    async fn server_maps_task_result_and_not_found() {
        let service = loaded_service().await;
        let session = start_with_key(&service, "", true).await;

        // The fake sky head finds no sky: a successful result without a mask.
        let sky = run_task(
            &service,
            &session.session_id,
            ProtoSegmentationTask::Sky,
            None,
        )
        .await;
        assert_eq!(
            header_status(&sky.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(!sky.found);
        assert!(sky.message.starts_with("No sky found"), "{}", sky.message);
        assert!(sky.mask_r8.is_empty() && sky.alpha_values.is_empty());
        assert!(sky.mask_box.is_none());
        assert_eq!(sky.presence, 0.0);
        assert_eq!(
            sky.presence_threshold,
            crate::service::segmentation::task_masks::SKY_FOUND_SHARE
        );

        let unspecified = run_task(
            &service,
            &session.session_id,
            ProtoSegmentationTask::Unspecified,
            None,
        )
        .await;
        assert_eq!(
            header_status(&unspecified.header),
            (
                AiResponseStatus::AiStatusInvalidArgument as i32,
                AiErrorCode::AiErrorInvalidInput as i32
            )
        );
        let missing = run_task(&service, "missing", ProtoSegmentationTask::People, None).await;
        assert_eq!(
            header_status(&missing.header).1,
            AiErrorCode::AiErrorSessionNotFound as i32
        );
    }

    #[tokio::test]
    async fn server_maps_sky_head_alpha_with_whole_image_box() {
        use crate::service::segmentation::model_set::SKY_ALPHA_SIZE;

        let factory = FakeFactory::new();
        let plane = SKY_ALPHA_SIZE * SKY_ALPHA_SIZE;
        let mut alpha = vec![0.1f32; plane];
        alpha[..3 * SKY_ALPHA_SIZE].fill(0.75);
        *factory.calls.sky_alpha.lock().expect("sky alpha") = Some(alpha.clone());
        let service =
            SegmentationServiceImpl::new(Arc::new(SegmentationEngine::new(Arc::new(factory))));
        service
            .load_segmentation_models(Request::new(LoadSegmentationModelsRequest {
                header: None,
                model_directory: model_directory("server").display().to_string(),
                device: "fake".to_string(),
            }))
            .await
            .expect("transport ok");
        let session = start_with_key(&service, "", true).await;
        let sky = run_task(
            &service,
            &session.session_id,
            ProtoSegmentationTask::Sky,
            None,
        )
        .await;
        assert_eq!(
            header_status(&sky.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(sky.found, "{}", sky.message);
        assert_eq!(sky.mask_kind, SegmentationTaskMaskKind::Alpha as i32);
        assert_eq!(
            (sky.mask_width, sky.mask_height),
            (SKY_ALPHA_SIZE as u32, SKY_ALPHA_SIZE as u32)
        );
        assert_eq!(sky.alpha_values, alpha);
        assert!(sky.mask_r8.is_empty());
        let mask_box = sky.mask_box.expect("mask box");
        assert_eq!(
            (mask_box.x0, mask_box.y0, mask_box.x1, mask_box.y1),
            (0.0, 0.0, 1.0, 1.0)
        );
        assert_eq!(sky.mask_area, 3 * SKY_ALPHA_SIZE as u32);
        assert_eq!(sky.region_count, 0);
    }

    /// A service whose fake head lists one person (a 4 x 8 cell box centered on the head grid cell
    /// at row 20, column 32) and whose fake S3OD alpha is @p alpha. Returns the service, the fake
    /// calls, and a session on a 640 x 480 image.
    async fn service_with_one_person(
        alpha: f32,
    ) -> (
        SegmentationServiceImpl,
        Arc<crate::service::segmentation::test_support::FakeCalls>,
        String,
    ) {
        use crate::service::segmentation::model_set::{
            CENTER_LOGITS_SHAPE, HEAD_GRID, LOG_SIZE_SHAPE,
        };
        use crate::service::segmentation::tensor::TensorF32;
        use crate::service::segmentation::test_support::empty_head_output;

        let factory = FakeFactory::new();
        let calls = factory.calls.clone();
        let mut head = empty_head_output();
        head.presence_logits = TensorF32::new(vec![1, 2], vec![-5.0, 5.0]).expect("presence");
        let plane = HEAD_GRID * HEAD_GRID;
        let cell = 20 * HEAD_GRID + 32;
        let mut center = vec![-10.0f32; plane];
        center[cell] = 3.0;
        head.center_logits = TensorF32::new(CENTER_LOGITS_SHAPE.to_vec(), center).expect("center");
        let mut log_size = vec![0.0f32; 2 * plane];
        log_size[cell] = 4f32.ln();
        log_size[plane + cell] = 8f32.ln();
        head.log_size = TensorF32::new(LOG_SIZE_SHAPE.to_vec(), log_size).expect("size");
        *calls.head_output.lock().expect("head output") = Some(head);
        *calls.subject_alpha.lock().expect("alpha") = alpha;
        let service =
            SegmentationServiceImpl::new(Arc::new(SegmentationEngine::new(Arc::new(factory))));
        service
            .load_segmentation_models(Request::new(LoadSegmentationModelsRequest {
                header: None,
                model_directory: model_directory("server").display().to_string(),
                device: "fake".to_string(),
            }))
            .await
            .expect("transport ok");
        let started = service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: None,
                width: 640,
                height: 480,
                rgb8_pixels: vec![50; 640 * 480 * 3],
                image_key: String::new(),
            }))
            .await
            .expect("transport ok")
            .into_inner();
        assert!(!started.session_id.is_empty());
        (service, calls, started.session_id)
    }

    #[tokio::test]
    async fn server_maps_people_alpha_list_and_fractions() {
        let (service, _calls, session_id) = service_with_one_person(0.8).await;
        let people = run_task(&service, &session_id, ProtoSegmentationTask::People, None).await;
        assert_eq!(
            header_status(&people.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(people.found);
        assert!(people.message.is_empty());
        assert_eq!(people.mask_kind, SegmentationTaskMaskKind::Alpha as i32);
        // The 640 x 480 image resized to a long edge of 1024.
        assert_eq!((people.mask_width, people.mask_height), (1024, 768));
        assert_eq!(people.alpha_values.len(), 1024 * 768);
        assert!(people.mask_r8.is_empty());
        let mask_box = people.mask_box.expect("mask box");
        assert_eq!(
            (mask_box.x0, mask_box.y0, mask_box.x1, mask_box.y1),
            (0.0, 0.0, 1.0, 1.0)
        );
        assert_eq!(people.people.len(), 1);
        let person = &people.people[0];
        assert_eq!(person.index, 0);
        let prompt_box = person.r#box.expect("box");
        assert_eq!(
            (prompt_box.x0, prompt_box.y0, prompt_box.x1, prompt_box.y1),
            (30.0 / 64.0, 16.0 / 64.0, 34.0 / 64.0, 24.0 / 64.0)
        );
        assert_eq!(person.subject_fraction, 1.0);
        assert!(person.in_subject);
        assert_eq!(people.subject_score, 0.95);
    }

    #[tokio::test]
    async fn server_maps_person_alpha_and_mask_box() {
        let (service, calls, session_id) = service_with_one_person(0.8).await;
        let found = run_task(
            &service,
            &session_id,
            ProtoSegmentationTask::Person,
            Some(0),
        )
        .await;
        assert_eq!(
            header_status(&found.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(found.found);
        assert_eq!(found.mask_kind, SegmentationTaskMaskKind::Alpha as i32);
        // The crop: x 30 to 34 cells (300 to 340 px) and y 16 to 24 cells (120 to 180 px),
        // widened by 4 px and 6 px: 296 to 344 and 114 to 186, 48 x 72 px, resized to 683 x 1024.
        assert_eq!((found.mask_width, found.mask_height), (683, 1024));
        assert_eq!(found.alpha_values.len(), 683 * 1024);
        let mask_box = found.mask_box.expect("mask box");
        assert_eq!(
            (mask_box.x0, mask_box.y0, mask_box.x1, mask_box.y1),
            (296.0 / 640.0, 114.0 / 480.0, 344.0 / 640.0, 186.0 / 480.0)
        );
        assert_eq!(found.people.len(), 1);

        // A second image whose crop alpha stays below 0.5: not found, with the message.
        *calls.subject_alpha.lock().expect("alpha") = 0.3;
        let started = service
            .start_segmentation_session(Request::new(StartSegmentationSessionRequest {
                header: None,
                width: 640,
                height: 480,
                rgb8_pixels: vec![60; 640 * 480 * 3],
                image_key: String::new(),
            }))
            .await
            .expect("transport ok")
            .into_inner();
        let not_found = run_task(
            &service,
            &started.session_id,
            ProtoSegmentationTask::Person,
            Some(0),
        )
        .await;
        assert_eq!(
            header_status(&not_found.header).0,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(!not_found.found);
        assert!(
            not_found
                .message
                .starts_with("No person found in the crop of person 0"),
            "{}",
            not_found.message
        );
        assert!(not_found.alpha_values.is_empty() && not_found.mask_box.is_none());
    }

    #[tokio::test]
    async fn server_rejects_person_without_index() {
        use std::sync::atomic::Ordering;

        let (service, calls, session_id) = service_with_one_person(0.8).await;
        for (task, person_index) in [
            (ProtoSegmentationTask::Person, None),
            (ProtoSegmentationTask::People, Some(0)),
        ] {
            let response = run_task(&service, &session_id, task, person_index).await;
            assert_eq!(
                header_status(&response.header),
                (
                    AiResponseStatus::AiStatusInvalidArgument as i32,
                    AiErrorCode::AiErrorInvalidInput as i32
                ),
                "{task:?}"
            );
            assert!(!response.found);
        }
        // The check comes before the session lookup: an unknown session also reports the index.
        let response = run_task(&service, "missing", ProtoSegmentationTask::Person, None).await;
        assert_eq!(
            header_status(&response.header).1,
            AiErrorCode::AiErrorInvalidInput as i32
        );
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 0);
    }
}
