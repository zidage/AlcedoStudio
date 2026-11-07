//! OpenAI Codex OAuth provider.
//!
//! This driver targets the ChatGPT/Codex subscription-auth route used by Codex
//! clients: authenticated calls go to `chatgpt.com/backend-api/codex`, not the
//! metered OpenAI Platform API host. The module is deliberately split so OAuth
//! token handling, Codex HTTP transport, and the image-analysis provider adapter
//! stay independently testable.

pub mod auth;
pub mod client;
pub mod provider;
pub mod responses;
mod stream;

pub use auth::{CodexOAuthProfile, CodexOAuthSettings, PkcePair};
pub use provider::OpenAiCodexOAuthProvider;
