//! `ImageAnalysisService` server implementation (Phase 5b).
//!
//! Routes `DescribeImage` / `ScoreImage` to a registered image-analysis provider
//! (the mock in Phase 5b; OpenRouter / Volcengine Ark drivers in Phase 5c). The
//! service owns the control-plane concerns the provider should not: credential
//! resolution against the vault, request timeout, cooperative cancellation via
//! the `CancellationRegistry`, and schema validation of the provider's typed
//! result. Outcomes travel inside the `AiResponseHeader` (status / error_code /
//! redacted error_message) — this is the Phase 5 structured-error-in-body mapping
//! deferred from Phase 4: image analysis has no legacy v1 caller, so the RPC
//! returns `Ok(Response{ header, ... })` with the header carrying the precise
//! status rather than a plain `tonic::Status`. A genuinely malformed request
//! (empty image bytes) is still a transport-level `tonic::Status::invalid_argument`,
//! since there is no provider outcome to report.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use tonic::{Request, Response, Status};
use tracing::{info, warn};

use crate::proto::alcedo::ai::{
    AiErrorCode, AiRequestHeader, AiResponseHeader, AiResponseStatus, AnalyzeImageRequest,
    AnalyzeImageResponse, BatchAnalyzeImageItemResponse, BatchAnalyzeImageRequest,
    BatchAnalyzeImageResponse, DescribeImageRequest, DescribeImageResponse,
    DiscoveredModel as DiscoveredModelProto, ImageRatingResult, ImageUnderstandingResult,
    ListModelsRequest, ListModelsResponse, RenditionMetadata, ScoreImageRequest,
    ScoreImageResponse, UsageMetadata, image_analysis_service_server::ImageAnalysisService,
};
use crate::service::cancellation_registry::CancellationRegistry;
use crate::service::credential_vault::{CredentialError, CredentialVault, SecretString};
use crate::service::image_analysis::{
    AnalyzeImageInput, AnalyzeOutcome, BatchAnalyzeOutcome, DescribeOutcome, ImageAnalysisProvider,
    ProviderError, ScoreOutcome, validate_analyze, validate_rating, validate_understanding,
};

/// Timeout used when the request header leaves `timeout_ms` at 0 (server default).
const DEFAULT_TIMEOUT_MS: u64 = 60_000;

pub struct ImageAnalysisServiceImpl {
    providers: HashMap<String, Arc<dyn ImageAnalysisProvider>>,
    default_provider_id: String,
    vault: Arc<CredentialVault>,
    cancel_registry: Arc<CancellationRegistry>,
}

impl ImageAnalysisServiceImpl {
    pub fn new(
        providers: HashMap<String, Arc<dyn ImageAnalysisProvider>>,
        default_provider_id: String,
        vault: Arc<CredentialVault>,
        cancel_registry: Arc<CancellationRegistry>,
    ) -> Self {
        Self {
            providers,
            default_provider_id,
            vault,
            cancel_registry,
        }
    }

    fn lookup_provider(&self, provider_id: &str) -> Option<Arc<dyn ImageAnalysisProvider>> {
        let id = if provider_id.trim().is_empty() {
            self.default_provider_id.as_str()
        } else {
            provider_id
        };
        self.providers.get(id).cloned()
    }

    /// Resolve the credential a provider requires to the secret itself. Returns
    /// `Ok(None)` when the provider does not require a credential, `Ok(Some(secret))`
    /// when the handle resolves, or a failure header triple when the credential is
    /// missing or the handle is invalid; the caller turns the triple into an
    /// `Ok(Response)` with that failure header. Provider ids are not secrets and
    /// may appear in error text; the message is still passed through the vault's
    /// redactor before placement. The resolved `SecretString` is handed to the
    /// provider trait method and never logged (Phase 3 invariant preserved).
    fn resolve_credential_secret(
        &self,
        header: &AiRequestHeader,
        provider: &dyn ImageAnalysisProvider,
    ) -> Result<Option<SecretString>, (AiResponseStatus, AiErrorCode, String)> {
        if !provider.requires_credential() {
            return Ok(None);
        }
        if header.credential_ref.trim().is_empty() {
            return Err((
                AiResponseStatus::AiStatusUnauthenticated,
                AiErrorCode::AiErrorMissingCredential,
                "image analysis task requires a credential but none was supplied".to_string(),
            ));
        }
        match self.vault.resolve(&header.credential_ref) {
            Ok(secret) => Ok(Some(secret)),
            Err(CredentialError::NotFound) | Err(CredentialError::Revoked) => Err((
                AiResponseStatus::AiStatusPermissionDenied,
                AiErrorCode::AiErrorCredentialRevoked,
                "credential handle is unknown or revoked".to_string(),
            )),
            Err(CredentialError::Expired) => Err((
                AiResponseStatus::AiStatusPermissionDenied,
                AiErrorCode::AiErrorCredentialExpired,
                "credential handle has expired".to_string(),
            )),
        }
    }

    fn timeout_duration(header: &AiRequestHeader) -> Duration {
        if header.timeout_ms > 0 {
            Duration::from_millis(header.timeout_ms as u64)
        } else {
            Duration::from_millis(DEFAULT_TIMEOUT_MS)
        }
    }

    fn payload_limit_error(
        provider: &dyn ImageAnalysisProvider,
        bytes: usize,
    ) -> Option<(AiResponseStatus, AiErrorCode, String)> {
        let max = provider.max_payload_bytes();
        if max == 0 || bytes <= max {
            return None;
        }
        Some((
            AiResponseStatus::AiStatusPayloadTooLarge,
            AiErrorCode::AiErrorPayloadDecode,
            format!("image payload is {bytes} bytes; provider limit is {max} bytes"),
        ))
    }

    fn batch_payload_limit_error(
        provider: &dyn ImageAnalysisProvider,
        items: &[crate::proto::alcedo::ai::BatchAnalyzeImageItem],
    ) -> Option<(AiResponseStatus, AiErrorCode, String)> {
        let max = provider.max_payload_bytes();
        if max == 0 {
            return None;
        }
        items.iter().enumerate().find_map(|(index, item)| {
            let bytes = item.image_bytes.len();
            (bytes > max).then(|| {
                (
                    AiResponseStatus::AiStatusPayloadTooLarge,
                    AiErrorCode::AiErrorPayloadDecode,
                    format!(
                        "batch item {index} image payload is {bytes} bytes; provider limit is {max} bytes"
                    ),
                )
            })
        })
    }

    /// Build an `AiResponseHeader` carrying a failure outcome. The error message
    /// is redacted of any secret the vault currently holds before placement.
    /// Returns `Some(...)` because the proto3 `header` field is `Option<AiResponseHeader>`.
    fn failure_header(
        &self,
        req: &AiRequestHeader,
        status: AiResponseStatus,
        error_code: AiErrorCode,
        message: &str,
        provider: &str,
        model_id: &str,
        elapsed_ms: u64,
    ) -> Option<AiResponseHeader> {
        let redacted_message = self.vault.redact_error_message(message);
        warn!(
            request_id = %req.request_id,
            task_id = %req.task_id,
            provider = %provider,
            model = %model_id,
            status = status as i32,
            error_code = error_code as i32,
            elapsed_ms,
            error = %redacted_message,
            "image analysis request failed"
        );
        Some(AiResponseHeader {
            request_id: req.request_id.clone(),
            task_id: req.task_id.clone(),
            status: status as i32,
            error_code: error_code as i32,
            error_message: redacted_message,
            provider: provider.to_string(),
            model_id: model_id.to_string(),
            elapsed_ms: elapsed_ms as i64,
        })
    }

    fn success_header(
        &self,
        req: &AiRequestHeader,
        provider: &str,
        model_id: &str,
        elapsed_ms: u64,
    ) -> Option<AiResponseHeader> {
        Some(AiResponseHeader {
            request_id: req.request_id.clone(),
            task_id: req.task_id.clone(),
            status: AiResponseStatus::AiStatusOk as i32,
            error_code: AiErrorCode::AiErrorNone as i32,
            error_message: String::new(),
            provider: provider.to_string(),
            model_id: model_id.to_string(),
            elapsed_ms: elapsed_ms as i64,
        })
    }

    /// Map a `ProviderError` (or a validator rejection) to a header failure triple.
    fn provider_error_to_header(err: &ProviderError) -> (AiResponseStatus, AiErrorCode, String) {
        match err {
            ProviderError::SchemaValidation => (
                AiResponseStatus::AiStatusProviderError,
                AiErrorCode::AiErrorPayloadDecode,
                "provider response failed schema validation".to_string(),
            ),
            ProviderError::SchemaValidationMessage(message) => (
                AiResponseStatus::AiStatusProviderError,
                AiErrorCode::AiErrorPayloadDecode,
                if message.trim().is_empty() {
                    "provider response failed schema validation".to_string()
                } else {
                    message.clone()
                },
            ),
            ProviderError::Transient => (
                AiResponseStatus::AiStatusProviderUnavailable,
                AiErrorCode::AiErrorProvider5xx,
                "provider returned a transient error".to_string(),
            ),
            ProviderError::Provider(message) => (
                AiResponseStatus::AiStatusProviderError,
                AiErrorCode::AiErrorInternal,
                if message.trim().is_empty() {
                    "provider returned an error".to_string()
                } else {
                    message.clone()
                },
            ),
            // Phase 6c: an unknown explicit model_id fails before any provider
            // HTTP call. Map to INVALID_ARGUMENT so the host surfaces a
            // configuration error rather than retrying or falling back. The
            // offending slug is in the error variant but is NOT placed in the
            // message (provider text is never echoed), so a slug that happens
            // to look like a key cannot leak.
            ProviderError::UnknownModel(_) => (
                AiResponseStatus::AiStatusInvalidArgument,
                AiErrorCode::AiErrorInternal,
                "requested model id is not present in the provider config".to_string(),
            ),
        }
    }
}

fn to_proto_understanding(out: &DescribeOutcome) -> ImageUnderstandingResult {
    ImageUnderstandingResult {
        caption: out.caption.clone(),
        tags: out.tags.clone(),
        scene: out.scene.clone(),
        confidence: out.confidence,
    }
}

fn to_proto_rating(out: &ScoreOutcome) -> ImageRatingResult {
    ImageRatingResult {
        rating: out.rating,
        rubric_id: out.rubric_id.clone(),
        rubric_version: out.rubric_version.clone(),
        reasons: out.reasons.clone(),
    }
}

fn to_proto_usage(out: &impl HasUsage) -> Option<UsageMetadata> {
    let u = out.usage();
    if u.input_tokens == 0 && u.output_tokens == 0 && u.total_tokens == 0 {
        return None;
    }
    Some(UsageMetadata {
        input_tokens: u.input_tokens,
        output_tokens: u.output_tokens,
        total_tokens: u.total_tokens,
    })
}

trait HasUsage {
    fn usage(&self) -> &crate::service::image_analysis::Usage;
}
impl HasUsage for DescribeOutcome {
    fn usage(&self) -> &crate::service::image_analysis::Usage {
        &self.usage
    }
}
impl HasUsage for ScoreOutcome {
    fn usage(&self) -> &crate::service::image_analysis::Usage {
        &self.usage
    }
}
impl HasUsage for AnalyzeOutcome {
    fn usage(&self) -> &crate::service::image_analysis::Usage {
        &self.usage
    }
}
impl HasUsage for BatchAnalyzeOutcome {
    fn usage(&self) -> &crate::service::image_analysis::Usage {
        &self.usage
    }
}

fn batch_item_failure(
    service: &ImageAnalysisServiceImpl,
    batch_header: &AiRequestHeader,
    request_id: &str,
    provider: &str,
    model_id: &str,
    rendition: Option<RenditionMetadata>,
    prompt_profile_id: &str,
    err: &ProviderError,
    elapsed: u64,
) -> BatchAnalyzeImageItemResponse {
    let mut header = batch_header.clone();
    header.request_id = request_id.to_string();
    let (status, code, msg) = ImageAnalysisServiceImpl::provider_error_to_header(err);
    BatchAnalyzeImageItemResponse {
        header: service.failure_header(&header, status, code, &msg, provider, model_id, elapsed),
        understanding: None,
        rating: None,
        rendition,
        usage: None,
        provider_request_id: String::new(),
        prompt_profile_id: prompt_profile_id.to_string(),
    }
}

#[tonic::async_trait]
impl ImageAnalysisService for ImageAnalysisServiceImpl {
    async fn describe_image(
        &self,
        request: Request<DescribeImageRequest>,
    ) -> Result<Response<DescribeImageResponse>, Status> {
        info!("received DescribeImage request");
        let start = std::time::Instant::now();
        let req = request.into_inner();
        let header = req.header.unwrap_or_default();

        if req.image_bytes.is_empty() {
            return Err(Status::invalid_argument("image_bytes must not be empty"));
        }

        let provider = match self.lookup_provider(&req.provider_id) {
            Some(p) => p,
            None => {
                return Ok(Response::new(DescribeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusUnsupportedTask,
                        AiErrorCode::AiErrorTaskUnknown,
                        "no provider registered for the requested provider_id",
                        &req.provider_id,
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        if let Some((status, code, msg)) =
            Self::payload_limit_error(provider.as_ref(), req.image_bytes.len())
        {
            return Ok(Response::new(DescribeImageResponse {
                header: self.failure_header(
                    &header,
                    status,
                    code,
                    &msg,
                    provider.provider_id(),
                    &req.model_id,
                    start.elapsed().as_millis() as u64,
                ),
                result: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            }));
        }

        let credential = match self.resolve_credential_secret(&header, provider.as_ref()) {
            Ok(c) => c,
            Err((status, code, msg)) => {
                return Ok(Response::new(DescribeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        let cancel_rx = self.cancel_registry.register(&header.request_id);
        let provider_fut = provider.describe_image(
            &req.image_bytes,
            &req.model_id,
            &req.prompt_profile_id,
            &req.output_language,
            credential.as_ref(),
        );
        tokio::pin!(provider_fut);
        let timeout_dur = Self::timeout_duration(&header);

        let outcome: Result<Result<DescribeOutcome, ProviderError>, _> = tokio::select! {
            biased;
            _ = cancel_rx => {
                self.cancel_registry.complete(&header.request_id);
                return Ok(Response::new(DescribeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusCancelled,
                        AiErrorCode::AiErrorCancelledByClient,
                        "task cancelled by client",
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
            result = tokio::time::timeout(timeout_dur, provider_fut) => result,
        };
        self.cancel_registry.complete(&header.request_id);
        let elapsed = start.elapsed().as_millis() as u64;

        let response = match outcome {
            Ok(Ok(out)) => match validate_understanding(&out) {
                Ok(()) => DescribeImageResponse {
                    header: self.success_header(
                        &header,
                        provider.provider_id(),
                        &out.model_id,
                        elapsed,
                    ),
                    result: Some(to_proto_understanding(&out)),
                    rendition: req.rendition.clone(),
                    usage: to_proto_usage(&out),
                    provider_request_id: out.provider_request_id.clone(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                },
                Err(err) => {
                    let (status, code, msg) = Self::provider_error_to_header(&err);
                    DescribeImageResponse {
                        header: self.failure_header(
                            &header,
                            status,
                            code,
                            &msg,
                            provider.provider_id(),
                            &out.model_id,
                            elapsed,
                        ),
                        result: None,
                        rendition: req.rendition.clone(),
                        usage: None,
                        provider_request_id: String::new(),
                        prompt_profile_id: req.prompt_profile_id.clone(),
                    }
                }
            },
            Ok(Err(err)) => {
                let (status, code, msg) = Self::provider_error_to_header(&err);
                DescribeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        elapsed,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }
            }
            Err(_elapsed) => DescribeImageResponse {
                header: self.failure_header(
                    &header,
                    AiResponseStatus::AiStatusDeadlineExceeded,
                    AiErrorCode::AiErrorProviderTimeout,
                    "provider call did not complete within the timeout",
                    provider.provider_id(),
                    &req.model_id,
                    elapsed,
                ),
                result: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            },
        };
        Ok(Response::new(response))
    }

    async fn score_image(
        &self,
        request: Request<ScoreImageRequest>,
    ) -> Result<Response<ScoreImageResponse>, Status> {
        info!("received ScoreImage request");
        let start = std::time::Instant::now();
        let req = request.into_inner();
        let header = req.header.unwrap_or_default();

        if req.image_bytes.is_empty() {
            return Err(Status::invalid_argument("image_bytes must not be empty"));
        }

        let provider = match self.lookup_provider(&req.provider_id) {
            Some(p) => p,
            None => {
                return Ok(Response::new(ScoreImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusUnsupportedTask,
                        AiErrorCode::AiErrorTaskUnknown,
                        "no provider registered for the requested provider_id",
                        &req.provider_id,
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        if let Some((status, code, msg)) =
            Self::payload_limit_error(provider.as_ref(), req.image_bytes.len())
        {
            return Ok(Response::new(ScoreImageResponse {
                header: self.failure_header(
                    &header,
                    status,
                    code,
                    &msg,
                    provider.provider_id(),
                    &req.model_id,
                    start.elapsed().as_millis() as u64,
                ),
                result: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            }));
        }

        let credential = match self.resolve_credential_secret(&header, provider.as_ref()) {
            Ok(c) => c,
            Err((status, code, msg)) => {
                return Ok(Response::new(ScoreImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        let cancel_rx = self.cancel_registry.register(&header.request_id);
        let provider_fut = provider.score_image(
            &req.image_bytes,
            &req.model_id,
            &req.prompt_profile_id,
            &req.rubric_id,
            &req.rating_severity,
            &req.output_language,
            &req.camera_context,
            req.include_rating_reasons,
            credential.as_ref(),
        );
        tokio::pin!(provider_fut);
        let timeout_dur = Self::timeout_duration(&header);

        let outcome: Result<Result<ScoreOutcome, ProviderError>, _> = tokio::select! {
            biased;
            _ = cancel_rx => {
                self.cancel_registry.complete(&header.request_id);
                return Ok(Response::new(ScoreImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusCancelled,
                        AiErrorCode::AiErrorCancelledByClient,
                        "task cancelled by client",
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
            result = tokio::time::timeout(timeout_dur, provider_fut) => result,
        };
        self.cancel_registry.complete(&header.request_id);
        let elapsed = start.elapsed().as_millis() as u64;

        let response = match outcome {
            Ok(Ok(out)) => match validate_rating(&out) {
                Ok(()) => ScoreImageResponse {
                    header: self.success_header(
                        &header,
                        provider.provider_id(),
                        &out.model_id,
                        elapsed,
                    ),
                    result: Some(to_proto_rating(&out)),
                    rendition: req.rendition.clone(),
                    usage: to_proto_usage(&out),
                    provider_request_id: out.provider_request_id.clone(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                },
                Err(err) => {
                    let (status, code, msg) = Self::provider_error_to_header(&err);
                    ScoreImageResponse {
                        header: self.failure_header(
                            &header,
                            status,
                            code,
                            &msg,
                            provider.provider_id(),
                            &out.model_id,
                            elapsed,
                        ),
                        result: None,
                        rendition: req.rendition.clone(),
                        usage: None,
                        provider_request_id: String::new(),
                        prompt_profile_id: req.prompt_profile_id.clone(),
                    }
                }
            },
            Ok(Err(err)) => {
                let (status, code, msg) = Self::provider_error_to_header(&err);
                ScoreImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        elapsed,
                    ),
                    result: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }
            }
            Err(_elapsed) => ScoreImageResponse {
                header: self.failure_header(
                    &header,
                    AiResponseStatus::AiStatusDeadlineExceeded,
                    AiErrorCode::AiErrorProviderTimeout,
                    "provider call did not complete within the timeout",
                    provider.provider_id(),
                    &req.model_id,
                    elapsed,
                ),
                result: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            },
        };
        Ok(Response::new(response))
    }

    async fn analyze_image(
        &self,
        request: Request<AnalyzeImageRequest>,
    ) -> Result<Response<AnalyzeImageResponse>, Status> {
        info!("received AnalyzeImage request");
        let start = std::time::Instant::now();
        let req = request.into_inner();
        let header = req.header.unwrap_or_default();

        if req.image_bytes.is_empty() {
            return Err(Status::invalid_argument("image_bytes must not be empty"));
        }
        if !req.include_understanding && !req.include_rating {
            return Err(Status::invalid_argument(
                "at least one analysis output must be requested",
            ));
        }

        let provider = match self.lookup_provider(&req.provider_id) {
            Some(p) => p,
            None => {
                return Ok(Response::new(AnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusUnsupportedTask,
                        AiErrorCode::AiErrorTaskUnknown,
                        "no provider registered for the requested provider_id",
                        &req.provider_id,
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    understanding: None,
                    rating: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        if let Some((status, code, msg)) =
            Self::payload_limit_error(provider.as_ref(), req.image_bytes.len())
        {
            return Ok(Response::new(AnalyzeImageResponse {
                header: self.failure_header(
                    &header,
                    status,
                    code,
                    &msg,
                    provider.provider_id(),
                    &req.model_id,
                    start.elapsed().as_millis() as u64,
                ),
                understanding: None,
                rating: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            }));
        }

        let credential = match self.resolve_credential_secret(&header, provider.as_ref()) {
            Ok(c) => c,
            Err((status, code, msg)) => {
                return Ok(Response::new(AnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    understanding: None,
                    rating: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
        };

        let cancel_rx = self.cancel_registry.register(&header.request_id);
        let provider_fut = provider.analyze_image(
            &req.image_bytes,
            &req.model_id,
            &req.prompt_profile_id,
            &req.rubric_id,
            &req.rating_severity,
            &req.output_language,
            &req.camera_context,
            req.include_understanding,
            req.include_rating,
            req.include_rating_reasons,
            credential.as_ref(),
        );
        tokio::pin!(provider_fut);
        let timeout_dur = Self::timeout_duration(&header);

        let outcome: Result<Result<AnalyzeOutcome, ProviderError>, _> = tokio::select! {
            biased;
            _ = cancel_rx => {
                self.cancel_registry.complete(&header.request_id);
                return Ok(Response::new(AnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusCancelled,
                        AiErrorCode::AiErrorCancelledByClient,
                        "task cancelled by client",
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    understanding: None,
                    rating: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }));
            }
            result = tokio::time::timeout(timeout_dur, provider_fut) => result,
        };
        self.cancel_registry.complete(&header.request_id);
        let elapsed = start.elapsed().as_millis() as u64;

        let response = match outcome {
            Ok(Ok(out)) => match validate_analyze(&out) {
                Ok(()) => AnalyzeImageResponse {
                    header: self.success_header(
                        &header,
                        provider.provider_id(),
                        &out.model_id,
                        elapsed,
                    ),
                    understanding: out.understanding.as_ref().map(to_proto_understanding),
                    rating: out.rating.as_ref().map(to_proto_rating),
                    rendition: req.rendition.clone(),
                    usage: to_proto_usage(&out),
                    provider_request_id: out.provider_request_id.clone(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                },
                Err(err) => {
                    let (status, code, msg) = Self::provider_error_to_header(&err);
                    AnalyzeImageResponse {
                        header: self.failure_header(
                            &header,
                            status,
                            code,
                            &msg,
                            provider.provider_id(),
                            &out.model_id,
                            elapsed,
                        ),
                        understanding: None,
                        rating: None,
                        rendition: req.rendition.clone(),
                        usage: None,
                        provider_request_id: String::new(),
                        prompt_profile_id: req.prompt_profile_id.clone(),
                    }
                }
            },
            Ok(Err(err)) => {
                let (status, code, msg) = Self::provider_error_to_header(&err);
                AnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        elapsed,
                    ),
                    understanding: None,
                    rating: None,
                    rendition: req.rendition.clone(),
                    usage: None,
                    provider_request_id: String::new(),
                    prompt_profile_id: req.prompt_profile_id.clone(),
                }
            }
            Err(_elapsed) => AnalyzeImageResponse {
                header: self.failure_header(
                    &header,
                    AiResponseStatus::AiStatusDeadlineExceeded,
                    AiErrorCode::AiErrorProviderTimeout,
                    "provider call did not complete within the timeout",
                    provider.provider_id(),
                    &req.model_id,
                    elapsed,
                ),
                understanding: None,
                rating: None,
                rendition: req.rendition.clone(),
                usage: None,
                provider_request_id: String::new(),
                prompt_profile_id: req.prompt_profile_id.clone(),
            },
        };
        Ok(Response::new(response))
    }

    async fn batch_analyze_image(
        &self,
        request: Request<BatchAnalyzeImageRequest>,
    ) -> Result<Response<BatchAnalyzeImageResponse>, Status> {
        info!("received BatchAnalyzeImage request");
        let start = std::time::Instant::now();
        let req = request.into_inner();
        let header = req.header.unwrap_or_default();

        if req.items.is_empty() {
            return Err(Status::invalid_argument("items must not be empty"));
        }
        if !req.include_understanding && !req.include_rating {
            return Err(Status::invalid_argument(
                "at least one batch output must be requested",
            ));
        }
        if req.items.iter().any(|item| item.image_bytes.is_empty()) {
            return Err(Status::invalid_argument(
                "batch item image_bytes must not be empty",
            ));
        }

        let provider = match self.lookup_provider(&req.provider_id) {
            Some(p) => p,
            None => {
                return Ok(Response::new(BatchAnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusUnsupportedTask,
                        AiErrorCode::AiErrorTaskUnknown,
                        "no provider registered for the requested provider_id",
                        &req.provider_id,
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    items: Vec::new(),
                }));
            }
        };

        if let Some((status, code, msg)) =
            Self::batch_payload_limit_error(provider.as_ref(), &req.items)
        {
            return Ok(Response::new(BatchAnalyzeImageResponse {
                header: self.failure_header(
                    &header,
                    status,
                    code,
                    &msg,
                    provider.provider_id(),
                    &req.model_id,
                    start.elapsed().as_millis() as u64,
                ),
                items: Vec::new(),
            }));
        }

        let credential = match self.resolve_credential_secret(&header, provider.as_ref()) {
            Ok(c) => c,
            Err((status, code, msg)) => {
                return Ok(Response::new(BatchAnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    items: Vec::new(),
                }));
            }
        };

        let inputs: Vec<AnalyzeImageInput<'_>> = req
            .items
            .iter()
            .map(|item| AnalyzeImageInput {
                image_bytes: item.image_bytes.as_slice(),
                camera_context: item.camera_context.as_str(),
            })
            .collect();
        let cancel_rx = self.cancel_registry.register(&header.request_id);
        let provider_fut = provider.batch_analyze_images(
            &inputs,
            &req.model_id,
            &req.prompt_profile_id,
            &req.rubric_id,
            &req.rating_severity,
            &req.output_language,
            req.include_understanding,
            req.include_rating,
            req.include_rating_reasons,
            credential.as_ref(),
        );
        tokio::pin!(provider_fut);
        let timeout_dur = Self::timeout_duration(&header);

        let outcome: Result<Result<BatchAnalyzeOutcome, ProviderError>, _> = tokio::select! {
            biased;
            _ = cancel_rx => {
                self.cancel_registry.complete(&header.request_id);
                return Ok(Response::new(BatchAnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusCancelled,
                        AiErrorCode::AiErrorCancelledByClient,
                        "task cancelled by client",
                        provider.provider_id(),
                        &req.model_id,
                        start.elapsed().as_millis() as u64,
                    ),
                    items: Vec::new(),
                }));
            }
            result = tokio::time::timeout(timeout_dur, provider_fut) => result,
        };
        self.cancel_registry.complete(&header.request_id);
        let elapsed = start.elapsed().as_millis() as u64;

        let response = match outcome {
            Ok(Ok(out)) if out.items.len() == req.items.len() => {
                let items = out
                    .items
                    .iter()
                    .zip(req.items.iter())
                    .map(|(item_out, req_item)| match validate_analyze(item_out) {
                        Ok(()) => BatchAnalyzeImageItemResponse {
                            header: self.success_header(
                                &AiRequestHeader {
                                    request_id: req_item.request_id.clone(),
                                    ..header.clone()
                                },
                                provider.provider_id(),
                                &item_out.model_id,
                                elapsed,
                            ),
                            understanding: item_out
                                .understanding
                                .as_ref()
                                .map(to_proto_understanding),
                            rating: item_out.rating.as_ref().map(to_proto_rating),
                            rendition: req_item.rendition.clone(),
                            usage: to_proto_usage(item_out),
                            provider_request_id: item_out.provider_request_id.clone(),
                            prompt_profile_id: req.prompt_profile_id.clone(),
                        },
                        Err(err) => batch_item_failure(
                            self,
                            &header,
                            &req_item.request_id,
                            provider.provider_id(),
                            &item_out.model_id,
                            req_item.rendition.clone(),
                            &req.prompt_profile_id,
                            &err,
                            elapsed,
                        ),
                    })
                    .collect();
                BatchAnalyzeImageResponse {
                    header: self.success_header(
                        &header,
                        provider.provider_id(),
                        &out.model_id,
                        elapsed,
                    ),
                    items,
                }
            }
            Ok(Ok(out)) => {
                let err = ProviderError::SchemaValidationMessage(format!(
                    "provider returned {} batch results for {} request items",
                    out.items.len(),
                    req.items.len()
                ));
                let (status, code, msg) = Self::provider_error_to_header(&err);
                BatchAnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &out.model_id,
                        elapsed,
                    ),
                    items: Vec::new(),
                }
            }
            Ok(Err(err)) => {
                let (status, code, msg) = Self::provider_error_to_header(&err);
                BatchAnalyzeImageResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        &req.model_id,
                        elapsed,
                    ),
                    items: Vec::new(),
                }
            }
            Err(_elapsed) => BatchAnalyzeImageResponse {
                header: self.failure_header(
                    &header,
                    AiResponseStatus::AiStatusDeadlineExceeded,
                    AiErrorCode::AiErrorProviderTimeout,
                    "provider call did not complete within the timeout",
                    provider.provider_id(),
                    &req.model_id,
                    elapsed,
                ),
                items: Vec::new(),
            },
        };
        Ok(Response::new(response))
    }

    /// Phase 6c: dry-run model discovery. Resolves the credential (when the
    /// provider requires one), calls the provider's `list_models`, and returns
    /// unverified candidate DTOs. No annotations are persisted. A provider that
    /// does not implement discovery (default trait impl) maps to a
    /// `PROVIDER_ERROR` header; auth/schema/network failures map through the
    /// same `provider_error_to_header` arms as the task RPCs. No cancellation
    /// registry entry is registered — discovery is a short bounded call wrapped
    /// only in the request timeout.
    async fn list_models(
        &self,
        request: Request<ListModelsRequest>,
    ) -> Result<Response<ListModelsResponse>, Status> {
        info!("received ListModels request");
        let start = std::time::Instant::now();
        let req = request.into_inner();
        let header = req.header.unwrap_or_default();

        let provider = match self.lookup_provider(&req.provider_id) {
            Some(p) => p,
            None => {
                return Ok(Response::new(ListModelsResponse {
                    header: self.failure_header(
                        &header,
                        AiResponseStatus::AiStatusUnsupportedTask,
                        AiErrorCode::AiErrorTaskUnknown,
                        "no provider registered for the requested provider_id",
                        &req.provider_id,
                        "",
                        start.elapsed().as_millis() as u64,
                    ),
                    models: vec![],
                }));
            }
        };

        let credential = match self.resolve_credential_secret(&header, provider.as_ref()) {
            Ok(c) => c,
            Err((status, code, msg)) => {
                return Ok(Response::new(ListModelsResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        "",
                        start.elapsed().as_millis() as u64,
                    ),
                    models: vec![],
                }));
            }
        };

        let timeout_dur = Self::timeout_duration(&header);
        let fut = provider.list_models(credential.as_ref());
        let outcome: Result<Vec<_>, ProviderError> =
            match tokio::time::timeout(timeout_dur, fut).await {
                Ok(inner) => inner,
                Err(_elapsed) => {
                    return Ok(Response::new(ListModelsResponse {
                        header: self.failure_header(
                            &header,
                            AiResponseStatus::AiStatusDeadlineExceeded,
                            AiErrorCode::AiErrorProviderTimeout,
                            "model-list call did not complete within the timeout",
                            provider.provider_id(),
                            "",
                            start.elapsed().as_millis() as u64,
                        ),
                        models: vec![],
                    }));
                }
            };
        let elapsed = start.elapsed().as_millis() as u64;

        match outcome {
            Ok(models) => {
                let proto_models: Vec<DiscoveredModelProto> = models
                    .into_iter()
                    .map(|m| DiscoveredModelProto {
                        model_id: m.model_id,
                        display_name: m.display_name,
                        source_provider_id: m.source_provider_id,
                    })
                    .collect();
                Ok(Response::new(ListModelsResponse {
                    header: self.success_header(&header, provider.provider_id(), "", elapsed),
                    models: proto_models,
                }))
            }
            Err(err) => {
                let (status, code, msg) = Self::provider_error_to_header(&err);
                Ok(Response::new(ListModelsResponse {
                    header: self.failure_header(
                        &header,
                        status,
                        code,
                        &msg,
                        provider.provider_id(),
                        "",
                        elapsed,
                    ),
                    models: vec![],
                }))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::alcedo::ai::{
        AiInputKind, AiOutputKind, AiPriority, RenditionMetadata as ProtoRendition,
    };
    use crate::service::image_analysis::{DiscoveredModel, MockFailure, MockImageAnalysisProvider};

    fn svc(mock: MockImageAnalysisProvider) -> ImageAnalysisServiceImpl {
        let vault = Arc::new(CredentialVault::new(None));
        let cancel_registry = Arc::new(CancellationRegistry::new());
        let mut providers: HashMap<String, Arc<dyn ImageAnalysisProvider>> = HashMap::new();
        let pid = mock.provider_id().to_string();
        providers.insert(pid.clone(), Arc::new(mock));
        ImageAnalysisServiceImpl::new(providers, pid, vault, cancel_registry)
    }

    fn header(request_id: &str, timeout_ms: i64, credential_ref: &str) -> AiRequestHeader {
        AiRequestHeader {
            request_id: request_id.to_string(),
            task_id: "image_understanding.describe".to_string(),
            timeout_ms,
            priority: AiPriority::Normal as i32,
            trace_id: String::new(),
            credential_ref: credential_ref.to_string(),
            client_capabilities: vec![],
        }
    }

    fn rendition() -> Option<ProtoRendition> {
        Some(ProtoRendition {
            kind: "preview".to_string(),
            width: 512,
            height: 512,
            bytes: 1024,
        })
    }

    fn image_bytes() -> Vec<u8> {
        vec![1, 2, 3, 4]
    }

    #[tokio::test]
    async fn describe_image_returns_valid_understanding() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-1", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "profile-1".to_string(),
            output_language: String::new(),
        });
        let resp = svc.describe_image(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(inner.header.as_ref().unwrap().request_id, "req-1");
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        let result = inner.result.expect("understanding result present");
        assert!(!result.caption.is_empty());
        assert!(!result.tags.is_empty());
        assert_eq!(inner.header.as_ref().unwrap().model_id, "alcedo-mock");
        assert_eq!(inner.header.as_ref().unwrap().provider, "mock");
        // Rendition + prompt profile echoed.
        assert_eq!(inner.rendition.as_ref().unwrap().kind, "preview");
        assert_eq!(inner.prompt_profile_id, "profile-1");
        // Usage captured.
        let usage = inner.usage.expect("usage present");
        assert!(usage.total_tokens > 0);
        assert_eq!(inner.provider_request_id, "mock-req-1");
    }

    #[tokio::test]
    async fn describe_image_rejects_provider_payload_over_limit_before_call() {
        let svc =
            svc(MockImageAnalysisProvider::new("mock", "alcedo-mock").with_max_payload_bytes(3));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-too-large", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "image/png".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: String::new(),
            output_language: String::new(),
        });
        let resp = svc.describe_image(req).await.expect("rpc ok").into_inner();
        let h = resp.header.expect("header");
        assert_eq!(h.status, AiResponseStatus::AiStatusPayloadTooLarge as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorPayloadDecode as i32);
        assert!(resp.result.is_none());
        assert!(h.error_message.contains("provider limit is 3 bytes"));
    }

    #[tokio::test]
    async fn score_image_returns_valid_rating() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(ScoreImageRequest {
            header: Some(AiRequestHeader {
                request_id: "req-2".into(),
                task_id: "image_rating.score".into(),
                ..header("req-2", 5000, "")
            }),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "profile-1".to_string(),
            rubric_id: "alcedo-default-v1".to_string(),
            output_language: String::new(),
            rating_severity: String::new(),
            camera_context: String::new(),
            include_rating_reasons: true,
        });
        let resp = svc.score_image(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        assert_eq!(inner.header.as_ref().unwrap().task_id, "image_rating.score");
        let result = inner.result.expect("rating result present");
        // The canned mock rating is 4 (1..=5 contract); distinct task_id + result
        // type: rating carries a single integer star rating, not caption/tags.
        assert_eq!(result.rating, 4);
        assert_eq!(inner.header.as_ref().unwrap().model_id, "alcedo-mock");
    }

    #[tokio::test]
    async fn analyze_image_returns_understanding_and_rating_in_one_response() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(AnalyzeImageRequest {
            header: Some(AiRequestHeader {
                request_id: "req-analyze".into(),
                task_id: "image_analysis.analyze".into(),
                ..header("req-analyze", 5000, "")
            }),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "profile-1".to_string(),
            include_understanding: true,
            include_rating: true,
            rubric_id: "alcedo-default-v1".to_string(),
            output_language: String::new(),
            rating_severity: String::new(),
            camera_context: String::new(),
            include_rating_reasons: true,
        });
        let resp = svc.analyze_image(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        assert_eq!(
            inner.header.as_ref().unwrap().task_id,
            "image_analysis.analyze"
        );
        assert!(inner.understanding.is_some());
        assert!(inner.rating.is_some());
        assert_eq!(inner.rating.as_ref().unwrap().rating, 4);
        assert_eq!(inner.provider_request_id, "mock-req-combined");
        assert!(inner.usage.as_ref().unwrap().total_tokens > 0);
    }

    #[tokio::test]
    async fn describe_image_missing_credential_returns_unauthenticated() {
        let svc =
            svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
                .with_requires_credential(true));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-3", 5000, "")), // empty credential_ref
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (failure in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusUnauthenticated as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorMissingCredential as i32);
        assert!(inner.result.is_none(), "no result on credential failure");
        // No secret material in the message.
        assert!(!h.error_message.contains("sk-"));
    }

    #[tokio::test]
    async fn describe_image_with_valid_credential_succeeds() {
        let svc =
            svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
                .with_requires_credential(true));
        // Register a credential in the vault and pass its handle.
        let handle = svc.vault.register("mock", "sk-test-key".to_string(), None);
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-cred", 5000, &handle)),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc.describe_image(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        assert!(inner.result.is_some());
        // The secret never appears in the response.
        assert!(!format!("{:?}", inner).contains("sk-test-key"));
    }

    #[tokio::test]
    async fn describe_image_timeout_returns_deadline_exceeded() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_failure(MockFailure::Slow(Duration::from_millis(500))));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-4", 50, "")), // 50ms timeout
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (timeout in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusDeadlineExceeded as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorProviderTimeout as i32);
        assert!(inner.result.is_none());
    }

    #[tokio::test]
    async fn describe_image_schema_validation_failure_returns_provider_error() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_failure(MockFailure::InvalidOutput));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-5", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (schema failure in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusProviderError as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorPayloadDecode as i32);
        assert!(
            inner.result.is_none(),
            "schema failure produces no active result"
        );
    }

    #[tokio::test]
    async fn describe_image_provider_error_returns_provider_error() {
        // Exercises the `ProviderError::Provider(_)` -> PROVIDER_ERROR / INTERNAL
        // arm of `provider_error_to_header`. The provider string is preserved so
        // the host can show the actionable, vault-redacted failure reason.
        let svc =
            svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
                .with_failure(MockFailure::Error));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-err", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (provider error in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusProviderError as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorInternal as i32);
        assert!(
            inner.result.is_none(),
            "provider error produces no active result"
        );
        assert!(h.error_message.contains("mock provider failed"));
    }

    #[tokio::test]
    async fn describe_image_transient_error_returns_provider_unavailable() {
        // Exercises the `ProviderError::Transient` -> PROVIDER_UNAVAILABLE /
        // PROVIDER_5XX arm of `provider_error_to_header`. Real 5xx / rate-limit
        // detection is a Phase 5c driver concern; this covers the service mapping.
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_failure(MockFailure::Transient));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-tx", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (transient in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(
            h.status,
            AiResponseStatus::AiStatusProviderUnavailable as i32
        );
        assert_eq!(h.error_code, AiErrorCode::AiErrorProvider5xx as i32);
        assert!(inner.result.is_none());
    }

    #[tokio::test]
    async fn describe_image_cancellation_returns_cancelled() {
        let svc = svc(
            MockImageAnalysisProvider::new("mock", "alcedo-mock").with_failure(
                // Long enough that the cancel branch wins the select! first.
                MockFailure::Slow(Duration::from_secs(30)),
            ),
        );
        let request_id = "req-cancel".to_string();
        let req = Request::new(DescribeImageRequest {
            header: Some(header(&request_id, 60000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });

        // Cancel before awaiting: register the id, then fire cancel from another task.
        let cancel_registry = svc.cancel_registry.clone();
        let rid = request_id.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(20)).await;
            cancel_registry.cancel(&rid);
        });

        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (cancel in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusCancelled as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorCancelledByClient as i32);
    }

    #[tokio::test]
    async fn describe_image_unknown_provider_returns_unsupported_task() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-6", 5000, "")),
            image_bytes: image_bytes(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "openrouter".to_string(), // not registered in 5b
            model_id: "qwen/qwen3.7-plus".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let resp = svc
            .describe_image(req)
            .await
            .expect("rpc ok (unsupported in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusUnsupportedTask as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorTaskUnknown as i32);
    }

    #[tokio::test]
    async fn describe_image_empty_bytes_is_transport_error() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(DescribeImageRequest {
            header: Some(header("req-7", 5000, "")),
            image_bytes: Vec::new(),
            image_format_hint: "rgba8:2x2".to_string(),
            rendition: rendition(),
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: "".to_string(),
            output_language: String::new(),
        });
        let err = svc
            .describe_image(req)
            .await
            .expect_err("empty image is a transport error");
        assert_eq!(err.code(), tonic::Code::InvalidArgument);
    }

    #[tokio::test]
    async fn batch_analyze_rejects_item_over_provider_payload_limit() {
        let svc =
            svc(MockImageAnalysisProvider::new("mock", "alcedo-mock").with_max_payload_bytes(3));
        let req = Request::new(BatchAnalyzeImageRequest {
            header: Some(header("batch-too-large", 5000, "")),
            items: vec![
                crate::proto::alcedo::ai::BatchAnalyzeImageItem {
                    request_id: "item-1".to_string(),
                    image_bytes: vec![1, 2, 3],
                    image_format_hint: "image/png".to_string(),
                    rendition: rendition(),
                    camera_context: String::new(),
                },
                crate::proto::alcedo::ai::BatchAnalyzeImageItem {
                    request_id: "item-2".to_string(),
                    image_bytes: vec![1, 2, 3, 4],
                    image_format_hint: "image/png".to_string(),
                    rendition: rendition(),
                    camera_context: String::new(),
                },
            ],
            provider_id: "mock".to_string(),
            model_id: "alcedo-mock".to_string(),
            prompt_profile_id: String::new(),
            include_understanding: true,
            include_rating: false,
            rubric_id: String::new(),
            output_language: String::new(),
            rating_severity: String::new(),
            include_rating_reasons: false,
        });
        let resp = svc
            .batch_analyze_image(req)
            .await
            .expect("rpc ok")
            .into_inner();
        let h = resp.header.expect("header");
        assert_eq!(h.status, AiResponseStatus::AiStatusPayloadTooLarge as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorPayloadDecode as i32);
        assert!(resp.items.is_empty());
        assert!(h.error_message.contains("batch item 1"));
    }

    // ----- Phase 6c: ListModels -----

    fn discovered() -> Vec<DiscoveredModel> {
        vec![
            DiscoveredModel {
                model_id: "gpt-4o".to_string(),
                display_name: "GPT-4o".to_string(),
                source_provider_id: "mock".to_string(),
            },
            DiscoveredModel {
                model_id: "gpt-4o-mini".to_string(),
                display_name: "gpt-4o-mini".to_string(),
                source_provider_id: "mock".to_string(),
            },
        ]
    }

    #[tokio::test]
    async fn list_models_returns_candidates_without_persisting() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_discovered_models(discovered()));
        let req = Request::new(ListModelsRequest {
            header: Some(header("list-1", 5000, "")),
            provider_id: "mock".to_string(),
        });
        let resp = svc.list_models(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        assert_eq!(inner.models.len(), 2);
        assert_eq!(inner.models[0].model_id, "gpt-4o");
        assert_eq!(inner.models[1].display_name, "gpt-4o-mini");
        assert_eq!(inner.models[0].source_provider_id, "mock");
    }

    #[tokio::test]
    async fn list_models_missing_credential_returns_unauthenticated() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_requires_credential(true)
            .with_discovered_models(discovered()));
        // No credential_ref -> the call must fail closed before list_models.
        let req = Request::new(ListModelsRequest {
            header: Some(header("list-2", 5000, "")),
            provider_id: "mock".to_string(),
        });
        let resp = svc.list_models(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        let h = inner.header.as_ref().unwrap();
        assert_eq!(h.status, AiResponseStatus::AiStatusUnauthenticated as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorMissingCredential as i32);
        assert!(inner.models.is_empty(), "no candidates on auth failure");
    }

    #[tokio::test]
    async fn list_models_unknown_provider_returns_unsupported_task() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock"));
        let req = Request::new(ListModelsRequest {
            header: Some(header("list-3", 5000, "")),
            provider_id: "no-such-provider".to_string(),
        });
        let resp = svc.list_models(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        let h = inner.header.as_ref().unwrap();
        assert_eq!(h.status, AiResponseStatus::AiStatusUnsupportedTask as i32);
        assert!(inner.models.is_empty());
    }

    #[tokio::test]
    async fn list_models_with_valid_credential_succeeds() {
        let svc = svc(MockImageAnalysisProvider::new("mock", "alcedo-mock")
            .with_requires_credential(true)
            .with_discovered_models(discovered()));
        let handle = svc.vault.register("mock", "sk-test-key".to_string(), None);
        let req = Request::new(ListModelsRequest {
            header: Some(header("list-4", 5000, &handle)),
            provider_id: "mock".to_string(),
        });
        let resp = svc.list_models(req).await.expect("rpc ok");
        let inner = resp.into_inner();
        assert_eq!(
            inner.header.as_ref().unwrap().status,
            AiResponseStatus::AiStatusOk as i32
        );
        assert_eq!(inner.models.len(), 2);
        assert!(!format!("{:?}", inner).contains("sk-test-key"));
    }

    #[test]
    fn mock_capability_advertises_no_credential_understanding() {
        let mock = MockImageAnalysisProvider::new("mock", "alcedo-mock");
        let cap = mock.capability();
        assert_eq!(cap.task_id, "image_understanding.describe");
        assert_eq!(cap.provider_id, "mock");
        assert_eq!(cap.model_id, "alcedo-mock");
        assert!(!cap.requires_credential);
        assert!(cap.supports_cancel);
        assert!(!cap.supports_batch);
        assert!(
            cap.input_kinds
                .contains(&(AiInputKind::AiInputPreview as i32))
        );
        assert!(
            cap.output_kinds
                .contains(&(AiOutputKind::AiOutputCaption as i32))
        );
    }
}
