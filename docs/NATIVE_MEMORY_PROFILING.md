# Native renderer memory profiling

The native memory profiler is independent of the 600-frame CPU/GPU performance capture. It samples retained memory on a wall-clock interval so a capture can span loading, gameplay, repeated traversal, and shutdown without producing per-frame overhead.

## Enable and control a capture

Launch Liberty Recompiled with the dedicated diagnostics category:

```sh
--diagnostics --diagnostics-categories=native-memory-profiler
```

The profiler defaults to manual control. From an attached LLDB session:

```lldb
expr (int)rex_gta4_native_memory_profile_start()
expr (int)rex_gta4_native_memory_profile_mark()
expr (int)rex_gta4_native_memory_profile_stop()
```

`mark` increments a numeric marker and forces a sample. Use it immediately before and after a scene transition or repeated test route. A start or stop request is consumed after the next successfully submitted native frame.

The relevant CVARs are:

```toml
gta4_profile_native_memory_interval_ms = 1000
gta4_profile_native_memory_autostart = false
gta4_profile_native_memory_deep = true
gta4_profile_native_memory_vm_interval_ms = 5000
gta4_profile_native_memory_aged_frames = 600
```

The capture is bounded to 7,200 aggregate samples, 131,072 lifecycle events, and 262,144 retained-resource rows. Long captures overwrite the oldest event/time-series rows or report dropped retained rows; the profiler never grows without a bound. Detailed macOS VM-region walks run on a background thread and are reused by aggregate samples until the next scan completes.

## Output

Stopping a capture writes these files under the LibertyRecomp Diagnostics user folder:

- `native-memory-latest.csv`: time series with process, Vulkan heap, and subsystem values.
- `native-memory-events-latest.csv`: identity-level create, replace, release, retirement, eviction, destruction, and pool events.
- `native-memory-deltas-latest.csv`: nonzero aggregate subsystem changes between samples.
- `native-memory-retained-latest.csv`: baseline, marker, and final resource inventories with generations, ages, capacities, allocation sizes, and pending-release state.
- `native-memory-pools-latest.csv`: descriptor-pool demand/high-water and aged cache metrics.
- `native-memory-live-latest.csv`: final subsystem totals.
- `native-memory-latest.log`: compact text summary.

Run the offline analyzer with:

```sh
python3 tools/analyze_native_memory.py \
  "$HOME/Library/Application Support/LibertyRecomp/Diagnostics/native-memory-latest.csv"
```

It creates a Markdown report, timeline SVG, subsystem SVG, aggregate suspects CSV, and `native-memory-leaks.csv` identity-level triage table beside the capture.

## Interpretation

Renderer-owned host categories report retained container capacity and useful payload separately. Renderer-owned GPU categories report exact dedicated `VkDeviceMemory` allocation sizes. macOS physical footprint and resident size are independent process-level envelopes. Vulkan heap usage/budget is driver telemetry when supported.

On Apple unified-memory systems these domains overlap, so they must not be summed. A renderer category that grows throughout the capture and retains a positive final-quarter slope is a leak suspect. Early growth that flattens is more consistent with cache warmup. High cumulative growth and shrink with a stable live value indicates bounded churn. Rising pending-release counts point to a fence/retirement backlog.

For process growth not explained by renderer-owned categories, follow up with Instruments Allocations, VM Tracker, and Metal Resource Events.

Identity-level classifications are evidence-driven triage labels:

- `release-retirement-suspect`: the guest release was observed but the resource remains live or pending at the final capture point.
- `aged-cache-suspect`: a large buffer/conversion allocation has not been used for the configured age window.
- `variant-explosion-suspect`: one guest buffer retains many converted vertex variants.
- `live-working-set`: retained without direct evidence of a lifecycle defect; this may be legitimate cache or game ownership.
