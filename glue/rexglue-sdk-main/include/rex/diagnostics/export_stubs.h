/**
 * @file diagnostics/export_stubs.h
 * @brief Accounting for Xbox exports implemented only as stubs (REX_EXPORT_STUB).
 *
 * A stub export does nothing (or returns a fixed value) and lets the game go
 * on, as on the desktop. Each one is logged on its first call, counted, and
 * listed with its count by LogExportStubSummary. With the cvar
 * nx_fatal_stubs a stub stops the run instead
 * (rex::diagnostics::StopAtMissingFunction).
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::diagnostics {

// One per REX_EXPORT_STUB, a function-local static: constant-initialised, so
// the hot path is one relaxed increment.
struct ExportStubSite {
  const char* name;  // "__imp__<Export>"
  std::atomic<uint64_t> hits{0};
  ExportStubSite* next = nullptr;  // list of the stubs hit, linked on the first hit
};

// Counts a call; on the first one logs the export and the guest LR. With
// nx_fatal_stubs, stops the run (does not return).
void RecordExportStubHit(ExportStubSite& site, uint32_t guest_lr);

// Logs how many distinct stubs were hit and one line per stub with its count,
// most called first. `when` says at which point ("at the stop", "at exit").
void LogExportStubSummary(const char* when);

}  // namespace rex::diagnostics
