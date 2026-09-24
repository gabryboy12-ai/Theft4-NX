// switch-smoke probes T1-T6: open questions of the Horizon guest-memory designs
// (docs/switch-port/03-memory.md).
//
//   T1 svcMapProcessCodeMemory + svcSetProcessMemoryPermission as an RW alias
//   T2 CodeMemory (svcCreateCodeMemory + svcControlCodeMemory) view permissions
//   T3 svcMapPhysicalMemory: outside vs inside the alias region, maximum size
//   T4 which backend libnx jitCreate picks on this system
//   T5 svcSetHeapSize: maximum heap and time to add 512 MiB
//   T6 cost of design A's address translation (microbenchmark)
//
// Rules: each probe logs BEGIN before doing anything; every syscall that may
// fault or kill the process is preceded by an "about to" line (each line is
// fsync'd, see main.cpp); every syscall result is logged with its Horizon
// result code. A syscall the loader does not report as available is skipped,
// never called: an unpermitted SVC raises an exception instead of returning.
// Memory is only read or written after svcQueryMemory shows the permission.

#include "probes.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

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

  auto pct = [](double a, double direct) { return (a - direct) * 100.0 / direct; };
  SMOKE_INFO("T6 RESULT sequential: A low %+.1f%%, A mixed %+.1f%% vs direct",
             pct(seq_low, seq_direct), pct(seq_mixed, seq_direct));
  SMOKE_INFO("T6 RESULT random:     A low %+.1f%%, A mixed %+.1f%% vs direct",
             pct(rnd_low, rnd_direct), pct(rnd_mixed, rnd_direct));
  std::free(base);
  SMOKE_INFO("T6 END");
}

}  // namespace

void RunProbes() {
  SMOKE_INFO("PROBES BEGIN (results are measurements; they do not change the SMOKE verdict)");
  ProbeProcessCodeMemory();
  ProbeCodeMemory();
  ProbeMapPhysicalMemory();
  ProbeJit();
  ProbeHeap();
  ProbeTranslationCost();
  SMOKE_INFO("PROBES DONE");
}
