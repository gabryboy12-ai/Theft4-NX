// switch-smoke probes T1-T11: open questions of the Horizon guest-memory designs
// (docs/switch-port/03-memory.md).
//
//   T1 svcMapProcessCodeMemory + svcSetProcessMemoryPermission as an RW alias
//   T2 CodeMemory (svcCreateCodeMemory + svcControlCodeMemory) view permissions
//   T3 svcMapPhysicalMemory: outside vs inside the alias region, maximum size
//   T4 which backend libnx jitCreate picks on this system
//   T5 svcSetHeapSize: maximum heap and time to add 512 MiB
//   T6 cost of design A's address translation (microbenchmark)
//   T7 cost of design A's translation through a 256-entry table (T6 addresses)
//   T8 on-demand commit: 2 MiB heap blocks moved into a guest-space reservation
//      with svcMapProcessCodeMemory, made RW, used, atomics, then unmapped
//   T9 fault round trip on guest memory (read-only / no-access page, handler
//      restores RW and resumes) and the runtime's physical write watch
//   T10 concurrent write faults: 3 threads, one per core, 10,000 faults each
//      on separate watched pages (is libnx's single exception stack a problem?)
//   T11 guest-memory models with svcMapProcessMemory aliases: the alias
//      primitive on our 2 MiB blocks, model D (every view at commit) and its
//      ceiling, and the access cost of direct / table / nfsmw-nx's macros
//
// Rules: each probe logs BEGIN before doing anything; every syscall that may
// fault or kill the process is preceded by an "about to" line (each line is
// fsync'd, see main.cpp); every syscall result is logged with its Horizon
// result code. A syscall the loader does not report as available is skipped,
// never called: an unpermitted SVC raises an exception instead of returning.
// Memory is only read or written after svcQueryMemory shows the permission.

#include "probes.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <switch.h>

#include <rex/exception_handler.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

namespace {

constexpr u64 kPattern1 = 0x1111222233334444ull;
constexpr u64 kPattern2 = 0x5555666677778888ull;
constexpr u64 kPattern3 = 0x9999AAAABBBBCCCCull;
constexpr size_t k2MiB = 0x200000;

std::string Rc(Result rc) {
  char buf[48];
  if (R_SUCCEEDED(rc)) {
    std::snprintf(buf, sizeof(buf), "rc=0x0 (ok)");
  } else {
    std::snprintf(buf, sizeof(buf), "rc=0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc),
                  R_DESCRIPTION(rc));
  }
  return buf;
}

std::string PermStr(u32 perm) {
  std::string s;
  s += (perm & Perm_R) ? 'R' : '-';
  s += (perm & Perm_W) ? 'W' : '-';
  s += (perm & Perm_X) ? 'X' : '-';
  return s;
}

struct Query {
  bool ok = false;
  MemoryInfo info{};
};

Query QueryAt(const void* address) {
  Query q;
  u32 page_info = 0;
  q.ok = R_SUCCEEDED(svcQueryMemory(&q.info, &page_info, reinterpret_cast<u64>(address)));
  return q;
}

std::string QueryStr(const void* address) {
  Query q = QueryAt(address);
  if (!q.ok) {
    return "svcQueryMemory failed";
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "block 0x%" PRIx64 "+0x%" PRIx64 " type=0x%02x perm=%s attr=0x%x",
                q.info.addr, q.info.size, q.info.type & 0xFF, PermStr(q.info.perm).c_str(),
                q.info.attr);
  return buf;
}

bool CanRead(const void* address) {
  Query q = QueryAt(address);
  return q.ok && (q.info.perm & Perm_R);
}

bool CanWrite(const void* address) {
  Query q = QueryAt(address);
  return q.ok && (q.info.perm & Perm_W);
}

u64 Read64(const void* address) {
  return *reinterpret_cast<const volatile u64*>(address);
}

void Write64(void* address, u64 value) {
  *reinterpret_cast<volatile u64*>(address) = value;
}

// Skips (and logs) when the loader does not report the SVC as available.
bool Hinted(const char* probe, unsigned id, const char* name) {
  if (envIsSyscallHinted(id)) {
    return true;
  }
  SMOKE_INFO("%s SKIP %s (0x%02x) not hinted by the loader; not called", probe, name, id);
  return false;
}

double TicksToMs(u64 ticks) {
  return double(armTicksToNs(ticks)) / 1e6;
}

void* ReserveCodeRegion(size_t size, VirtmemReservation** out) {
  virtmemLock();
  void* address = virtmemFindCodeMemory(size, 0x1000);
  *out = address ? virtmemAddReservation(address, size) : nullptr;
  virtmemUnlock();
  return *out ? address : nullptr;
}

void ReleaseReservation(VirtmemReservation* reservation) {
  if (reservation) {
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
  }
}

// "Write through one view, read through the other", guarded by permissions.
void CrossCheck(const char* probe, const char* from_name, void* from, const char* to_name,
                void* to, u64 pattern) {
  if (!CanWrite(from)) {
    SMOKE_INFO("%s alias %s->%s: %s not writable (%s), skipped", probe, from_name, to_name,
               from_name, QueryStr(from).c_str());
    return;
  }
  if (!CanRead(to)) {
    SMOKE_INFO("%s alias %s->%s: %s not readable (%s), skipped", probe, from_name, to_name,
               to_name, QueryStr(to).c_str());
    return;
  }
  SMOKE_INFO("%s about to write 0x%016" PRIx64 " at %s %p, read at %s %p", probe, pattern,
             from_name, from, to_name, to);
  Write64(from, pattern);
  const u64 read = Read64(to);
  SMOKE_INFO("%s alias %s->%s: wrote 0x%016" PRIx64 " read 0x%016" PRIx64 " -> %s", probe,
             from_name, to_name, pattern, read,
             read == pattern ? "SAME MEMORY" : "DIFFERENT");
}

// ── T1 ──────────────────────────────────────────────────────────────────────

void ProbeProcessCodeMemory() {
  const char* p = "T1";
  SMOKE_INFO("T1 BEGIN svcMapProcessCodeMemory + svcSetProcessMemoryPermission alias");
  if (!Hinted(p, 0x77, "svcMapProcessCodeMemory") ||
      !Hinted(p, 0x78, "svcUnmapProcessCodeMemory") ||
      !Hinted(p, 0x73, "svcSetProcessMemoryPermission")) {
    return;
  }
  const Handle process = envGetOwnProcessHandle();
  if (process == INVALID_HANDLE) {
    SMOKE_INFO("T1 SKIP envGetOwnProcessHandle() returned no handle");
    return;
  }
  SMOKE_INFO("T1 own process handle 0x%x", process);

  const size_t size = 0x10000;
  auto* src = static_cast<u8*>(std::aligned_alloc(0x1000, size));
  VirtmemReservation* reservation = nullptr;
  auto* dst = static_cast<u8*>(ReserveCodeRegion(size, &reservation));
  if (!src || !dst) {
    SMOKE_INFO("T1 SKIP cannot allocate source (%p) or reserve code region (%p)", src, dst);
    std::free(src);
    ReleaseReservation(reservation);
    return;
  }
  std::memset(src, 0, size);
  Write64(src, kPattern1);
  SMOKE_INFO("T1 src %p: %s", src, QueryStr(src).c_str());
  SMOKE_INFO("T1 dst %p: %s", dst, QueryStr(dst).c_str());

  SMOKE_INFO("T1 about to svcMapProcessCodeMemory(dst=%p, src=%p, 0x%zx)", dst, src, size);
  Result rc = svcMapProcessCodeMemory(process, reinterpret_cast<u64>(dst),
                                      reinterpret_cast<u64>(src), size);
  SMOKE_INFO("T1 svcMapProcessCodeMemory %s", Rc(rc).c_str());
  if (R_FAILED(rc)) {
    std::free(src);
    ReleaseReservation(reservation);
    return;
  }
  SMOKE_INFO("T1 after map: src %s", QueryStr(src).c_str());
  SMOKE_INFO("T1 after map: dst %s", QueryStr(dst).c_str());
  if (CanRead(dst)) {
    const u64 v = Read64(dst);
    SMOKE_INFO("T1 dst[0] = 0x%016" PRIx64 " (%s the value written to src before mapping)", v,
               v == kPattern1 ? "equals" : "differs from");
  }

  SMOKE_INFO("T1 about to svcSetProcessMemoryPermission(dst=%p, 0x%zx, RW)", dst, size);
  rc = svcSetProcessMemoryPermission(process, reinterpret_cast<u64>(dst), size, Perm_Rw);
  SMOKE_INFO("T1 svcSetProcessMemoryPermission(RW) %s", Rc(rc).c_str());
  SMOKE_INFO("T1 after RW: src %s", QueryStr(src).c_str());
  SMOKE_INFO("T1 after RW: dst %s", QueryStr(dst).c_str());

  CrossCheck(p, "dst", dst + 8, "src", src + 8, kPattern2);
  CrossCheck(p, "src", src + 16, "dst", dst + 16, kPattern3);

  SMOKE_INFO("T1 about to svcUnmapProcessCodeMemory(dst=%p, src=%p)", dst, src);
  rc = svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(dst), reinterpret_cast<u64>(src),
                                 size);
  SMOKE_INFO("T1 svcUnmapProcessCodeMemory %s", Rc(rc).c_str());
  if (R_FAILED(rc)) {
    SMOKE_INFO("T1 about to svcSetProcessMemoryPermission(dst, R-X) before retrying unmap");
    Result rc2 = svcSetProcessMemoryPermission(process, reinterpret_cast<u64>(dst), size,
                                               Perm_Rx);
    SMOKE_INFO("T1 svcSetProcessMemoryPermission(R-X) %s", Rc(rc2).c_str());
    SMOKE_INFO("T1 about to retry svcUnmapProcessCodeMemory");
    rc = svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(dst),
                                   reinterpret_cast<u64>(src), size);
    SMOKE_INFO("T1 svcUnmapProcessCodeMemory (retry) %s", Rc(rc).c_str());
  }
  SMOKE_INFO("T1 after unmap: src %s", QueryStr(src).c_str());
  if (R_SUCCEEDED(rc)) {
    if (CanRead(src)) {
      const u64 v = Read64(src + 8);
      SMOKE_INFO("T1 src[8] after unmap = 0x%016" PRIx64 " (%s the value written through dst)", v,
                 v == kPattern2 ? "equals" : "differs from");
    }
    std::free(src);
    ReleaseReservation(reservation);
  } else {
    SMOKE_INFO("T1 mapping left in place; source buffer intentionally leaked");
  }
  SMOKE_INFO("T1 END");
}

// ── T2 ──────────────────────────────────────────────────────────────────────

struct ViewTry {
  const char* name;
  u32 perm;
};

void ProbeCodeMemory() {
  const char* p = "T2";
  SMOKE_INFO("T2 BEGIN svcCreateCodeMemory + svcControlCodeMemory");
  if (!Hinted(p, 0x4B, "svcCreateCodeMemory") || !Hinted(p, 0x4C, "svcControlCodeMemory")) {
    return;
  }
  const size_t size = 0x10000;
  auto* src = static_cast<u8*>(std::aligned_alloc(0x1000, size));
  if (!src) {
    SMOKE_INFO("T2 SKIP cannot allocate source");
    return;
  }
  std::memset(src, 0, size);
  Write64(src, kPattern1);
  SMOKE_INFO("T2 src %p before: %s", src, QueryStr(src).c_str());

  Handle code = INVALID_HANDLE;
  SMOKE_INFO("T2 about to svcCreateCodeMemory(src=%p, 0x%zx)", src, size);
  Result rc = svcCreateCodeMemory(&code, src, size);
  SMOKE_INFO("T2 svcCreateCodeMemory %s handle=0x%x", Rc(rc).c_str(), code);
  if (R_FAILED(rc)) {
    std::free(src);
    return;
  }
  SMOKE_INFO("T2 src after create: %s", QueryStr(src).c_str());

  static const ViewTry kTries[] = {
      {"R--", Perm_R}, {"RW-", Perm_Rw}, {"R-X", Perm_Rx}, {"RWX", Perm_R | Perm_W | Perm_X}};

  // Which permissions each side accepts, one mapping at a time.
  for (int side = 0; side < 2; ++side) {
    const CodeMapOperation map_op = side == 0 ? CodeMapOperation_MapOwner : CodeMapOperation_MapSlave;
    const CodeMapOperation unmap_op =
        side == 0 ? CodeMapOperation_UnmapOwner : CodeMapOperation_UnmapSlave;
    const char* side_name = side == 0 ? "owner" : "slave";
    for (const ViewTry& t : kTries) {
      VirtmemReservation* reservation = nullptr;
      void* view = ReserveCodeRegion(size, &reservation);
      if (!view) {
        SMOKE_INFO("T2 cannot reserve a code region");
        break;
      }
      SMOKE_INFO("T2 about to Map%s at %p perm %s", side == 0 ? "Owner" : "Slave", view, t.name);
      rc = svcControlCodeMemory(code, map_op, view, size, t.perm);
      SMOKE_INFO("T2 %s %s: %s", side_name, t.name, Rc(rc).c_str());
      if (R_SUCCEEDED(rc)) {
        SMOKE_INFO("T2   view %s", QueryStr(view).c_str());
        SMOKE_INFO("T2 about to Unmap%s at %p", side == 0 ? "Owner" : "Slave", view);
        Result urc = svcControlCodeMemory(code, unmap_op, view, size, 0);
        SMOKE_INFO("T2   unmap %s", Rc(urc).c_str());
      }
      ReleaseReservation(reservation);
    }
  }

  // Both views at once, as jitCreate does (owner RW, slave R-X), then a second
  // owner and a second slave mapping of the same object.
  VirtmemReservation *r_owner = nullptr, *r_slave = nullptr, *r_owner2 = nullptr,
                     *r_slave2 = nullptr;
  auto* owner = static_cast<u8*>(ReserveCodeRegion(size, &r_owner));
  auto* slave = static_cast<u8*>(ReserveCodeRegion(size, &r_slave));
  auto* owner2 = static_cast<u8*>(ReserveCodeRegion(size, &r_owner2));
  auto* slave2 = static_cast<u8*>(ReserveCodeRegion(size, &r_slave2));
  if (owner && slave && owner2 && slave2) {
    SMOKE_INFO("T2 about to MapOwner RW at %p", owner);
    Result ro = svcControlCodeMemory(code, CodeMapOperation_MapOwner, owner, size, Perm_Rw);
    SMOKE_INFO("T2 pair owner RW %s", Rc(ro).c_str());
    SMOKE_INFO("T2 about to MapSlave R-X at %p", slave);
    Result rs = svcControlCodeMemory(code, CodeMapOperation_MapSlave, slave, size, Perm_Rx);
    SMOKE_INFO("T2 pair slave R-X %s", Rc(rs).c_str());
    if (R_SUCCEEDED(ro) && R_SUCCEEDED(rs)) {
      SMOKE_INFO("T2 owner %s", QueryStr(owner).c_str());
      SMOKE_INFO("T2 slave %s", QueryStr(slave).c_str());
      SMOKE_INFO("T2 src   %s", QueryStr(src).c_str());
      if (CanRead(owner)) {
        const u64 v = Read64(owner);
        SMOKE_INFO("T2 owner[0] = 0x%016" PRIx64 " (%s the value written to src)", v,
                   v == kPattern1 ? "equals" : "differs from");
      }
      CrossCheck(p, "owner", owner + 8, "slave", slave + 8, kPattern2);
      CrossCheck(p, "owner", owner + 16, "src", src + 16, kPattern3);

      SMOKE_INFO("T2 about to MapOwner a SECOND time at %p", owner2);
      Result ro2 = svcControlCodeMemory(code, CodeMapOperation_MapOwner, owner2, size, Perm_Rw);
      SMOKE_INFO("T2 second owner RW %s", Rc(ro2).c_str());
      if (R_SUCCEEDED(ro2)) {
        CrossCheck(p, "owner", owner + 24, "owner2", owner2 + 24, kPattern1);
        SMOKE_INFO("T2 about to UnmapOwner second at %p", owner2);
        SMOKE_INFO("T2   unmap %s",
                   Rc(svcControlCodeMemory(code, CodeMapOperation_UnmapOwner, owner2, size, 0))
                       .c_str());
      }
      SMOKE_INFO("T2 about to MapSlave a SECOND time at %p", slave2);
      Result rs2 = svcControlCodeMemory(code, CodeMapOperation_MapSlave, slave2, size, Perm_R);
      SMOKE_INFO("T2 second slave R-- %s", Rc(rs2).c_str());
      if (R_SUCCEEDED(rs2)) {
        SMOKE_INFO("T2 about to UnmapSlave second at %p", slave2);
        SMOKE_INFO("T2   unmap %s",
                   Rc(svcControlCodeMemory(code, CodeMapOperation_UnmapSlave, slave2, size, 0))
                       .c_str());
      }
    }
    if (R_SUCCEEDED(rs)) {
      SMOKE_INFO("T2 about to UnmapSlave at %p", slave);
      SMOKE_INFO("T2   unmap slave %s",
                 Rc(svcControlCodeMemory(code, CodeMapOperation_UnmapSlave, slave, size, 0))
                     .c_str());
    }
    if (R_SUCCEEDED(ro)) {
      SMOKE_INFO("T2 about to UnmapOwner at %p", owner);
      SMOKE_INFO("T2   unmap owner %s",
                 Rc(svcControlCodeMemory(code, CodeMapOperation_UnmapOwner, owner, size, 0))
                     .c_str());
    }
  }
  ReleaseReservation(r_owner);
  ReleaseReservation(r_slave);
  ReleaseReservation(r_owner2);
  ReleaseReservation(r_slave2);

  SMOKE_INFO("T2 about to svcCloseHandle(code memory)");
  SMOKE_INFO("T2 close %s", Rc(svcCloseHandle(code)).c_str());
  SMOKE_INFO("T2 src after close: %s", QueryStr(src).c_str());
  if (CanRead(src)) {
    const u64 v = Read64(src + 8);
    SMOKE_INFO("T2 src[8] after close = 0x%016" PRIx64 " (%s the value written through owner)", v,
               v == kPattern2 ? "equals" : "differs from");
    std::free(src);
  } else {
    SMOKE_INFO("T2 source not readable after close; buffer intentionally leaked");
  }
  SMOKE_INFO("T2 END");
}

// ── T3 ──────────────────────────────────────────────────────────────────────

bool TryMapPhysical(const char* probe, void* address, size_t size, bool probe_ends,
                    double* out_ms) {
  SMOKE_INFO("%s about to svcMapPhysicalMemory(%p, 0x%zx = %zu MiB)", probe, address, size,
             size >> 20);
  const u64 t0 = armGetSystemTick();
  Result rc = svcMapPhysicalMemory(address, size);
  const double ms = TicksToMs(armGetSystemTick() - t0);
  SMOKE_INFO("%s   map %s in %.2f ms", probe, Rc(rc).c_str(), ms);
  if (out_ms) {
    *out_ms = ms;
  }
  if (R_FAILED(rc)) {
    return false;
  }
  SMOKE_INFO("%s   %s", probe, QueryStr(address).c_str());
  if (probe_ends) {
    auto* first = static_cast<u8*>(address);
    auto* last = first + size - sizeof(u64);
    if (CanWrite(first) && CanWrite(last)) {
      SMOKE_INFO("%s about to write/read both ends of the mapping", probe);
      Write64(first, kPattern1);
      Write64(last, kPattern2);
      SMOKE_INFO("%s   first %s, last %s", probe, Read64(first) == kPattern1 ? "ok" : "MISMATCH",
                 Read64(last) == kPattern2 ? "ok" : "MISMATCH");
    }
  }
  SMOKE_INFO("%s about to svcUnmapPhysicalMemory(%p, 0x%zx)", probe, address, size);
  SMOKE_INFO("%s   unmap %s", probe, Rc(svcUnmapPhysicalMemory(address, size)).c_str());
  return true;
}

void ProbeMapPhysicalMemory() {
  const char* p = "T3";
  SMOKE_INFO("T3 BEGIN svcMapPhysicalMemory");
  if (!Hinted(p, 0x2C, "svcMapPhysicalMemory") || !Hinted(p, 0x2D, "svcUnmapPhysicalMemory")) {
    return;
  }
  u64 sr_total = 0, sr_used = 0, total = 0, used = 0, alias_start = 0, alias_size = 0;
  Result r1 = svcGetInfo(&sr_total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  Result r2 = svcGetInfo(&sr_used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&alias_start, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
  SMOKE_INFO("T3 system resource total 0x%" PRIx64 " (%s) used 0x%" PRIx64 " (%s)", sr_total,
             Rc(r1).c_str(), sr_used, Rc(r2).c_str());
  SMOKE_INFO("T3 memory total 0x%" PRIx64 " used 0x%" PRIx64 " -> free pool %" PRIu64 " MiB",
             total, used, (total - used) >> 20);

  // 1) 2 MiB outside the alias region (ASLR), as memory_switch.cpp does today.
  {
    VirtmemReservation* reservation = nullptr;
    virtmemLock();
    void* address = virtmemFindAslr(k2MiB, 0);
    reservation = address ? virtmemAddReservation(address, k2MiB) : nullptr;
    virtmemUnlock();
    if (reservation) {
      SMOKE_INFO("T3 outside alias region (ASLR) at %p:", address);
      TryMapPhysical(p, address, k2MiB, true, nullptr);
      ReleaseReservation(reservation);
    }
  }

  // 2) Inside the alias region: largest free block at its start, then a
  //    binary search (2 MiB units) for the largest size that maps.
  auto* start = reinterpret_cast<u8*>((alias_start + k2MiB - 1) & ~u64(k2MiB - 1));
  Query q = QueryAt(start);
  if (!q.ok || (q.info.type & 0xFF) != MemType_Unmapped) {
    SMOKE_INFO("T3 alias region start %p is not free (%s); stopping", start,
               QueryStr(start).c_str());
    return;
  }
  u64 free_end = std::min<u64>(q.info.addr + q.info.size, alias_start + alias_size);
  const u64 max_units = (free_end - reinterpret_cast<u64>(start)) / k2MiB;
  SMOKE_INFO("T3 alias region %p..0x%" PRIx64 ": free block of %" PRIu64 " MiB at %p",
             reinterpret_cast<void*>(alias_start), alias_start + alias_size, (max_units * k2MiB) >> 20,
             start);
  VirtmemReservation* reservation = nullptr;
  virtmemLock();
  reservation = virtmemAddReservation(start, max_units * k2MiB);
  virtmemUnlock();

  if (!TryMapPhysical(p, start, k2MiB, true, nullptr)) {
    SMOKE_INFO("T3 2 MiB inside the alias region failed; no size search");
    ReleaseReservation(reservation);
    SMOKE_INFO("T3 END");
    return;
  }
  u64 lo = 1, hi = max_units;
  while (lo < hi) {
    const u64 mid = (lo + hi + 1) / 2;
    if (TryMapPhysical(p, start, mid * k2MiB, false, nullptr)) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  double ms = 0;
  SMOKE_INFO("T3 largest mappable size in the alias region: %" PRIu64 " MiB; re-mapping it", (lo * k2MiB) >> 20);
  TryMapPhysical(p, start, lo * k2MiB, true, &ms);
  if (lo * k2MiB >= 512ull << 20) {
    double ms512 = 0;
    TryMapPhysical(p, start, 512ull << 20, false, &ms512);
    SMOKE_INFO("T3 512 MiB via svcMapPhysicalMemory: %.2f ms", ms512);
  }
  ReleaseReservation(reservation);
  SMOKE_INFO("T3 END");
}

// ── T4 ──────────────────────────────────────────────────────────────────────

void ProbeJit() {
  SMOKE_INFO("T4 BEGIN jitCreate backend selection");
  SMOKE_INFO("T4 hints: CreateCodeMemory(0x4B)=%d ControlCodeMemory(0x4C)=%d "
             "SetProcessMemoryPermission(0x73)=%d MapProcessCodeMemory(0x77)=%d "
             "UnmapProcessCodeMemory(0x78)=%d own process handle=0x%x",
             envIsSyscallHinted(0x4B), envIsSyscallHinted(0x4C), envIsSyscallHinted(0x73),
             envIsSyscallHinted(0x77), envIsSyscallHinted(0x78), envGetOwnProcessHandle());
  Jit jit{};
  SMOKE_INFO("T4 about to jitCreate(0x1000)");
  Result rc = jitCreate(&jit, 0x1000);
  SMOKE_INFO("T4 jitCreate %s type=%s rw=%p rx=%p", Rc(rc).c_str(),
             R_FAILED(rc)                               ? "none"
             : jit.type == JitType_CodeMemory           ? "JitType_CodeMemory"
                                                        : "JitType_SetProcessMemoryPermission",
             jit.rw_addr, jit.rx_addr);
  if (R_SUCCEEDED(rc)) {
    SMOKE_INFO("T4 about to jitClose");
    SMOKE_INFO("T4 jitClose %s", Rc(jitClose(&jit)).c_str());
  }
  SMOKE_INFO("T4 END");
}

// ── T5 ──────────────────────────────────────────────────────────────────────

void ProbeHeap() {
  const char* p = "T5";
  SMOKE_INFO("T5 BEGIN svcSetHeapSize");
  u64 heap_region = 0, total = 0, used = 0;
  svcGetInfo(&heap_region, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  Query q = QueryAt(reinterpret_cast<void*>(heap_region));
  const bool has_heap = q.ok && (q.info.type & 0xFF) == MemType_Heap;
  const u64 current = has_heap ? q.info.size : 0;
  SMOKE_INFO("T5 heap override from loader: %s; current heap %" PRIu64 " MiB at 0x%" PRIx64
             "; free pool %" PRIu64 " MiB",
             envHasHeapOverride() ? "yes" : "no", current >> 20, heap_region, (total - used) >> 20);
  if (!Hinted(p, 0x01, "svcSetHeapSize")) {
    return;
  }
  // Only ever grow: malloc owns the current heap, so it must never shrink
  // below its present size. Restored to `current` at the end.
  const u64 max_units = (current + (total - used)) / k2MiB;
  u64 lo = current / k2MiB, hi = max_units;
  while (lo < hi) {
    const u64 mid = (lo + hi + 1) / 2;
    void* address = nullptr;
    SMOKE_INFO("T5 about to svcSetHeapSize(%" PRIu64 " MiB)", (mid * k2MiB) >> 20);
    Result rc = svcSetHeapSize(&address, mid * k2MiB);
    SMOKE_INFO("T5   %s", Rc(rc).c_str());
    if (R_SUCCEEDED(rc)) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  SMOKE_INFO("T5 largest heap: %" PRIu64 " MiB (%" PRIu64 " MiB above the current heap)",
             (lo * k2MiB) >> 20, (lo * k2MiB - current) >> 20);

  const u64 grow = 512ull << 20;
  if (lo * k2MiB >= current + grow) {
    void* address = nullptr;
    SMOKE_INFO("T5 about to svcSetHeapSize(current) then (current + 512 MiB), timed");
    Result rc = svcSetHeapSize(&address, current);
    SMOKE_INFO("T5   back to current %s", Rc(rc).c_str());
    const u64 t0 = armGetSystemTick();
    rc = svcSetHeapSize(&address, current + grow);
    const double svc_ms = TicksToMs(armGetSystemTick() - t0);
    SMOKE_INFO("T5 +512 MiB: svcSetHeapSize %s in %.2f ms", Rc(rc).c_str(), svc_ms);
    if (R_SUCCEEDED(rc)) {
      auto* extra = static_cast<u8*>(address) + current;
      SMOKE_INFO("T5 about to touch the new 512 MiB (memset)");
      const u64 t1 = armGetSystemTick();
      std::memset(extra, 0xA5, grow);
      SMOKE_INFO("T5 +512 MiB: first-touch memset %.2f ms", TicksToMs(armGetSystemTick() - t1));
    }
  } else {
    SMOKE_INFO("T5 cannot grow the heap by 512 MiB; measuring malloc(512 MiB) instead");
    const u64 t0 = armGetSystemTick();
    void* block = std::malloc(grow);
    const double malloc_ms = TicksToMs(armGetSystemTick() - t0);
    SMOKE_INFO("T5 malloc(512 MiB) %s in %.2f ms", block ? "ok" : "FAILED", malloc_ms);
    if (block) {
      const u64 t1 = armGetSystemTick();
      std::memset(block, 0xA5, grow);
      SMOKE_INFO("T5 memset(512 MiB) %.2f ms", TicksToMs(armGetSystemTick() - t1));
      std::free(block);
    }
  }
  void* address = nullptr;
  SMOKE_INFO("T5 about to restore svcSetHeapSize(%" PRIu64 " MiB)", current >> 20);
  SMOKE_INFO("T5   restore %s", Rc(svcSetHeapSize(&address, current)).c_str());
  SMOKE_INFO("T5 END");
}

// ── T6 ──────────────────────────────────────────────────────────────────────

constexpr u64 kBenchIterations = 100'000'000;
constexpr u32 kBenchMask = 0x03FFFFFCu;  // 64 MiB, 4-byte aligned

enum BenchMode { kDirect = 0, kTranslatedLow = 1, kTranslatedMixed = 2 };

// T6 timings in ms, kept for the T7 comparison.
struct BenchResults {
  double seq_direct = 0, seq_low = 0, seq_mixed = 0;
  double rnd_direct = 0, rnd_low = 0, rnd_mixed = 0;
};
BenchResults g_t6;

double Pct(double ms, double direct_ms) {
  return (ms - direct_ms) * 100.0 / direct_ms;
}

// One read + one write of a big-endian u32 per iteration, like REX_LOAD_U32 /
// REX_STORE_U32 (volatile accesses, byte swaps). Addresses come from the same
// generator in every mode, so all modes touch the same bytes in the same
// order; only the host-address computation differs:
//   kDirect          host = base + off                       (today's macros)
//   kTranslatedLow   guest = off;  design A translation      (all < 0xA0000000)
//   kTranslatedMixed guest = off | (window ? 0xA0000000 : 0); design A
//                    translation, half the accesses through the physical path
// `phys` is passed separately (it equals `base`) so the compiler cannot fold
// the two paths together.
template <BenchMode kMode, bool kRandom>
__attribute__((noinline)) u64 BenchLoop(u8* base, u8* phys, u64 iterations) {
  u32 x = 0x12345678u;
  u32 off = 0;
  u64 sum = 0;
  for (u64 i = 0; i < iterations; ++i) {
    u32 window;
    if constexpr (kRandom) {
      x = x * 1664525u + 1013904223u;
      off = x & kBenchMask;
      window = x >> 31;
    } else {
      off = (off + 4) & kBenchMask;
      window = u32(i) & 1;
    }
    u8* host;
    if constexpr (kMode == kDirect) {
      (void)window;
      host = base + off;
    } else {
      u32 guest = kMode == kTranslatedLow ? off : (off | (window ? 0xA0000000u : 0u));
      asm volatile("" : "+r"(guest));  // the window is unknown at compile time
      host = guest >= 0xA0000000u ? phys + (guest & 0x1FFFFFFFu) : base + guest;
    }
    volatile u32* p = reinterpret_cast<volatile u32*>(host);
    const u32 v = __builtin_bswap32(*p);
    *p = __builtin_bswap32(v + 1);
    sum += v;
  }
  return sum;
}

template <BenchMode kMode, bool kRandom>
double TimeBench(u8* base, u8* phys, const char* name) {
  SMOKE_INFO("T6 running %s ...", name);
  const u64 t0 = armGetSystemTick();
  const u64 sum = BenchLoop<kMode, kRandom>(base, phys, kBenchIterations);
  const double ms = TicksToMs(armGetSystemTick() - t0);
  SMOKE_INFO("T6   %-34s %9.1f ms  (%.2f ns/iter, checksum %" PRIx64 ")", name, ms,
             ms * 1e6 / double(kBenchIterations), sum);
  return ms;
}

void ProbeTranslationCost() {
  SMOKE_INFO("T6 BEGIN design A translation cost: 100M read+write on 64 MiB (takes ~1 min)");
  const size_t size = 64u << 20;
  auto* base = static_cast<u8*>(std::aligned_alloc(k2MiB, size));
  if (!base) {
    SMOKE_INFO("T6 SKIP cannot allocate 64 MiB");
    return;
  }
  std::memset(base, 0, size);
  u8* phys = base;
  asm volatile("" : "+r"(phys));

  const double seq_direct = TimeBench<kDirect, false>(base, phys, "sequential direct");
  const double seq_low = TimeBench<kTranslatedLow, false>(base, phys, "sequential A, all < 0xA0000000");
  const double seq_mixed = TimeBench<kTranslatedMixed, false>(base, phys, "sequential A, 50% windows");
  const double rnd_direct = TimeBench<kDirect, true>(base, phys, "random direct");
  const double rnd_low = TimeBench<kTranslatedLow, true>(base, phys, "random A, all < 0xA0000000");
  const double rnd_mixed = TimeBench<kTranslatedMixed, true>(base, phys, "random A, 50% windows");

  SMOKE_INFO("T6 RESULT sequential: A low %+.1f%%, A mixed %+.1f%% vs direct",
             Pct(seq_low, seq_direct), Pct(seq_mixed, seq_direct));
  SMOKE_INFO("T6 RESULT random:     A low %+.1f%%, A mixed %+.1f%% vs direct",
             Pct(rnd_low, rnd_direct), Pct(rnd_mixed, rnd_direct));
  g_t6 = {seq_direct, seq_low, seq_mixed, rnd_direct, rnd_low, rnd_mixed};
  std::free(base);
  SMOKE_INFO("T6 END");
}

// ── T7 ──────────────────────────────────────────────────────────────────────

// Design A with a lookup table instead of compare + select:
//   host = guest + table[guest >> 24]
// Each entry is the host address of guest 0 for that 16 MiB slice, so the
// window offsets (and the aliases) live in the table instead of in the code:
//   default      base                              (virtual heaps, XEX 64K)
//   0x7F         phys - 0x7F000000                 (GPU writeback -> physical 0)
//   0x90-0x9F    base - 0x10000000                 (XEX 4K alias of 0x80-0x8F)
//   0xA0-0xBF    phys - 0xA0000000                 (physical window)
//   0xC0-0xDF    phys - 0xC0000000                 (physical window)
//   0xE0-0xFF    phys - 0xE0000000 + 0x1000        (physical window, +4 KiB as
//                                                   PhysicalHeap's host_address_offset)
// Same address generator and the same two schemes as T6. For the benchmark
// the 0xE0 slice is not used (the 64 MiB buffer has no room for +4 KiB).
void FillTranslationTable(u64* table, u8* base, u8* phys) {
  const u64 b = reinterpret_cast<u64>(base);
  const u64 p = reinterpret_cast<u64>(phys);
  for (u32 i = 0; i < 256; ++i) {
    u64 entry = b;
    if (i == 0x7F) {
      entry = p - 0x7F000000ull;
    } else if (i >= 0x90 && i < 0xA0) {
      entry = b - 0x10000000ull;
    } else if (i >= 0xA0 && i < 0xC0) {
      entry = p - 0xA0000000ull;
    } else if (i >= 0xC0 && i < 0xE0) {
      entry = p - 0xC0000000ull;
    } else if (i >= 0xE0) {
      entry = p - 0xE0000000ull + 0x1000;
    }
    table[i] = entry;
  }
}

template <bool kMixed, bool kRandom>
__attribute__((noinline)) u64 BenchLoopTable(const u64* table, u64 iterations) {
  u32 x = 0x12345678u;
  u32 off = 0;
  u64 sum = 0;
  for (u64 i = 0; i < iterations; ++i) {
    u32 window;
    if constexpr (kRandom) {
      x = x * 1664525u + 1013904223u;
      off = x & kBenchMask;
      window = x >> 31;
    } else {
      off = (off + 4) & kBenchMask;
      window = u32(i) & 1;
    }
    u32 guest = kMixed ? (off | (window ? 0xA0000000u : 0u)) : off;
    asm volatile("" : "+r"(guest));  // the window is unknown at compile time
    u8* host = reinterpret_cast<u8*>(u64(guest) + table[guest >> 24]);
    volatile u32* p = reinterpret_cast<volatile u32*>(host);
    const u32 v = __builtin_bswap32(*p);
    *p = __builtin_bswap32(v + 1);
    sum += v;
  }
  return sum;
}

template <bool kMixed, bool kRandom>
double TimeBenchTable(const u64* table, const char* name) {
  SMOKE_INFO("T7 running %s ...", name);
  const u64 t0 = armGetSystemTick();
  const u64 sum = BenchLoopTable<kMixed, kRandom>(table, kBenchIterations);
  const double ms = TicksToMs(armGetSystemTick() - t0);
  SMOKE_INFO("T7   %-34s %9.1f ms  (%.2f ns/iter, checksum %" PRIx64 ")", name, ms,
             ms * 1e6 / double(kBenchIterations), sum);
  return ms;
}

void ProbeTranslationTable() {
  SMOKE_INFO("T7 BEGIN design A table translation: 100M read+write on 64 MiB (takes ~1 min)");
  const size_t size = 64u << 20;
  auto* base = static_cast<u8*>(std::aligned_alloc(k2MiB, size));
  alignas(64) static u64 table[256];
  if (!base) {
    SMOKE_INFO("T7 SKIP cannot allocate 64 MiB");
    return;
  }
  std::memset(base, 0, size);
  u8* phys = base;
  asm volatile("" : "+r"(phys));
  FillTranslationTable(table, base, phys);

  // Encoding check on the real table, on a few guest addresses per window.
  struct Check {
    u32 guest;
    u64 expected_offset_from;  // 0 = base, 1 = phys
    u64 expected_offset;
  };
  static const Check kChecks[] = {
      {0x00001000u, 0, 0x00001000u}, {0x7EFFFFFCu, 0, 0x7EFFFFFCu},
      {0x7F000010u, 1, 0x00000010u}, {0x80000020u, 0, 0x80000020u},
      {0x90000020u, 0, 0x80000020u}, {0xA0000030u, 1, 0x00000030u},
      {0xC0000040u, 1, 0x00000040u}, {0xE0000050u, 1, 0x00001050u},
      {0xFFFFEFFCu, 1, 0x1FFFFFFCu},
  };
  int bad = 0;
  for (const Check& c : kChecks) {
    const u64 host = u64(c.guest) + table[c.guest >> 24];
    const u64 expected = (c.expected_offset_from ? reinterpret_cast<u64>(phys)
                                                 : reinterpret_cast<u64>(base)) +
                         c.expected_offset;
    if (host != expected) {
      SMOKE_INFO("T7 table check 0x%08X -> 0x%" PRIx64 ", expected 0x%" PRIx64 " MISMATCH",
                 c.guest, host, expected);
      ++bad;
    }
  }
  SMOKE_INFO("T7 table encoding check: %d/%zu addresses ok", int(std::size(kChecks)) - bad,
             std::size(kChecks));

  const u64* t = table;
  asm volatile("" : "+r"(t));
  const double seq_direct = TimeBench<kDirect, false>(base, phys, "sequential direct (T7 run)");
  const double seq_low = TimeBenchTable<false, false>(t, "sequential table, all < 0xA0000000");
  const double seq_mixed = TimeBenchTable<true, false>(t, "sequential table, 50% windows");
  const double rnd_direct = TimeBench<kDirect, true>(base, phys, "random direct (T7 run)");
  const double rnd_low = TimeBenchTable<false, true>(t, "random table, all < 0xA0000000");
  const double rnd_mixed = TimeBenchTable<true, true>(t, "random table, 50% windows");

  SMOKE_INFO("T7 RESULT sequential: table low %+.1f%%, table mixed %+.1f%% vs direct",
             Pct(seq_low, seq_direct), Pct(seq_mixed, seq_direct));
  SMOKE_INFO("T7 RESULT random:     table low %+.1f%%, table mixed %+.1f%% vs direct",
             Pct(rnd_low, rnd_direct), Pct(rnd_mixed, rnd_direct));
  if (g_t6.seq_direct > 0) {
    SMOKE_INFO("T7 RESULT vs T6 csel (same run): sequential low %.1f vs %.1f ms, mixed %.1f vs "
               "%.1f ms; random low %.1f vs %.1f ms, mixed %.1f vs %.1f ms",
               seq_low, g_t6.seq_low, seq_mixed, g_t6.seq_mixed, rnd_low, g_t6.rnd_low, rnd_mixed,
               g_t6.rnd_mixed);
  }
  std::free(base);
  SMOKE_INFO("T7 END");
}

// ── T8 ──────────────────────────────────────────────────────────────────────

constexpr u64 kT8Region = 0xA0000000ull;  // guest 0x00000000-0x9FFFFFFF
constexpr int kT8Blocks = 256;             // 256 x 2 MiB = 512 MiB
constexpr u64 kT8Stride = kT8Region / kT8Blocks;  // 10 MiB: blocks spread over the region

u64 T8Pattern(int block, u64 offset) {
  return (u64(block) << 56) ^ (offset * 0x9E3779B97F4A7C15ull) ^ 0x5A5A0000A5A50000ull;
}

// The same exclusive-monitor sequence as a PPC lwarx/stwcx. pair emulated
// with ldaxr/stlxr: returns the number of retries.
u32 ExclusiveAdd32(u32* p, u32 add) {
  u32 value, status, retries = 0;
  for (;;) {
    asm volatile("ldaxr %w0, [%2]\n\tadd %w0, %w0, %w3\n\tstlxr %w1, %w0, [%2]"
                 : "=&r"(value), "=&r"(status)
                 : "r"(p), "r"(add)
                 : "memory");
    if (status == 0) {
      return retries;
    }
    ++retries;
  }
}

u32 ExclusiveAdd64(u64* p, u64 add) {
  u64 value;
  u32 status, retries = 0;
  for (;;) {
    asm volatile("ldaxr %0, [%2]\n\tadd %0, %0, %3\n\tstlxr %w1, %0, [%2]"
                 : "=&r"(value), "=&r"(status)
                 : "r"(p), "r"(add)
                 : "memory");
    if (status == 0) {
      return retries;
    }
    ++retries;
  }
}

// Exclusives and compare-and-swap on the moved memory, as used by the
// recompiled code (stwcx./stdcx. -> __sync_bool_compare_and_swap on
// REX_RAW_ADDR) and by the kernel's Interlocked* helpers.
void T8Atomics(u8* block) {
  auto* w = reinterpret_cast<u32*>(block + 0x100);
  auto* d = reinterpret_cast<u64*>(block + 0x200);
  SMOKE_INFO("T8 about to run ldaxr/stlxr and CAS on %p", block);
  *w = 0;
  *d = 0;
  u32 retries = 0;
  for (int i = 0; i < 100000; ++i) {
    retries += ExclusiveAdd32(w, 3);
    retries += ExclusiveAdd64(d, 0x100000001ull);
  }
  const bool excl_ok = *w == 300000u && *d == 100000ull * 0x100000001ull;
  SMOKE_INFO("T8 ldaxr/stlxr 32: 0x%08x (expected 0x%08x), 64: 0x%016" PRIx64
             " (expected 0x%016" PRIx64 "), retries %u -> %s",
             *w, 300000u, *d, 100000ull * 0x100000001ull, retries, excl_ok ? "ok" : "WRONG");

  *w = 0x11111111u;
  *d = 0x2222222222222222ull;
  const bool cas32_hit = __sync_bool_compare_and_swap(w, 0x11111111u, 0x33333333u);
  const bool cas32_miss = __sync_bool_compare_and_swap(w, 0x11111111u, 0x44444444u);
  const bool cas64_hit =
      __sync_bool_compare_and_swap(d, 0x2222222222222222ull, 0x5555555555555555ull);
  const bool cas64_miss =
      __sync_bool_compare_and_swap(d, 0x2222222222222222ull, 0x6666666666666666ull);
  const bool cas_ok = cas32_hit && !cas32_miss && *w == 0x33333333u && cas64_hit &&
                      !cas64_miss && *d == 0x5555555555555555ull;
  SMOKE_INFO("T8 CAS 32: hit=%d miss=%d value 0x%08x; CAS 64: hit=%d miss=%d value 0x%016" PRIx64
             " -> %s",
             cas32_hit, cas32_miss, *w, cas64_hit, cas64_miss, *d, cas_ok ? "ok" : "WRONG");
}

struct T8Block {
  u8* src = nullptr;
  u8* dst = nullptr;
  bool mapped = false;
};

void ProbeOnDemandCommit() {
  const char* p = "T8";
  SMOKE_INFO("T8 BEGIN on-demand commit with svcMapProcessCodeMemory: %d x 2 MiB in a 0x%" PRIx64
             " reservation",
             kT8Blocks, kT8Region);
  if (!Hinted(p, 0x77, "svcMapProcessCodeMemory") ||
      !Hinted(p, 0x78, "svcUnmapProcessCodeMemory") ||
      !Hinted(p, 0x73, "svcSetProcessMemoryPermission")) {
    return;
  }
  const Handle process = envGetOwnProcessHandle();
  if (process == INVALID_HANDLE) {
    SMOKE_INFO("T8 SKIP envGetOwnProcessHandle() returned no handle");
    return;
  }

  // Guest space reservation: virtmem bookkeeping only, 2 MiB aligned.
  VirtmemReservation* reservation = nullptr;
  virtmemLock();
  void* raw = virtmemFindCodeMemory(kT8Region + k2MiB, 0);
  u8* region = raw ? reinterpret_cast<u8*>((reinterpret_cast<u64>(raw) + k2MiB - 1) &
                                           ~u64(k2MiB - 1))
                   : nullptr;
  reservation = region ? virtmemAddReservation(region, kT8Region) : nullptr;
  virtmemUnlock();
  if (!reservation) {
    SMOKE_INFO("T8 SKIP cannot reserve 0x%" PRIx64 " bytes of code address space", kT8Region);
    return;
  }
  SMOKE_INFO("T8 reservation %p-%p: %s", region, region + kT8Region - 1, QueryStr(region).c_str());

  std::vector<T8Block> blocks(kT8Blocks);
  std::vector<double> map_ms(kT8Blocks, 0.0), perm_ms(kT8Blocks, 0.0);
  int mapped = 0;
  double total_ms = 0;
  for (int i = 0; i < kT8Blocks; ++i) {
    T8Block& b = blocks[i];
    b.src = static_cast<u8*>(std::aligned_alloc(k2MiB, k2MiB));
    b.dst = region + u64(i) * kT8Stride;
    if (!b.src) {
      SMOKE_INFO("T8 aligned_alloc(2 MiB) failed at block %d; stopping", i);
      break;
    }
    if (i == 0 || i == kT8Blocks - 1) {
      SMOKE_INFO("T8 about to map block %d: src %p -> dst %p (+0x%" PRIx64 ")", i, b.src, b.dst,
                 u64(i) * kT8Stride);
    }
    const u64 t0 = armGetSystemTick();
    Result rc = svcMapProcessCodeMemory(process, reinterpret_cast<u64>(b.dst),
                                        reinterpret_cast<u64>(b.src), k2MiB);
    const u64 t1 = armGetSystemTick();
    Result rc2 = R_SUCCEEDED(rc) ? svcSetProcessMemoryPermission(
                                       process, reinterpret_cast<u64>(b.dst), k2MiB, Perm_Rw)
                                 : rc;
    const u64 t2 = armGetSystemTick();
    map_ms[i] = TicksToMs(t1 - t0);
    perm_ms[i] = TicksToMs(t2 - t1);
    total_ms += map_ms[i] + perm_ms[i];
    if (R_FAILED(rc)) {
      SMOKE_INFO("T8 block %d svcMapProcessCodeMemory %s; stopping", i, Rc(rc).c_str());
      std::free(b.src);
      b.src = nullptr;
      break;
    }
    b.mapped = true;
    ++mapped;
    if (R_FAILED(rc2)) {
      SMOKE_INFO("T8 block %d svcSetProcessMemoryPermission(RW) %s; stopping", i,
                 Rc(rc2).c_str());
      break;
    }
  }
  if (mapped > 0) {
    double min_ms = 1e9, max_ms = 0;
    for (int i = 0; i < mapped; ++i) {
      min_ms = std::min(min_ms, map_ms[i] + perm_ms[i]);
      max_ms = std::max(max_ms, map_ms[i] + perm_ms[i]);
    }
    const double avg_ms = total_ms / mapped;
    // Per-block lines only for outliers (errors are logged in the loop).
    int slow = 0;
    for (int i = 0; i < mapped; ++i) {
      if (map_ms[i] + perm_ms[i] > 3.0 * avg_ms) {
        ++slow;
        SMOKE_INFO("T8 block %d slow: map %.3f ms + RW %.3f ms (> 3x avg %.3f ms)", i, map_ms[i],
                   perm_ms[i], avg_ms);
      }
    }
    SMOKE_INFO("T8 RESULT map+RW: %d blocks (%d MiB) in %.2f ms; per block min %.3f avg %.3f max "
               "%.3f ms; %d above 3x avg",
               mapped, mapped * 2, total_ms, min_ms, avg_ms, max_ms, slow);
    SMOKE_INFO("T8 first dst %s", QueryStr(blocks[0].dst).c_str());
    SMOKE_INFO("T8 first src %s", QueryStr(blocks[0].src).c_str());
  }

  // Distinct pattern in every block, then read everything back.
  int usable = 0;
  while (usable < mapped && CanWrite(blocks[usable].dst) && CanRead(blocks[usable].dst)) {
    ++usable;
  }
  if (usable > 0) {
    SMOKE_INFO("T8 about to write a distinct pattern into %d blocks", usable);
    const u64 t0 = armGetSystemTick();
    for (int i = 0; i < usable; ++i) {
      auto* q = reinterpret_cast<u64*>(blocks[i].dst);
      for (u64 k = 0; k < k2MiB / 8; ++k) {
        q[k] = T8Pattern(i, k);
      }
    }
    const double write_ms = TicksToMs(armGetSystemTick() - t0);
    const u64 t1 = armGetSystemTick();
    u64 bad_words = 0;
    int bad_blocks = 0;
    for (int i = 0; i < usable; ++i) {
      const auto* q = reinterpret_cast<const volatile u64*>(blocks[i].dst);
      u64 bad = 0;
      for (u64 k = 0; k < k2MiB / 8; ++k) {
        bad += q[k] != T8Pattern(i, k);
      }
      bad_words += bad;
      bad_blocks += bad != 0;
    }
    const double read_ms = TicksToMs(armGetSystemTick() - t1);
    SMOKE_INFO("T8 RESULT write %d MiB %.2f ms, read back %.2f ms: %d bad blocks, %" PRIu64
               " bad words -> %s",
               usable * 2, write_ms, read_ms, bad_blocks, bad_words,
               bad_words == 0 ? "ok" : "MISMATCH");
    T8Atomics(blocks[0].dst);
    T8Atomics(blocks[usable - 1].dst + k2MiB - 0x1000);
  } else if (mapped > 0) {
    SMOKE_INFO("T8 dst not RW (%s); no write test", QueryStr(blocks[0].dst).c_str());
  }

  // Unmap every block, timed per block.
  std::vector<double> unmap_ms(kT8Blocks, 0.0);
  double unmap_total = 0;
  int unmapped = 0, unmap_failed = 0;
  for (int i = 0; i < mapped; ++i) {
    T8Block& b = blocks[i];
    if (i == 0 || i == mapped - 1) {
      SMOKE_INFO("T8 about to svcUnmapProcessCodeMemory block %d (dst %p, src %p)", i, b.dst,
                 b.src);
    }
    const u64 t0 = armGetSystemTick();
    Result rc = svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(b.dst),
                                          reinterpret_cast<u64>(b.src), k2MiB);
    unmap_ms[i] = TicksToMs(armGetSystemTick() - t0);
    if (R_FAILED(rc)) {
      ++unmap_failed;
      SMOKE_INFO("T8 unmap block %d %s", i, Rc(rc).c_str());
      continue;
    }
    b.mapped = false;
    unmap_total += unmap_ms[i];
    ++unmapped;
  }
  if (mapped > 0) {
    const double avg_ms = unmapped ? unmap_total / unmapped : 0.0;
    double min_ms = 1e9, max_ms = 0;
    int slow = 0;
    for (int i = 0; i < mapped; ++i) {
      if (blocks[i].mapped) {
        continue;
      }
      min_ms = std::min(min_ms, unmap_ms[i]);
      max_ms = std::max(max_ms, unmap_ms[i]);
      if (unmap_ms[i] > 3.0 * avg_ms) {
        ++slow;
        SMOKE_INFO("T8 unmap block %d slow: %.3f ms (> 3x avg %.3f ms)", i, unmap_ms[i], avg_ms);
      }
    }
    SMOKE_INFO("T8 RESULT unmap: %d/%d blocks in %.2f ms; per block min %.3f avg %.3f max %.3f ms; "
               "%d above 3x avg, %d failed",
               unmapped, mapped, unmap_total, unmapped ? min_ms : 0.0, avg_ms, max_ms, slow,
               unmap_failed);
  }

  // What the sources hold after the unmap: the pattern written through dst
  // survives only if the unmap moves the pages back.
  int kept = 0, lost = 0, unreadable = 0;
  for (int i = 0; i < usable; ++i) {
    const T8Block& b = blocks[i];
    if (b.mapped || !CanRead(b.src)) {
      ++unreadable;
      continue;
    }
    const auto* q = reinterpret_cast<const volatile u64*>(b.src);
    const bool same = q[0] == T8Pattern(i, 0) && q[k2MiB / 16] == T8Pattern(i, k2MiB / 16) &&
                      q[k2MiB / 8 - 1] == T8Pattern(i, k2MiB / 8 - 1);
    same ? ++kept : ++lost;
  }
  if (usable > 0) {
    SMOKE_INFO("T8 first src after unmap: %s", QueryStr(blocks[0].src).c_str());
    SMOKE_INFO("T8 RESULT source after unmap: %d blocks keep the data written through dst, %d "
               "LOST it, %d not readable%s",
               kept, lost, unreadable,
               lost ? " -> NOTE: data written through the destination is lost on unmap" : "");
  }

  int leaked = 0;
  for (T8Block& b : blocks) {
    if (!b.src) {
      continue;
    }
    if (b.mapped) {
      ++leaked;  // still moved: the heap block must not be returned to malloc
    } else {
      std::free(b.src);
    }
  }
  if (leaked) {
    SMOKE_INFO("T8 %d blocks still mapped; their source buffers are intentionally leaked", leaked);
  } else {
    ReleaseReservation(reservation);
  }
  SMOKE_INFO("T8 END");
}

// ── T9 ──────────────────────────────────────────────────────────────────────

// Fault round trip on guest memory: a page made read-only (or inaccessible)
// with svcSetMemoryPermission, a write, and an exception handler
// (installed through exception_handler_switch.cpp, after the runtime's
// MMIOHandler) that restores RW and resumes the faulting instruction.
struct T9State {
  u8* page = nullptr;
  u32 faults = 0;
  u32 writes = 0;
  u64 last_pc = 0;
  u64 last_address = 0;
  bool restore_failed = false;
};

bool T9Handler(rex::arch::Exception* ex, void* data) {
  auto* state = static_cast<T9State*>(data);
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  const u64 address = ex->fault_address();
  const u64 page = reinterpret_cast<u64>(state->page);
  if (address < page || address >= page + 0x1000) {
    return false;
  }
  ++state->faults;
  state->writes += ex->access_violation_operation() ==
                   rex::arch::Exception::AccessViolationOperation::kWrite;
  state->last_pc = ex->pc();
  state->last_address = address;
  // Signal-handler context: no logging here. rex::memory::Protect inside the
  // arena is svcSetMemoryPermission.
  if (!rex::memory::Protect(state->page, 0x1000, rex::memory::PageAccess::kReadWrite)) {
    state->restore_failed = true;
    return false;
  }
  return true;  // resume at the faulting instruction
}

struct T9Watch {
  u32 calls = 0;
  u32 last_start = 0;
  u32 last_length = 0;
};

std::pair<uint32_t, uint32_t> T9Invalidation(void* context, uint32_t physical_address_start,
                                             uint32_t length, bool exact_range) {
  (void)exact_range;
  auto* watch = static_cast<T9Watch*>(context);
  ++watch->calls;
  watch->last_start = physical_address_start;
  watch->last_length = length;
  return {0, UINT32_MAX};
}

void ProbeFaultRoundTrip(rex::memory::Memory* memory) {
  const char* p = "T9";
  SMOKE_INFO("T9 BEGIN fault round trip on guest memory (exception_handler_switch.cpp)");
  if (!memory) {
    SMOKE_INFO("T9 SKIP no rex::memory::Memory (step 3 failed)");
    return;
  }
  if (!Hinted(p, 0x73, "svcSetProcessMemoryPermission")) {
    return;
  }
  const Handle process = envGetOwnProcessHandle();
  auto* heap = memory->LookupHeap(0x40000000);
  uint32_t guest = 0;
  if (!heap->Alloc(0x10000, 0x10000,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                   &guest)) {
    SMOKE_INFO("T9 SKIP cannot allocate 64 KiB of guest memory");
    return;
  }
  T9State state;
  state.page = memory->TranslateVirtual(guest);
  auto* word = reinterpret_cast<volatile u32*>(state.page + 0x40);
  SMOKE_INFO("T9 guest %08X -> host %p: %s", guest, state.page, QueryStr(state.page).c_str());
  // 0. Run 4's failure: after the RW that follows svcMapProcessCodeMemory the
  //    page is AliasCodeData, which has no FlagCode, so the kernel refuses
  //    svcSetProcessMemoryPermission (expected 0xD401, InvalidCurrentMemory,
  //    before any change). memory_switch.cpp now uses svcSetMemoryPermission.
  SMOKE_INFO("T9 about to svcSetProcessMemoryPermission(%p, 0x1000, R) on the RW arena page",
             state.page);
  Result rc =
      svcSetProcessMemoryPermission(process, reinterpret_cast<u64>(state.page), 0x1000, Perm_R);
  SMOKE_INFO("T9 svcSetProcessMemoryPermission on AliasCodeData %s (expected 0xD401): %s",
             Rc(rc).c_str(), QueryStr(state.page).c_str());
  if (R_SUCCEEDED(rc)) {
    svcSetMemoryPermission(state.page, 0x1000, Perm_Rw);
  }
  rex::arch::ExceptionHandler::Install(T9Handler, &state);

  // 1. One read-only page, one write.
  SMOKE_INFO("T9 about to svcSetMemoryPermission(%p, 0x1000, R)", state.page);
  rc = svcSetMemoryPermission(state.page, 0x1000, Perm_R);
  SMOKE_INFO("T9 set R %s: %s", Rc(rc).c_str(), QueryStr(state.page).c_str());
  if (R_SUCCEEDED(rc)) {
    SMOKE_INFO("T9 about to write to the read-only page");
    const u64 t0 = armGetSystemTick();
    *word = 0x600DF00Du;
    const double ms = TicksToMs(armGetSystemTick() - t0);
    SMOKE_INFO("T9 write done: faults %u (write %u), pc 0x%" PRIx64 ", address 0x%" PRIx64
               ", value %08X -> %s, %.3f ms, page now %s",
               state.faults, state.writes, state.last_pc, state.last_address, *word,
               state.faults == 1 && *word == 0x600DF00Du ? "ok" : "UNEXPECTED", ms,
               QueryStr(state.page).c_str());
  }

  // 2. Cost of a full round trip, and of the two SVCs alone.
  constexpr int kRounds = 1000;
  u32 before = state.faults;
  SMOKE_INFO("T9 about to run %d x (set R, write -> fault -> set RW -> resume)", kRounds);
  u64 t0 = armGetSystemTick();
  for (int i = 0; i < kRounds; ++i) {
    svcSetMemoryPermission(state.page, 0x1000, Perm_R);
    *word = u32(i);
  }
  const double round_ms = TicksToMs(armGetSystemTick() - t0);
  const u32 round_faults = state.faults - before;
  t0 = armGetSystemTick();
  for (int i = 0; i < kRounds; ++i) {
    svcSetMemoryPermission(state.page, 0x1000, Perm_R);
    svcSetMemoryPermission(state.page, 0x1000, Perm_Rw);
  }
  const double svc_ms = TicksToMs(armGetSystemTick() - t0);
  SMOKE_INFO("T9 RESULT round trip: %u faults for %d writes, %.2f us each; the two SVCs alone "
             "%.2f us; exception path %.2f us; last value %u -> %s",
             round_faults, kRounds, round_ms * 1000.0 / kRounds, svc_ms * 1000.0 / kRounds,
             (round_ms - svc_ms) * 1000.0 / kRounds, *word,
             round_faults == u32(kRounds) && *word == u32(kRounds - 1) ? "ok" : "UNEXPECTED");

  // 3. No access at all (read fault).
  SMOKE_INFO("T9 about to svcSetMemoryPermission(%p, 0x1000, ---)", state.page);
  rc = svcSetMemoryPermission(state.page, 0x1000, Perm_None);
  SMOKE_INFO("T9 set --- %s: %s", Rc(rc).c_str(), QueryStr(state.page).c_str());
  if (R_SUCCEEDED(rc)) {
    before = state.faults;
    SMOKE_INFO("T9 about to read the inaccessible page");
    const u32 v = *word;
    SMOKE_INFO("T9 read done: faults %u, value %u -> %s", state.faults - before, v,
               state.faults - before == 1 && v == u32(kRounds - 1) ? "ok" : "UNEXPECTED");
  }
  rex::arch::ExceptionHandler::Uninstall(T9Handler, &state);
  if (state.restore_failed) {
    SMOKE_INFO("T9 NOTE the handler could not restore RW (rex::memory::Protect failed)");
  }
  heap->Release(guest);

  // 4. The runtime's own write watch: physical memory watched through
  //    EnablePhysicalMemoryAccessCallbacks, written through another window.
  //    Faults go MMIOHandler -> Memory::NxPhysicalAccessViolation.
  auto* vA = memory->LookupHeapByType(true, 64 * 1024);
  uint32_t a_address = 0;
  if (!vA->Alloc(0x10000, 0x10000,
                 rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                 &a_address)) {
    SMOKE_INFO("T9 SKIP write watch: cannot allocate physical memory");
    SMOKE_INFO("T9 END");
    return;
  }
  const uint32_t physical = memory->GetPhysicalAddress(a_address);
  T9Watch watch;
  void* handle = memory->RegisterPhysicalMemoryInvalidationCallback(T9Invalidation, &watch);
  u8* through_c = memory->TranslateVirtual(0xC0000000u + physical);
  SMOKE_INFO("T9 watch physical %08X (guest %08X), write through 0xC view %p", physical,
             a_address, through_c);
  SMOKE_INFO("T9 about to EnablePhysicalMemoryAccessCallbacks + write");
  memory->EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
  SMOKE_INFO("T9 page after enable: %s", QueryStr(through_c).c_str());
  t0 = armGetSystemTick();
  *reinterpret_cast<volatile u32*>(through_c + 0x80) = 0x5EED5EEDu;
  const double watch_ms = TicksToMs(armGetSystemTick() - t0);
  const u32 read_a =
      *reinterpret_cast<volatile u32*>(memory->TranslateVirtual(a_address) + 0x80);
  SMOKE_INFO("T9 RESULT write watch: %u invalidation call(s) (last %08X+%X), value through 0xA "
             "%08X -> %s, %.3f ms, page now %s",
             watch.calls, watch.last_start, watch.last_length, read_a,
             watch.calls >= 1 && read_a == 0x5EED5EEDu ? "ok" : "UNEXPECTED", watch_ms,
             QueryStr(through_c).c_str());
  memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
  vA->Release(a_address);
  SMOKE_INFO("T9 END");
}

// ── T10 ─────────────────────────────────────────────────────────────────────

// Concurrent write faults: three threads, one per core, each on its own
// watched physical page, 10,000 times (arm the watch, write -> fault ->
// runtime write watch -> invalidation callback -> page RW -> resume, read
// back). libnx has one exception stack and one ThreadExceptionDump for the
// process; the question is whether two faults ever run the handler at the
// same time. The callback counts the threads inside it at once (with a short
// spin to widen the window): a maximum of 1 means the handler never
// overlapped with itself.
constexpr int kT10Threads = 3;
constexpr u32 kT10Rounds = 10000;

struct T10Slot {
  uint32_t physical = 0;
  std::atomic<u32> calls{0};
};

struct T10Watch {
  T10Slot slots[kT10Threads];
  std::atomic<u32> in_flight{0};
  std::atomic<u32> max_in_flight{0};
  std::atomic<u32> stray_calls{0};
};

// Everything the worker threads touch; heap-allocated so it can be leaked
// (with the threads) if they hang.
struct T10Run {
  T10Watch watch;
  std::atomic<bool> go{false};
  std::atomic<u32> bad_reads[kT10Threads] = {};
  std::atomic<int> ran_on[kT10Threads] = {};
  std::atomic<u64> ticks[kT10Threads] = {};
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
};

std::pair<uint32_t, uint32_t> T10Invalidation(void* context, uint32_t physical_address_start,
                                              uint32_t length, bool exact_range) {
  (void)exact_range;
  auto* watch = static_cast<T10Watch*>(context);
  const u32 inside = watch->in_flight.fetch_add(1) + 1;
  u32 seen = watch->max_in_flight.load();
  while (inside > seen && !watch->max_in_flight.compare_exchange_weak(seen, inside)) {
  }
  bool matched = false;
  for (T10Slot& slot : watch->slots) {
    if (physical_address_start <= slot.physical &&
        slot.physical - physical_address_start < length) {
      slot.calls.fetch_add(1);
      matched = true;
    }
  }
  if (!matched) {
    watch->stray_calls.fetch_add(1);
  }
  // ~2 us: long enough for another core's fault to arrive while this one is
  // still inside the handler, if the kernel let it.
  const u64 until = armGetSystemTick() + armNsToTicks(2000);
  while (armGetSystemTick() < until) {
  }
  watch->in_flight.fetch_sub(1);
  // Exact range: unwatching more would disarm the other threads' pages.
  return {physical_address_start, length};
}

void ProbeConcurrentFaults(rex::memory::Memory* memory) {
  SMOKE_INFO("T10 BEGIN concurrent write faults: %d threads x %u faults on separate watched "
             "pages",
             kT10Threads, kT10Rounds);
  if (!memory) {
    SMOKE_INFO("T10 SKIP no rex::memory::Memory (step 3 failed)");
    return;
  }
  const uint64_t guest_cores = rex::thread::nx_guest_core_mask();
  auto* vA = memory->LookupHeapByType(true, 64 * 1024);
  auto run = std::make_unique<T10Run>();
  T10Watch& watch = run->watch;
  uint32_t guest[kT10Threads] = {};
  for (int i = 0; i < kT10Threads; ++i) {
    if (!vA->Alloc(0x10000, 0x10000,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                   &guest[i])) {
      SMOKE_INFO("T10 SKIP cannot allocate 64 KiB of physical memory for thread %d", i);
      for (int j = 0; j < i; ++j) {
        vA->Release(guest[j]);
      }
      return;
    }
    watch.slots[i].physical = memory->GetPhysicalAddress(guest[i]);
    SMOKE_INFO("T10 thread %d: guest %08X physical %08X host %p", i, guest[i],
               watch.slots[i].physical, memory->TranslateVirtual(guest[i]));
  }
  void* handle = memory->RegisterPhysicalMemoryInvalidationCallback(T10Invalidation, &watch);

  auto& threads = run->threads;
  threads.resize(kT10Threads);
  for (int i = 0; i < kT10Threads; ++i) {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 64 * 1024;
    params.create_suspended = true;
    const uint32_t physical = watch.slots[i].physical;
    auto* word = reinterpret_cast<volatile u32*>(memory->TranslateVirtual(guest[i]) + 0x100);
    T10Run* const r = run.get();
    r->ran_on[i].store(-1);
    threads[i] = rex::thread::Thread::Create(
        params, [i, physical, word, memory, r] {
          while (!r->go.load()) {
          }
          const u64 t0 = armGetSystemTick();
          for (u32 k = 0; k < kT10Rounds; ++k) {
            memory->EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
            const u32 value = (u32(i) << 24) | k;
            *word = value;  // write fault on the read-only page
            if (*word != value) {
              r->bad_reads[i].fetch_add(1);
            }
          }
          r->ticks[i].store(armGetSystemTick() - t0);
          r->ran_on[i].store(int(svcGetCurrentProcessorNumber()));
        });
    if (!threads[i]) {
      SMOKE_INFO("T10 Thread::Create failed for thread %d", i);
      continue;
    }
    if (guest_cores & (uint64_t(1) << i)) {
      threads[i]->set_affinity_mask(uint64_t(1) << i);
    }
    threads[i]->Resume();
  }
  SMOKE_INFO("T10 about to release the threads (a hang here means a deadlock in the fault path)");
  const u64 t0 = armGetSystemTick();
  run->go.store(true);
  bool timed_out = false;
  for (int i = 0; i < kT10Threads; ++i) {
    if (threads[i] && rex::thread::Wait(threads[i].get(), false, std::chrono::seconds(60)) !=
                          rex::thread::WaitResult::kSuccess) {
      timed_out = true;
    }
  }
  const double total_ms = TicksToMs(armGetSystemTick() - t0);
  bool ok = !timed_out && watch.stray_calls.load() == 0;
  for (int i = 0; i < kT10Threads; ++i) {
    const u32 calls = watch.slots[i].calls.load();
    const bool thread_ok =
        threads[i] && calls == kT10Rounds && run->bad_reads[i].load() == 0;
    ok &= thread_ok;
    SMOKE_INFO("T10 thread %d on core %d: %u callbacks for %u writes, %u bad read-backs, "
               "%.2f us per fault -> %s",
               i, run->ran_on[i].load(), calls, kT10Rounds, run->bad_reads[i].load(),
               TicksToMs(run->ticks[i].load()) * 1000.0 / kT10Rounds,
               thread_ok ? "ok" : "WRONG");
  }
  const u32 max_in_flight = watch.max_in_flight.load();
  SMOKE_INFO("T10 RESULT %s in %.1f ms; %u stray callbacks; at most %u thread(s) inside the "
             "handler at once -> %s",
             timed_out ? "TIMEOUT (threads still stuck)" : "done", total_ms,
             watch.stray_calls.load(), max_in_flight,
             max_in_flight <= 1 ? "faults are serialised (one exception stack in use at a time)"
                                : "OVERLAP: two faults shared the libnx exception stack");
  SMOKE_INFO("T10 RESULT %s", ok ? "ok" : "UNEXPECTED");
  if (timed_out) {
    // The stuck threads still use the run state, the callback and the pages:
    // leak all of them.
    (void)run.release();
    SMOKE_INFO("T10 END (threads, callback and pages left in place)");
    return;
  }
  memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
  for (int i = 0; i < kT10Threads; ++i) {
    vA->Release(guest[i]);
  }
  SMOKE_INFO("T10 END");
}

// ── T11 ─────────────────────────────────────────────────────────────────────

// Three guest-memory models side by side (docs/switch-port/03-memory.md §13):
//   A      no aliases; guest -> host through rex_guest_table (current design)
//   nfsmw  aliases with svcMapProcessMemory, each view mapped on its first
//          access from the exception handler (StevensND/nfsmw-nx)
//   D      the same aliases, every view mapped when the block is committed
// T11.1 the alias primitive on one of our 2 MiB blocks; T11.2 model D for
// 512 MiB of guest physical memory, then the ceiling; T11.3 the access cost of
// direct, table and nfsmw-nx's macro on real aliased memory.

constexpr u64 k4GiB = 0x100000000ull;

// A block of backing memory moved to a code alias and made RW, as MapBlock in
// memory_switch.cpp does: the source of every view.
struct T11Block {
  u8* backing = nullptr;
  u8* shadow = nullptr;
  bool moved = false;
};

// A view mapped with svcMapProcessMemory, kept for the unmap.
struct T11View {
  u8* dst = nullptr;
  u64 src = 0;
  u64 size = 0;
};

// Moves `b.backing` to `shadow` and makes it RW. Returns the failing Result.
Result T11Commit(Handle process, T11Block& b, u8* shadow) {
  b.shadow = shadow;
  Result rc = svcMapProcessCodeMemory(process, reinterpret_cast<u64>(shadow),
                                      reinterpret_cast<u64>(b.backing), k2MiB);
  if (R_FAILED(rc)) {
    return rc;
  }
  b.moved = true;
  return svcSetProcessMemoryPermission(process, reinterpret_cast<u64>(shadow), k2MiB, Perm_Rw);
}

Result T11MapView(Handle process, std::vector<T11View>& views, u8* dst, u64 src, u64 size) {
  const Result rc = svcMapProcessMemory(dst, process, src, size);
  if (R_SUCCEEDED(rc)) {
    views.push_back({dst, src, size});
  }
  return rc;
}

// Unmaps every view (newest first), then moves every block back and frees it.
// Returns the number of failures; a block that stays moved is leaked.
int T11Release(Handle process, std::vector<T11View>& views, std::vector<T11Block>& blocks) {
  int failed = 0;
  for (auto it = views.rbegin(); it != views.rend(); ++it) {
    if (R_FAILED(svcUnmapProcessMemory(it->dst, process, it->src, it->size))) {
      ++failed;
    }
  }
  views.clear();
  for (T11Block& b : blocks) {
    if (b.moved) {
      if (R_FAILED(svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(b.shadow),
                                             reinterpret_cast<u64>(b.backing), k2MiB))) {
        ++failed;
        continue;  // still moved: must not go back to malloc
      }
      b.moved = false;
    }
    std::free(b.backing);
    b.backing = nullptr;
  }
  blocks.clear();
  return failed;
}

// Reserves `size` bytes (2 MiB aligned) in the ASLR region, like nfsmw-nx's
// guest window, or in the code region for the code aliases.
u8* T11Reserve(bool code, u64 size, VirtmemReservation** out) {
  virtmemLock();
  void* raw = code ? virtmemFindCodeMemory(size + k2MiB, 0) : virtmemFindAslr(size + k2MiB, 0);
  u8* base = raw ? reinterpret_cast<u8*>((reinterpret_cast<u64>(raw) + k2MiB - 1) &
                                         ~u64(k2MiB - 1))
                 : nullptr;
  *out = base ? virtmemAddReservation(base, size) : nullptr;
  virtmemUnlock();
  return *out ? base : nullptr;
}

struct T11Limits {
  bool ok = false;
  s64 limit = 0;
  s64 current = 0;
};

// LimitableResource_Memory of this process: "how much memory can a process map".
T11Limits T11ReadLimits() {
  T11Limits l;
  if (!envIsSyscallHinted(0x30) || !envIsSyscallHinted(0x31)) {
    return l;
  }
  u64 handle = 0;
  if (R_FAILED(svcGetInfo(&handle, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || handle == 0) {
    return l;
  }
  const Handle h = Handle(handle);
  l.ok = R_SUCCEEDED(svcGetResourceLimitLimitValue(&l.limit, h, LimitableResource_Memory)) &&
         R_SUCCEEDED(svcGetResourceLimitCurrentValue(&l.current, h, LimitableResource_Memory));
  svcCloseHandle(h);
  return l;
}

void T11LogMemory(const char* when) {
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  const T11Limits l = T11ReadLimits();
  if (l.ok) {
    SMOKE_INFO("T11 memory %s: total %" PRIu64 " MiB, used %" PRIu64
               " MiB; resource limit Memory %" PRId64 " / %" PRId64 " MiB",
               when, total >> 20, used >> 20, l.current >> 20, l.limit >> 20);
  } else {
    SMOKE_INFO("T11 memory %s: total %" PRIu64 " MiB, used %" PRIu64
               " MiB; resource limit not readable",
               when, total >> 20, used >> 20);
  }
}

// ── T11.1: the alias primitive ──

void T11ConcurrentAtomics(u8* const views[3], u64 offset) {
  constexpr int kRounds = 100000;
  Write64(views[0] + offset, 0);
  std::atomic<u32> retries{0};
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  const u64 guest_mask = rex::thread::nx_guest_core_mask();
  for (int n = 0; n < 3; ++n) {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 64 * 1024;
    params.create_suspended = true;
    u64* word = reinterpret_cast<u64*>(views[n] + offset);
    auto thread = rex::thread::Thread::Create(params, [word, &retries] {
      u32 r = 0;
      for (int i = 0; i < kRounds; ++i) {
        r += ExclusiveAdd64(word, 0x100000001ull);
      }
      retries.fetch_add(r);
    });
    if (!thread) {
      SMOKE_INFO("T11.1 Thread::Create failed; concurrent atomics skipped");
      return;
    }
    const uint32_t core = uint32_t(n);
    if (guest_mask & (u64(1) << core)) {
      thread->set_ideal_core(core, u64(1) << core);
    }
    threads.push_back(std::move(thread));
  }
  SMOKE_INFO("T11.1 about to run 3 threads x %d ldaxr/stlxr adds on one word, each through its "
             "own view",
             kRounds);
  for (auto& t : threads) {
    t->Resume();
  }
  bool joined = true;
  for (auto& t : threads) {
    joined &= rex::thread::Wait(t.get(), false, std::chrono::milliseconds(20000)) ==
              rex::thread::WaitResult::kSuccess;
  }
  const u64 expected = 3ull * kRounds * 0x100000001ull;
  const u64 got = Read64(views[1] + offset);
  SMOKE_INFO("T11.1 concurrent atomics through 3 views: 0x%016" PRIx64 " (expected 0x%016" PRIx64
             "), %u retries%s -> %s",
             got, expected, retries.load(), joined ? "" : ", TIMEOUT",
             got == expected ? "ok" : "WRONG");
}

void T11AliasPrimitive(Handle process) {
  SMOKE_INFO("T11.1 BEGIN svcMapProcessMemory with a svcMapProcessCodeMemory alias as source");
  std::vector<T11Block> blocks(1);
  std::vector<T11View> views;
  VirtmemReservation* code_res = nullptr;
  VirtmemReservation* view_res = nullptr;
  u8* shadow = T11Reserve(true, k2MiB, &code_res);
  u8* window = T11Reserve(false, 2 * k2MiB, &view_res);
  blocks[0].backing = static_cast<u8*>(std::aligned_alloc(k2MiB, k2MiB));
  if (!shadow || !window || !blocks[0].backing) {
    SMOKE_INFO("T11.1 SKIP reservation or backing failed");
    std::free(blocks[0].backing);
    ReleaseReservation(code_res);
    ReleaseReservation(view_res);
    return;
  }
  std::memset(blocks[0].backing, 0, k2MiB);
  SMOKE_INFO("T11.1 about to move %p to the code alias %p and make it RW", blocks[0].backing,
             shadow);
  Result rc = T11Commit(process, blocks[0], shadow);
  SMOKE_INFO("T11.1 commit %s: alias %s", Rc(rc).c_str(), QueryStr(shadow).c_str());
  u8* view[3] = {shadow, window, window + k2MiB};
  if (R_SUCCEEDED(rc)) {
    for (int n = 1; n < 3 && R_SUCCEEDED(rc); ++n) {
      SMOKE_INFO("T11.1 about to svcMapProcessMemory(dst %p, own process, src %p, 2 MiB)",
                 view[n], shadow);
      rc = T11MapView(process, views, view[n], reinterpret_cast<u64>(shadow), k2MiB);
      SMOKE_INFO("T11.1 view %d %s: %s", n + 1, Rc(rc).c_str(), QueryStr(view[n]).c_str());
    }
    SMOKE_INFO("T11.1 alias after the views: %s", QueryStr(shadow).c_str());
  }
  if (R_SUCCEEDED(rc)) {
    // Every write through one view must show in the other two.
    CrossCheck("T11.1", "alias", shadow + 0x100, "view2", view[1] + 0x100, kPattern1);
    CrossCheck("T11.1", "view2", view[1] + 0x108, "view3", view[2] + 0x108, kPattern2);
    CrossCheck("T11.1", "view3", view[2] + 0x110, "alias", shadow + 0x110, kPattern3);
    CrossCheck("T11.1", "view2", view[1] + k2MiB - 8, "alias", shadow + k2MiB - 8, kPattern1);

    if (CanWrite(shadow) && CanWrite(view[1]) && CanWrite(view[2])) {
      // Exclusives and CAS through every view on the same words.
      auto* w0 = reinterpret_cast<u32*>(shadow + 0x400);
      *w0 = 0;
      u32 retries = 0;
      SMOKE_INFO("T11.1 about to run ldaxr/stlxr alternating the three views on one word");
      for (int i = 0; i < 300000; ++i) {
        retries += ExclusiveAdd32(reinterpret_cast<u32*>(view[i % 3] + 0x400), 1);
      }
      SMOKE_INFO("T11.1 alternating exclusives: 0x%08x (expected 0x%08x), %u retries -> %s",
                 *w0, 300000u, retries, *w0 == 300000u ? "ok" : "WRONG");
      Write64(shadow + 0x500, 0x2222222222222222ull);
      const bool hit = __sync_bool_compare_and_swap(reinterpret_cast<u64*>(view[1] + 0x500),
                                                    0x2222222222222222ull, 0x5555555555555555ull);
      const bool miss = __sync_bool_compare_and_swap(reinterpret_cast<u64*>(view[2] + 0x500),
                                                     0x2222222222222222ull, 0x6666666666666666ull);
      const u64 seen = Read64(shadow + 0x500);
      SMOKE_INFO("T11.1 CAS: hit through view2=%d, stale miss through view3=%d, alias reads "
                 "0x%016" PRIx64 " -> %s",
                 hit, miss, seen, hit && !miss && seen == 0x5555555555555555ull ? "ok" : "WRONG");
      T11ConcurrentAtomics(view, 0x600);
    } else {
      SMOKE_INFO("T11.1 views not all writable; atomics skipped");
    }

    // Protection on a view, and on the alias with views mapped (nfsmw-nx:
    // ProcessMem has no PermChangeAllowed). RW is restored at once.
    SMOKE_INFO("T11.1 about to svcSetMemoryPermission(view2, 4 KiB, R)");
    Result prc = svcSetMemoryPermission(view[1], 0x1000, Perm_R);
    SMOKE_INFO("T11.1 view2 set R %s: %s", Rc(prc).c_str(), QueryStr(view[1]).c_str());
    if (R_SUCCEEDED(prc)) {
      prc = svcSetMemoryPermission(view[1], 0x1000, Perm_Rw);
      SMOKE_INFO("T11.1 view2 back to RW %s", Rc(prc).c_str());
    }
    SMOKE_INFO("T11.1 about to svcSetMemoryPermission(alias, 4 KiB, R) with two views mapped");
    prc = svcSetMemoryPermission(shadow, 0x1000, Perm_R);
    SMOKE_INFO("T11.1 alias set R %s: alias %s; view2 %s", Rc(prc).c_str(),
               QueryStr(shadow).c_str(), QueryStr(view[1]).c_str());
    if (R_SUCCEEDED(prc)) {
      prc = svcSetMemoryPermission(shadow, 0x1000, Perm_Rw);
      SMOKE_INFO("T11.1 alias back to RW %s", Rc(prc).c_str());
    }
  }
  SMOKE_INFO("T11.1 about to unmap %zu views and the alias", views.size());
  const int failed = T11Release(process, views, blocks);
  SMOKE_INFO("T11.1 release: %d failures", failed);
  if (!failed) {
    ReleaseReservation(code_res);
    ReleaseReservation(view_res);
  }
  SMOKE_INFO("T11.1 END");
}

// ── T11.2: model D and the ceiling ──

// nfsmw-nx's views of guest physical memory (xmemory.cpp map_info), as host
// offsets from the window base: guest 0x7F (first 16 MiB), 0xA0, 0xC0 and
// 0xE0 with the +4 KiB of PhysicalHeap, so generated code is base + addr.
// Maps block `k` (physical k * 2 MiB) into every view that covers it; on a
// refusal *failed_view names the view.
Result T11MapBlockViews(Handle process, std::vector<T11View>& views, u8* window, u64 k,
                        const T11Block& b, int* failed_view) {
  const u64 phys = k * k2MiB;
  const u64 src = reinterpret_cast<u64>(b.shadow);
  struct View {
    u64 guest;
    bool covers;
  };
  const View kViews[] = {
      {0x7F000000ull + phys, phys < 0x1000000ull},
      {0xA0000000ull + phys, true},
      {0xC0000000ull + phys, true},
  };
  for (int n = 0; n < 3; ++n) {
    if (!kViews[n].covers) {
      continue;
    }
    const Result rc = T11MapView(process, views, window + kViews[n].guest, src, k2MiB);
    if (R_FAILED(rc)) {
      *failed_view = n;
      return rc;
    }
  }
  // 0xE0: guest 0xE0000000 + x is physical x + 0x1000. Block 0 loses its
  // first page (as in nfsmw-nx, the view starts at physical 0x1000).
  const u64 e_src = k == 0 ? src + 0x1000 : src;
  const u64 e_size = k == 0 ? k2MiB - 0x1000 : k2MiB;
  u8* e_dst = window + 0xE0000000ull + (k == 0 ? 0 : phys - 0x1000);
  const Result rc = T11MapView(process, views, e_dst, e_src, e_size);
  if (R_FAILED(rc)) {
    *failed_view = 3;
  }
  return rc;
}

void T11ModelD(Handle process) {
  constexpr u64 kPhysBlocks = 256;       // 512 MiB, the guest physical memory
  constexpr u64 kMaxBlocks = 1536;       // 3 GiB of backing at most
  constexpr u64 kMaxExtraViewsGiB = 48;  // cap of the views-only phase
  static const char* const kViewName[] = {"0x7F", "0xA0", "0xC0", "0xE0"};
  SMOKE_INFO("T11.2 BEGIN model D: %" PRIu64 " x 2 MiB with the 0x7F/0xA0/0xC0/0xE0 views mapped "
             "at commit, then more blocks and views until the kernel refuses",
             kPhysBlocks);
  T11LogMemory("before");

  VirtmemReservation* code_res = nullptr;
  VirtmemReservation* window_res = nullptr;
  u8* shadows = T11Reserve(true, kMaxBlocks * k2MiB, &code_res);
  u8* window = T11Reserve(false, k4GiB, &window_res);
  if (!shadows || !window) {
    SMOKE_INFO("T11.2 SKIP cannot reserve %" PRIu64 " MiB of code space or a 4 GiB window",
               (kMaxBlocks * k2MiB) >> 20);
    ReleaseReservation(code_res);
    ReleaseReservation(window_res);
    return;
  }
  SMOKE_INFO("T11.2 window %p (4 GiB), aliases %p", window, shadows);

  // Heap kept aside so that logging and cleanup still have memory when the
  // backing allocations exhaust the heap.
  void* reserve = std::malloc(32u << 20);
  std::vector<T11Block> blocks;
  std::vector<T11View> views;
  blocks.reserve(kMaxBlocks);
  views.reserve(kMaxBlocks * 4 + 8192);
  u64 backing_bytes = 0, mapped_view_bytes = 0;
  double commit_ms_total = 0, views_ms_total = 0, min_ms = 1e9, max_ms = 0;
  std::string stop = "limit of the probe reached";
  bool kernel_refused = false;
  // Blocks beyond 512 MiB get their views in further 4 GiB windows, 512 MiB of
  // guest physical memory per window.
  std::vector<std::pair<VirtmemReservation*, u8*>> extra_windows;

  for (u64 k = 0; k < kMaxBlocks; ++k) {
    u8* win = window;
    u64 k_in_window = k;
    if (k >= kPhysBlocks) {
      const u64 w = (k - kPhysBlocks) / kPhysBlocks;
      k_in_window = (k - kPhysBlocks) % kPhysBlocks;
      if (w >= extra_windows.size()) {
        VirtmemReservation* r = nullptr;
        u8* base = T11Reserve(false, k4GiB, &r);
        if (!base) {
          stop = "no address space for another 4 GiB window (not a kernel refusal)";
          break;
        }
        extra_windows.push_back({r, base});
      }
      win = extra_windows[w].second;
    }
    if (k == 0 || k == kPhysBlocks - 1 || k == kPhysBlocks || (k % 128) == 0) {
      SMOKE_INFO("T11.2 about to commit block %" PRIu64 " and map its views", k);
    }
    T11Block b;
    b.backing = static_cast<u8*>(std::aligned_alloc(k2MiB, k2MiB));
    if (!b.backing) {
      stop = "heap exhausted (aligned_alloc), not a kernel refusal";
      break;
    }
    const u64 t0 = armGetSystemTick();
    Result rc = T11Commit(process, b, shadows + k * k2MiB);
    const u64 t1 = armGetSystemTick();
    blocks.push_back(b);  // released below whether it moved or not
    if (R_FAILED(rc)) {
      SMOKE_INFO("T11.2 block %" PRIu64 " commit %s", k, Rc(rc).c_str());
      stop = "commit refused: " + Rc(rc);
      kernel_refused = true;
      break;
    }
    backing_bytes += k2MiB;
    const size_t views_before = views.size();
    int failed_view = 0;
    rc = T11MapBlockViews(process, views, win, k_in_window, blocks.back(), &failed_view);
    const u64 t2 = armGetSystemTick();
    for (size_t v = views_before; v < views.size(); ++v) {
      mapped_view_bytes += views[v].size;
    }
    if (R_FAILED(rc)) {
      SMOKE_INFO("T11.2 block %" PRIu64 " view %s %s", k, kViewName[failed_view], Rc(rc).c_str());
      stop = std::string("view ") + kViewName[failed_view] + " of block " + std::to_string(k) +
             " refused: " + Rc(rc);
      kernel_refused = true;
      break;
    }
    const double ms = TicksToMs(t2 - t0);
    commit_ms_total += TicksToMs(t1 - t0);
    views_ms_total += TicksToMs(t2 - t1);
    min_ms = std::min(min_ms, ms);
    max_ms = std::max(max_ms, ms);
    if (k + 1 == kPhysBlocks) {
      SMOKE_INFO("T11.2 RESULT model D, %" PRIu64 " blocks (512 MiB): commit %.2f ms + views %.2f "
                 "ms; per block min %.3f avg %.3f max %.3f ms; %zu views, %" PRIu64
                 " MiB mapped in views",
                 kPhysBlocks, commit_ms_total, views_ms_total, min_ms,
                 (commit_ms_total + views_ms_total) / double(kPhysBlocks), max_ms, views.size(),
                 mapped_view_bytes >> 20);
      T11LogMemory("with model D for 512 MiB");
    }
  }
  u64 committed = 0;
  for (const T11Block& b : blocks) {
    committed += b.moved ? 1 : 0;
  }

  // Views only: more views of the committed blocks, no new backing, until the
  // kernel refuses (skipped if it already did).
  u64 extra_view_bytes = 0;
  if (!kernel_refused && committed) {
    std::free(reserve);
    reserve = nullptr;
    SMOKE_INFO("T11.2 about to add views of the %" PRIu64 " committed blocks only (no backing), "
               "up to %" PRIu64 " GiB",
               committed, kMaxExtraViewsGiB);
    Result rc = 0;
    while (R_SUCCEEDED(rc) && extra_view_bytes < (kMaxExtraViewsGiB << 30)) {
      VirtmemReservation* r = nullptr;
      u8* base = T11Reserve(false, k4GiB, &r);
      if (!base) {
        stop += "; views only: no address space left (not a kernel refusal)";
        break;
      }
      extra_windows.push_back({r, base});
      for (u64 k = 0; k < committed && k * k2MiB < k4GiB && R_SUCCEEDED(rc); ++k) {
        rc = T11MapView(process, views, base + k * k2MiB,
                        reinterpret_cast<u64>(blocks[k].shadow), k2MiB);
        if (R_SUCCEEDED(rc)) {
          extra_view_bytes += k2MiB;
        }
      }
    }
    if (R_FAILED(rc)) {
      stop += "; views only refused: " + Rc(rc);
    }
  }
  SMOKE_INFO("T11.2 RESULT ceiling: %" PRIu64 " blocks committed (%" PRIu64
             " MiB real), %zu views, %" PRIu64 " MiB mapped in views (%" PRIu64
             " MiB counting the aliases); stopped: %s",
             committed, backing_bytes >> 20, views.size(),
             (mapped_view_bytes + extra_view_bytes) >> 20,
             (mapped_view_bytes + extra_view_bytes + backing_bytes) >> 20, stop.c_str());
  T11LogMemory("at the ceiling");

  SMOKE_INFO("T11.2 about to unmap %zu views and %zu blocks", views.size(), blocks.size());
  const u64 t0 = armGetSystemTick();
  const int failed = T11Release(process, views, blocks);
  SMOKE_INFO("T11.2 release in %.1f ms: %d failures", TicksToMs(armGetSystemTick() - t0), failed);
  std::free(reserve);
  if (!failed) {
    for (auto& [r, base] : extra_windows) {
      (void)base;
      ReleaseReservation(r);
    }
    ReleaseReservation(code_res);
    ReleaseReservation(window_res);
  }
  T11LogMemory("after release");
  SMOKE_INFO("T11.2 END");
}

// ── T11.3: access cost on real aliased memory ──

// nfsmw-nx's generated-code macros, verbatim from
// sdk/resources/templates/codegen/pch_h.inja of StevensND/nfsmw-nx (ReXGlue SDK
// changes, BSD 3-Clause, Copyright (c) 2026 Tom Clay and contributors). On
// Switch the #else branch applies: the +4 KiB of the 0xE0 window lives in the
// view placement, and the macro is base + addr.
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
#define T11_NFSMW_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)
#else
#define T11_NFSMW_PHYS_HOST_OFFSET(addr) 0u
#endif
#define T11_NFSMW_LOAD_U32(x) \
  __builtin_bswap32(*(volatile u32*)(base + (u32)(x) + T11_NFSMW_PHYS_HOST_OFFSET(x)))
#define T11_NFSMW_STORE_U32(x, y) \
  (*(volatile u32*)(base + (u32)(x) + T11_NFSMW_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
// Their Windows / Apple Silicon form, for reference: compare + select.
#define T11_PC_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)

enum T11Mode { kT11Direct, kT11Table, kT11Nfsmw, kT11NfsmwPc };

// One read + one write of a big-endian u32 per iteration, as T6/T7. Low:
// every address below 0xA0000000; mixed: half of them through the 0xE0 window.
template <T11Mode kMode, bool kMixed, bool kRandom>
__attribute__((noinline)) u64 T11Loop(u8* base, const u64* table, u64 iterations) {
  u32 x = 0x12345678u;
  u32 off = 0;
  u64 sum = 0;
  for (u64 i = 0; i < iterations; ++i) {
    u32 window;
    if constexpr (kRandom) {
      x = x * 1664525u + 1013904223u;
      off = x & kBenchMask;
      window = x >> 31;
    } else {
      off = (off + 4) & kBenchMask;
      window = u32(i) & 1;
    }
    u32 guest = kMixed ? (off | (window ? 0xE0000000u : 0u)) : off;
    asm volatile("" : "+r"(guest));  // the window is unknown at compile time
    u32 v;
    if constexpr (kMode == kT11Nfsmw) {
      v = T11_NFSMW_LOAD_U32(guest);
      T11_NFSMW_STORE_U32(guest, v + 1);
    } else {
      u8* host;
      if constexpr (kMode == kT11Direct) {
        host = base + guest;
      } else if constexpr (kMode == kT11Table) {
        host = reinterpret_cast<u8*>(u64(guest) + table[guest >> 24]);
      } else {
        host = base + guest + T11_PC_PHYS_HOST_OFFSET(guest);
      }
      volatile u32* p = reinterpret_cast<volatile u32*>(host);
      v = __builtin_bswap32(*p);
      *p = __builtin_bswap32(v + 1);
    }
    sum += v;
  }
  return sum;
}

template <T11Mode kMode, bool kMixed, bool kRandom>
double T11Bench(u8* base, const u64* table, u8* shadows, u64 phys_bytes, const char* name,
                u64* checksum) {
  std::memset(shadows, 0, phys_bytes);  // same start for every variant
  SMOKE_INFO("T11.3 running %s ...", name);
  const u64 t0 = armGetSystemTick();
  *checksum = T11Loop<kMode, kMixed, kRandom>(base, table, kBenchIterations);
  const double ms = TicksToMs(armGetSystemTick() - t0);
  SMOKE_INFO("T11.3   %-40s %9.1f ms  (%.2f ns/iter, checksum %" PRIx64 ")", name, ms,
             ms * 1e6 / double(kBenchIterations), *checksum);
  return ms;
}

void T11AccessCost(Handle process) {
  constexpr u64 kBlocks = 33;  // 64 MiB + the 4 KiB shift of the 0xE0 window
  SMOKE_INFO("T11.3 BEGIN access cost on aliased memory: direct (model D, and nfsmw-nx's macro "
             "on Switch), table (model A), nfsmw-nx's PC macro; 100M read+write each");
  VirtmemReservation* code_res = nullptr;
  VirtmemReservation* d_res = nullptr;
  VirtmemReservation* pc_res = nullptr;
  u8* shadows = T11Reserve(true, kBlocks * k2MiB, &code_res);
  u8* d_base = T11Reserve(false, k4GiB, &d_res);
  u8* pc_base = T11Reserve(false, k4GiB, &pc_res);
  std::vector<T11Block> blocks;
  std::vector<T11View> views;
  bool ok = shadows && d_base && pc_base;
  if (ok) {
    SMOKE_INFO("T11.3 about to commit %" PRIu64 " blocks and map their views", kBlocks);
  }
  for (u64 k = 0; ok && k < kBlocks; ++k) {
    T11Block b;
    b.backing = static_cast<u8*>(std::aligned_alloc(k2MiB, k2MiB));
    if (!b.backing) {
      ok = false;
      break;
    }
    const Result rc = T11Commit(process, b, shadows + k * k2MiB);
    blocks.push_back(b);
    if (R_FAILED(rc)) {
      SMOKE_INFO("T11.3 block %" PRIu64 " commit %s", k, Rc(rc).c_str());
      ok = false;
      break;
    }
    const u64 src = reinterpret_cast<u64>(blocks.back().shadow);
    const u64 phys = k * k2MiB;
    // Low guest addresses: the same physical memory at base + 0 (benchmark
    // only), in both windows.
    Result vrc = T11MapView(process, views, d_base + phys, src, k2MiB);
    if (R_SUCCEEDED(vrc)) {
      vrc = T11MapView(process, views, pc_base + phys, src, k2MiB);
    }
    // 0xE0 of model D and nfsmw-nx on Switch: the view is shifted by 4 KiB.
    if (R_SUCCEEDED(vrc)) {
      vrc = k == 0 ? T11MapView(process, views, d_base + 0xE0000000ull, src + 0x1000,
                                k2MiB - 0x1000)
                   : T11MapView(process, views, d_base + 0xE0000000ull + phys - 0x1000, src,
                                k2MiB);
    }
    // 0xE0 of the PC form: an unshifted view, the macro adds the 4 KiB.
    if (R_SUCCEEDED(vrc)) {
      vrc = T11MapView(process, views, pc_base + 0xE0000000ull + phys, src, k2MiB);
    }
    if (R_FAILED(vrc)) {
      SMOKE_INFO("T11.3 block %" PRIu64 " view %s", k, Rc(vrc).c_str());
      ok = false;
    }
  }
  if (!ok) {
    SMOKE_INFO("T11.3 SKIP setup failed");
  } else {
    alignas(64) static u64 table[256];
    for (u32 i = 0; i < 256; ++i) {
      table[i] = i >= 0xE0 ? reinterpret_cast<u64>(shadows) - 0xE0000000ull + 0x1000
                           : reinterpret_cast<u64>(shadows);
    }
    // Every variant must reach the same bytes: guest 0xE0000008 is physical
    // 0x1008 in each of them.
    const u32 probe_guest = 0xE0000008u;
    Write64(shadows + 0x1008, kPattern2);
    const u64 via_d = Read64(d_base + probe_guest);
    const u64 via_table = Read64(reinterpret_cast<u8*>(probe_guest + table[probe_guest >> 24]));
    const u64 via_pc = Read64(pc_base + probe_guest + T11_PC_PHYS_HOST_OFFSET(probe_guest));
    const bool same = via_d == kPattern2 && via_table == kPattern2 && via_pc == kPattern2;
    SMOKE_INFO("T11.3 guest 0xE0000008 through D, table and PC form: %s",
               same ? "same physical bytes -> ok" : "DIFFERENT");
    u8* db = d_base;
    u8* pb = pc_base;
    const u64* t = table;
    asm volatile("" : "+r"(db), "+r"(pb), "+r"(t));
    const u64 bytes = kBlocks * k2MiB;
    u64 c[16] = {};
    double ms[16] = {};
    ms[0] = T11Bench<kT11Direct, false, false>(db, t, shadows, bytes, "direct seq low", &c[0]);
    ms[1] = T11Bench<kT11Table, false, false>(db, t, shadows, bytes, "table seq low", &c[1]);
    ms[2] = T11Bench<kT11Nfsmw, false, false>(db, t, shadows, bytes, "nfsmw-nx Switch seq low", &c[2]);
    ms[3] = T11Bench<kT11NfsmwPc, false, false>(pb, t, shadows, bytes, "nfsmw-nx PC form seq low", &c[3]);
    ms[4] = T11Bench<kT11Direct, true, false>(db, t, shadows, bytes, "direct seq mixed", &c[4]);
    ms[5] = T11Bench<kT11Table, true, false>(db, t, shadows, bytes, "table seq mixed", &c[5]);
    ms[6] = T11Bench<kT11Nfsmw, true, false>(db, t, shadows, bytes, "nfsmw-nx Switch seq mixed", &c[6]);
    ms[7] = T11Bench<kT11NfsmwPc, true, false>(pb, t, shadows, bytes, "nfsmw-nx PC form seq mixed", &c[7]);
    ms[8] = T11Bench<kT11Direct, false, true>(db, t, shadows, bytes, "direct rnd low", &c[8]);
    ms[9] = T11Bench<kT11Table, false, true>(db, t, shadows, bytes, "table rnd low", &c[9]);
    ms[10] = T11Bench<kT11Nfsmw, false, true>(db, t, shadows, bytes, "nfsmw-nx Switch rnd low", &c[10]);
    ms[11] = T11Bench<kT11NfsmwPc, false, true>(pb, t, shadows, bytes, "nfsmw-nx PC form rnd low", &c[11]);
    ms[12] = T11Bench<kT11Direct, true, true>(db, t, shadows, bytes, "direct rnd mixed", &c[12]);
    ms[13] = T11Bench<kT11Table, true, true>(db, t, shadows, bytes, "table rnd mixed", &c[13]);
    ms[14] = T11Bench<kT11Nfsmw, true, true>(db, t, shadows, bytes, "nfsmw-nx Switch rnd mixed", &c[14]);
    ms[15] = T11Bench<kT11NfsmwPc, true, true>(pb, t, shadows, bytes, "nfsmw-nx PC form rnd mixed", &c[15]);
    static const char* const kRow[] = {"sequential low", "sequential mixed", "random low",
                                       "random mixed"};
    for (int r = 0; r < 4; ++r) {
      const double* m = ms + r * 4;
      const u64* s = c + r * 4;
      const bool same_sum = s[0] == s[1] && s[0] == s[2] && s[0] == s[3];
      SMOKE_INFO("T11.3 RESULT %-16s direct %.1f ms; table %+.1f%%; nfsmw-nx Switch %+.1f%%; "
                 "nfsmw-nx PC form %+.1f%%; checksums %s",
                 kRow[r], m[0], Pct(m[1], m[0]), Pct(m[2], m[0]), Pct(m[3], m[0]),
                 same_sum ? "equal" : "DIFFERENT");
    }
  }
  SMOKE_INFO("T11.3 about to unmap %zu views and %zu blocks", views.size(), blocks.size());
  const int failed = T11Release(process, views, blocks);
  SMOKE_INFO("T11.3 release: %d failures", failed);
  if (!failed) {
    ReleaseReservation(code_res);
    ReleaseReservation(d_res);
    ReleaseReservation(pc_res);
  }
  SMOKE_INFO("T11.3 END");
}

void ProbeAliasModels() {
  const char* p = "T11";
  SMOKE_INFO("T11 BEGIN guest-memory models A / nfsmw-nx / D (svcMapProcessMemory aliases)");
  if (!Hinted(p, 0x74, "svcMapProcessMemory") || !Hinted(p, 0x75, "svcUnmapProcessMemory") ||
      !Hinted(p, 0x77, "svcMapProcessCodeMemory") ||
      !Hinted(p, 0x78, "svcUnmapProcessCodeMemory") ||
      !Hinted(p, 0x73, "svcSetProcessMemoryPermission")) {
    return;
  }
  const Handle process = envGetOwnProcessHandle();
  if (process == INVALID_HANDLE) {
    SMOKE_INFO("T11 SKIP envGetOwnProcessHandle() returned no handle");
    return;
  }
  T11AliasPrimitive(process);
  T11ModelD(process);
  T11AccessCost(process);
  SMOKE_INFO("T11 END");
}

}  // namespace

void RunProbes(rex::memory::Memory* memory) {
  SMOKE_INFO("PROBES BEGIN (results are measurements; they do not change the SMOKE verdict)");
  ProbeProcessCodeMemory();
  ProbeCodeMemory();
  ProbeMapPhysicalMemory();
  ProbeJit();
  ProbeHeap();
  ProbeTranslationCost();
  ProbeTranslationTable();
  ProbeOnDemandCommit();
  ProbeFaultRoundTrip(memory);
  ProbeConcurrentFaults(memory);
  ProbeAliasModels();
  SMOKE_INFO("PROBES DONE");
}
