/**
 * @file        core/memory_switch.cpp
 * @brief       Nintendo Switch (libnx) memory backend: guest memory design A
 *
 * See docs/switch-port/03-memory.md. Horizon gives an application no way to
 * map the same pages at two addresses (svcMapProcessCodeMemory moves memory,
 * CodeMemory gives one RW owner view only, svcMapPhysicalMemory needs a
 * system resource the process does not have) and the loader hands the whole
 * pool to the heap. So on NX:
 *
 * - The guest address space is ONE virtmem reservation (the "arena") with
 *   no aliased views:
 *     arena + 0x00000000 .. 0x8FFFFFFF   guest 0x00000000-0x8FFFFFFF
 *     arena + 0x90000000 .. +512 MiB+4K  guest physical memory (physical_base)
 *   Guest 0x90000000-0x9FFFFFFF (XEX 4 KiB pages) reuses the 0x80000000
 *   host range; 0x7F000000 and the 0xA0000000/0xC0000000/0xE0000000 windows
 *   reuse physical_base (0xE0000000 with the +4 KiB offset the physical heap
 *   applies). rex_guest_table[addr >> 24] holds, per 16 MiB slice, the host
 *   address of guest 0 for that slice: host = addr + rex_guest_table[addr >> 24].
 *
 * - Backing is committed on demand in 2 MiB blocks: a block taken with
 *   aligned_alloc from the heap is moved to its arena offset with
 *   svcMapProcessCodeMemory and made RW with svcSetProcessMemoryPermission.
 *   Every 4 KiB host page carries a reference count: xmemory.cpp commits and
 *   decommits one reference per heap page transition (the physical heap and
 *   the 0xA/0xC/0xE window heaps, and the 0x80/0x90 XEX heaps, share host
 *   pages). A block is moved back to the heap and freed when none of its
 *   pages is referenced.
 *
 * - Protection: svcSetProcessMemoryPermission, 4 KiB granularity, on mapped
 *   blocks only. Unmapped blocks have no pages to protect.
 *
 * Addresses outside the arena (none in the runtime today) fall back to heap
 * memory for AllocFixed(nullptr) and plain svcSetMemoryPermission for Protect.
 * The file-mapping API is not used on NX: xmemory.cpp reserves the arena
 * through rex::memory::nx::ReserveGuestArena instead.
 *
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/memory/guest_table.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>

extern "C" {
#include <switch/arm/counter.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/virtmem.h>
#include <switch/result.h>
#include <switch/runtime/env.h>
}

extern "C" {
alignas(64) uint64_t rex_guest_table[256];
uint8_t* rex_guest_virtual_base;
uint8_t* rex_guest_physical_base;
}

namespace rex {
namespace memory {

namespace {

constexpr size_t kNxPageSize = 0x1000;
constexpr size_t kNxBlockSize = 0x200000;
constexpr uint32_t kNxPagesPerBlock = kNxBlockSize / kNxPageSize;

// Arena layout (see the file header).
constexpr uint64_t kVirtualSpan = 0x90000000ull;
constexpr uint64_t kPhysicalSpan = 0x20000000ull + kNxBlockSize;  // +4 KiB of 0xE0, rounded
constexpr uint64_t kArenaSize = kVirtualSpan + kPhysicalSpan;
constexpr size_t kArenaBlocks = kArenaSize / kNxBlockSize;

struct Block {
  void* source = nullptr;      // aligned_alloc'd heap memory while moved into the arena
  uint16_t referenced = 0;     // pages of this block with a non-zero reference count
};

struct Arena {
  std::mutex mutex;
  uint8_t* base = nullptr;
  VirtmemReservation* reservation = nullptr;
  Handle process = INVALID_HANDLE;
  std::vector<uint16_t> page_refs;  // one per 4 KiB page of the arena
  std::vector<Block> blocks;
  nx::GuestArenaStats stats;
};

Arena g_arena;

inline size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

inline bool InArena(const void* address) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(address);
  const uintptr_t base = reinterpret_cast<uintptr_t>(g_arena.base);
  return g_arena.base && a >= base && a < base + kArenaSize;
}

uint32_t ToNxPermission(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return Perm_None;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      return Perm_R;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      return Perm_Rw;
    default:
      return Perm_None;
  }
}

PageAccess NxPermToPageAccess(uint32_t perm) {
  const bool r = (perm & Perm_R) != 0;
  const bool w = (perm & Perm_W) != 0;
  const bool x = (perm & Perm_X) != 0;
  if (!r && !w && !x) return PageAccess::kNoAccess;
  if (x) return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

uint8_t* BlockAddress(size_t block) {
  return g_arena.base + block * kNxBlockSize;
}

// Moves a zeroed 2 MiB heap block to its arena offset and makes it RW.
// Caller holds g_arena.mutex.
bool MapBlock(size_t block) {
  Block& b = g_arena.blocks[block];
  void* source = std::aligned_alloc(kNxBlockSize, kNxBlockSize);
  if (!source) {
    REXSYS_ERROR("memory_switch: aligned_alloc(2 MiB) failed for arena block {} ({} mapped)",
                 block, g_arena.stats.mapped_blocks);
    return false;
  }
  std::memset(source, 0, kNxBlockSize);
  const u64 dst = reinterpret_cast<u64>(BlockAddress(block));
  const u64 t0 = armGetSystemTick();
  Result rc = svcMapProcessCodeMemory(g_arena.process, dst, reinterpret_cast<u64>(source),
                                      kNxBlockSize);
  if (R_SUCCEEDED(rc)) {
    rc = svcSetProcessMemoryPermission(g_arena.process, dst, kNxBlockSize, Perm_Rw);
    if (R_FAILED(rc)) {
      REXSYS_ERROR("memory_switch: svcSetProcessMemoryPermission(RW) block {} rc=0x{:x}", block,
                   rc);
      svcUnmapProcessCodeMemory(g_arena.process, dst, reinterpret_cast<u64>(source),
                                kNxBlockSize);
    }
  } else {
    REXSYS_ERROR("memory_switch: svcMapProcessCodeMemory block {} rc=0x{:x}", block, rc);
  }
  if (R_FAILED(rc)) {
    std::free(source);
    return false;
  }
  g_arena.stats.map_ticks += armGetSystemTick() - t0;
  b.source = source;
  ++g_arena.stats.mapped_blocks;
  ++g_arena.stats.map_calls;
  g_arena.stats.peak_mapped_blocks =
      std::max(g_arena.stats.peak_mapped_blocks, g_arena.stats.mapped_blocks);
  return true;
}

// Moves a block back to the heap and frees it. Caller holds g_arena.mutex.
void UnmapBlock(size_t block) {
  Block& b = g_arena.blocks[block];
  const u64 dst = reinterpret_cast<u64>(BlockAddress(block));
  // Pages may have been protected R or none; the unmap wants the block as mapped.
  svcSetProcessMemoryPermission(g_arena.process, dst, kNxBlockSize, Perm_Rw);
  const Result rc = svcUnmapProcessCodeMemory(g_arena.process, dst,
                                              reinterpret_cast<u64>(b.source), kNxBlockSize);
  if (R_FAILED(rc)) {
    // The source stays moved: it must not go back to malloc. Keep the block
    // mapped so a later commit can reuse it.
    REXSYS_ERROR("memory_switch: svcUnmapProcessCodeMemory block {} rc=0x{:x}; block kept",
                 block, rc);
    return;
  }
  std::free(b.source);
  b.source = nullptr;
  --g_arena.stats.mapped_blocks;
  ++g_arena.stats.unmap_calls;
}

// Applies `perm` to [address, address + length) on mapped blocks only.
// Caller holds g_arena.mutex.
bool ProtectArena(uint8_t* address, size_t length, uint32_t perm) {
  bool ok = true;
  uint8_t* end = address + length;
  while (address < end) {
    const size_t block = size_t(address - g_arena.base) / kNxBlockSize;
    uint8_t* block_end = std::min(end, BlockAddress(block) + kNxBlockSize);
    if (g_arena.blocks[block].source) {
      const Result rc = svcSetProcessMemoryPermission(
          g_arena.process, reinterpret_cast<u64>(address), size_t(block_end - address), perm);
      if (R_FAILED(rc)) {
        REXSYS_ERROR("memory_switch: svcSetProcessMemoryPermission({}, 0x{:x}, {}) rc=0x{:x}",
                     static_cast<void*>(address), size_t(block_end - address), perm, rc);
        ok = false;
      }
    }
    address = block_end;
  }
  return ok;
}

void DecommitArena(uint8_t* address, size_t length);

// One reference on every page of the range; maps blocks and zeroes pages
// that become referenced. Caller holds g_arena.mutex.
bool CommitArena(uint8_t* address, size_t length) {
  const size_t first_page = size_t(address - g_arena.base) / kNxPageSize;
  const size_t last_page = first_page + length / kNxPageSize - 1;
  for (size_t page = first_page; page <= last_page; ++page) {
    const size_t block = page / kNxPagesPerBlock;
    Block& b = g_arena.blocks[block];
    uint16_t& refs = g_arena.page_refs[page];
    if (refs == 0) {
      if (!b.source) {
        if (!MapBlock(block)) {
          if (page > first_page) {
            DecommitArena(address, (page - first_page) * kNxPageSize);
          }
          return false;
        }
      } else {
        // Reused page of a block that stayed mapped: give it zeroed contents
        // as a fresh commit would.
        uint8_t* p = g_arena.base + page * kNxPageSize;
        svcSetProcessMemoryPermission(g_arena.process, reinterpret_cast<u64>(p), kNxPageSize,
                                      Perm_Rw);
        std::memset(p, 0, kNxPageSize);
      }
      ++b.referenced;
      ++g_arena.stats.committed_pages;
    }
    ++refs;
  }
  return true;
}

// Drops one reference on every referenced page of the range and returns
// blocks with no referenced page. Caller holds g_arena.mutex.
void DecommitArena(uint8_t* address, size_t length) {
  const size_t first_page = size_t(address - g_arena.base) / kNxPageSize;
  const size_t last_page = first_page + length / kNxPageSize - 1;
  for (size_t page = first_page; page <= last_page; ++page) {
    uint16_t& refs = g_arena.page_refs[page];
    if (refs == 0) {
      continue;
    }
    if (--refs == 0) {
      const size_t block = page / kNxPagesPerBlock;
      Block& b = g_arena.blocks[block];
      --g_arena.stats.committed_pages;
      if (--b.referenced == 0) {
        UnmapBlock(block);
      }
    }
  }
}

}  // namespace

size_t page_size() { return kNxPageSize; }
size_t allocation_granularity() { return kNxBlockSize; }

bool IsWritableExecutableMemorySupported() { return false; }

// ── Guest arena ──────────────────────────────────────────────────────────────

namespace nx {

bool ReserveGuestArena(GuestArena* out) {
  std::lock_guard<std::mutex> lock(g_arena.mutex);
  if (g_arena.base) {
    REXSYS_ERROR("memory_switch: guest arena already reserved at {}",
                 static_cast<void*>(g_arena.base));
    return false;
  }
  if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78) || !envIsSyscallHinted(0x73)) {
    REXSYS_ERROR(
        "memory_switch: svcMapProcessCodeMemory/svcUnmapProcessCodeMemory/"
        "svcSetProcessMemoryPermission not available to this process");
    return false;
  }
  const Handle process = envGetOwnProcessHandle();
  if (process == INVALID_HANDLE) {
    REXSYS_ERROR("memory_switch: no handle to the own process (envGetOwnProcessHandle)");
    return false;
  }

  // Code-region address space: svcMapProcessCodeMemory destinations must be
  // there. One extra block for 2 MiB alignment.
  virtmemLock();
  void* raw = virtmemFindCodeMemory(kArenaSize + kNxBlockSize, 0);
  uint8_t* base = raw ? reinterpret_cast<uint8_t*>(AlignUp(reinterpret_cast<uintptr_t>(raw),
                                                           kNxBlockSize))
                      : nullptr;
  VirtmemReservation* reservation = base ? virtmemAddReservation(base, kArenaSize) : nullptr;
  virtmemUnlock();
  if (!reservation) {
    REXSYS_ERROR("memory_switch: cannot reserve 0x{:x} bytes of code address space", kArenaSize);
    return false;
  }

  g_arena.base = base;
  g_arena.reservation = reservation;
  g_arena.process = process;
  g_arena.page_refs.assign(kArenaSize / kNxPageSize, 0);
  g_arena.blocks.assign(kArenaBlocks, Block{});
  g_arena.stats = {};

  const uint64_t v = reinterpret_cast<uint64_t>(base);
  const uint64_t p = v + kVirtualSpan;
  for (uint32_t i = 0; i < 256; ++i) {
    uint64_t entry = v;                                   // 0x00-0x7E, 0x80-0x8F
    if (i == 0x7F) {
      entry = p - 0x7F000000ull;                          // GPU writeback -> physical 0
    } else if (i >= 0x90 && i < 0xA0) {
      entry = v - 0x10000000ull;                          // XEX 4 KiB -> XEX 64 KiB
    } else if (i >= 0xA0 && i < 0xC0) {
      entry = p - 0xA0000000ull;
    } else if (i >= 0xC0 && i < 0xE0) {
      entry = p - 0xC0000000ull;
    } else if (i >= 0xE0) {
      entry = p - 0xE0000000ull + 0x1000;                 // PhysicalHeap host_address_offset
    }
    rex_guest_table[i] = entry;
  }
  rex_guest_virtual_base = base;
  rex_guest_physical_base = base + kVirtualSpan;

  out->virtual_base = base;
  out->physical_base = base + kVirtualSpan;
  out->size = kArenaSize;
  REXSYS_INFO("memory_switch: guest arena {}-{} (virtual {}, physical {})",
              static_cast<void*>(base), static_cast<void*>(base + kArenaSize - 1),
              static_cast<void*>(base), static_cast<void*>(base + kVirtualSpan));
  return true;
}

void ReleaseGuestArena() {
  std::lock_guard<std::mutex> lock(g_arena.mutex);
  if (!g_arena.base) {
    return;
  }
  size_t kept = 0;
  for (size_t block = 0; block < g_arena.blocks.size(); ++block) {
    if (g_arena.blocks[block].source) {
      UnmapBlock(block);
      kept += g_arena.blocks[block].source != nullptr;
    }
  }
  if (kept) {
    // Still-moved blocks keep their addresses; leave the reservation in place.
    REXSYS_ERROR("memory_switch: {} arena blocks could not be unmapped; arena leaked", kept);
    return;
  }
  virtmemLock();
  virtmemRemoveReservation(g_arena.reservation);
  virtmemUnlock();
  std::memset(rex_guest_table, 0, sizeof(rex_guest_table));
  rex_guest_virtual_base = nullptr;
  rex_guest_physical_base = nullptr;
  g_arena.base = nullptr;
  g_arena.reservation = nullptr;
  g_arena.page_refs.clear();
  g_arena.page_refs.shrink_to_fit();
  g_arena.blocks.clear();
  g_arena.blocks.shrink_to_fit();
}

GuestArenaStats GetGuestArenaStats() {
  std::lock_guard<std::mutex> lock(g_arena.mutex);
  return g_arena.stats;
}

}  // namespace nx

// ── AllocFixed ───────────────────────────────────────────────────────────────
// Inside the arena:
//   kReserve        nothing to do (the arena reservation covers it)
//   kCommit         one reference per 4 KiB page (see the file header), then
//                   `access` on the whole range
// Outside the arena only base_address == nullptr is supported (heap memory).

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  if (length == 0) return nullptr;
  const size_t page_len = AlignUp(length, kNxPageSize);
  const bool do_commit = allocation_type == AllocationType::kCommit ||
                         allocation_type == AllocationType::kReserveCommit;

  if (base_address && InArena(base_address)) {
    auto* address = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(base_address) & ~uintptr_t(kNxPageSize - 1));
    if (!do_commit) {
      return base_address;
    }
    std::lock_guard<std::mutex> lock(g_arena.mutex);
    if (!CommitArena(address, page_len)) {
      return nullptr;
    }
    if (!ProtectArena(address, page_len, ToNxPermission(access))) {
      return nullptr;
    }
    return base_address;
  }

  if (base_address) {
    REXSYS_ERROR("memory_switch: AllocFixed at {} outside the guest arena is not supported",
                 base_address);
    return nullptr;
  }
  void* memory = std::aligned_alloc(kNxPageSize, page_len);
  if (memory) {
    std::memset(memory, 0, page_len);
  }
  return memory;
}

// ── DeallocFixed ─────────────────────────────────────────────────────────────

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (!base_address) return false;
  if (InArena(base_address)) {
    // Decommit and release both drop the references taken by AllocFixed.
    (void)deallocation_type;
    std::lock_guard<std::mutex> lock(g_arena.mutex);
    DecommitArena(static_cast<uint8_t*>(base_address), AlignUp(length, kNxPageSize));
    return true;
  }
  if (deallocation_type == DeallocationType::kRelease) {
    std::free(base_address);
    return true;
  }
  return false;
}

// ── Protect ──────────────────────────────────────────────────────────────────

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
    MemoryInfo mem_info;
    u32 page_info;
    if (R_SUCCEEDED(svcQueryMemory(&mem_info, &page_info, (u64)base_address))) {
      *out_old_access = NxPermToPageAccess(mem_info.perm);
    }
  }
  const size_t perm_len = AlignUp(length, kNxPageSize);
  if (InArena(base_address)) {
    std::lock_guard<std::mutex> lock(g_arena.mutex);
    return ProtectArena(static_cast<uint8_t*>(base_address), perm_len, ToNxPermission(access));
  }
  return R_SUCCEEDED(svcSetMemoryPermission(base_address, perm_len, ToNxPermission(access)));
}

// ── QueryProtect ─────────────────────────────────────────────────────────────

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  length = 0;

  MemoryInfo mem_info;
  u32 page_info;
  Result rc = svcQueryMemory(&mem_info, &page_info, (u64)base_address);
  if (R_FAILED(rc)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end = mem_info.addr + mem_info.size;
  if (addr < mem_info.addr || addr >= region_end) {
    return false;
  }
  length = static_cast<size_t>(region_end - addr);
  access_out = NxPermToPageAccess(mem_info.perm);
  return true;
}

// ── File mapping ─────────────────────────────────────────────────────────────
// Not available on NX: Horizon has no application-level way to map the same
// memory twice. xmemory.cpp uses nx::ReserveGuestArena instead.

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)access;
  (void)commit;
  REXSYS_ERROR(
      "memory_switch: CreateFileMappingHandle(0x{:x}) is not supported on Switch; guest memory "
      "uses rex::memory::nx::ReserveGuestArena",
      length);
  return kFileMappingHandleInvalid;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)handle;
  (void)path;
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)handle;
  (void)base_address;
  (void)length;
  (void)access;
  (void)file_offset;
  return nullptr;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  (void)base_address;
  (void)length;
  return false;
}

}  // namespace memory
}  // namespace rex
