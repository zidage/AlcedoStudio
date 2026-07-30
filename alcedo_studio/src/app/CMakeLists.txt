# Application-service target ownership.
# The source and binary roots are defined by the source build root and are
# intentionally reused here so moving this manifest does not move generated
# protobuf or sidecar build artifacts.

set(ALCEDO_APP_ROOT "${ALCEDO_SRC_ROOT}/app")

def_library(FSService
    SOURCES
        "${ALCEDO_APP_ROOT}/sleeve_service.cpp"
    PUBLIC_DEPS SleeveFS EditHistory
)

def_library(SleeveFilterService
    SOURCES
        "${ALCEDO_APP_ROOT}/sleeve_filter_service.cpp"
        "${ALCEDO_APP_ROOT}/search_query_classifier.cpp"
    PUBLIC_DEPS SleeveElement StorageService SleeveFilter StrConv
)

def_library(AlbumBrowseService
    SOURCES
        "${ALCEDO_APP_ROOT}/album_browse_service.cpp"
    PUBLIC_DEPS FSService SleeveElement SleeveFilterService
)

def_library(ImagePoolService
    SOURCES
        "${ALCEDO_APP_ROOT}/image_pool_service.cpp"
    PUBLIC_DEPS ImagePool Image StorageService
)

set(ALCEDO_SEMANTIC_PROTO_DIR "${CMAKE_SOURCE_DIR}/rust/puerh_mind/proto")
set(ALCEDO_SEMANTIC_PROTO "${ALCEDO_SEMANTIC_PROTO_DIR}/semantic.proto")
set(ALCEDO_SEMANTIC_PROTO_GEN_DIR "${ALCEDO_BINARY_ROOT}/generated/semantic")
set(ALCEDO_SEMANTIC_PROTO_SRCS
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.pb.cc"
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.grpc.pb.cc"
)
set(ALCEDO_SEMANTIC_PROTO_HDRS
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.pb.h"
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.grpc.pb.h"
)

# semantic.proto imports ai_common.proto, so keep the AI paths available for
# both generated command dependencies and the semantic target.
set(ALCEDO_AI_PROTO_DIR "${ALCEDO_SEMANTIC_PROTO_DIR}")
set(ALCEDO_AI_COMMON_PROTO "${ALCEDO_AI_PROTO_DIR}/ai_common.proto")
set(ALCEDO_AI_RUNTIME_PROTO "${ALCEDO_AI_PROTO_DIR}/ai_runtime.proto")
set(ALCEDO_AI_IMAGE_ANALYSIS_PROTO "${ALCEDO_AI_PROTO_DIR}/image_analysis.proto")
set(ALCEDO_AI_PROTO_GEN_DIR "${ALCEDO_BINARY_ROOT}/generated/ai")

add_custom_command(
    OUTPUT ${ALCEDO_SEMANTIC_PROTO_SRCS} ${ALCEDO_SEMANTIC_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_SEMANTIC_PROTO_DIR}
        --cpp_out=${ALCEDO_SEMANTIC_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_SEMANTIC_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_SEMANTIC_PROTO}"
    DEPENDS
        "${ALCEDO_SEMANTIC_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        "${ALCEDO_AI_COMMON_PROTO}"
    DEPENDS
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.cc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_AI_RUNTIME_PROTO}"
    DEPENDS
        "${ALCEDO_AI_RUNTIME_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.cc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_AI_IMAGE_ANALYSIS_PROTO}"
    DEPENDS
        "${ALCEDO_AI_IMAGE_ANALYSIS_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

set(ALCEDO_AI_PROTO_SRCS
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.cc"
)
set(ALCEDO_AI_PROTO_HDRS
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.h"
)

add_library(AiProto ${ALCEDO_AI_PROTO_SRCS} ${ALCEDO_AI_PROTO_HDRS})
target_include_directories(AiProto PUBLIC "${ALCEDO_AI_PROTO_GEN_DIR}")
target_link_libraries(AiProto PUBLIC puerhlab::grpc++ puerhlab::protobuf)

add_library(SemanticProto ${ALCEDO_SEMANTIC_PROTO_SRCS} ${ALCEDO_SEMANTIC_PROTO_HDRS})
target_include_directories(SemanticProto PUBLIC
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}"
    "${ALCEDO_AI_PROTO_GEN_DIR}"
)
target_link_libraries(SemanticProto PUBLIC puerhlab::grpc++ puerhlab::protobuf AiProto)

def_library(AiSidecarRuntimeService
    SOURCES
        "${ALCEDO_APP_ROOT}/ai_sidecar_runtime_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/ai_sidecar_runtime_service.hpp"
    PUBLIC_DEPS Qt6::Core Qt6::Network SidecarClient
    PRIVATE_DEPS AppDiagnostics
)

def_library(AiCredentialStore
    SOURCES
        "${ALCEDO_APP_ROOT}/ai_credential_store.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/ai_credential_store.hpp"
)
if(WIN32)
    target_link_libraries(AiCredentialStore PRIVATE Advapi32 Crypt32)
elseif(APPLE)
    target_link_libraries(AiCredentialStore PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

def_library(AiProviderProfile
    SOURCES
        "${ALCEDO_APP_ROOT}/ai_provider_profile.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/ai_provider_profile.hpp"
    PUBLIC_DEPS AiCredentialStore Qt6::Core
)

def_library(ModelDownloadService
    SOURCES
        "${ALCEDO_APP_ROOT}/model_download_service.cpp"
        "${ALCEDO_APP_ROOT}/model_asset_catalog.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/model_download_service.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/model_asset_catalog.hpp"
    PUBLIC_DEPS Qt6::Core Qt6::Network
)

def_library(ProjectService
    SOURCES
        "${ALCEDO_APP_ROOT}/project_service.cpp"
        "${ALCEDO_APP_ROOT}/project_package_backend.cpp"
        "${ALCEDO_APP_ROOT}/project_package_service.cpp"
        "${ALCEDO_SRC_ROOT}/ui/alcedo_main/album_backend/path_utils.cpp"
    PUBLIC_DEPS FSService StorageService ImagePoolService AlbumBrowseService SleeveFilterService
                AiSidecarRuntimeService ModelDownloadService
                StrConv JSON xxHash stduuid
    PRIVATE_DEPS
        AppDiagnostics
        Qt6::Core
        Qt6::Gui
        opencv_core
)

def_library(PipelineMgmtService
    SOURCES "${ALCEDO_APP_ROOT}/pipeline_service.cpp"
    PUBLIC_DEPS ProjectService EditPipeline
)

def_library(AdjustmentTransferService
    SOURCES "${ALCEDO_APP_ROOT}/adjustment_transfer_service.cpp"
    PUBLIC_DEPS PipelineMgmtService EditPipeline
    PRIVATE_DEPS EditorAdjustmentPipeline
)

def_library(EditHistoryMgmtService
    SOURCES "${ALCEDO_APP_ROOT}/history_mgmt_service.cpp"
    PUBLIC_DEPS ProjectService EditHistory
)

def_library(EditorHistoryMaterializer
    SOURCES "${ALCEDO_APP_ROOT}/editor_history_materializer.cpp"
    PUBLIC_DEPS ProjectService EditHistory StorageService
)

def_library(EditorSaveCheckpointCoordinator
    SOURCES "${ALCEDO_APP_ROOT}/editor_save_checkpoint_coordinator.cpp"
)

def_library(EditorSessionCommandQueue
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_command_queue.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_command_queue.hpp"
    PUBLIC_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS}
)

def_library(EditorSaveCheckpointService
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_save_checkpoint_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_save_checkpoint_service.hpp"
    PUBLIC_DEPS EditorSaveCheckpointCoordinator EditorSessionCommandQueue xxHash
)

def_library(EditorActionPolicy
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_action_policy.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_action_policy.hpp"
    PUBLIC_DEPS EditorSessionCommandQueue
)

def_library(EditorSessionLifecycle
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_lifecycle.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_lifecycle.hpp"
    PUBLIC_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS}
)

def_library(EditorSessionNavigationController
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_navigation_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_navigation_controller.hpp"
    PUBLIC_DEPS EditHistory xxHash
    PRIVATE_DEPS EditorSessionLifecycle EditorSaveCheckpointService
                 EditorSessionRenderController EditorSessionEditController
                 EditorMiniGitMaterializer
)

def_library(EditorSessionRenderController
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_render_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_render_controller.hpp"
    PUBLIC_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS}
)

# Session headers pull image_buffer.hpp -> metal_image.hpp under HAVE_METAL.
# Provide metal-cpp include/link deps the same way ThreadPool does.
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(EditorSessionCommandQueue PUBLIC alcedo::metal_cpp)
    target_link_libraries(EditorSessionLifecycle PUBLIC alcedo::metal_cpp)
    target_link_libraries(EditorSessionRenderController PUBLIC alcedo::metal_cpp)
endif()

def_library(EditorSessionEditController
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_edit_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_edit_controller.hpp"
    PUBLIC_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS}
    PRIVATE_DEPS EditHistory
)

def_library(EditorMiniGitJournalFold
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_journal_fold.cpp"
    PUBLIC_DEPS EditHistory
)

def_library(EditorMiniGitCommitWriter
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_commit_writer.cpp"
    PUBLIC_DEPS ProjectService EditHistory StorageService
)

def_library(EditorMiniGitJournalRecovery
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_journal_recovery.cpp"
    PUBLIC_DEPS EditHistory StorageService
    PRIVATE_DEPS EditorMiniGitJournalFold EditorMiniGitCommitWriter
)

def_library(EditorMiniGitMaterializer
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_materializer.cpp"
    PUBLIC_DEPS ProjectService EditHistory StorageService
    PRIVATE_DEPS EditorSaveCheckpointCoordinator EditorMiniGitJournalFold
                 EditorMiniGitCommitWriter EditorMiniGitJournalRecovery
)

def_library(EditorAdjustmentPipeline
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_adjustment_pipeline.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_adjustment_pipeline.hpp"
    PUBLIC_DEPS EditPipeline JSON
)

def_library(EditorSessionService
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_session_service.cpp"
        "${ALCEDO_APP_ROOT}/editor_render_coordinator.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_types.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_adjustment_types.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_render_intent.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_ports.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_service.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_render_coordinator.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_session_bootstrap.hpp"
        "${ALCEDO_INCLUDE_ROOT}/edit/frame_presentation_types.hpp"
    # The session facade is application-only; no Widgets dependency belongs
    # on this target or on its public link interface.
    PRIVATE_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS}
                 EditHistory EditorMiniGitMaterializer EditorSaveCheckpointService EditorSessionLifecycle
                 EditorSessionNavigationController EditorSessionRenderController
                 EditorSessionEditController EditorSessionCommandQueue EditorActionPolicy
)

# Facade consumers (tests/UI) include session headers that pull metal_image.hpp
# under HAVE_METAL. Private session-controller deps do not propagate metal-cpp
# include dirs, so expose them on the facade itself.
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(EditorSessionService PUBLIC alcedo::metal_cpp)
endif()

def_library(ImportService
    SOURCES "${ALCEDO_APP_ROOT}/import_service.cpp"
    PUBLIC_DEPS SleeveManager ImageWriter ImagePoolService FSService
)

def_library(ThumbnailService
    SOURCES "${ALCEDO_APP_ROOT}/thumbnail_service.cpp"
    PUBLIC_DEPS EditPipeline ThreadPool StrConv ImagePoolService PipelineMgmtService
                ThumbnailDiskCacheService StorageService
)

def_library(SemanticGenerationService
    SOURCES
        "${ALCEDO_APP_ROOT}/semantic_generation_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/semantic_generation_service.hpp"
        "${ALCEDO_INCLUDE_ROOT}/storage/controller/semantic/semantic_label_config.hpp"
    PUBLIC_DEPS AiSidecarRuntimeService ThumbnailService StorageService opencv_core opencv_imgproc
    PRIVATE_DEPS AppDiagnostics
)

def_library(ImageAnalysisEncoder
    SOURCES
        "${ALCEDO_APP_ROOT}/image_analysis_encoder.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/image_analysis_encoder.hpp"
    PUBLIC_DEPS ThumbnailService ImageBuffer OpenImageIO::OpenImageIO opencv_core opencv_imgproc
)

def_library(ImageAnalysisService
    SOURCES
        "${ALCEDO_APP_ROOT}/image_analysis_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/image_analysis_service.hpp"
    PUBLIC_DEPS AiSidecarRuntimeService AiCredentialStore ThumbnailService ImageAnalysisEncoder ImageBuffer
    PRIVATE_DEPS AppDiagnostics
)

def_library(ThumbnailDiskCacheService
    SOURCES "${ALCEDO_APP_ROOT}/thumbnail_disk_cache_service.cpp"
    PUBLIC_DEPS ImageBuffer JSON xxHash opencv_imgcodecs OpenImageIO::OpenImageIO
)

def_library(ExportService
    SOURCES "${ALCEDO_APP_ROOT}/export_service.cpp"
    PUBLIC_DEPS ImageWriter ImageLoader ImagePoolService PipelineMgmtService PipelineScheduler ThreadPool
)
