#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>

namespace rex::system {

namespace file_io {

constexpr uint32_t kScatterPageSize = 4096;
constexpr uint64_t kUseCurrentPosition = std::numeric_limits<uint64_t>::max();

constexpr size_t ScatterSegmentCount(uint32_t length) {
  return (static_cast<size_t>(length) + kScatterPageSize - 1) / kScatterPageSize;
}

constexpr uint32_t ScatterSegmentLength(uint32_t remaining) {
  return remaining < kScatterPageSize ? remaining : kScatterPageSize;
}

constexpr bool UsesCurrentPosition(uint64_t byte_offset) {
  return byte_offset == kUseCurrentPosition;
}

constexpr bool AdvancesPosition(bool synchronous_file, uint64_t byte_offset) {
  return synchronous_file || UsesCurrentPosition(byte_offset);
}

constexpr bool PositionAdvanceOverflows(uint64_t effective_offset, uint32_t requested) {
  return effective_offset > std::numeric_limits<uint64_t>::max() - requested;
}

constexpr bool ScatterOffsetOverflows(uint64_t byte_offset, uint32_t transferred) {
  return byte_offset > std::numeric_limits<uint64_t>::max() - transferred;
}

constexpr uint64_t ScatterExplicitOffset(uint64_t byte_offset, uint32_t transferred) {
  return byte_offset + transferred;
}

constexpr bool ContinueScatter(X_STATUS status, uint32_t requested, uint32_t transferred) {
  return XSUCCEEDED(status) && transferred == requested;
}

enum class ReadWaitObject {
  kFile,
  kSuppliedEvent,
};

constexpr ReadWaitObject SelectReadWaitObject(bool has_supplied_event) {
  return has_supplied_event ? ReadWaitObject::kSuppliedEvent : ReadWaitObject::kFile;
}

// An asynchronous file operation is permitted to complete inline. Prefer that
// path when the caller supplied no per-request event or APC and the file has no
// completion-port association. Callers waiting on FileHandle are required to
// do so only after STATUS_PENDING, so returning the final status inline is a
// complete and durable contract for this case.
constexpr bool IsInlineCompletionEligible(bool synchronous_file, bool has_supplied_event,
                                          bool has_apc_routine, bool has_completion_port) {
  return synchronous_file || (!has_supplied_event && !has_apc_routine && !has_completion_port);
}

}  // namespace file_io

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_directory_information
class X_FILE_DIRECTORY_INFORMATION {
 public:
  // FILE_DIRECTORY_INFORMATION
  be<uint32_t> next_entry_offset;  // 0x0
  be<uint32_t> file_index;         // 0x4
  be<uint64_t> creation_time;      // 0x8
  be<uint64_t> last_access_time;   // 0x10
  be<uint64_t> last_write_time;    // 0x18
  be<uint64_t> change_time;        // 0x20
  be<uint64_t> end_of_file;        // 0x28 size in bytes
  be<uint64_t> allocation_size;    // 0x30
  be<uint32_t> attributes;         // 0x38 X_FILE_ATTRIBUTES
  be<uint32_t> file_name_length;   // 0x3C
  char file_name[1];               // 0x40

  void Write(uint8_t* base, uint32_t p) {
    uint8_t* dst = base + p;
    uint8_t* src = reinterpret_cast<uint8_t*>(this);
    X_FILE_DIRECTORY_INFORMATION* right;
    do {
      auto left = reinterpret_cast<X_FILE_DIRECTORY_INFORMATION*>(dst);
      right = reinterpret_cast<X_FILE_DIRECTORY_INFORMATION*>(src);
      left->next_entry_offset = right->next_entry_offset;
      left->file_index = right->file_index;
      left->creation_time = right->creation_time;
      left->last_access_time = right->last_access_time;
      left->last_write_time = right->last_write_time;
      left->change_time = right->change_time;
      left->end_of_file = right->end_of_file;
      left->allocation_size = right->allocation_size;
      left->attributes = right->attributes;
      left->file_name_length = right->file_name_length;
      std::memcpy(left->file_name, right->file_name, right->file_name_length);

      dst += right->next_entry_offset;
      src += right->next_entry_offset;
    } while (right->next_entry_offset != 0);
  }
};

class XFile : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::File;

  XFile(KernelState* kernel_state, rex::filesystem::File* file, bool synchronous);
  ~XFile() override;

  rex::filesystem::Device* device() const { return file_->entry()->device(); }
  rex::filesystem::Entry* entry() const { return file_->entry(); }
  rex::filesystem::File* file() const { return file_; }
  uint32_t file_access() const { return file_->file_access(); }

  const std::string& path() const { return file_->entry()->path(); }
  const std::string& name() const { return file_->entry()->name(); }

  uint64_t position() const;
  void set_position(uint64_t value);

  X_STATUS QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info, size_t length,
                          const std::string_view file_name, bool restart);

  // Don't do within the global critical region because invalidation callbacks
  // may be triggered (as per the usual rule of not doing I/O within the global
  // critical region).
  X_STATUS Read(uint32_t buffer_guess_address, uint32_t buffer_length, uint64_t byte_offset,
                uint32_t* out_bytes_read, uint32_t apc_context, bool notify_completion = true);

  // Transfer-only entry points used by Nt*File. They never signal the file or
  // queue completion-port packets; the caller must publish completion once.
  X_STATUS ReadTransfer(uint32_t buffer_guest_address, uint32_t buffer_length, uint64_t byte_offset,
                        uint32_t* out_bytes_read);

  X_STATUS ReadScatterTransfer(std::span<const uint32_t> segments, uint32_t length,
                               uint64_t byte_offset, uint32_t* out_bytes_read);

  void BeginIO();
  void CompleteIO(X_STATUS status, uint32_t num_bytes, uint32_t apc_context);
  void ResetWaitEvent();
  void SignalWaitEvent();
  void NotifyIOCompletion(X_STATUS status, uint32_t num_bytes, uint32_t apc_context);

  bool ValidateGuestRange(uint32_t guest_address, uint32_t length, bool require_write) const;
  X_STATUS SnapshotReadScatter(uint32_t segments_guest_address, uint32_t length,
                               std::vector<uint32_t>* out_segments) const;

  X_STATUS ReadScatter(uint32_t segments_guest_address, uint32_t length, uint64_t byte_offset,
                       uint32_t* out_bytes_read, uint32_t apc_context);

  X_STATUS Write(uint32_t buffer_guess_address, uint32_t buffer_length, uint64_t byte_offset,
                 uint32_t* out_bytes_written, uint32_t apc_context);

  X_STATUS SetLength(size_t length);
  X_STATUS Rename(const std::filesystem::path& file_path);

  void RegisterIOCompletionPort(uint32_t key, object_ref<XIOCompletion> port);
  void RemoveIOCompletionPort(uint32_t key);
  bool HasIOCompletionPorts();

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XFile> Restore(KernelState* kernel_state, stream::ByteStream* stream);

  bool is_synchronous() const { return is_synchronous_; }

  void SetFindPattern(const std::string_view pattern) {
    find_engine_.SetRule(pattern);
    find_index_ = 0;
  }

  rex::filesystem::Entry* FindNext() {
    return file_->entry()->IterateChildren(find_engine_, &find_index_);
  }

 protected:
  void NotifyIOCompletionPorts(XIOCompletion::IONotification& notification);

  rex::thread::WaitHandle* GetWaitHandle() override { return async_event_.get(); }

 private:
  XFile();

  bool IsIoTraceEnabled() const;

  X_STATUS ReadInternalLocked(uint32_t buffer_guest_address, uint32_t buffer_length,
                              uint64_t byte_offset, uint32_t* out_bytes_read);

  rex::filesystem::File* file_ = nullptr;
  std::unique_ptr<rex::thread::Event> async_event_ = nullptr;

  mutable std::mutex file_lock_;
  std::mutex completion_port_lock_;
  std::vector<std::pair<uint32_t, object_ref<XIOCompletion>>> completion_ports_;

  // TODO(benvanik): create flags, open state, etc.

  uint64_t position_ = 0;

  rex::filesystem::WildcardEngine find_engine_;
  size_t find_index_ = 0;

  bool is_synchronous_ = false;
};

}  // namespace rex::system
