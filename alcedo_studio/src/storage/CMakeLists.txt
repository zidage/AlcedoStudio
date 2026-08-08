# Storage & DB target ownership (DuckDB ORM, mappers, storage service).

# ORM Layer
def_library(DuckORM
    SOURCES
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_orm.cpp"
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_types.cpp"
        "${ALCEDO_SRC_ROOT}/storage/mapper/duckorm/duckdb_expr.cpp"
    PUBLIC_DEPS DuckDB
)

# Mappers
set(MAPPER_SRCS
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/file_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/folder_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/element_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/element/element_id_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/base/base_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/image/image_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/filter/filter_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/history_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/recovery_metadata_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/edit_commit_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/version_ref_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/sleeve/edit_history/image_edit_state_mapper.cpp"
    "${ALCEDO_SRC_ROOT}/storage/mapper/pipeline/pipeline_mapper.cpp"
)

def_library(SleeveMapper
    SOURCES ${MAPPER_SRCS}
    PUBLIC_DEPS DuckDB DuckORM Image SleeveElement ImagePool EditPipeline PipelineScheduler
)

# Services
set(SERVICE_SRCS
    "${ALCEDO_SRC_ROOT}/sleeve/storage_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/controller_types.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/db_controller.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/image/image_controller.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/semantic/semantic_storage_controller.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/ai/ai_storage_controller.cpp"
    "${ALCEDO_SRC_ROOT}/storage/controller/sleeve/element_controller.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/image/image_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/base/base_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/base/root_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/element/element_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/element/element_id_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/element/file_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/element/folder_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/edit_history/history_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/sleeve/edit_history/commit_graph_service.cpp"
    "${ALCEDO_SRC_ROOT}/storage/service/pipeline/pipeline_service.cpp"
)

def_library(StorageService
    SOURCES ${SERVICE_SRCS}
    PUBLIC_DEPS Ai DuckDB DuckORM SleeveMapper StrConv PipelineScheduler
    PRIVATE_DEPS AppDiagnostics
)
