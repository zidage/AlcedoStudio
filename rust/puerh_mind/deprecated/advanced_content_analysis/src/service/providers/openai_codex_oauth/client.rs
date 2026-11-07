use reqwest::header::HeaderMap;
use serde_json::Value;

use crate::service::image_analysis::{DiscoveredModel, ProviderError};
use crate::service::provider_config::{ModelListResponseConfig, ProviderConfig};
use crate::service::providers::http_util::{
    MAX_TRANSIENT_RETRIES, parse_discovered_models, read_response, send_get_with_retry,
    send_with_retry,
};
use crate::service::providers::openai_codex_oauth::auth::CodexOAuthProfile;
use crate::service::providers::openai_codex_oauth::stream::collect_completed_response_from_sse;

pub const DEFAULT_CODEX_CLIENT_VERSION: &str = "0.111.0";

#[derive(Clone)]
pub struct CodexBackendClient {
    http: reqwest::Client,
    codex_client_version: String,
}

impl CodexBackendClient {
    pub fn new(http: reqwest::Client) -> Self {
        Self {
            http,
            codex_client_version: DEFAULT_CODEX_CLIENT_VERSION.to_string(),
        }
    }

    pub fn with_codex_client_version(
        http: reqwest::Client,
        codex_client_version: impl Into<String>,
    ) -> Self {
        Self {
            http,
            codex_client_version: codex_client_version.into(),
        }
    }

    pub async fn post_responses(
        &self,
        config: &ProviderConfig,
        auth: &CodexOAuthProfile,
        body: &Value,
    ) -> Result<(HeaderMap, Value), ProviderError> {
        let mut body = body.clone();
        body["stream"] = Value::Bool(true);
        let resp = send_with_retry(
            &self.http,
            &responses_url(config),
            &body,
            &codex_headers(auth),
            Some(&auth.access_token),
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        if response_is_json(&resp) {
            return read_response(resp).await;
        }
        collect_completed_response_from_sse(resp).await
    }

    pub async fn list_models(
        &self,
        config: &ProviderConfig,
        auth: &CodexOAuthProfile,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        let resp = send_get_with_retry(
            &self.http,
            &models_url(config, &self.codex_client_version),
            &codex_headers(auth),
            Some(&auth.access_token),
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (_, body) = read_response(resp).await?;
        let parser = model_list_parser(config);
        parse_discovered_models(&body, &parser, &config.provider_id, "OpenAI Codex OAuth")
    }
}

pub fn codex_headers(auth: &CodexOAuthProfile) -> Vec<(String, String)> {
    vec![
        ("chatgpt-account-id".to_string(), auth.account_id.clone()),
        (
            "OpenAI-Beta".to_string(),
            "responses=experimental".to_string(),
        ),
    ]
}

fn response_is_json(resp: &reqwest::Response) -> bool {
    resp.headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        .map(|v| v.to_ascii_lowercase().contains("application/json"))
        .unwrap_or(false)
}

fn responses_url(config: &ProviderConfig) -> String {
    format!("{}{}", config.base_url, config.endpoint)
}

fn models_url(config: &ProviderConfig, codex_client_version: &str) -> String {
    let endpoint = config.models_endpoint.as_deref().unwrap_or("/models");
    format!(
        "{}{}?client_version={}",
        config.base_url, endpoint, codex_client_version
    )
}

fn model_list_parser(config: &ProviderConfig) -> ModelListResponseConfig {
    let mut parser = config.models_response.clone();
    if parser.data_json_pointer.is_none() {
        parser.data_json_pointer = Some("/models".to_string());
    }
    if parser.id_json_pointer.is_none() {
        parser.id_json_pointer = Some("/slug".to_string());
    }
    if parser.display_name_json_pointer.is_none() {
        parser.display_name_json_pointer = Some("/display_name".to_string());
    }
    parser
}
