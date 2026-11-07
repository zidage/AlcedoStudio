# Advanced Content Analysis (archived)

This directory keeps the Rust sidecar and proto sources of Advanced Content Analysis, kept for
reference only. Alcedo Studio 0.3.4 is the last release with the feature. The release after 0.3.4
removes it ([#710](https://github.com/zidage/AlcedoStudio/issues/710)).

The files moved here unchanged with `git mv` and keep their relative paths from
`rust/puerh_mind/`:

| Archived path | What it held |
| --- | --- |
| `proto/image_analysis.proto` | gRPC `ImageAnalysisService` (`DescribeImage`, `ScoreImage`, `AnalyzeImage`, `BatchAnalyzeImage`, `ListModels`) |
| `proto/ai_runtime.proto` | gRPC `AiRuntimeService` (`ListCapabilities`, `RegisterCredential`, `RevokeCredential`, `CancelTask`) |
| `src/server/image_analysis.rs`, `src/server/ai_runtime.rs` | The tonic implementations of the two services |
| `src/service/image_analysis.rs` | The provider interface and `MockImageAnalysisProvider` |
| `src/service/providers/` | The remote providers: OpenRouter, Volcengine Ark, OpenAI chat compatible, Anthropic Messages, OpenAI Codex OAuth, and the HTTP helpers |
| `src/service/provider_config.rs`, `src/service/prompt_profiles.rs` | Provider configs and system prompt profiles |
| `src/service/capabilities.rs`, `src/service/credential_vault.rs`, `src/service/cancellation_registry.rs` | Capability descriptors, the credential vault, and task cancellation |
| `configs/providers/`, `configs/prompts/` | The built-in provider configs and system prompts that the files above load |
| `tests/openai_codex_oauth_auth.rs`, `tests/openai_codex_oauth_provider.rs` | Integration tests of the Codex OAuth provider |
| `.env.test.example` | The API key template of the live provider tests |

These files are outside the Cargo build: this directory is outside `src/` and `tests/`, no module
declaration names it, and `build.rs` compiles neither proto. `cargo build`, `cargo test`, and
`cargo fmt` do not read them, and the code here does not compile against the current crate.

The removal also took these items out of the files that stay in the build:

- `AiRequestHeader.credential_ref` (field 6) is reserved in `proto/ai_common.proto`. The messages
  and enums `AiCapability`, `AiInputKind`, and `AiOutputKind` are removed from that file.
- `src/config.rs` no longer reads `--provider-config-dir`, `ALCEDO_MIND_PROVIDER_CONFIG_DIR`,
  `--credential-ttl-ms`, or `ALCEDO_MIND_CREDENTIAL_TTL_MS`.
- `Cargo.toml` no longer depends on `reqwest`, `rustls`, `base64`, `zeroize`, `thiserror`,
  `wiremock`, or `dotenvy`.

A host that calls a removed RPC receives gRPC `UNIMPLEMENTED`.
