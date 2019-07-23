/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_manager.h"

#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/stfs_container_device.h"

namespace xe {
namespace kernel {
namespace xam {

static const char* kThumbnailFileName = "__thumbnail.png";

static const char* kGameUserContentDirName = "profile";

static int content_device_id_ = 0;

ContentPackage::ContentPackage(KernelState* kernel_state,
                               const std::string_view root_name,
                               const XCONTENT_DATA& data,
                               const std::filesystem::path& package_path)
    : kernel_state_(kernel_state), root_name_(root_name) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);

  auto fs = kernel_state_->file_system();

  std::unique_ptr<vfs::Device> device;
  // If this isn't a folder try mounting as STFS package
  // Otherwise mount as a local host path
  filesystem::FileInfo entry;
  bool doesEntryExist = filesystem::GetInfo(package_path, &entry);

  if (doesEntryExist && entry.type != filesystem::FileInfo::Type::kDirectory) {
    device =
        std::make_unique<vfs::StfsContainerDevice>(device_path_, package_path);
  } else {
    device = std::make_unique<vfs::HostPathDevice>(device_path_, package_path,
                                                   false);
  }

  device->Initialize();
  fs->RegisterDevice(std::move(device));
  fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

ContentManager::ContentManager(KernelState* kernel_state,
                               const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(
    uint32_t content_type) {
  auto title_id = fmt::format("{:8X}", kernel_state_->title_id());

  std::string type_name;
  switch (content_type) {
    case 1:
      // Save games.
      type_name = "00000001";
      break;
    case 2:
      // DLC from the marketplace.
      type_name = "00000002";
      break;
    case 3:
      // Publisher content?
      type_name = "00000003";
      break;
    case 0x000D0000:
      // ???
      type_name = "000D0000";
      break;
    default:
      assert_unhandled_case(data.content_type);
      return std::filesystem::path();
  }

  // Package root path:
  // content_root/title_id/type_name/
  return root_path_ / title_id / type_name;
}

std::filesystem::path ContentManager::ResolvePackagePath(
    const XCONTENT_DATA& data) {
  // Content path:
  // content_root/title_id/type_name/data_file_name/
  auto package_root = ResolvePackageRoot(data.content_type);
  auto package_path = package_root / xe::to_path(data.file_name);
  // Add slash to end of path if this is a folder
  // (or package doesn't exist, meaning we're creating a new folder)
  filesystem::FileInfo entry;
  bool doesEntryExist = filesystem::GetInfo(package_path, &entry);

  if (!doesEntryExist || entry.type == filesystem::FileInfo::Type::kDirectory) {
    package_path += xe::kPathSeparator;
  }
  return package_path;
}

std::vector<XCONTENT_DATA> ContentManager::ListContent(uint32_t device_id,
                                                       uint32_t content_type) {
  std::vector<XCONTENT_DATA> result;

  // Search path:
  // content_root/title_id/type_name/*
  auto package_root = ResolvePackageRoot(content_type);
  auto file_infos = xe::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {
    XCONTENT_DATA content_data;
    content_data.device_id = device_id;
    content_data.content_type = content_type;
    content_data.display_name = xe::path_to_utf16(file_info.name);
    content_data.file_name = xe::path_to_utf8(file_info.name);

    auto headers_path = file_info.path / file_info.name;
    if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
      headers_path = headers_path / L".headers";
    }

    filesystem::FileInfo entry;
    bool doesEntryExist = filesystem::GetInfo(headers_path, &entry);

    if (doesEntryExist) {
      // File is either package or directory that has .headers file

      if (file_info.type != xe::filesystem::FileInfo::Type::kDirectory) {
        // Not a directory so must be a package, verify size to make sure
        if (file_info.total_size <= vfs::StfsHeader::kHeaderLength) {
          continue;  // Invalid package (maybe .headers file)
        }
      }

      auto map = MappedMemory::Open(headers_path, MappedMemory::Mode::kRead, 0,
                                    vfs::StfsHeader::kHeaderLength);
      if (map) {
        vfs::StfsHeader header;
        header.Read(map->data());

        content_data.content_type = static_cast<uint32_t>(header.content_type);
        content_data.display_name = header.display_names;
        // TODO: select localized display name
        // some games may expect different ones depending on language setting.
        map->Close();
      }
    }
    result.emplace_back(std::move(content_data));
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, const XCONTENT_DATA& data) {
  auto package_path = ResolvePackagePath(data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name,
                                                  data, package_path);
  return package;
}

bool ContentManager::ContentExists(const XCONTENT_DATA& data) {
  auto path = ResolvePackagePath(data);
  return std::filesystem::exists(path);
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name,
                                       const XCONTENT_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key(root_name))) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  auto package_path = ResolvePackagePath(data);
  if (std::filesystem::exists(package_path)) {
    // Exists, must not!
    return X_ERROR_ALREADY_EXISTS;
  }

  if (!std::filesystem::create_directories(package_path)) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto package = ResolvePackage(root_name, data);
  assert_not_null(package);

  open_packages_.insert({string_key::create(root_name), package.release()});

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name,
                                     const XCONTENT_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (open_packages_.count(string_key(root_name))) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  auto package_path = ResolvePackagePath(data);
  if (!std::filesystem::exists(package_path)) {
    // Does not exist, must be created.
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Open package.
  auto package = ResolvePackage(root_name, data);
  assert_not_null(package);

  open_packages_.insert({string_key::create(root_name), package.release()});

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  auto global_lock = global_critical_region_.Acquire();

  auto it = open_packages_.find(string_key(root_name));
  if (it == open_packages_.end()) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto package = it->second;
  open_packages_.erase(it);
  delete package;

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(const XCONTENT_DATA& data,
                                             std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(data);
  auto thumb_path = package_path / kThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = xe::filesystem::OpenFile(thumb_path, "rb");
    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::SetContentThumbnail(const XCONTENT_DATA& data,
                                             std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(data);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    auto file = xe::filesystem::OpenFile(thumb_path, "wb");
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::DeleteContent(const XCONTENT_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  auto package_path = ResolvePackagePath(data);

  filesystem::FileInfo entry;
  bool doesEntryExist = filesystem::GetInfo(package_path, &entry);

  if (doesEntryExist) {
    remove(package_path);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = fmt::format("{:8X}", kernel_state_->title_id());
  auto user_name = xe::to_path(kernel_state_->user_profile()->name());

  // Per-game per-profile data location:
  // content_root/title_id/profile/user_name
  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
