/**
 * @file core/export_stubs.cpp
 * @brief First-call log, counts and the optional stop for stub exports.
 */

#include <rex/diagnostics/export_stubs.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/diagnostics/missing_function.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nx_fatal_stubs, false, "Kernel",
                    "Stop the run at the first Xbox export that is only a stub (REX_EXPORT_STUB) "
                    "instead of logging it and going on");

namespace rex::diagnostics {
namespace {

std::mutex g_stub_list_mutex;
ExportStubSite* g_stub_list = nullptr;

const char* ExportName(const char* symbol) {
  constexpr const char kPrefix[] = "__imp__";
  return std::strncmp(symbol, kPrefix, sizeof(kPrefix) - 1) == 0 ? symbol + sizeof(kPrefix) - 1
                                                                 : symbol;
}

}  // namespace

void RecordExportStubHit(ExportStubSite& site, uint32_t guest_lr) {
  if (site.hits.fetch_add(1, std::memory_order_relaxed) == 0) {
    {
      std::lock_guard<std::mutex> lock(g_stub_list_mutex);
      site.next = g_stub_list;
      g_stub_list = &site;
    }
    REXKRNL_WARN("export stub, first call: {} (guest LR {:08X})", ExportName(site.name),
                 guest_lr);
  }
  if (REXCVAR_GET(nx_fatal_stubs)) {
    StopAtMissingFunction("Xbox export is only a stub (nx_fatal_stubs)", ExportName(site.name),
                          guest_lr);
  }
}

void LogExportStubSummary(const char* when) {
  std::vector<std::pair<const char*, uint64_t>> hits;
  {
    std::lock_guard<std::mutex> lock(g_stub_list_mutex);
    for (ExportStubSite* site = g_stub_list; site; site = site->next) {
      hits.emplace_back(ExportName(site->name), site->hits.load(std::memory_order_relaxed));
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  uint64_t total = 0;
  for (const auto& [name, count] : hits) {
    total += count;
  }
  REXKRNL_INFO("export stubs {}: {} distinct, {} calls", when, hits.size(), total);
  for (const auto& [name, count] : hits) {
    REXKRNL_INFO("  {:>10}  {}", count, name);
  }
}

}  // namespace rex::diagnostics
