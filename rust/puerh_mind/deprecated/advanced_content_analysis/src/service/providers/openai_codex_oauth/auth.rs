use base64::Engine;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crate::service::credential_vault::SecretString;
use crate::service::image_analysis::ProviderError;

pub const DEFAULT_CLIENT_ID: &str = "app_EMoamEEZ73f0CkXaXp7hrann";
pub const DEFAULT_ISSUER: &str = "https://auth.openai.com";
pub const DEFAULT_REDIRECT_URI: &str = "http://127.0.0.1:1455/auth/callback";
pub const DEFAULT_SCOPE: &str = "openid profile email offline_access";

const REFRESH_MARGIN: Duration = Duration::from_secs(5 * 60);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexOAuthSettings {
    pub client_id: String,
    pub issuer: String,
    pub token_url: String,
    pub redirect_uri: String,
    pub scope: String,
}

impl Default for CodexOAuthSettings {
    fn default() -> Self {
        Self {
            client_id: DEFAULT_CLIENT_ID.to_string(),
            issuer: DEFAULT_ISSUER.to_string(),
            token_url: format!("{DEFAULT_ISSUER}/oauth/token"),
            redirect_uri: DEFAULT_REDIRECT_URI.to_string(),
            scope: DEFAULT_SCOPE.to_string(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PkcePair {
    pub verifier: String,
    pub challenge: String,
}

impl PkcePair {
    pub fn generate() -> Self {
        let verifier = format!(
            "{}{}{}",
            uuid::Uuid::new_v4().simple(),
            uuid::Uuid::new_v4().simple(),
            uuid::Uuid::new_v4().simple()
        );
        let digest = Sha256::digest(verifier.as_bytes());
        let challenge = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(digest);
        Self {
            verifier,
            challenge,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthorizationRequest {
    pub state: String,
    pub pkce: PkcePair,
    pub url: String,
}

impl AuthorizationRequest {
    pub fn new(settings: &CodexOAuthSettings) -> Result<Self, ProviderError> {
        let state = uuid::Uuid::new_v4().to_string();
        let pkce = PkcePair::generate();
        let mut url = reqwest::Url::parse(&format!(
            "{}/oauth/authorize",
            settings.issuer.trim_end_matches('/')
        ))
        .map_err(|err| ProviderError::Provider(format!("invalid OAuth issuer URL: {err}")))?;
        url.query_pairs_mut()
            .append_pair("response_type", "code")
            .append_pair("client_id", &settings.client_id)
            .append_pair("redirect_uri", &settings.redirect_uri)
            .append_pair("scope", &settings.scope)
            .append_pair("state", &state)
            .append_pair("code_challenge", &pkce.challenge)
            .append_pair("code_challenge_method", "S256");
        Ok(Self {
            state,
            pkce,
            url: url.to_string(),
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct CodexOAuthProfile {
    pub access_token: String,
    #[serde(default)]
    pub refresh_token: Option<String>,
    #[serde(default)]
    pub id_token: Option<String>,
    pub account_id: String,
    #[serde(default)]
    pub expires_at: Option<u64>,
}

impl CodexOAuthProfile {
    pub fn from_secret(secret: &SecretString) -> Result<Self, ProviderError> {
        Self::from_str(secret.expose())
    }

    pub fn from_str(raw: &str) -> Result<Self, ProviderError> {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            return Err(ProviderError::Provider(
                "OpenAI Codex OAuth credential is empty".to_string(),
            ));
        }
        if !trimmed.starts_with('{') {
            return Err(ProviderError::Provider(
                "OpenAI Codex OAuth credential must be a JSON token profile".to_string(),
            ));
        }
        let value: Value = serde_json::from_str(trimmed).map_err(|err| {
            ProviderError::Provider(format!(
                "OpenAI Codex OAuth credential JSON is invalid: {err}"
            ))
        })?;
        Self::from_value(&value)
    }

    pub fn from_value(value: &Value) -> Result<Self, ProviderError> {
        let tokens = value.get("tokens").unwrap_or(value);
        let access_token = required_string(tokens, "access_token")?;
        let refresh_token = optional_string(tokens, "refresh_token");
        let id_token = optional_string(tokens, "id_token");
        let account_id = optional_string(tokens, "account_id")
            .or_else(|| optional_string(tokens, "accountId"))
            .or_else(|| id_token.as_deref().and_then(derive_account_id))
            .or_else(|| derive_account_id(&access_token))
            .ok_or_else(|| {
                ProviderError::Provider(
                    "OpenAI Codex OAuth credential is missing account_id".to_string(),
                )
            })?;
        let expires_at = optional_u64(tokens, "expires_at")
            .or_else(|| optional_u64(tokens, "expires"))
            .or_else(|| jwt_exp(&access_token));
        Ok(Self {
            access_token,
            refresh_token,
            id_token,
            account_id,
            expires_at,
        })
    }

    pub fn needs_refresh(&self, now_unix: u64) -> bool {
        if self.access_token.is_empty() {
            return true;
        }
        let expires_at = self.expires_at.or_else(|| jwt_exp(&self.access_token));
        match expires_at {
            Some(exp) => exp <= now_unix.saturating_add(REFRESH_MARGIN.as_secs()),
            None => false,
        }
    }

    pub fn fingerprint_source(&self) -> String {
        let source = self
            .refresh_token
            .as_deref()
            .unwrap_or(self.access_token.as_str());
        let digest = Sha256::digest(source.as_bytes());
        base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(digest)
    }

    fn merged_with_refresh_payload(
        &self,
        payload: &Value,
        now_unix: u64,
    ) -> Result<Self, ProviderError> {
        let access_token = required_string(payload, "access_token")?;
        let refresh_token =
            optional_string(payload, "refresh_token").or(self.refresh_token.clone());
        let id_token = optional_string(payload, "id_token").or(self.id_token.clone());
        let expires_at = optional_u64(payload, "expires_at")
            .or_else(|| optional_u64(payload, "expires_in").map(|expires_in| now_unix + expires_in))
            .or_else(|| jwt_exp(&access_token));
        let account_id = optional_string(payload, "account_id")
            .or_else(|| optional_string(payload, "accountId"))
            .or_else(|| id_token.as_deref().and_then(derive_account_id))
            .or_else(|| derive_account_id(&access_token))
            .unwrap_or_else(|| self.account_id.clone());
        if account_id.trim().is_empty() {
            return Err(ProviderError::Provider(
                "OpenAI Codex OAuth token response did not include an account id".to_string(),
            ));
        }
        Ok(Self {
            access_token,
            refresh_token,
            id_token,
            account_id,
            expires_at,
        })
    }
}

pub async fn exchange_authorization_code(
    client: &reqwest::Client,
    settings: &CodexOAuthSettings,
    code: &str,
    verifier: &str,
    now_unix: u64,
) -> Result<CodexOAuthProfile, ProviderError> {
    let body = json!({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": settings.redirect_uri,
        "client_id": settings.client_id,
        "code_verifier": verifier,
    });
    let payload = post_token_request(client, &settings.token_url, &body).await?;
    CodexOAuthProfile {
        access_token: String::new(),
        refresh_token: None,
        id_token: None,
        account_id: String::new(),
        expires_at: None,
    }
    .merged_with_refresh_payload(&payload, now_unix)
}

pub async fn refresh_profile(
    client: &reqwest::Client,
    settings: &CodexOAuthSettings,
    profile: &CodexOAuthProfile,
    now_unix: u64,
) -> Result<CodexOAuthProfile, ProviderError> {
    let refresh_token = profile.refresh_token.as_deref().ok_or_else(|| {
        ProviderError::Provider("OpenAI Codex OAuth refresh token is missing".to_string())
    })?;
    let body = json!({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": settings.client_id,
        "scope": settings.scope,
    });
    let payload = post_token_request(client, &settings.token_url, &body).await?;
    profile.merged_with_refresh_payload(&payload, now_unix)
}

pub fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

pub fn derive_account_id(token: &str) -> Option<String> {
    let claims = parse_jwt_claims(token)?;
    if let Some(auth) = claims
        .get("https://api.openai.com/auth")
        .and_then(|v| v.as_object())
    {
        if let Some(id) = auth
            .get("chatgpt_account_id")
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty())
        {
            return Some(id.to_string());
        }
    }
    for key in ["account_id", "accountId", "chatgpt_account_id"] {
        if let Some(id) = claims
            .get(key)
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty())
        {
            return Some(id.to_string());
        }
    }
    None
}

fn jwt_exp(token: &str) -> Option<u64> {
    parse_jwt_claims(token)?.get("exp").and_then(|v| v.as_u64())
}

fn parse_jwt_claims(token: &str) -> Option<Value> {
    let mut parts = token.split('.');
    parts.next()?;
    let payload = parts.next()?;
    parts.next()?;
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(payload)
        .ok()?;
    serde_json::from_slice::<Value>(&bytes).ok()
}

fn required_string(value: &Value, key: &str) -> Result<String, ProviderError> {
    optional_string(value, key).ok_or_else(|| {
        ProviderError::Provider(format!("OpenAI Codex OAuth credential is missing {key}"))
    })
}

fn optional_string(value: &Value, key: &str) -> Option<String> {
    value
        .get(key)
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

fn optional_u64(value: &Value, key: &str) -> Option<u64> {
    value.get(key).and_then(|v| match v {
        Value::Number(n) => n.as_u64(),
        Value::String(s) => s.parse::<u64>().ok(),
        _ => None,
    })
}

async fn post_token_request(
    client: &reqwest::Client,
    token_url: &str,
    body: &Value,
) -> Result<Value, ProviderError> {
    let resp = client
        .post(token_url)
        .json(body)
        .send()
        .await
        .map_err(|_| ProviderError::Transient)?;
    let status = resp.status();
    let text = resp.text().await.map_err(|_| ProviderError::Transient)?;
    if !status.is_success() {
        return Err(ProviderError::Provider(format!(
            "OpenAI Codex OAuth token endpoint returned {}; body omitted",
            status.as_u16()
        )));
    }
    serde_json::from_str::<Value>(&text).map_err(|err| {
        ProviderError::Provider(format!(
            "OpenAI Codex OAuth token endpoint returned invalid JSON: {err}"
        ))
    })
}
