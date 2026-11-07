use std::sync::Arc;

use tonic::transport::Server;
use tracing::info;

use crate::config::AppConfig;
use crate::service::embedding::EmbeddingEngine;
use crate::service::registry::register_services;
use crate::service::segmentation::engine::SegmentationEngine;

pub async fn start_server(
    config: AppConfig,
    semantic_engine: Arc<dyn EmbeddingEngine>,
    segmentation_engine: Arc<SegmentationEngine>,
) -> Result<(), Box<dyn std::error::Error>> {
    let addr = config.listen_addr().parse()?;

    info!("starting alcedo_mind on {}", addr);

    let router = register_services(
        Server::builder(),
        &config,
        semantic_engine,
        segmentation_engine,
    )?;

    router.serve(addr).await?;

    Ok(())
}
