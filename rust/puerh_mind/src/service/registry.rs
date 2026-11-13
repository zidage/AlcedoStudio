use std::sync::Arc;

use tonic::transport::Server;

use crate::config::AppConfig;
use crate::proto::alcedo::ai::segmentation_service_server::SegmentationServiceServer;
use crate::proto::common::health_service_server::HealthServiceServer;
use crate::proto::semantic::model_manager_service_server::ModelManagerServiceServer;
use crate::proto::semantic::semantic_service_server::SemanticServiceServer;
use crate::server::health::HealthServiceImpl;
use crate::server::model_manager::ModelManagerServiceImpl;
use crate::server::segmentation::SegmentationServiceImpl;
use crate::server::semantic::SemanticServiceImpl;
use crate::service::embedding::EmbeddingEngine;
use crate::service::segmentation::engine::SegmentationEngine;

const FILE_DESCRIPTOR_SET: &[u8] = tonic::include_file_descriptor_set!("semantic_descriptor");
/// Message limit of the segmentation service: a 4096 x 4096 RGB8 image is 50.3 MB.
const SEGMENTATION_MAX_MESSAGE_BYTES: usize = 64 * 1024 * 1024;

pub fn register_services(
    mut builder: Server,
    config: &AppConfig,
    semantic_engine: Arc<dyn EmbeddingEngine>,
    segmentation_engine: Arc<SegmentationEngine>,
) -> anyhow::Result<tonic::transport::server::Router> {
    let health_service = HealthServiceImpl;
    let semantic_service = SemanticServiceImpl::new(
        semantic_engine,
        config.semantic.batch_cap,
        std::time::Duration::from_millis(config.semantic.batch_wait_ms),
    );
    let model_manager_service =
        ModelManagerServiceImpl::new(&config.semantic.model_root, &config.semantic.hf_endpoint);

    let reflection_service = tonic_reflection::server::Builder::configure()
        .register_encoded_file_descriptor_set(FILE_DESCRIPTOR_SET)
        .build_v1alpha()
        .expect("failed to build reflection service");

    Ok(builder
        .add_service(reflection_service)
        .add_service(HealthServiceServer::new(health_service))
        .add_service(ModelManagerServiceServer::new(model_manager_service))
        .add_service(
            SemanticServiceServer::new(semantic_service)
                .max_decoding_message_size(config.max_message_bytes)
                .max_encoding_message_size(config.max_message_bytes),
        )
        .add_service(
            SegmentationServiceServer::new(SegmentationServiceImpl::new(segmentation_engine))
                .max_decoding_message_size(SEGMENTATION_MAX_MESSAGE_BYTES)
                .max_encoding_message_size(SEGMENTATION_MAX_MESSAGE_BYTES),
        ))
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::net::SocketAddr;

    use tonic::codegen::tokio_stream;
    use tonic::transport::{Channel, Endpoint, server::TcpIncoming};
    use tonic_reflection::pb::v1alpha::ServerReflectionRequest;
    use tonic_reflection::pb::v1alpha::server_reflection_client::ServerReflectionClient;
    use tonic_reflection::pb::v1alpha::server_reflection_request::MessageRequest;
    use tonic_reflection::pb::v1alpha::server_reflection_response::MessageResponse;

    use super::*;
    use crate::service::embedding::UnavailableEmbeddingEngine;
    use crate::service::segmentation::platform_backend_factory;

    /// Serves the router of `register_services` on a loopback port and returns a channel to it.
    async fn serve_registered_services() -> Channel {
        let config = AppConfig::from_args(Vec::<String>::new()).expect("default config");
        let router = register_services(
            Server::builder(),
            &config,
            Arc::new(UnavailableEmbeddingEngine::no_model()),
            Arc::new(SegmentationEngine::new(platform_backend_factory())),
        )
        .expect("services register");
        let incoming =
            TcpIncoming::bind(SocketAddr::from(([127, 0, 0, 1], 0))).expect("loopback port binds");
        let addr = incoming.local_addr().expect("bound address");
        tokio::spawn(router.serve_with_incoming(incoming));
        Endpoint::from_shared(format!("http://{addr}"))
            .expect("endpoint")
            .connect()
            .await
            .expect("client connects")
    }

    #[tokio::test]
    async fn reflection_lists_kept_services_and_no_image_analysis_or_runtime_service() {
        let mut client = ServerReflectionClient::new(serve_registered_services().await);
        let request = ServerReflectionRequest {
            host: String::new(),
            message_request: Some(MessageRequest::ListServices(String::new())),
        };
        let mut responses = client
            .server_reflection_info(tokio_stream::iter([request]))
            .await
            .expect("reflection call starts")
            .into_inner();
        let response = responses
            .message()
            .await
            .expect("reflection response")
            .expect("one response");
        let Some(MessageResponse::ListServicesResponse(list)) = response.message_response else {
            panic!("expected a list services response");
        };
        let services: BTreeSet<String> = list.service.into_iter().map(|s| s.name).collect();

        for kept in [
            "common.HealthService",
            "semantic.ModelManagerService",
            "semantic.SemanticService",
            "alcedo.ai.SegmentationService",
        ] {
            assert!(services.contains(kept), "{kept} missing from {services:?}");
        }
        for removed in [
            "alcedo.ai.ImageAnalysisService",
            "alcedo.ai.AiRuntimeService",
        ] {
            assert!(
                !services.contains(removed),
                "{removed} listed in {services:?}"
            );
        }
    }

    #[tokio::test]
    async fn removed_image_analysis_and_runtime_rpcs_return_unimplemented() {
        let channel = serve_registered_services().await;
        for path in [
            "/alcedo.ai.AiRuntimeService/ListCapabilities",
            "/alcedo.ai.AiRuntimeService/RegisterCredential",
            "/alcedo.ai.AiRuntimeService/RevokeCredential",
            "/alcedo.ai.AiRuntimeService/CancelTask",
            "/alcedo.ai.ImageAnalysisService/DescribeImage",
            "/alcedo.ai.ImageAnalysisService/ScoreImage",
            "/alcedo.ai.ImageAnalysisService/AnalyzeImage",
            "/alcedo.ai.ImageAnalysisService/BatchAnalyzeImage",
            "/alcedo.ai.ImageAnalysisService/ListModels",
        ] {
            let mut grpc = tonic::client::Grpc::new(channel.clone());
            grpc.ready().await.expect("channel ready");
            let status = grpc
                .unary::<(), (), _>(
                    tonic::Request::new(()),
                    path.parse().expect("method path"),
                    tonic_prost::ProstCodec::default(),
                )
                .await
                .expect_err("removed RPC fails");
            assert_eq!(
                status.code(),
                tonic::Code::Unimplemented,
                "{path}: {status:?}"
            );
        }
    }
}
