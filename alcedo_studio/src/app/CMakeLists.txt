# Application-service target ownership.
# The source and binary roots are defined by the source build root and are
# intentionally reused here so moving this manifest does not move generated
# protobuf or sidecar build artifacts.

set(ALCEDO_APP_ROOT "${ALCEDO_SRC_ROOT}/app")

# The updater uses a small, static Ed25519 verifier. Prefer a package supplied
# by the active toolchain. The pinned fallback keeps macOS builds independent
# of vcpkg and prevents an unreviewed upstream revision from entering a build.
find_package(unofficial-ed25519 CONFIG QUIET)
if(NOT TARGET unofficial::ed25519::ed25519)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(alcedo_ed25519_source
        URL
            "https://github.com/orlp/ed25519/archive/b1f19fab4aebe607805620d25a5e42566ce46a0e.tar.gz"
        URL_HASH
            "SHA512=fcbeba58591543304dd93ae7c1b62a720d89c80c4c07c323eabb6e1f41b93562660181973bda345976e5361e925f243ba9abaec19fc8a05235011957367c6e7e"
    )
    FetchContent_GetProperties(alcedo_ed25519_source)
    if(NOT alcedo_ed25519_source_POPULATED)
        FetchContent_Populate(alcedo_ed25519_source)
    endif()
    add_library(alcedo_ed25519 STATIC
        "${alcedo_ed25519_source_SOURCE_DIR}/src/add_scalar.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/fe.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/ge.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/key_exchange.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/keypair.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/sc.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/seed.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/sha512.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/sign.c"
        "${alcedo_ed25519_source_SOURCE_DIR}/src/verify.c"
    )
    target_include_directories(alcedo_ed25519 PUBLIC
        "${alcedo_ed25519_source_SOURCE_DIR}/src")
    set_target_properties(alcedo_ed25519 PROPERTIES AUTOMOC OFF)
    add_library(unofficial::ed25519::ed25519 ALIAS alcedo_ed25519)
endif()

def_library(DownloadService
    SOURCES
        "${ALCEDO_APP_ROOT}/download_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/download_service.hpp"
    PUBLIC_DEPS Qt6::Core Qt6::Network
)

if(WIN32)
    def_library(UpdateInstallerWindows
        SOURCES
            "${ALCEDO_APP_ROOT}/update_installer_windows.cpp"
            "${ALCEDO_INCLUDE_ROOT}/app/update_installer_windows.hpp"
        PUBLIC_DEPS Qt6::Core
    )
endif()

def_library(UpdateService
    SOURCES
        "${ALCEDO_APP_ROOT}/update_manifest.cpp"
        "${ALCEDO_APP_ROOT}/update_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/update_manifest.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/update_service.hpp"
    PUBLIC_DEPS DownloadService Qt6::Core Qt6::Network
    PRIVATE_DEPS unofficial::ed25519::ed25519 Qt6::Gui
)
if(ALCEDO_UPDATE_ALLOW_INSTALL)
    set(_alcedo_update_allow_install 1)
else()
    set(_alcedo_update_allow_install 0)
endif()
target_compile_definitions(UpdateService PRIVATE
    ALCEDO_APP_VERSION="${PROJECT_VERSION}"
    ALCEDO_BUILD_NUMBER=${ALCEDO_BUILD_NUMBER}
    ALCEDO_UPDATE_CHANNEL="${ALCEDO_UPDATE_CHANNEL}"
    ALCEDO_UPDATE_MANIFEST_URL="${ALCEDO_UPDATE_MANIFEST_URL}"
    ALCEDO_UPDATE_PUBLIC_KEY_BASE64="${ALCEDO_UPDATE_PUBLIC_KEY_BASE64}"
    ALCEDO_UPDATE_ALLOW_INSTALL=${_alcedo_update_allow_install}
)
unset(_alcedo_update_allow_install)

def_library(FSService
    SOURCES
        "${ALCEDO_APP_ROOT}/sleeve_service.cpp"
    PUBLIC_DEPS SleeveFS EditHistory
)

def_library(SleeveFilterService
    SOURCES
        "${ALCEDO_APP_ROOT}/sleeve_filter_service.cpp"
        "${ALCEDO_APP_ROOT}/search_query_classifier.cpp"
    PUBLIC_DEPS SleeveElement Storage SleeveFilter StrConv
)

def_library(AlbumBrowseService
    SOURCES
        "${ALCEDO_APP_ROOT}/album_browse_service.cpp"
    PUBLIC_DEPS FSService SleeveElement SleeveFilterService
)

def_library(ImagePoolService
    SOURCES
        "${ALCEDO_APP_ROOT}/image_pool_service.cpp"
    PUBLIC_DEPS ImagePool Image Storage
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
    PUBLIC_DEPS DownloadService Qt6::Core Qt6::Network
)

def_library(ProjectService
    SOURCES
        "${ALCEDO_APP_ROOT}/project_service.cpp"
        "${ALCEDO_APP_ROOT}/project_package_backend.cpp"
        "${ALCEDO_APP_ROOT}/project_package_service.cpp"
        "${ALCEDO_SRC_ROOT}/ui/alcedo_main/album_backend/path_utils.cpp"
    PUBLIC_DEPS FSService Storage ImagePoolService AlbumBrowseService SleeveFilterService
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
    PUBLIC_DEPS ProjectService EditPipeline EditGraph
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
    PUBLIC_DEPS ProjectService EditHistory Storage
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
    PRIVATE_DEPS EditHistory EditorAdjustmentPipeline
)

def_library(EditorMiniGitJournalFold
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_journal_fold.cpp"
    PUBLIC_DEPS EditHistory
)

def_library(EditorMiniGitCommitWriter
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_commit_writer.cpp"
    PUBLIC_DEPS ProjectService EditHistory Storage
)

def_library(EditorMiniGitJournalRecovery
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_journal_recovery.cpp"
    PUBLIC_DEPS EditHistory Storage
)

def_library(EditorMiniGitMaterializer
    SOURCES "${ALCEDO_APP_ROOT}/editor_mini_git_materializer.cpp"
    PUBLIC_DEPS ProjectService EditHistory Storage
    PRIVATE_DEPS EditorSaveCheckpointCoordinator
                 EditorMiniGitCommitWriter EditorMiniGitJournalRecovery
)

def_library(EditorAdjustmentPipeline
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_adjustment_pipeline.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_adjustment_pipeline.hpp"
    PUBLIC_DEPS EditPipeline EditHistory JSON
)

def_library(EditorPipelineCommandService
    SOURCES
        "${ALCEDO_APP_ROOT}/editor_pipeline_command_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/editor_pipeline_command_service.hpp"
    PUBLIC_DEPS EditGraph JSON
)

def_library(PipelineHistoryApplier
    SOURCES
        "${ALCEDO_APP_ROOT}/pipeline_history_applier.cpp"
        "${ALCEDO_APP_ROOT}/pipeline_document_history.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/pipeline_history_applier.hpp"
        "${ALCEDO_INCLUDE_ROOT}/app/pipeline_document_history.hpp"
    PUBLIC_DEPS EditorPipelineCommandService EditHistory EditGraph EditMaskStore
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
    PRIVATE_DEPS xxHash JSON ${ALCEDO_OPENCV_CORE_DEPS} AppDiagnostics
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
    # Import assembly calls LoadPipeline / SyncPipeline / InitializeImageRoot /
    # SavePipeline on the optional PipelineMgmtService injected at construction.
    PUBLIC_DEPS SleeveManager ImageWriter ImagePoolService FSService PipelineMgmtService
)

def_library(ThumbnailService
    SOURCES "${ALCEDO_APP_ROOT}/thumbnail_service.cpp"
    PUBLIC_DEPS EditPipeline ThreadPool StrConv ImagePoolService PipelineMgmtService
                ThumbnailDiskCacheService Storage
)

def_library(SemanticGenerationService
    SOURCES
        "${ALCEDO_APP_ROOT}/semantic_generation_service.cpp"
        "${ALCEDO_INCLUDE_ROOT}/app/semantic_generation_service.hpp"
        "${ALCEDO_INCLUDE_ROOT}/storage/store/semantic/semantic_label_config.hpp"
    PUBLIC_DEPS AiSidecarRuntimeService ThumbnailService Storage opencv_core opencv_imgproc
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
    PUBLIC_DEPS ExportRecipe ImageWriter ImageLoader ImagePoolService PipelineMgmtService PipelineScheduler ThreadPool
)
