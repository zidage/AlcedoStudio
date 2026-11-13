//! Env-gated live smoke tests against the real OpenRouter, Opencode
//! OpenAI-compatible, Volcengine Ark, and Volcengine Ark Coding Plan
//! (Anthropic-compatible) APIs.
//!
//! These are NOT part of the default CI gate — they call real provider endpoints
//! and may incur cost. Each test loads a gitignored `.env.test` (via `dotenvy`) and
//! skips (prints + returns) unless explicitly opted in with
//! `ALCEDO_MIND_LIVE_SMOKE=1`, so `cargo test` without real-model intent is a
//! clean no-op pass even if the shell happens to contain stale provider keys. To
//! run them, copy `.env.test.example` to `.env.test`, fill in real keys, set
//! `ALCEDO_MIND_LIVE_SMOKE=1`, and run `cargo test`.
//!
//! Purpose: the mock-server tests in `openrouter.rs` / `volcengine_ark.rs` /
//! `anthropic_messages.rs` cover the drivers' request/response shapes against
//! `wiremock` fixtures. These live smokes
//! are the ground-truth check that the documented shapes match the real providers —
//! if a provider changes its response JSON shape, the live test fails and the parser/fixture gap
//! surfaces here rather than in production. On success they assert the parsed
//! outcome validates against the code-owned Alcedo contract (no active annotation
//! on a malformed real response), and print usage + provider request id for manual
//! inspection.

use crate::service::credential_vault::SecretString;
use crate::service::image_analysis::{
    AnalyzeImageInput, ImageAnalysisProvider, validate_batch_analyze, validate_rating,
    validate_understanding,
};
use crate::service::provider_config::load_provider_configs;
use crate::service::providers::anthropic_messages::AnthropicMessagesProvider;
use crate::service::providers::openai_chat_compatible::OpenAiChatCompatibleProvider;
use crate::service::providers::openrouter::OpenRouterChatProvider;
use crate::service::providers::volcengine_ark::VolcengineArkResponsesProvider;

fn live_smoke_enabled_or_skip() -> bool {
    let enabled = std::env::var("ALCEDO_MIND_LIVE_SMOKE")
        .ok()
        .map(|v| matches!(v.trim(), "1" | "true" | "TRUE" | "yes" | "YES"))
        .unwrap_or(false);
    if !enabled {
        eprintln!("skip: ALCEDO_MIND_LIVE_SMOKE=1 not set (live smoke)");
    }
    enabled
}

/// Return the first non-empty env var from `keys`, or `None` after printing a
/// skip line. Existing process env vars take precedence over `.env.test`
/// (`dotenvy` does not override already-set vars).
fn env_or_skip(keys: &[&str]) -> Option<String> {
    for k in keys {
        if let Ok(v) = std::env::var(k) {
            let v = v.trim().to_string();
            if !v.is_empty() {
                return Some(v);
            }
        }
    }
    eprintln!(
        "skip: {} not set (live smoke; set it in .env.test to enable)",
        keys.join(" or ")
    );
    None
}

/// A small 32x32 RGB gradient PNG. Large enough for a vision model to describe
/// meaningfully, small enough to upload quickly and cheaply on every smoke run.
fn smoke_image_png() -> Vec<u8> {
    let (w, h) = (32u32, 32u32);
    let mut img = image::RgbImage::new(w, h);
    for y in 0..h {
        for x in 0..w {
            let r = (x * 8) as u8;
            let g = (y * 8) as u8;
            let b = ((x + y) * 4) as u8;
            img.put_pixel(x, y, image::Rgb([r, g, b]));
        }
    }
    let mut cursor = std::io::Cursor::new(Vec::new());
    image::DynamicImage::ImageRgb8(img)
        .write_to(&mut cursor, image::ImageFormat::Png)
        .expect("encode smoke png");
    cursor.into_inner()
}

#[tokio::test]
async fn live_openrouter_smoke_describe_and_score() {
    let _ = dotenvy::from_filename(".env.test").ok();
    if !live_smoke_enabled_or_skip() {
        return;
    }
    let key = env_or_skip(&["ALCEDO_OPENROUTER_API_KEY", "OPENROUTER_API_KEY"]);
    let Some(key) = key else {
        return;
    };

    let config = load_provider_configs(None)
        .expect("built-ins load")
        .get("openrouter")
        .expect("openrouter built-in")
        .clone();
    let provider = OpenRouterChatProvider::new(config).expect("provider builds");
    let secret = SecretString::new(key);
    let img = smoke_image_png();

    let out = provider
        .describe_image(&img, "", "alcedo-live-smoke", "", Some(&secret))
        .await
        .expect("live OpenRouter describe succeeded");
    eprintln!(
        "live openrouter describe: caption={:?} tags={:?} scene={:?} confidence={} usage={:?} req_id={}",
        out.caption, out.tags, out.scene, out.confidence, out.usage, out.provider_request_id
    );
    validate_understanding(&out).expect("live outcome validates against the Alcedo contract");

    let score = provider
        .score_image(
            &img,
            "",
            "alcedo-live-smoke",
            "alcedo-default-v1",
            "",
            "",
            "",
            true,
            Some(&secret),
        )
        .await
        .expect("live OpenRouter score succeeded");
    eprintln!(
        "live openrouter score: rating={} rubric={} usage={:?} req_id={}",
        score.rating, score.rubric_id, score.usage, score.provider_request_id
    );
    validate_rating(&score).expect("live rating validates against the Alcedo contract");
}

#[tokio::test]
async fn live_opencode_openai_batch_analyze_smoke() {
    // Ground-truth check for the OpenAI Chat-compatible batch parser against an
    // Opencode-routed model. Some OpenAI-compatible providers return structured
    // JSON in `message.tool_calls[].function.arguments` with `message.content =
    // null`; this live test catches that real response JSON drift for the batch path.
    let _ = dotenvy::from_filename(".env.test").ok();
    if !live_smoke_enabled_or_skip() {
        return;
    }
    let key = env_or_skip(&["ALCEDO_OPENCODE_API_KEY", "OPENCODE_API_KEY"]);
    let Some(key) = key else {
        return;
    };

    let config = load_provider_configs(None)
        .expect("built-ins load")
        .get("opencode_go_openai")
        .expect("opencode_go_openai built-in")
        .clone();
    let provider = OpenAiChatCompatibleProvider::new(config).expect("provider builds");
    let secret = SecretString::new(key);
    let img_a = smoke_image_png();
    let img_b = smoke_image_png();
    let images = [
        AnalyzeImageInput {
            image_bytes: &img_a,
            camera_context: "",
        },
        AnalyzeImageInput {
            image_bytes: &img_b,
            camera_context: "",
        },
    ];

    let out = provider
        .batch_analyze_images(
            &images,
            "",
            "alcedo-live-smoke",
            "general",
            "normal",
            "zh",
            true,
            true,
            true,
            Some(&secret),
        )
        .await
        .expect("live Opencode OpenAI-compatible batch analyze succeeded");
    eprintln!(
        "live opencode_openai batch: items={} usage={:?} req_id={}",
        out.items.len(),
        out.usage,
        out.provider_request_id
    );
    validate_batch_analyze(&out).expect("live batch validates against the Alcedo contract");
}

#[tokio::test]
async fn live_opencode_openai_kimi_k26_no_max_tokens_smoke() {
    // Ground-truth check that kimi-k2.6 — a reasoning model served via OpenCode's
    // Moonshot upstream — succeeds through the OpenAI-compatible driver when the
    // driver omits `max_tokens`. Reasoning models consume reasoning tokens out of
    // the same `max_tokens` budget as the visible content, so a server-side cap
    // starves the content (content=null + finish_reason="length"); omitting the
    // cap lets the model generate as much as it needs (bounded only by the
    // model's own max output). This is the live end-to-end verification of the
    // kimi-k2.6 fix; the mock-server `*_request_body_omits_max_tokens` tests in
    // openai_chat_compatible.rs cover the wire shape against a fixture.
    let _ = dotenvy::from_filename(".env.test").ok();
    if !live_smoke_enabled_or_skip() {
        return;
    }
    let key = env_or_skip(&["ALCEDO_OPENCODE_API_KEY", "OPENCODE_API_KEY"]);
    let Some(key) = key else {
        return;
    };

    let config = load_provider_configs(None)
        .expect("built-ins load")
        .get("opencode_go_openai")
        .expect("opencode_go_openai built-in")
        .clone();
    // The driver ignores `max_output_tokens` and sends no `max_tokens` — verify
    // kimi-k2.6 reasons freely and emits valid structured content. (The built-in
    // default `max_output_tokens: 1200` would have starved kimi-k2.6 on a real
    // photo batch; omitting max_tokens is the fix.)
    let provider = OpenAiChatCompatibleProvider::new(config).expect("provider builds");
    let secret = SecretString::new(key);

    // Discover + commit kimi-k2.6 so resolve_model accepts it. The built-in ships
    // kimi-k2.7-code/kimi-k2.7 as documented models; kimi-k2.6 is the working
    // alternative the user selected (served via OpenCode's Moonshot upstream).
    let discovered = provider
        .list_models(Some(&secret))
        .await
        .expect("list_models ok");
    assert!(
        discovered.iter().any(|m| m.model_id == "kimi-k2.6"),
        "kimi-k2.6 must be discoverable on OpenCode"
    );

    let img = smoke_image_png();
    let out = provider
        .describe_image(&img, "kimi-k2.6", "alcedo-live-smoke", "zh", Some(&secret))
        .await
        .expect("live kimi-k2.6 describe succeeded (no max_tokens cap)");
    eprintln!(
        "live kimi-k2.6 no-max-tokens describe: caption='{}' usage={:?} req_id={}",
        out.caption, out.usage, out.provider_request_id
    );
    validate_understanding(&out).expect("live kimi-k2.6 describe validates");
}

#[tokio::test]
async fn live_volcengine_ark_smoke_describe_and_score() {
    let _ = dotenvy::from_filename(".env.test").ok();
    if !live_smoke_enabled_or_skip() {
        return;
    }
    let key = env_or_skip(&["ALCEDO_VOLCENGINE_ARK_API_KEY", "ALCEDO_ARK_API_KEY"]);
    let Some(key) = key else {
        return;
    };

    let config = load_provider_configs(None)
        .expect("built-ins load")
        .get("volcengine_ark")
        .expect("volcengine_ark built-in")
        .clone();
    let provider = VolcengineArkResponsesProvider::new(config).expect("provider builds");
    let secret = SecretString::new(key);
    let img = smoke_image_png();

    let out = provider
        .describe_image(&img, "", "alcedo-live-smoke", "", Some(&secret))
        .await
        .expect("live Volcengine Ark describe succeeded");
    eprintln!(
        "live volcengine_ark describe: caption={:?} tags={:?} scene={:?} confidence={} usage={:?} req_id={}",
        out.caption, out.tags, out.scene, out.confidence, out.usage, out.provider_request_id
    );
    validate_understanding(&out).expect("live outcome validates against the Alcedo contract");

    let score = provider
        .score_image(
            &img,
            "",
            "alcedo-live-smoke",
            "alcedo-default-v1",
            "",
            "",
            "",
            true,
            Some(&secret),
        )
        .await
        .expect("live Volcengine Ark score succeeded");
    eprintln!(
        "live volcengine_ark score: rating={} rubric={} usage={:?} req_id={}",
        score.rating, score.rubric_id, score.usage, score.provider_request_id
    );
    validate_rating(&score).expect("live rating validates against the Alcedo contract");
}

#[tokio::test]
async fn live_volcengine_ark_coding_smoke_describe_and_score() {
    // Ground-truth check for the Anthropic Messages driver against the Volcengine
    // Ark Coding Plan endpoint (`/api/coding/v1/messages`). Reuses the same Ark
    // API key as `live_volcengine_ark_smoke_*` — the Coding Plan is reached with
    // the same account credential, just a different base URL + Anthropic wire
    // shape. See `anthropic_messages.rs` for the Coding Plan ToS + vision caveats:
    // this is a validation smoke, NOT a production path. If the live call 401s,
    // flip `auth.type` in `volcengine_ark_coding.json` to `api_key_header`; if it
    // 404s on the model or rejects the image, adjust the `slug` to a confirmed
    // vision-capable Coding Plan model.
    let _ = dotenvy::from_filename(".env.test").ok();
    if !live_smoke_enabled_or_skip() {
        return;
    }
    let key = env_or_skip(&["ALCEDO_VOLCENGINE_ARK_API_KEY", "ALCEDO_ARK_API_KEY"]);
    let Some(key) = key else {
        return;
    };

    let config = load_provider_configs(None)
        .expect("built-ins load")
        .get("volcengine_ark_coding")
        .expect("volcengine_ark_coding built-in")
        .clone();
    let provider = AnthropicMessagesProvider::new(config).expect("provider builds");
    let secret = SecretString::new(key);
    let img = smoke_image_png();

    let out = provider
        .describe_image(&img, "", "alcedo-live-smoke", "", Some(&secret))
        .await
        .expect("live Volcengine Ark Coding Plan describe succeeded");
    eprintln!(
        "live volcengine_ark_coding describe: caption={:?} tags={:?} scene={:?} confidence={} usage={:?} req_id={}",
        out.caption, out.tags, out.scene, out.confidence, out.usage, out.provider_request_id
    );
    validate_understanding(&out).expect("live outcome validates against the Alcedo contract");

    let score = provider
        .score_image(
            &img,
            "",
            "alcedo-live-smoke",
            "alcedo-default-v1",
            "",
            "",
            "",
            true,
            Some(&secret),
        )
        .await
        .expect("live Volcengine Ark Coding Plan score succeeded");
    eprintln!(
        "live volcengine_ark_coding score: rating={} rubric={} usage={:?} req_id={}",
        score.rating, score.rubric_id, score.usage, score.provider_request_id
    );
    validate_rating(&score).expect("live rating validates against the Alcedo contract");
}
