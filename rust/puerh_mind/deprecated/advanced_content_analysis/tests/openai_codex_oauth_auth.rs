use alcedo_mind::service::providers::openai_codex_oauth::auth::{
    AuthorizationRequest, CodexOAuthProfile, CodexOAuthSettings, exchange_authorization_code,
    refresh_profile,
};
use base64::Engine;
use serde_json::json;
use wiremock::matchers::{method, path};
use wiremock::{Mock, MockServer, ResponseTemplate};

#[test]
fn authorization_request_uses_pkce_s256_and_local_callback() {
    let settings = CodexOAuthSettings::default();
    let auth = AuthorizationRequest::new(&settings).expect("authorization URL builds");

    assert!(auth.pkce.verifier.len() >= 43);
    assert!(!auth.pkce.challenge.contains('='));
    assert_ne!(auth.pkce.verifier, auth.pkce.challenge);

    let url = reqwest::Url::parse(&auth.url).expect("valid URL");
    let pairs = url.query_pairs().collect::<Vec<_>>();
    assert!(
        pairs
            .iter()
            .any(|(k, v)| k == "client_id" && v == &settings.client_id)
    );
    assert!(
        pairs
            .iter()
            .any(|(k, v)| k == "redirect_uri" && v == &settings.redirect_uri)
    );
    assert!(
        pairs
            .iter()
            .any(|(k, v)| k == "code_challenge" && v == &auth.pkce.challenge)
    );
    assert!(
        pairs
            .iter()
            .any(|(k, v)| k == "code_challenge_method" && v == "S256")
    );
    assert!(pairs.iter().any(|(k, v)| k == "state" && v == &auth.state));
}

#[test]
fn parses_codex_auth_json_and_derives_account_id_from_id_token() {
    let profile = CodexOAuthProfile::from_str(
        &json!({
            "tokens": {
                "access_token": fake_jwt(json!({"exp": 4_000_000_000u64})),
                "refresh_token": "refresh-a",
                "id_token": fake_jwt(json!({
                    "https://api.openai.com/auth": {
                        "chatgpt_account_id": "acct_123"
                    }
                }))
            },
            "last_refresh": "2026-07-05T00:00:00Z"
        })
        .to_string(),
    )
    .expect("profile parses");

    assert_eq!(profile.account_id, "acct_123");
    assert_eq!(profile.refresh_token.as_deref(), Some("refresh-a"));
    assert_eq!(profile.expires_at, Some(4_000_000_000));
}

#[tokio::test]
async fn refresh_profile_posts_codex_oauth_payload_and_accepts_rotated_token() {
    let server = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/oauth/token"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "access_token": fake_jwt(json!({"exp": 4_000_000_111u64})),
            "refresh_token": "refresh-rotated",
            "id_token": fake_jwt(json!({
                "https://api.openai.com/auth": {
                    "chatgpt_account_id": "acct_refreshed"
                }
            })),
            "expires_in": 3600
        })))
        .mount(&server)
        .await;

    let settings = CodexOAuthSettings {
        token_url: format!("{}/oauth/token", server.uri()),
        ..CodexOAuthSettings::default()
    };
    let stale = CodexOAuthProfile {
        access_token: "old-access".to_string(),
        refresh_token: Some("refresh-old".to_string()),
        id_token: None,
        account_id: "acct_old".to_string(),
        expires_at: Some(1),
    };
    let refreshed = refresh_profile(&reqwest::Client::new(), &settings, &stale, 1_000)
        .await
        .expect("refresh succeeds");

    assert_eq!(refreshed.refresh_token.as_deref(), Some("refresh-rotated"));
    assert_eq!(refreshed.account_id, "acct_refreshed");
    assert_eq!(refreshed.expires_at, Some(4_600));

    let requests = server.received_requests().await.expect("requests recorded");
    let body: serde_json::Value =
        serde_json::from_slice(&requests[0].body).expect("request body JSON");
    assert_eq!(body["grant_type"], "refresh_token");
    assert_eq!(body["refresh_token"], "refresh-old");
    assert_eq!(body["client_id"], settings.client_id);
    assert_eq!(body["scope"], settings.scope);
}

#[tokio::test]
async fn exchange_authorization_code_posts_pkce_verifier() {
    let server = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/oauth/token"))
        .respond_with(ResponseTemplate::new(200).set_body_json(json!({
            "access_token": fake_jwt(json!({
                "exp": 4_000_000_222u64,
                "account_id": "acct_code"
            })),
            "refresh_token": "refresh-code"
        })))
        .mount(&server)
        .await;

    let settings = CodexOAuthSettings {
        token_url: format!("{}/oauth/token", server.uri()),
        redirect_uri: "http://127.0.0.1:1455/auth/callback".to_string(),
        ..CodexOAuthSettings::default()
    };
    let profile = exchange_authorization_code(
        &reqwest::Client::new(),
        &settings,
        "code-123",
        "verifier-xyz",
        1_000,
    )
    .await
    .expect("code exchange succeeds");

    assert_eq!(profile.account_id, "acct_code");
    assert_eq!(profile.refresh_token.as_deref(), Some("refresh-code"));

    let requests = server.received_requests().await.expect("requests recorded");
    let body: serde_json::Value =
        serde_json::from_slice(&requests[0].body).expect("request body JSON");
    assert_eq!(body["grant_type"], "authorization_code");
    assert_eq!(body["code"], "code-123");
    assert_eq!(body["code_verifier"], "verifier-xyz");
    assert_eq!(body["redirect_uri"], settings.redirect_uri);
}

fn fake_jwt(payload: serde_json::Value) -> String {
    let header =
        base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(r#"{"alg":"none","typ":"JWT"}"#);
    let payload = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .encode(serde_json::to_vec(&payload).expect("payload serializes"));
    format!("{header}.{payload}.sig")
}
