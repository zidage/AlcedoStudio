use alcedo_mind::service::credential_vault::SecretString;
use alcedo_mind::service::image_analysis::{AnalyzeImageInput, ImageAnalysisProvider};
use alcedo_mind::service::provider_config::ProviderConfig;
use alcedo_mind::service::providers::openai_codex_oauth::auth::CodexOAuthSettings;
use alcedo_mind::service::providers::openai_codex_oauth::provider::OpenAiCodexOAuthProvider;
use serde_json::json;
use wiremock::matchers::{header, method, path, query_param};
use wiremock::{Mock, MockServer, ResponseTemplate};

#[tokio::test]
async fn describe_image_sends_codex_headers_and_responses_body() {
    let server = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/responses"))
        .and(header("authorization", "Bearer access-live"))
        .and(header("chatgpt-account-id", "acct_live"))
        .and(header("OpenAI-Beta", "responses=experimental"))
        .respond_with(sse_response(json!({
            "id": "resp_1",
            "output": [{
                "type": "message",
                "content": [{
                    "type": "output_text",
                    "text": r#"{"description":"A lit studio portrait","tags":["portrait"],"scene":"studio","confidence":0.91}"#
                }]
            }],
            "usage": {"input_tokens": 11, "output_tokens": 7, "total_tokens": 18}
        })).insert_header("x-request-id", "codex-req-1"))
        .mount(&server)
        .await;

    let provider = provider_for(&server, None).await;
    let out = provider
        .describe_image(&test_image_png(), "", "profile-1", "", Some(&live_secret()))
        .await
        .expect("describe succeeds");

    assert_eq!(out.caption, "A lit studio portrait");
    assert_eq!(out.tags, vec!["portrait"]);
    assert_eq!(out.scene, "studio");
    assert_eq!(out.provider_request_id, "codex-req-1");
    assert_eq!(out.usage.total_tokens, 18);

    let requests = server.received_requests().await.expect("requests recorded");
    let body: serde_json::Value =
        serde_json::from_slice(&requests[0].body).expect("request body JSON");
    assert_eq!(body["model"], "gpt-test-codex");
    assert_eq!(body["store"], false);
    assert_eq!(body["stream"], true);
    assert!(
        body.get("temperature").is_none(),
        "Codex request must not send temperature"
    );
    assert_eq!(body["text"]["format"]["type"], "json_schema");
    assert_eq!(body["text"]["format"]["name"], "alcedo_image_understanding");
    assert_eq!(body["input"][0]["role"], "system");
    assert_eq!(body["input"][1]["content"][1]["type"], "input_image");
    assert!(
        body["input"][1]["content"][1]["image_url"]
            .as_str()
            .expect("image URL")
            .starts_with("data:image/png;base64,")
    );
}

#[tokio::test]
async fn expired_profile_is_refreshed_before_call_and_cached_afterwards() {
    let backend = MockServer::start().await;
    let auth = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/oauth/token"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "access_token": "access-refreshed",
            "refresh_token": "refresh-rotated",
            "account_id": "acct_refreshed",
            "expires_in": 3600
        })))
        .expect(1)
        .mount(&auth)
        .await;
    Mock::given(method("POST"))
        .and(path("/responses"))
        .and(header("authorization", "Bearer access-refreshed"))
        .and(header("chatgpt-account-id", "acct_refreshed"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "id": "resp_cached",
            "output_text": r#"{"rating":4,"rating_reason":"Strong color and subject separation"}"#,
            "usage": {"input_tokens": 3, "output_tokens": 2, "total_tokens": 5}
        })))
        .expect(2)
        .mount(&backend)
        .await;

    let provider = provider_for(
        &backend,
        Some(CodexOAuthSettings {
            token_url: format!("{}/oauth/token", auth.uri()),
            ..CodexOAuthSettings::default()
        }),
    )
    .await;
    let secret = expired_secret();
    for _ in 0..2 {
        let out = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "",
                "normal",
                "",
                "",
                true,
                Some(&secret),
            )
            .await
            .expect("score succeeds with refreshed token");
        assert_eq!(out.rating, 4);
    }
}

#[tokio::test]
async fn list_models_reads_codex_catalog_shape() {
    let server = MockServer::start().await;
    Mock::given(method("GET"))
        .and(path("/models"))
        .and(query_param("client_version", "0.111.0"))
        .and(header("authorization", "Bearer access-live"))
        .and(header("chatgpt-account-id", "acct_live"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "models": [
                {"slug": "gpt-5.3-codex", "display_name": "GPT-5.3 Codex"},
                {"slug": "gpt-5.4", "display_name": "GPT-5.4"}
            ]
        })))
        .mount(&server)
        .await;

    let provider = provider_for(&server, None).await;
    let models = provider
        .list_models(Some(&live_secret()))
        .await
        .expect("models list succeeds");

    assert_eq!(models.len(), 2);
    assert_eq!(models[0].model_id, "gpt-5.3-codex");
    assert_eq!(models[0].display_name, "GPT-5.3 Codex");
    assert_eq!(models[0].source_provider_id, "openai_codex_oauth_test");
}

#[tokio::test]
async fn list_models_defaults_codex_slug_parser_when_profile_only_sets_data_pointer() {
    let server = MockServer::start().await;
    Mock::given(method("GET"))
        .and(path("/models"))
        .and(query_param("client_version", "0.111.0"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "models": [
                {"slug": "gpt-5.4", "display_name": "GPT-5.4"},
                {"slug": "gpt-5.4-mini", "display_name": "GPT-5.4 Mini"}
            ]
        })))
        .mount(&server)
        .await;

    let mut config = codex_provider_config(&server);
    config.models_response.id_json_pointer = None;
    config.models_response.display_name_json_pointer = None;
    let provider = provider_from_config(config, None);

    let models = provider
        .list_models(Some(&live_secret()))
        .await
        .expect("models list succeeds");

    assert_eq!(models.len(), 2);
    assert_eq!(models[0].model_id, "gpt-5.4");
    assert_eq!(models[1].display_name, "GPT-5.4 Mini");
}

#[tokio::test]
async fn batch_analyze_uses_single_codex_responses_request() {
    let server = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/responses"))
        .respond_with(sse_response(json!({
            "id": "resp_batch",
            "output_text": r#"{"results":[{"index":0,"description":"First image","rating":3,"rating_reason":"Solid"},{"index":1,"description":"Second image","rating":5,"rating_reason":"Excellent"}]}"#,
            "usage": {"input_tokens": 20, "output_tokens": 9, "total_tokens": 29}
        })))
        .expect(1)
        .mount(&server)
        .await;

    let provider = provider_for(&server, None).await;
    let img = test_image_png();
    let images = [
        AnalyzeImageInput {
            image_bytes: &img,
            camera_context: "50mm",
        },
        AnalyzeImageInput {
            image_bytes: &img,
            camera_context: "35mm",
        },
    ];
    let out = provider
        .batch_analyze_images(
            &images,
            "",
            "",
            "",
            "normal",
            "",
            true,
            true,
            true,
            Some(&live_secret()),
        )
        .await
        .expect("batch succeeds");

    assert_eq!(out.items.len(), 2);
    assert_eq!(
        out.items[1]
            .understanding
            .as_ref()
            .expect("understanding")
            .caption,
        "Second image"
    );
    assert_eq!(out.items[1].rating.as_ref().expect("rating").rating, 5);

    let requests = server.received_requests().await.expect("requests recorded");
    let body: serde_json::Value =
        serde_json::from_slice(&requests[0].body).expect("request body JSON");
    assert_eq!(
        body["text"]["format"]["name"],
        "alcedo_image_analysis_batch"
    );
    assert_eq!(body["stream"], true);
    let content = body["input"][1]["content"]
        .as_array()
        .expect("content array");
    let image_blocks = content
        .iter()
        .filter(|block| block["type"] == "input_image")
        .count();
    assert_eq!(image_blocks, 2);
}

fn sse_response(response: serde_json::Value) -> ResponseTemplate {
    let body = [
        "event: response.created".to_string(),
        r#"data: {"response":{"id":"resp_pending","status":"in_progress"}}"#.to_string(),
        String::new(),
        "event: response.output_text.delta".to_string(),
        r#"data: {"delta":"ignored when completed response has output"}"#.to_string(),
        String::new(),
        "event: response.completed".to_string(),
        format!(
            "data: {}",
            serde_json::to_string(&json!({ "response": response })).expect("SSE response JSON")
        ),
        String::new(),
    ]
    .join("\n");
    ResponseTemplate::new(200)
        .insert_header("content-type", "text/event-stream")
        .set_body_string(body)
}

async fn provider_for(
    server: &MockServer,
    oauth_settings: Option<CodexOAuthSettings>,
) -> OpenAiCodexOAuthProvider {
    provider_from_config(codex_provider_config(server), oauth_settings)
}

fn codex_provider_config(server: &MockServer) -> ProviderConfig {
    serde_json::from_value(json!({
        "schema_version": 1,
        "provider_id": "openai_codex_oauth_test",
        "display_name": "OpenAI Codex OAuth Test",
        "driver": "openai_codex_oauth",
        "base_url": server.uri(),
        "endpoint": "/responses",
        "models_endpoint": "/models",
        "models_response": {
            "data_json_pointer": "/models",
            "id_json_pointer": "/slug",
            "display_name_json_pointer": "/display_name"
        },
        "auth": {"type": "bearer", "credential_slot": "openai_codex_oauth"},
        "defaults": {"model": "gpt-test-codex", "stream": false, "temperature": 0.2},
        "structured_output": {"mode": "responses_json_schema", "strict": true},
        "response": {
            "content_json_pointer": "/output_text",
            "usage_json_pointer": "/usage",
            "provider_request_id_json_pointer": "/id",
            "provider_request_id_header": "x-request-id"
        },
        "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
        "models": [{
            "slug": "gpt-test-codex",
            "display_name": "GPT Test Codex",
            "supports_vision": true,
            "supports_structured_output": true,
            "live_confirmed": true,
            "max_image_bytes": 4194304,
            "recommended_rendition": "preview"
        }]
    }))
    .expect("config shape")
}

fn provider_from_config(
    config: ProviderConfig,
    oauth_settings: Option<CodexOAuthSettings>,
) -> OpenAiCodexOAuthProvider {
    OpenAiCodexOAuthProvider::with_clients(
        config,
        reqwest::Client::new(),
        reqwest::Client::new(),
        oauth_settings.unwrap_or_default(),
    )
}

fn live_secret() -> SecretString {
    SecretString::new(
        json!({
            "access_token": "access-live",
            "refresh_token": "refresh-live",
            "account_id": "acct_live",
            "expires_at": 4_000_000_000u64
        })
        .to_string(),
    )
}

fn expired_secret() -> SecretString {
    SecretString::new(
        json!({
            "access_token": "access-expired",
            "refresh_token": "refresh-old",
            "account_id": "acct_old",
            "expires_at": 1u64
        })
        .to_string(),
    )
}

fn test_image_png() -> Vec<u8> {
    let img = image::RgbImage::from_pixel(2, 2, image::Rgb([32, 64, 128]));
    let mut cursor = std::io::Cursor::new(Vec::new());
    image::DynamicImage::ImageRgb8(img)
        .write_to(&mut cursor, image::ImageFormat::Png)
        .expect("encode png");
    cursor.into_inner()
}
