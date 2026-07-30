# Sidecar gRPC client target ownership.
# The source and binary roots are defined by the source build root and are
# intentionally reused here so moving this manifest does not move generated
# protobuf or sidecar build artifacts.

def_library(SidecarClient
    SOURCES
        "${ALCEDO_SRC_ROOT}/sidecar_client/client.cpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/credential_client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/image_analysis_client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/model_manager_client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/proto_dto.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/runtime_control_client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/semantic_embedding_client.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/dto/image_analysis.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/dto/model_manager.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/dto/runtime.hpp"
        "${ALCEDO_INCLUDE_ROOT}/sidecar_client/dto/semantic_embedding.hpp"
    PUBLIC_DEPS Qt6::Core
    PRIVATE_DEPS SemanticProto AiProto AppDiagnostics
)
