use alcedo_mind::{bootstrap, config, logging, service};
use anyhow::{Context, anyhow};
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
    let config = config::AppConfig::load()?;
    logging::init_logging(config.log_file.as_deref())?;
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
            segmentation_engine,
        ))
        .map_err(|err| anyhow!("{err}"))
}
