use alcedo_mind::{bootstrap, config, logging, service};
use anyhow::{Context, anyhow};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

const SERVER_THREAD_STACK_BYTES: usize = 64 * 1024 * 1024;
/// How often loaded encoders are checked against their idle unload time.
const ENCODER_IDLE_CHECK_INTERVAL: Duration = Duration::from_secs(5);

fn main() -> anyhow::Result<()> {
    let server_thread = std::thread::Builder::new()
        .name("alcedo-mind-server".to_string())
        .stack_size(SERVER_THREAD_STACK_BYTES)
        .spawn(run_server)
        .context("failed to spawn alcedo_mind server thread")?;

    server_thread
        .join()
        .map_err(|_| anyhow!("alcedo_mind server thread panicked"))?
}

fn run_server() -> anyhow::Result<()> {
    logging::init_logging();
    let config = config::AppConfig::load()?;
    // The CLIP profile is prepared on the first semantic request, not here: the AI Mask tool
    // starts this process too and needs no CLIP model (#518). Without `--model-id` the open
    // project has no active model: semantic requests report that, the other services run.
    let semantic_engine: Arc<dyn service::embedding::EmbeddingEngine> =
        if config.semantic.has_model() {
            Arc::new(service::lazy_semantic_engine::LazySemanticEngine::new(
                &config.semantic,
                &config.residency,
            ))
        } else {
            Arc::new(service::embedding::UnavailableEmbeddingEngine::no_model())
        };
    let credential_vault = Arc::new(service::credential_vault::CredentialVault::new(
        if config.credential_ttl_ms == 0 {
            None
        } else {
            Some(Duration::from_millis(config.credential_ttl_ms))
        },
    ));
    let cancel_registry = Arc::new(service::cancellation_registry::CancellationRegistry::new());
    // Phase 5a: load built-in provider configs plus optional user-provider configs
    // from the configured directory. An invalid built-in is a hard error (the binary
    // is broken); invalid user configs are skipped with a warning by the loader.
    let provider_registry = service::provider_config::load_provider_configs(
        config
            .provider_config_dir
            .as_deref()
            .map(std::path::Path::new),
    )
    .context("failed to load provider configs")?;

    // Phase 5b/5c: register the mock image-analysis provider (no-credential, no
    // HTTP, returns valid typed results) plus the wired remote drivers constructed
    // from the loaded provider registry (OpenRouter, Volcengine Ark). The mock
    // stays the default provider_id, so a request that leaves provider_id empty
    // uses the mock; a request naming "openrouter" or "volcengine_ark" uses the
    // matching remote driver. A config naming a reserved-but-unimplemented driver
    // is skipped by `build_real_image_providers` (fail closed -> UNSUPPORTED_TASK).
    let mock_provider: Arc<dyn service::image_analysis::ImageAnalysisProvider> = Arc::new(
        service::image_analysis::MockImageAnalysisProvider::new("mock", "alcedo-mock"),
    );
    let mock_capability = mock_provider.capability();
    let real_providers = service::providers::build_real_image_providers(&provider_registry);
    let mut image_providers: HashMap<
        String,
        Arc<dyn service::image_analysis::ImageAnalysisProvider>,
    > = HashMap::new();
    image_providers.insert(
        mock_provider.provider_id().to_string(),
        mock_provider.clone(),
    );
    for (id, provider) in real_providers {
        image_providers.insert(id, provider);
    }
    let default_image_provider_id = mock_provider.provider_id().to_string();

    let capabilities = service::capabilities::build_capability_descriptors(
        &*semantic_engine,
        config.max_message_bytes,
        &provider_registry,
        // The mock provider advertises its own no-credential image-analysis capability.
        &[mock_capability],
    );
    // AI Mask segmentation: no model loads here; the host sends the model directory in
    // LoadSegmentationModels (native Core ML on macOS, ONNX Runtime elsewhere).
    let segmentation_engine = Arc::new(service::segmentation::engine::SegmentationEngine::new(
        service::segmentation::platform_backend_factory(),
    ));
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to initialize alcedo_mind tokio runtime")?;
    // Unloads each encoder that is idle for longer than its idle time. The check takes the
    // encoder lock, so it runs on the blocking pool and waits for a running request.
    let idle_engine = semantic_engine.clone();
    runtime.spawn(async move {
        let mut interval = tokio::time::interval(ENCODER_IDLE_CHECK_INTERVAL);
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            interval.tick().await;
            let engine = idle_engine.clone();
            let _ = tokio::task::spawn_blocking(move || engine.unload_idle_encoders()).await;
        }
    });
    runtime
        .block_on(bootstrap::start_server(
            config,
            semantic_engine,
            credential_vault,
            cancel_registry,
            capabilities,
            image_providers,
            default_image_provider_id,
            segmentation_engine,
        ))
        .map_err(|err| anyhow!("{err}"))
}
