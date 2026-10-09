/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform/env.h>
#include <rex/string.h>

#include <dirent.h>
#include <ftw.h>
#include <libgen.h>
#include <pwd.h>

#if REX_PLATFORM_NX
#include <errno.h>

#include <mutex>
#endif

namespace rex {

std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return rex::string::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) {
  return source;
}

std::filesystem::path to_path(const std::u16string_view source) {
  return rex::string::to_utf8(source);
}

namespace filesystem {

std::filesystem::path GetExecutablePath() {
  char buff[FILENAME_MAX] = "";
  readlink("/proc/self/exe", buff, FILENAME_MAX);
  std::string s(buff);
  return s;
}

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetUserFolder() {
#if REX_PLATFORM_NX
  // Horizon has no users, HOME or passwd database (libnx declares getuid and
  // getpwuid_r but does not implement them). All user data lives on the SD.
  return std::filesystem::path("sdmc:/switch/theft4");
#else
  // get preferred data home
  if (auto xdg = rex::platform::env::get("XDG_DATA_HOME")) {
    return std::filesystem::path(*xdg);
  }

  // if XDG_DATA_HOME not set, fallback to HOME directory
  if (auto home = rex::platform::env::get("HOME")) {
    return std::filesystem::path(*home) / ".local" / "share";
  }

  // if HOME not set, fall back to passwd entry
  struct passwd pw1;
  struct passwd* pw;
  char buf[4096];  // could potentionally lower this
  getpwuid_r(getuid(), &pw1, buf, sizeof(buf), &pw);
  assert(&pw1 == pw);  // sanity check
  return std::filesystem::path(pw->pw_dir) / ".local" / "share";
#endif
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return fopen(path.c_str(), std::string(mode).c_str());
}

#if REX_PLATFORM_NX
// newlib has no LFS64 API; on aarch64 off_t is already 64-bit.
static_assert(sizeof(off_t) == 8, "Switch off_t must be 64-bit");

bool Seek(FILE* file, int64_t offset, int origin) {
  return fseeko(file, off_t(offset), origin) == 0;
}

int64_t Tell(FILE* file) {
  return int64_t(ftello(file));
}
#else
bool Seek(FILE* file, int64_t offset, int origin) {
  return fseeko64(file, off64_t(offset), origin) == 0;
}

int64_t Tell(FILE* file) {
  return int64_t(ftello64(file));
}
#endif

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file)) {
    return false;
  }
  int64_t position = Tell(file);
  if (position < 0) {
    return false;
  }
#if REX_PLATFORM_NX
  if (ftruncate(fileno(file), off_t(length))) {
    return false;
  }
#else
  if (ftruncate64(fileno(file), off64_t(length))) {
    return false;
  }
#endif
  if (uint64_t(position) > length) {
    if (!Seek(file, 0, SEEK_END)) {
      return false;
    }
  }
  return true;
}

static int removeCallback(const char* fpath, const struct stat* sb, int typeflag,
                          struct FTW* ftwbuf) {
  int rv = remove(fpath);
  return rv;
}

static uint64_t convertUnixtimeToWinFiletime(time_t unixtime) {
  // Linux uses number of seconds since 1/1/1970, and Windows uses
  // number of nanoseconds since 1/1/1601
  // so we convert linux time to nanoseconds and then add the number of
  // nanoseconds from 1601 to 1970
  // see https://msdn.microsoft.com/en-us/library/ms724228
  uint64_t filetime = filetime = (unixtime * 10000000) + 116444736000000000;
  return filetime;
}

#if REX_PLATFORM_NX
// newlib/libnx have no pread/pwrite. Emulate them with lseek + read/write and
// restore the file position afterwards. The mutexes serialise these helpers
// on the same descriptor only: any other read/write/lseek on the same fd
// outside them can interleave and see or move the temporary position.
// Run 18: one mutex for every file made the reads of the async worker wait
// behind the 14 ms writes of the cache: copy; now one of 64, picked by fd.
static std::mutex g_nx_positional_io_mutex[64];
static std::atomic<uint64_t> g_nx_short_reads{0}, g_nx_short_writes{0};

static ssize_t NxPositionalIo(int fd, void* buffer, size_t length, off_t offset, bool write) {
  std::lock_guard<std::mutex> lock(g_nx_positional_io_mutex[unsigned(fd) % 64]);
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved < 0 || lseek(fd, offset, SEEK_SET) < 0) {
    return -1;
  }
  const ssize_t result = write ? ::write(fd, buffer, length) : ::read(fd, buffer, length);
  const int saved_errno = errno;
  lseek(fd, saved, SEEK_SET);
  errno = saved_errno;
  return result;
}

static ssize_t pread(int fd, void* buffer, size_t length, off_t offset) {
  return NxPositionalIo(fd, buffer, length, offset, false);
}

static ssize_t pwrite(int fd, const void* buffer, size_t length, off_t offset) {
  return NxPositionalIo(fd, const_cast<void*>(buffer), length, offset, true);
}
#endif  // REX_PLATFORM_NX

#if REX_PLATFORM_NX
void GetNxShortTransfers(uint64_t* short_reads, uint64_t* short_writes) {
  *short_reads = g_nx_short_reads.load(std::memory_order_relaxed);
  *short_writes = g_nx_short_writes.load(std::memory_order_relaxed);
}
#endif

bool CreateEmptyFile(const std::filesystem::path& path) {
#if REX_PLATFORM_NX
  // newlib/libnx have no creat(); this is its POSIX definition.
  int file = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0774);
#else
  int file = creat(path.c_str(), 0774);
#endif
  if (file >= 0) {
    close(file);
    return true;
  }
  return false;
}

class PosixFileHandle : public FileHandle {
 public:
  PosixFileHandle(std::filesystem::path path, int handle)
      : FileHandle(std::move(path)), handle_(handle) {}
  ~PosixFileHandle() override {
    close(handle_);
    handle_ = -1;
  }
  // A transfer shorter than asked is repeated for the rest until the whole
  // length is done, end of file (0) or an error; on NX each repeat counts as
  // a short read or write (GetNxShortTransfers).
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    size_t done = 0;
    while (done < buffer_length) {
      ssize_t out = pread(handle_, static_cast<uint8_t*>(buffer) + done, buffer_length - done,
                          off_t(file_offset + done));
      if (out < 0) {
        *out_bytes_read = done;
        LogFailure("read", file_offset + done, buffer_length - done);
        return false;
      }
      if (out == 0) {
        break;
      }
      done += size_t(out);
#if REX_PLATFORM_NX
      if (done < buffer_length) {
        g_nx_short_reads.fetch_add(1, std::memory_order_relaxed);
      }
#endif
    }
    *out_bytes_read = done;
    return true;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    size_t done = 0;
    while (done < buffer_length) {
      ssize_t out = pwrite(handle_, static_cast<const uint8_t*>(buffer) + done,
                           buffer_length - done, off_t(file_offset + done));
      if (out <= 0) {
        *out_bytes_written = done;
        LogFailure("write", file_offset + done, buffer_length - done);
        return false;
      }
      done += size_t(out);
#if REX_PLATFORM_NX
      if (done < buffer_length) {
        g_nx_short_writes.fetch_add(1, std::memory_order_relaxed);
      }
#endif
    }
    *out_bytes_written = done;
    return true;
  }
  bool SetLength(size_t length) override { return ftruncate(handle_, length) >= 0 ? true : false; }
  void Flush() override { fsync(handle_); }

 private:
  // The guest sees only a status (END_OF_FILE for any failure): the first 32
  // failures say which file, where and the errno.
  void LogFailure(const char* what, size_t offset, size_t length) const {
    const int error = errno;
    static std::atomic<uint32_t> failures{0};
    if (failures.fetch_add(1, std::memory_order_relaxed) < 32) {
      REXFS_WARN("host {} failed: '{}' offset {} length {} fd {} errno {} ({})", what,
                 path().string(), offset, length, handle_, error, strerror(error));
    }
    errno = error;
  }

  int handle_ = -1;
};

#if REX_PLATFORM_NX
// An open folder (see OpenExisting): listing goes through the VFS entry, not
// through this handle.
class NxDirectoryHandle : public FileHandle {
 public:
  explicit NxDirectoryHandle(std::filesystem::path path) : FileHandle(std::move(path)) {}
  bool Read(size_t, void*, size_t, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    return false;
  }
  bool Write(size_t, const void*, size_t, size_t* out_bytes_written) override {
    *out_bytes_written = 0;
    return false;
  }
  bool SetLength(size_t) override { return false; }
  void Flush() override {}
};
#endif  // REX_PLATFORM_NX

std::unique_ptr<FileHandle> FileHandle::OpenExisting(const std::filesystem::path& path,
                                                     uint32_t desired_access,
                                                     bool /*allow_share_delete*/) {
  // POSIX allows unlinking/replacing an open file, so there is no share-delete
  // analog to thread through here.
  // O_RDONLY is 0 and O_WRONLY 1: OR-ing them gave O_WRONLY for a handle
  // asked for reading and writing, and every read on it failed (EBADF). Run 17
  // on Switch: the game reads back the cache:\ copies it writes, got
  // C0000011 eleven times and called XamShowDirtyDiscErrorUI.
  const bool want_read =
      desired_access & (FileAccess::kGenericRead | FileAccess::kGenericExecute |
                        FileAccess::kGenericAll | FileAccess::kFileReadData);
  const bool want_write =
      desired_access & (FileAccess::kGenericWrite | FileAccess::kGenericAll |
                        FileAccess::kFileWriteData | FileAccess::kFileAppendData);
  int open_access = want_write ? (want_read ? O_RDWR : O_WRONLY) : O_RDONLY;
  if (desired_access & FileAccess::kFileAppendData) {
    open_access |= O_APPEND;
  }
  int handle = open(path.c_str(), open_access);
  if (handle == -1) {
#if REX_PLATFORM_NX
    // libnx's open() goes through fsFsOpenFile, which refuses directories, so
    // a guest NtOpenFile of a folder ("game:\" before NtQueryDirectoryFile,
    // run 13) failed with NO_SUCH_FILE. Linux and Windows open folders; here
    // the handle has no descriptor and its reads and writes fail, as on them.
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      return std::make_unique<NxDirectoryHandle>(path);
    }
#endif
    // TODO(benvanik): pick correct response.
    return nullptr;
  }
  return std::make_unique<PosixFileHandle>(path, handle);
}

bool GetInfo(const std::filesystem::path& path, FileInfo* out_info) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      out_info->type = FileInfo::Type::kDirectory;
      out_info->total_size = 0;
    } else {
      out_info->type = FileInfo::Type::kFile;
      out_info->total_size = st.st_size;
    }
    out_info->path = path.parent_path();
    out_info->name = path.filename();
    out_info->create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    out_info->access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    out_info->write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    return true;
  }
  return false;
}

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }

  while (auto ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;

    info.name = ent->d_name;
    struct stat st;
    stat((path / info.name).c_str(), &st);
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    info.path = path;
    if (ent->d_type == DT_DIR) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    result.push_back(info);
  }
  closedir(dir);
  return result;
}

}  // namespace filesystem
}  // namespace rex
