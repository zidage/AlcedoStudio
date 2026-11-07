//! OpenRouter Chat Completions image-analysis driver (Phase 5c, `openrouter_chat`).
//!
//! As of Phase 6b the OpenAI Chat-compatible implementation lives in
//! [`super::openai_chat_compatible`] as `OpenAiChatCompatibleProvider`. The
//! OpenRouter-specific routing/attribution knobs (`attribution_headers`,
//! `structured_output.provider_require_parameters`, per-model `data_collection`)
//! are config-gated there, so they are emitted for the OpenRouter built-in config
//! and omitted for an Opencode / plain OpenAI-compatible preset. `openrouter_chat`
//! and `openai_chat_compatible` dispatch to the same implementation; the
//! `OpenRouterChatProvider` name is kept as a type alias so existing call sites
//! (`build_one`, `live_smoke`, these tests) compile unchanged. Phase 6g may retire
//! the `openrouter_chat` driver id once the generic driver is the only product
//! path.
//!
//! See [`super::openai_chat_compatible`] for the request/response contract, the
//! fail-closed structured-output policy, and the redaction discipline.

pub type OpenRouterChatProvider = super::openai_chat_compatible::OpenAiChatCompatibleProvider;

#[cfg(test)]
mod tests {
    use super::OpenRouterChatProvider;
    use crate::proto::alcedo::ai::image_analysis_service_server::ImageAnalysisService;
    use crate::service::credential_vault::SecretString;
    use crate::service::image_analysis::{
        ImageAnalysisProvider, ProviderError, validate_rating, validate_understanding,
    };
    use crate::service::provider_config::load_provider_configs;
    use serde_json::{Value, json};
    use std::collections::HashMap;
    use std::sync::Arc;
    use wiremock::matchers::{header, method, path};
    use wiremock::{Mock, MockServer, ResponseTemplate};

    const TEST_SECRET: &str = "or-test-key-DO-NOT-LEAK";
    const TEST_PROMPT: &str = "Describe this image for a photo library.";
    const RAW_BODY_SENTINEL: &str = "RAW_PROVIDER_BODY_SENTINEL";

    fn test_image_png() -> Vec<u8> {
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([10, 20, 30]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        cursor.into_inner()
    }

    fn provider_for(server: &MockServer) -> OpenRouterChatProvider {
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("openrouter")
            .expect("openrouter built-in")
            .clone();
        config.base_url = server.uri();
        OpenRouterChatProvider::new(config).expect("provider builds")
    }

    fn ok_understanding_body(content_json: &str) -> serde_json::Value {
        json!({
            "id": "or-req-123",
            "choices": [
                { "message": { "content": content_json } }
            ],
            "usage": { "prompt_tokens": 100, "completion_tokens": 40, "total_tokens": 140 }
        })
    }

    fn secret() -> SecretString {
        SecretString::new(TEST_SECRET.to_string())
    }

    #[tokio::test]
    async fn sends_bearer_authorization_and_attribution_headers() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .and(header("authorization", format!("Bearer {TEST_SECRET}")))
            .and(header("http-referer", "https://alcedo.studio"))
            .and(header("x-openrouter-title", "Alcedo Studio"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                r#"{"caption":"a small image","tags":["test"],"scene":"studio","confidence":0.7}"#,
            )))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "profile-1", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "a small image");
        assert_eq!(out.tags, vec!["test".to_string()]);
        assert_eq!(out.provider_request_id, "or-req-123");
        assert_eq!(out.usage.total_tokens, 140);
    }

    #[tokio::test]
    async fn request_body_has_structured_output_and_require_parameters() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        provider
            .describe_image(
                &test_image_png(),
                "qwen/qwen3.7-plus",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(body["model"], "qwen/qwen3.7-plus");
        assert_eq!(body["stream"], false);
        assert_eq!(
            body["tools"][0]["function"]["name"],
            "alcedo_image_understanding"
        );
        assert_eq!(
            body["tool_choice"]["function"]["name"],
            "alcedo_image_understanding"
        );
        assert_eq!(body["tools"][0]["function"]["strict"], true);
        assert_eq!(body["parallel_tool_calls"], false);
        // The code-owned schema is injected (sanitized to strict-compatible: all
        // properties required, additionalProperties false, constraints dropped).
        let schema = &body["tools"][0]["function"]["parameters"];
        assert_eq!(schema["type"], "object");
        assert_eq!(schema["additionalProperties"], false);
        let required: Vec<&str> = schema["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert_eq!(required, vec!["description"]);
        // provider.require_parameters + data_collection=deny (built-in qwen model).
        assert_eq!(body["provider"]["require_parameters"], true);
        assert_eq!(body["provider"]["data_collection"], "deny");
        // The image is carried as a data URI in the user message.
        let img_url = &body["messages"][1]["content"][1]["image_url"]["url"];
        assert!(
            img_url
                .as_str()
                .unwrap()
                .starts_with("data:image/png;base64,")
        );
    }

    #[tokio::test]
    async fn parses_understanding_response_and_captures_usage() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                r#"{"caption":"sunrise over mountains","tags":["sunrise","mountain","landscape"],"scene":"outdoor","confidence":0.82}"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "sunrise over mountains");
        assert_eq!(out.tags.len(), 3);
        assert_eq!(out.scene, "outdoor");
        assert!((out.confidence - 0.82).abs() < 1e-9);
        assert_eq!(out.model_id, "qwen/qwen3.7-plus");
        assert_eq!(out.usage.input_tokens, 100);
        assert_eq!(out.usage.output_tokens, 40);
        assert_eq!(out.usage.total_tokens, 140);
        // validate_understanding passed (returned Ok).
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[tokio::test]
    async fn parses_rating_response_and_captures_usage() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "or-req-456",
            "choices": [ { "message": { "content":
                r#"{"rating":4,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"good"}"# } } ],
            "usage": { "prompt_tokens": 80, "completion_tokens": 50, "total_tokens": 130 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "alcedo-default-v1",
                "",
                "",
                "",
                true,
                Some(&secret()),
            )
            .await
            .expect("score ok");
        // Single 1..=5 integer star rating; no scores array, no confidence.
        assert_eq!(out.rating, 4);
        assert_eq!(out.rubric_id, "alcedo-default-v1");
        assert_eq!(out.rubric_version, "1");
        assert_eq!(out.reasons, "good");
        assert_eq!(out.usage.total_tokens, 130);
        assert_eq!(out.provider_request_id, "or-req-456");
        validate_rating(&out).expect("canned rating validates");
    }

    #[tokio::test]
    async fn parses_rating_accepts_float_rating_as_integer() {
        // A model may emit `4.0` despite the `integer` schema; the parser accepts
        // the float form and coerces it to the integer 4. validate_rating still
        // enforces the 1..=5 range.
        let server = MockServer::start().await;
        let body = json!({
            "id": "or-req-float",
            "choices": [ { "message": { "content":
                r#"{"rating":5.0,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"great"}"# } } ]
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "alcedo-default-v1",
                "",
                "",
                "",
                true,
                Some(&secret()),
            )
            .await
            .expect("score ok");
        assert_eq!(out.rating, 5);
        validate_rating(&out).expect("float rating coerces and validates");
    }

    #[tokio::test]
    async fn parses_rating_rejects_fractional_float() {
        // A fractional rating (4.9) is schema-invalid. It must NOT be truncated
        // to 4 and pass validation — fail closed: the parser yields the
        // out-of-contract sentinel 0 and validate_rating maps it to
        // SchemaValidation (no active annotation).
        let server = MockServer::start().await;
        let body = json!({
            "id": "or-req-frac",
            "choices": [ { "message": { "content":
                r#"{"rating":4.9,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"x"}"# } } ]
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "alcedo-default-v1",
                "",
                "",
                "",
                true,
                Some(&secret()),
            )
            .await
            .expect_err("fractional rating rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn out_of_range_rating_maps_to_schema_validation() {
        // 0 is the app's "unrated" sentinel and outside the 1..=5 remote contract;
        // validate_rating rejects it (no active result).
        let server = MockServer::start().await;
        let body = json!({
            "id": "or-req-zero",
            "choices": [ { "message": { "content":
                r#"{"rating":0,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":""}"# } } ]
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "alcedo-default-v1",
                "",
                "",
                "",
                true,
                Some(&secret()),
            )
            .await
            .expect_err("rating 0 rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn rate_limit_maps_to_transient() {
        let server = MockServer::start().await;
        // 429 twice (initial + 1 retry), no 200 mock -> Transient after exhaustion.
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(429))
            .up_to_n_times(2)
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("transient after retries");
        assert_eq!(err, ProviderError::Transient);
        // Two attempts: initial + 1 retry.
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn server_500_is_retried_then_succeeds() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(500))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("succeeds after retry");
        assert_eq!(out.caption, "c");
        // Exactly 2 requests: the failed 500 + the successful retry.
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn client_4xx_is_not_retried_and_maps_to_provider_error() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(400)
                    .set_body_json(json!({ "error": { "message": RAW_BODY_SENTINEL } })),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("400 not retried");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        // No retry: exactly one request.
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
        // The raw provider body is NOT surfaced in the error string.
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body leaked: {err}"
        );
    }

    #[tokio::test]
    async fn schema_failure_does_not_produce_active_result() {
        let server = MockServer::start().await;
        // Valid JSON but violates the understanding contract: empty description.
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_understanding_body(r#"{"description":""}"#)),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("empty description rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn non_json_content_maps_to_schema_validation() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body("not json")),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("fenced json rejected");
        assert!(matches!(err, ProviderError::SchemaValidationMessage(_)));
    }

    #[tokio::test]
    async fn bearer_required_without_credential_errors() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", None)
            .await
            .expect_err("missing credential");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        // No request was sent (the credential check fails before HTTP).
        assert!(server.received_requests().await.unwrap().is_empty());
    }

    #[test]
    fn no_secret_image_prompt_or_body_in_logs_or_error_strings() {
        // Capture tracing output emitted during a call that triggers a retry (warn)
        // then a 400 with a raw body sentinel. A current_thread runtime drives the
        // whole call on THIS thread, so the thread-local capturing subscriber set by
        // `with_default` is in scope for every `warn!` the driver emits. A
        // multi_thread runtime could poll the future's continuation on a worker
        // thread where the subscriber is NOT set, leaving `captured` empty and the
        // no-leak assertions passing trivially (a false pass). The first assertion
        // confirms capture actually worked; the rest confirm nothing leaked.
        use std::io::Write;
        use std::sync::Mutex;
        use tracing_subscriber::fmt::MakeWriter;

        #[derive(Clone)]
        struct BufWriter(Arc<Mutex<Vec<u8>>>);
        impl<'a> MakeWriter<'a> for BufWriter {
            type Writer = BufWriteImpl;
            fn make_writer(&'a self) -> Self::Writer {
                BufWriteImpl {
                    inner: self.0.clone(),
                }
            }
        }
        struct BufWriteImpl {
            inner: Arc<Mutex<Vec<u8>>>,
        }
        impl Write for BufWriteImpl {
            fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
                self.inner.lock().unwrap().extend_from_slice(b);
                Ok(b.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        let buf = Arc::new(Mutex::new(Vec::new()));
        let subscriber = tracing_subscriber::fmt()
            .with_writer(BufWriter(buf.clone()))
            .with_ansi(false)
            .with_env_filter("warn")
            .finish();

        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("current_thread runtime");
        let err = tracing::subscriber::with_default(subscriber, || {
            rt.block_on(async {
                let server = MockServer::start().await;
                // 500 once (triggers a retry warn), then 400 with a raw-body sentinel.
                Mock::given(method("POST"))
                    .and(path("/chat/completions"))
                    .respond_with(ResponseTemplate::new(500))
                    .up_to_n_times(1)
                    .mount(&server)
                    .await;
                Mock::given(method("POST"))
                    .and(path("/chat/completions"))
                    .respond_with(
                        ResponseTemplate::new(400)
                            .set_body_json(json!({ "error": { "message": RAW_BODY_SENTINEL } })),
                    )
                    .mount(&server)
                    .await;
                let provider = provider_for(&server);
                provider
                    .describe_image(&test_image_png(), "", "profile-1", "", Some(&secret()))
                    .await
            })
        });
        let err = err.expect_err("400 after retry");

        let captured = String::from_utf8_lossy(&buf.lock().unwrap()).to_string();
        // Capture worked: the retry warn was emitted and reached the buffer.
        assert!(
            captured.contains("retrying"),
            "log capture did not work (no retry warn captured): {captured}"
        );
        // The captured logs must not leak the secret, image, prompt, or raw body.
        assert!(
            !captured.contains(TEST_SECRET),
            "secret in logs: {captured}"
        );
        assert!(
            !captured.contains("data:image/png;base64,"),
            "image in logs: {captured}"
        );
        assert!(
            !captured.contains(TEST_PROMPT),
            "prompt in logs: {captured}"
        );
        assert!(
            !captured.contains(RAW_BODY_SENTINEL),
            "raw body in logs: {captured}"
        );
        // The error string must not leak either.
        assert!(
            !err.to_string().contains(TEST_SECRET),
            "secret in error: {err}"
        );
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body in error: {err}"
        );
    }

    #[tokio::test]
    async fn cancellation_drops_in_flight_request() {
        use crate::proto::alcedo::ai::{
            AiErrorCode, AiPriority, AiRequestHeader, AiResponseStatus, DescribeImageRequest,
            RenditionMetadata as ProtoRendition,
        };
        use crate::server::image_analysis::ImageAnalysisServiceImpl;
        use crate::service::cancellation_registry::CancellationRegistry;
        use crate::service::credential_vault::CredentialVault;

        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_understanding_body(
                        r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                    ))
                    .set_delay(std::time::Duration::from_secs(2)),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let vault = Arc::new(CredentialVault::new(None));
        let cancel_registry = Arc::new(CancellationRegistry::new());
        let mut providers: HashMap<String, Arc<dyn ImageAnalysisProvider>> = HashMap::new();
        let pid = provider.provider_id().to_string();
        providers.insert(pid.clone(), Arc::new(provider));
        let svc =
            ImageAnalysisServiceImpl::new(providers, pid, vault.clone(), cancel_registry.clone());

        let handle = vault.register("openrouter", TEST_SECRET.to_string(), None);
        let request_id = "req-cancel-or".to_string();
        let req = DescribeImageRequest {
            header: Some(AiRequestHeader {
                request_id: request_id.clone(),
                task_id: "image_understanding.describe".to_string(),
                timeout_ms: 60_000,
                priority: AiPriority::Normal as i32,
                trace_id: String::new(),
                credential_ref: handle,
                client_capabilities: vec![],
            }),
            image_bytes: test_image_png(),
            image_format_hint: "image/png".to_string(),
            rendition: Some(ProtoRendition {
                kind: "preview".to_string(),
                width: 2,
                height: 2,
                bytes: 64,
            }),
            provider_id: "openrouter".to_string(),
            model_id: "qwen/qwen3.7-plus".to_string(),
            prompt_profile_id: String::new(),
            output_language: String::new(),
        };

        let cancel_registry2 = cancel_registry.clone();
        let rid = request_id.clone();
        tokio::spawn(async move {
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
            cancel_registry2.cancel(&rid);
        });

        let resp = svc
            .describe_image(tonic::Request::new(req))
            .await
            .expect("rpc ok");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusCancelled as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorCancelledByClient as i32);
        assert!(inner.result.is_none());
    }

    #[tokio::test]
    async fn timeout_returns_deadline_exceeded() {
        use crate::proto::alcedo::ai::{
            AiErrorCode, AiPriority, AiRequestHeader, AiResponseStatus, DescribeImageRequest,
            RenditionMetadata as ProtoRendition,
        };
        use crate::server::image_analysis::ImageAnalysisServiceImpl;
        use crate::service::cancellation_registry::CancellationRegistry;
        use crate::service::credential_vault::CredentialVault;

        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_understanding_body(
                        r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                    ))
                    .set_delay(std::time::Duration::from_secs(2)),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let vault = Arc::new(CredentialVault::new(None));
        let cancel_registry = Arc::new(CancellationRegistry::new());
        let mut providers: HashMap<String, Arc<dyn ImageAnalysisProvider>> = HashMap::new();
        let pid = provider.provider_id().to_string();
        providers.insert(pid.clone(), Arc::new(provider));
        let svc =
            ImageAnalysisServiceImpl::new(providers, pid, vault.clone(), cancel_registry.clone());

        let handle = vault.register("openrouter", TEST_SECRET.to_string(), None);
        let req = DescribeImageRequest {
            header: Some(AiRequestHeader {
                request_id: "req-timeout-or".into(),
                task_id: "image_understanding.describe".into(),
                timeout_ms: 80, // 80ms; the mock delays 2s.
                priority: AiPriority::Normal as i32,
                trace_id: String::new(),
                credential_ref: handle,
                client_capabilities: vec![],
            }),
            image_bytes: test_image_png(),
            image_format_hint: "image/png".to_string(),
            rendition: Some(ProtoRendition {
                kind: "preview".to_string(),
                width: 2,
                height: 2,
                bytes: 64,
            }),
            provider_id: "openrouter".to_string(),
            model_id: "qwen/qwen3.7-plus".to_string(),
            prompt_profile_id: String::new(),
            output_language: String::new(),
        };

        let resp = svc
            .describe_image(tonic::Request::new(req))
            .await
            .expect("rpc ok (timeout in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusDeadlineExceeded as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorProviderTimeout as i32);
        assert!(inner.result.is_none());
    }
}
