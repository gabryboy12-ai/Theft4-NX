// switch-smoke probes T1-T8: open questions of the Horizon guest-memory designs
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
    SMOKE_INFO("T8 about to map block %d: src %p -> dst %p (+0x%" PRIx64 ")", i, b.src, b.dst,
               u64(i) * kT8Stride);
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
    SMOKE_INFO("T8 block %d map %.3f ms + RW %.3f ms", i, map_ms[i], perm_ms[i]);
  }
  if (mapped > 0) {
    double min_ms = 1e9, max_ms = 0;
    for (int i = 0; i < mapped; ++i) {
      min_ms = std::min(min_ms, map_ms[i] + perm_ms[i]);
      max_ms = std::max(max_ms, map_ms[i] + perm_ms[i]);
    }
    SMOKE_INFO("T8 RESULT map+RW: %d blocks (%d MiB) in %.2f ms; per block min %.3f avg %.3f max "
               "%.3f ms",
               mapped, mapped * 2, total_ms, min_ms, total_ms / mapped, max_ms);
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

  // Unmap: the first block alone (timed), then the rest.
  double unmap_total = 0, unmap_first = 0;
  int unmapped = 0, unmap_failed = 0;
  for (int i = 0; i < mapped; ++i) {
    T8Block& b = blocks[i];
    if (i < 2) {
      SMOKE_INFO("T8 about to svcUnmapProcessCodeMemory block %d (dst %p, src %p)", i, b.dst,
                 b.src);
    } else if (i == 2) {
      SMOKE_INFO("T8 about to unmap blocks 2..%d", mapped - 1);
    }
    const u64 t0 = armGetSystemTick();
    Result rc = svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(b.dst),
                                          reinterpret_cast<u64>(b.src), k2MiB);
    const double ms = TicksToMs(armGetSystemTick() - t0);
    if (i == 0) {
      unmap_first = ms;
      SMOKE_INFO("T8 unmap block 0 %s in %.3f ms", Rc(rc).c_str(), ms);
    }
    if (R_FAILED(rc)) {
      ++unmap_failed;
      if (unmap_failed <= 4) {
        SMOKE_INFO("T8 unmap block %d %s", i, Rc(rc).c_str());
      }
      continue;
    }
    b.mapped = false;
    unmap_total += ms;
    ++unmapped;
  }
  if (mapped > 0) {
    SMOKE_INFO("T8 RESULT unmap: %d/%d blocks in %.2f ms (first %.3f ms, avg %.3f ms), %d failed",
               unmapped, mapped, unmap_total, unmap_first,
               unmapped ? unmap_total / unmapped : 0.0, unmap_failed);
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

}  // namespace

void RunProbes() {
  SMOKE_INFO("PROBES BEGIN (results are measurements; they do not change the SMOKE verdict)");
  ProbeProcessCodeMemory();
  ProbeCodeMemory();
  ProbeMapPhysicalMemory();
  ProbeJit();
  ProbeHeap();
  ProbeTranslationCost();
  ProbeTranslationTable();
  ProbeOnDemandCommit();
  SMOKE_INFO("PROBES DONE");
}
