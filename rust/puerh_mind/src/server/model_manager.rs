use std::path::{Path, PathBuf};

use tonic::{Request, Response, Status};

use crate::proto::semantic::{
    DeleteModelRequest, ListInstalledModelsRequest, ListInstalledModelsResponse,
    ListModelProfilesRequest, ListModelProfilesResponse, ModelAsset, ModelManagerResponse,
    ModelProfile, ResolvedModelManifest as ProtoResolvedModelManifest, ValidateModelRequest,
    model_manager_service_server::ModelManagerService,
};
use crate::service::model_assets::{
    ModelAssetSpec, ModelProfileStatus, ResolvedModelManifest, delete_model_profile,
    list_installed_profiles, list_profiles, validate_model_profile,
};

#[derive(Debug, Clone)]
pub struct ModelManagerServiceImpl {
    default_model_root: PathBuf,
    #[allow(dead_code)]
    default_hf_endpoint: String,
}

impl ModelManagerServiceImpl {
    pub fn new(
        default_model_root: impl Into<PathBuf>,
        default_hf_endpoint: impl Into<String>,
    ) -> Self {
        Self {
            default_model_root: default_model_root.into(),
            default_hf_endpoint: default_hf_endpoint.into(),
        }
    }

    fn base_root(&self, model_root: &str) -> PathBuf {
        if model_root.trim().is_empty() {
            self.default_model_root.clone()
        } else {
            PathBuf::from(model_root)
        }
    }

    fn profile_root(&self, profile_id: &str, model_root: &str) -> PathBuf {
        let base = self.base_root(model_root);
        if base.file_name().and_then(|name| name.to_str()) == Some(profile_id) {
            base
        } else {
            base.join(profile_id)
        }
    }

    fn response_from_result(
        status: &'static str,
        profile_id: &str,
        root: &Path,
        result: anyhow::Result<ResolvedModelManifest>,
    ) -> ModelManagerResponse {
        match result {
            Ok(manifest) => {
                let profile = crate::service::model_assets::find_profile(profile_id).ok();
                ModelManagerResponse {
                    ok: true,
                    status: status.to_string(),
                    error: String::new(),
                    profile: profile.map(|profile| {
                        status_to_proto(&ModelProfileStatus {
                            profile,
                            model_root: root.to_path_buf(),
                            installed: true,
                            status: status.to_string(),
                        })
                    }),
                    manifest: Some(manifest_to_proto(&manifest)),
                }
            }
            Err(err) => ModelManagerResponse {
                ok: false,
                status: "error".to_string(),
                error: err.to_string(),
                profile: crate::service::model_assets::find_profile(profile_id)
                    .ok()
                    .map(|profile| {
                        status_to_proto(&ModelProfileStatus {
                            profile,
                            model_root: root.to_path_buf(),
                            installed: false,
                            status: err.to_string(),
                        })
                    }),
                manifest: None,
            },
        }
    }
}

#[tonic::async_trait]
impl ModelManagerService for ModelManagerServiceImpl {
    async fn list_model_profiles(
        &self,
        request: Request<ListModelProfilesRequest>,
    ) -> Result<Response<ListModelProfilesResponse>, Status> {
        let req = request.into_inner();
        let base = self.base_root(&req.model_root);
        let profiles = list_profiles(Some(&base))
            .iter()
            .map(status_to_proto)
            .collect();
        Ok(Response::new(ListModelProfilesResponse { profiles }))
    }

    async fn list_installed_models(
        &self,
        request: Request<ListInstalledModelsRequest>,
    ) -> Result<Response<ListInstalledModelsResponse>, Status> {
        let req = request.into_inner();
        let base = self.base_root(&req.model_root);
        let profiles = list_installed_profiles(Some(&base))
            .iter()
            .map(status_to_proto)
            .collect();
        Ok(Response::new(ListInstalledModelsResponse { profiles }))
    }

    async fn validate_model(
        &self,
        request: Request<ValidateModelRequest>,
    ) -> Result<Response<ModelManagerResponse>, Status> {
        let req = request.into_inner();
        let root = self.profile_root(&req.profile_id, &req.model_root);
        let response = Self::response_from_result(
            "installed",
            &req.profile_id,
            &root,
            validate_model_profile(&req.profile_id, &root),
        );
        Ok(Response::new(response))
    }

    async fn delete_model(
        &self,
        request: Request<DeleteModelRequest>,
    ) -> Result<Response<ModelManagerResponse>, Status> {
        let req = request.into_inner();
        let root = self.profile_root(&req.profile_id, &req.model_root);
        let result = delete_model_profile(&req.profile_id, &root).and_then(|_| {
            validate_model_profile(&req.profile_id, &root)
                .map_err(|_| anyhow::anyhow!("model profile deleted"))
        });
        let mut response = Self::response_from_result("deleted", &req.profile_id, &root, result);
        if !response.ok && response.error == "model profile deleted" {
            response.ok = true;
            response.status = "deleted".to_string();
            response.error.clear();
        }
        Ok(Response::new(response))
    }
}

fn asset_to_proto(asset: &ModelAssetSpec, root: &Path) -> ModelAsset {
    ModelAsset {
        role: asset.role.as_str().to_string(),
        repo_id: asset.repo_id.to_string(),
        revision: asset.revision.to_string(),
        remote_path: asset.remote_path.to_string(),
        local_path: root.join(asset.local_path).to_string_lossy().into_owned(),
        size_bytes: asset.size_bytes,
        sha256: asset.sha256.unwrap_or_default().to_string(),
    }
}

fn status_to_proto(status: &ModelProfileStatus) -> ModelProfile {
    ModelProfile {
        profile_id: status.profile.profile_id.to_string(),
        display_name: status.profile.display_name.to_string(),
        model_id: status.profile.model_id.to_string(),
        revision: status.profile.revision.to_string(),
        engine_profile_id: status.profile.engine_profile_id.to_string(),
        language: status.profile.language.as_str().to_string(),
        embedding_dimension: status.profile.embedding_dimension,
        native_embedding_dimension: status.profile.native_embedding_dimension,
        image_size: status.profile.image_size,
        installed: status.installed,
        local_root: status.model_root.to_string_lossy().into_owned(),
        status: status.status.clone(),
        assets: status
            .profile
            .assets
            .iter()
            .map(|asset| asset_to_proto(asset, &status.model_root))
            .collect(),
        embedding_transform: status.profile.embedding_transform.to_string(),
    }
}

fn manifest_to_proto(manifest: &ResolvedModelManifest) -> ProtoResolvedModelManifest {
    ProtoResolvedModelManifest {
        profile_id: manifest.profile_id.clone(),
        model_id: manifest.model_id.clone(),
        revision: manifest.revision.clone(),
        engine_profile_id: manifest.engine_profile_id.clone(),
        language: manifest.language.clone(),
        embedding_dimension: manifest.embedding_dimension,
        native_embedding_dimension: manifest.native_embedding_dimension,
        image_size: manifest.image_size,
        model_root: manifest.model_root.clone(),
        assets: manifest
            .assets
            .iter()
            .map(|asset| ModelAsset {
                role: asset.role.clone(),
                repo_id: asset.repo_id.clone(),
                revision: asset.revision.clone(),
                remote_path: asset.remote_path.clone(),
                local_path: asset.local_path.clone(),
                size_bytes: asset.size_bytes,
                sha256: asset.sha256.clone(),
            })
            .collect(),
        embedding_transform: manifest.embedding_transform.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unique_root() -> PathBuf {
        std::env::temp_dir().join(format!(
            "alcedo-model-manager-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system clock should be valid")
                .as_nanos()
        ))
    }

    #[tokio::test]
    async fn lists_fixed_model_profiles_with_dimension_policy() {
        let root = unique_root();
        let service = ModelManagerServiceImpl::new(&root, "https://hf-mirror.com");
        let response = service
            .list_model_profiles(Request::new(ListModelProfilesRequest {
                model_root: root.to_string_lossy().into_owned(),
            }))
            .await
            .expect("list should succeed")
            .into_inner();

        assert_eq!(
            response.profiles.len(),
            crate::service::model_assets::supported_model_profiles().count()
        );
        assert!(
            response
                .profiles
                .iter()
                .any(|profile| profile.language == "multilingual"
                    && profile.profile_id == "jina-clip-v2-int8-multilingual"
                    && profile.embedding_dimension == 512
                    && profile.native_embedding_dimension == 1024
                    && profile.embedding_transform == "matryoshka_truncate_then_l2_normalize")
        );
        assert!(
            response
                .profiles
                .iter()
                .any(|profile| profile.language == "multilingual"
                    && profile.profile_id == "siglip2-b32-256-multilingual"
                    && profile.embedding_dimension == 768
                    && profile.native_embedding_dimension == 768
                    && profile.embedding_transform == "l2_normalize")
        );
        let has_coreml = response.profiles.iter().any(|profile| {
            profile.language == "multilingual"
                && profile.profile_id == "siglip2-base-256-coreml-macos"
                && profile.engine_profile_id == "siglip2-coreml-native"
                && profile.embedding_dimension == 768
                && profile.native_embedding_dimension == 768
                && profile.embedding_transform == "l2_normalize"
        });
        if cfg!(target_os = "macos") {
            assert!(has_coreml);
        } else {
            assert!(!has_coreml);
        }
        assert!(response.profiles.iter().all(|profile| profile.profile_id
            != "siglip2-base-256-coreml-macos"
            || cfg!(target_os = "macos")));
        assert!(response.profiles.iter().all(|profile| !profile.installed));
        assert!(!root.exists());
    }

    #[tokio::test]
    async fn validate_missing_model_returns_structured_error() {
        let root = unique_root();
        let service = ModelManagerServiceImpl::new(&root, "https://hf-mirror.com");
        let response = service
            .validate_model(Request::new(ValidateModelRequest {
                profile_id: crate::service::model_assets::MOBILECLIP2_ONNX_PROFILE.to_string(),
                model_root: root.to_string_lossy().into_owned(),
            }))
            .await
            .expect("validate should return a response")
            .into_inner();

        assert!(!response.ok);
        assert!(response.error.contains("missing model root directory"));
    }
}
