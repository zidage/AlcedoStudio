//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/photo_stack_service.hpp"

#include <QUuid>
#include <stdexcept>
#include <utility>

namespace alcedo {

PhotoStackService::PhotoStackService(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("Photo stack service requires project storage");
  }
}

auto PhotoStackService::CreateStack(std::span<const sl_element_id_t> file_ids,
                                    sl_element_id_t cover_file_id) -> PhotoStackChange {
  return storage_->GetPhotoStackStore().CreateStack(
      QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(), file_ids, cover_file_id);
}

auto PhotoStackService::AddMembers(const std::string&               stack_id,
                                   std::span<const sl_element_id_t> file_ids) -> PhotoStackChange {
  return storage_->GetPhotoStackStore().AddMembers(stack_id, file_ids);
}

auto PhotoStackService::RemoveMembers(std::span<const sl_element_id_t> file_ids)
    -> PhotoStackChange {
  return storage_->GetPhotoStackStore().RemoveMembers(file_ids);
}

auto PhotoStackService::UngroupStack(const std::string& stack_id) -> PhotoStackChange {
  return storage_->GetPhotoStackStore().UngroupStack(stack_id);
}

auto PhotoStackService::SetCover(const std::string& stack_id, sl_element_id_t file_id)
    -> PhotoStackChange {
  return storage_->GetPhotoStackStore().SetCover(stack_id, file_id);
}

auto PhotoStackService::MoveMember(sl_element_id_t file_id, PhotoStackMove direction)
    -> PhotoStackChange {
  return storage_->GetPhotoStackStore().MoveMember(file_id, direction);
}

auto PhotoStackService::ReadMemberIds(const std::string& stack_id) -> std::vector<sl_element_id_t> {
  return storage_->GetPhotoStackStore().ReadMemberIds(stack_id);
}

}  // namespace alcedo
