// switch-smoke step 6b: SD card read and write ceiling (sd_bench.cpp).
#pragma once

// Reads `path` (a large game file) for `seconds` per test: sequential 1 MiB
// and random 64 KiB, 1 and 3 threads; then 5 s of 32 KiB appends to a
// scratch file. One line per test in smoke.log.
void RunSdBenchmark(const char* path, int seconds);
