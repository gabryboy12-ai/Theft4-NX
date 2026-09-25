// switch-smoke: shared logging entry point and the exploratory probes.
#pragma once

// Prints a line on screen and appends it durably to smoke.log (see main.cpp).
void Report(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define SMOKE_INFO(...) Report(false, __VA_ARGS__)

// T1-T8: questions for the guest-memory designs in docs/switch-port/03-memory.md.
// Results are measurements, not pass/fail: they do not change the SMOKE verdict.
void RunProbes();
