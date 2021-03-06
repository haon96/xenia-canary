/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_NULL_DEVICE_H_
#define XENIA_VFS_DEVICES_NULL_DEVICE_H_

#include <string>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class NullEntry;

class NullDevice : public Device {
 public:
  NullDevice(const std::string_view mount_path,
             const std::initializer_list<std::string_view> null_paths);
  ~NullDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return false; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }

  // STFC/cache code seems to require the product of these two to equal 0x10000!
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x10000; }

 private:
  std::string name_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<std::string_view> null_paths_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_NULL_DEVICE_H_
