# GTA IV Asset Streaming System

## Evidence-based reconstruction of the Xbox 360 retail path and its LibertyRecomp execution

**Status:** research document, 22 August 2026

**Scope:** understanding the existing system only. This document intentionally contains no replacement architecture, migration plan, or rewrite proposal.

## 1. Purpose and evidence policy

GTA IV does not have one monolithic function called “the streaming system.” Streaming is a stack of cooperating subsystems: world and script code decides what should be resident; a title-level manager tracks assets, dependencies, state, priority, and memory; a fixed set of in-flight load slots constructs resource requests; RAGE file devices resolve archives and optional caches; `pgStreamer` performs queued bulk reads; per-asset callbacks construct and publish objects; and the renderer consumes the resulting resources.

This reconstruction uses the Xbox 360 retail assembly/excavation database, the generated PPC emitted from that executable, the active RexGlue runtime and GTA IV native renderer, and current runtime logs. No decompiler pseudocode was used. The obsolete top-level `LibertyRecomp/` implementation is excluded completely.

Claims use these confidence labels:

| Label | Meaning |
|---|---|
| **Verified — retail** | Directly demonstrated by retail Xbox 360 instructions, class/vtable recovery, strings, or call relationships. |
| **Verified — active runtime** | Directly demonstrated by the currently active RexGlue/native-renderer source or its logs. |
| **Corroborated** | Consistent with a primary external source or a later open-source RAGE implementation, but not proof of the exact GTA IV retail implementation. |
| **Inferred** | A semantic name or architectural interpretation supported by several operations, but absent from surviving symbols. |
| **Unknown** | Evidence is not yet sufficient to assign an exact meaning. |

All byte, alignment, capacity, and timing conversions quoted here were reproduced with a Python script as required by the repository instructions.

## 2. The shortest accurate mental model

The retail path is:

```text
camera / world cells / scripts / cutscenes / gameplay
                    |
                    v
title streaming registry (24-byte record per streamable item)
  - requested/loading/loaded/unloaded state
  - dependency references and per-type callbacks
  - intrusive priority/request lists
  - physical/virtual memory accounting
                    |
                    v
8 title load slots (resource allocation + completion bookkeeping)
                    |
                    v
"stream:\\<global-index>" through fiStreamingDevice
                    |
                    v
pgStreamer handle table + worker request rings
                    |
                    v
fiDevice -> fiPackfile / fiCachedDevice -> DVD or optional HDD cache
                    |
                    v
guest virtual/physical memory -> type fixup/publish callback
                    |
                    +--> physics, renderer, animation, AI, audio, scripts, etc.
                    |
                    v
LibertyRecomp gta4-native renderer captures bound guest GPU resources,
untile/endian-converts them, and uploads host Vulkan images/buffers
```

The title-level manager answers **what should live in memory**. `pgStreamer` answers **how a byte range is read without blocking the requesting code**. `fiDiskCache` answers **whether DVD content can be served from an installed cache**. The active native renderer answers **how an already resident Xbox GPU resource becomes a host Vulkan resource**. Conflating these layers obscures most of the important behavior.

## 3. Why the retail design looks this way

### 3.1 A small unified memory machine

Xbox 360 presents 512 MiB of unified memory shared by CPU and GPU, rather than separate large system and graphics pools. Microsoft’s architecture paper also describes three symmetric CPU cores, six hardware threads, and SATA attachment for optical and hard-disk storage. These facts impose two simultaneous requirements: content must be admitted and evicted aggressively, and loaded resources may need both CPU-visible and GPU-appropriate page layouts. See Andrews and Baker, [“Xbox 360 System Architecture”](https://doi.org/10.1109/MM.2006.45), and Microsoft’s [Hot Chips Xbox 360 system presentation](https://www.cs.wustl.edu/~roger/569M/HC17.S8T4.pdf).

The retail binary’s separate “physical” and “virtual” terminology, RSC page calculations, scatter reads, two memory counters/limits, per-type allocators, and fragmentation diagnostics are therefore not incidental implementation details. They are the resource residency mechanism for a unified, tightly constrained machine.

### 3.2 DVD remained a required path; HDD caching was conditional

An Xbox 360 hard drive was not universal across the original product line. Microsoft’s launch announcement lists a detachable 20 GB drive with the premium system, omits it from the Core system, and lists it as a separately purchasable accessory. See Microsoft’s [Xbox 360 launch pricing and SKU announcement](https://news.microsoft.com/source/2005/08/17/microsoft-announces-xbox-360-price-for-europe-and-north-america-starts-at-299-99-u-s-e299-99209-99/). The streaming path consequently could not assume a persistent installed cache. The binary reflects that: `cache:\valid.txt` and `cache1:\valid.txt` are probed, messages explicitly say the cache may be missing or out of date, and dual-cache-partition support can be enabled or disabled. The cache is an acceleration layer around the source device, not the source of truth.

Microsoft’s optical-I/O guidance explains several otherwise conspicuous constants. A physical DVD ECC block is 32 KiB; a read smaller than that can still consume the whole block, and small/random reads plus seeks and layer changes damage throughput. Microsoft recommends large, asynchronous, well-ordered reads and notes that direct asynchronous reads avoid an extra application copy. See [Optimizing DVD Performance for Windows Games](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/optimizing-dvd-performance-for-windows-games). GTA IV’s retail `pgStreamer` reads paged-resource data in at most 32 KiB chunks, exactly one such ECC block. This source is general Microsoft DVD guidance, not GTA IV-specific documentation, but the constant-level match is strong corroboration.

The archive/device stack, numeric `stream:` namespace, worker queues, and HDD cache collectively turn thousands of logical assets into ordered bulk reads. This is why the system is visibly shaped around optical-media latency even when an HDD is available.

### 3.3 Streaming is not a background convenience

Rockstar North developer Adam Fowler later described streaming as “the backbone of everything we do,” while Phil Hooker explained that assets were sufficiently large that essentially everything had a streaming lifecycle. Those statements concern later RAGE work, but accurately describe the architecture visible in GTA IV. See [Inside Rockstar North, Part 3: The Tech](https://mcvuk.com/development-news/inside-rockstar-north-part-3-the-tech/). The important point is architectural, not promotional: rendering, physics, collision, animation, population, scripts, and cutscenes all rely on a shared residency system.

## 4. Storage and naming layers

### 4.1 RAGE file devices

RAGE routes paths through `fiDevice` rather than issuing platform file calls from every consumer. Retail RTTI and vtable recovery identify:

- `rage::fiPackfile`, a 31-method file device for archive contents;
- `rage::fiCachedDevice`, a 31-method wrapper able to route reads through cached media;
- `fiStreamingDevice`, a 30-method synthetic device mounted at `stream:\`;
- ordinary device mounts including `dvd:`, `game:`, `update:/`, `cache:`, `cache1:`, `memory:`, `tcpip:`, and `embedded:/`.

Evidence: `gta_iv/xex_excavation_retail/rtti_classes.txt:3657-3665`, `classes_with_methods.txt:32781-33007`, and `function_class_map.txt:4313-4320,6282-6336`.

Packfiles are therefore not special-cased inside the high-level streaming manager. An archive is mounted as a device, its entries expose offsets and resource metadata, and bulk reads continue through the common virtual interface. The open-source CitizenFX RPF2 reader corroborates this device model: it validates the `RPF2` magic, reads the table of contents from the archive, and maps an entry read to a parent-device bulk read at the entry’s data offset. See [CitizenFX `VFSRagePackfile.cpp`](https://github.com/citizenfx/fivem/blob/master/code/components/vfs-core/src/VFSRagePackfile.cpp). Swage’s source-level [RPF documentation](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rpf.md) provides additional format corroboration. These projects are format evidence, not proof that every later implementation detail exists in GTA IV.

### 4.2 The synthetic `stream:` namespace

The streaming manager constructs and mounts an `fiStreamingDevice` at `stream:\` in `sub_82512DC0`; its recovered virtual functions occupy `0x82512E70` through `0x82513250`. The title loader formats `stream:\%u` in `sub_82678D38` and passes that path to `pgStreamer::Open`.

This is a key indirection. The decimal component is a **global streaming-entry index**, not a real filename. `fiStreamingDevice` translates the index through the central registry and per-type metadata to the actual backing device, archive entry, offset, size, and RSC flags. The lower I/O layer can consequently treat a global streaming asset like a normal file while the high-level manager retains authoritative identity and type information.

Evidence: retail strings at `all_strings_with_addrs.txt:3916,8243`; assembly references at `default_decrypted.xex.asm:1660143-1660145,2159180-2159182`; generated PPC at `gta4_recomp.33.cpp:53888` and `gta4_recomp.46.cpp:16819`.

Recovered virtual methods make the translation concrete. `0x82512E70` parses the numeric relative name; `0x82512EE0` resolves the entry and reports its module-relative offset in 2 KiB units; `0x82512FA8` selects the module’s underlying device and bulk-reads after adding its 64-bit base offset; `0x82513048` returns the low 30 bits of entry `+0x08`; `0x825130C8` returns the module base; `0x82513130` tests material availability; `0x825131C8` reports the packed size descriptor for resource entries; and `0x82513250` resolves the live object address. Evidence: `default_decrypted.xex.asm:1659737-1660131` and `function_class_map.txt:4313-4320`.

### 4.3 Paged RSC resources versus raw streams

The loader distinguishes paged resources from ordinary byte streams. For a paged RSC entry it derives virtual and physical page requirements from the entry’s resource descriptor, reserves the appropriate memory, and gives `pgStreamer` a scatter/page description. The low-level worker recognizes the resource form, accounts for its resource header, and reads the individual ranges into their final destinations. Ordinary streams take a simpler contiguous path.

This distinction is why “asset size” cannot always be represented by one byte count. A drawable or texture resource may consume separately described virtual and physical page regions, while a raw stream has a direct length. It also explains the manager’s dual budget vocabulary and why completion must execute type-specific fixups before publishing a pointer.

CitizenFX’s independent RSC5 reconstruction corroborates the broad resource contract: it validates an RSC header, decodes virtual and physical sizes, expands the resource payload, partitions it into virtual and physical blocks, and constructs a block map. See [`pgBase.cpp`](https://github.com/citizenfx/fivem/blob/master/code/components/rage-formats-x/src/pgBase.cpp). Its exact bit decoding is community reverse engineering and is not substituted for the locally verified GTA IV instructions.

## 5. Central streaming registry

### 5.1 Addressing and record size

The active instrumentation and retail instructions agree that the global entry-array pointer is stored at guest address `0x83032744`, and that each entry is 24 bytes. An asset index is converted to a record by multiplying by 24. Current trace code independently uses the same address and stride at `glue/rexglue-sdk-main/gta4-recomp/src/gta4_physics_trace.cpp:25-27,793-798`.

Subsystem initialization begins in `sub_822D30B8`: it installs the allocation callback, constructs the manager with `sub_825132A0`, allocates the title loader, and initializes disk caching. `sub_822D3190` later sizes/initializes the registry through `sub_82511D60`. Evidence: `default_decrypted.xex.asm:868585-868695`.

### 5.2 Reconstructed 24-byte entry

The following layout describes observed behavior. Semantic field names marked “inferred” are deliberately conservative.

| Offset | Width | Observed use | Interpretation |
|---:|---:|---|---|
| `+0x00` | 4 | Decoded with packed-field shifts/extracts during allocation and accounting. | Packed virtual/physical resource-size descriptor. |
| `+0x04` | 4 | High byte selects a per-type/store record; low 24 bits participate in backing-offset or loaded-address resolution. | Packed store/type plus local offset/reference (**inferred name**). |
| `+0x08` | 4 | Upper two bits are state; lower 30 bits are used in address/size and sector-rounding operations. | Packed state and size/data field. |
| `+0x0C` | 2 | Incremented/decremented when dependent resources are requested, loaded, or released. | Dependency/reference count (**inferred name**). |
| `+0x0E` | 2 | Request, priority, protection, and behavior tests. | Request/behavior flags (**inferred name**). |
| `+0x10` | 2 | Linked-list navigation with `0xFFFF` sentinel. | Previous entry index (**inferred orientation**). |
| `+0x12` | 2 | Linked-list navigation with `0xFFFF` sentinel. | Next entry index (**inferred orientation**). |
| `+0x14` | 2 | Established during registration and used as a related-entry link. | Related/child entry index (**inferred relationship name**). |
| `+0x16` | 1 | Used during module-local registration/type lookup. | Module-local registration byte (**inferred name**). |
| `+0x17` | 1 | Multiplied by a 100-byte per-module descriptor stride. | Registry/module descriptor index (**inferred name**). |

The strongest implementation evidence is `sub_82511890` (state and dependent-count transitions), `sub_82511A10` (loaded-address resolution), `sub_82511A80` (2 KiB rounding), and `sub_825120E8` (request/dependency/list insertion): `glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.33.cpp:50904,51110,51173,52073`.

The per-module table uses 100-byte records and supplies callbacks for dependency enumeration, allocation/construction, publication, unload/destruction, and accounting. Exact class names for every module are not recovered, so the table is best understood as a plugin interface between generic residency policy and asset-family-specific behavior. Registry initialization also creates six extra 24-byte sentinel entries, arranged as three paired intrusive lists.

### 5.3 Four-state machine

The upper two bits of the `+0x08` word encode:

| Bits | State | Meaning |
|---:|---|---|
| `00` | Unloaded | No published resident object. |
| `01` | Loaded | Type construction/publication completed; consumers may resolve it. |
| `10` | Requested | Present in the manager’s request/dependency machinery but not yet issued. |
| `11` | Loading | Assigned to a title load slot; I/O or completion work is in flight. |

This mapping is verified by retail transition behavior and the active trace decoder at `gta4_physics_trace.cpp:654-673,821-835`.

A normal transition is:

```text
Unloaded --request--> Requested --slot/I/O issue--> Loading
    ^                                           |
    |                                           v
    +----------- unload/release <----------- Loaded
```

There are cancellation and failure edges as well. A requested item can return to unloaded, an allocation/read/fixup failure cleans its slot and accounting, and the manager may reject or defer work when dependencies or memory cannot be satisfied. State publication is not merely an I/O completion bit: the per-type constructor/fixup and publication callbacks run before the asset becomes loaded.

## 6. Request production, dependencies, and priority

### 6.1 Who creates requests

Requests originate above the registry. Camera/world-scene code evaluates spatial relevance; model, texture, collision, animation, population, cutscene, and script systems add explicit needs; dependency callbacks add resources required by another asset. Script-native names recovered from the retail executable include:

- `START_STREAMING_REQUEST_LIST` and `FINISH_STREAMING_REQUEST_LIST`;
- `GET_NUM_STREAMING_REQUESTS` and `IS_STREAMING_PRIORITY_REQUESTS`;
- `PRIORITIZE_STREAMING_REQUEST`;
- `ENABLE_SCENE_STREAMING` and `SWITCH_STREAMING`;
- `ALLOW_GAME_TO_PAUSE_FOR_STREAMING`.

The names occur together in the native-registration code at `default_decrypted.xex.asm:1820717-1820784`; the string table is at `all_strings_with_addrs.txt:4799-4812`. A scene request-list resource path, `common:/data/streaming/%s.srl`, appears at `all_strings_with_addrs.txt:228`.

This demonstrates that the core manager supports both continuous scene-driven demand and explicit bounded request batches. “Pause for streaming” is a policy exposed to game/script logic, not an implicit property of every read.

### 6.2 Recursive dependency acquisition

`sub_825120E8` is the central request operation. It checks the current state and request flags, invokes a per-type dependency resolver, recursively requests those dependency indices, adjusts their dependent/reference counts, links a newly requested entry into manager lists, updates request counters, and writes the requested state.

Dependencies therefore participate in both ordering and lifetime:

1. An owner cannot be safely published until its required children are loaded.
2. Dependency counts protect children from eviction while owners need them.
3. Unload walks the corresponding relationships and releases those counts.

This is stronger than a flat “load nearby filenames” queue. The registry represents a live asset graph mediated by compact indices and per-type callbacks.

### 6.3 Intrusive lists and candidate selection

The two 16-bit link fields let the manager keep requested/resident entries in index-based intrusive lists without allocating queue nodes. `0xFFFF` is the no-entry sentinel. `sub_825132D0` traverses these lists, considers dependency readiness and flags, and may ask the unload path to free candidates before returning the next issueable asset. `sub_82513F10` consumes that selection.

The manager owns three paired sentinel lists: `+0x08/+0x0C` is a resident pair, `+0x10/+0x14` is the verified request/dispatch pair, and `+0x18/+0x1C` is a second resident pair for a protected/flagged class. The original names and exact ordering policy of the two resident lists remain unknown; it is not safe to call either a strict LRU or FIFO.

Request accounting is clearer. Manager `+0x38` counts all requests, `+0x3C` counts a subset that excludes one flagged class, and `+0x40` counts priority requests. Flag `0x10` marks/counts a priority request once, and `sub_825132D0` continues dispatch processing while priority requests remain. Thus list order, request flags, dependencies, priority counts, and memory feasibility all affect selection.

## 7. Residency accounting, allocation, and fragmentation

### 7.1 Default limits

The streaming-manager constructor `sub_825132A0` writes `52,428,800` bytes to two fields and initializes the `stream:\` device. Python conversion gives exactly 50 MiB for each literal. The two fields are verified limits/counters; their exact names are not. Given their later uses and the surrounding physical/virtual terminology, they are very likely the default limits for the two streaming memory classes, but that semantic label remains inferred.

Evidence: `gta4_recomp.33.cpp:54594-54619`.

`sub_821CFD10` reads `platform:/stream.ini` keys `virtual`, `physical`, `virtual_optimised`, and `physical_optimised`. Values are shifted by 10, so the file expresses them in 1,024-byte/KiB units; nonzero optimized values override their normal counterparts. Virtual and physical limits are then capped through separate allocator classes and stored at manager `+0x20` and `+0x2C`. This allows platform/profile data to tune the two sides rather than treating all assets as one heap. Evidence: `default_decrypted.xex.asm:516362-516485,1658150-1658340`.

### 7.2 Admission is not just total-free-byte accounting

Before issuing a load, `sub_826793F0` calculates the required resource memory, invokes allocator callbacks, and prepares the title slot. Paged resources may need multiple regions with specific page placement. A request can therefore fail even when aggregate free bytes appear sufficient.

The retail executable contains both `Unable to make space for %s because memory is fragmented, I give up` and `***** Fragmented Streaming Memory *****` (`all_strings_with_addrs.txt:8247` and adjacent strings). This is direct evidence that the manager distinguishes capacity pressure from placement/fragmentation failure. Functions around `0x821C6890` and `0x821C69B0` use the literal `defrag` and establish a resource relocation/fixup context that walks embedded pointers. That proves resource pointer-fixup support, but does **not** by itself prove those functions are the streaming-heap compactor. The actual heap movement and synchronization protocol remains unresolved.

### 7.3 Eviction

`sub_82513A10` is the central unload operation. It checks state, reference/dependency counts and flags, calls the type-specific unload/destructor callback, updates manager memory counters, releases dependencies, unlinks the entry, and returns it to unloaded. The load selector can invoke this path to make room for a higher-priority request.

This establishes an important ownership boundary: `pgStreamer` does not decide what to evict. Eviction belongs to the title manager because only it understands asset dependencies, type destructors, memory classes, and game-visible state.

## 8. Title-level in-flight loader

### 8.1 Eight slots

`sub_82678EB0` initializes eight fixed load slots. Each slot is `0x620` bytes (1,568 bytes), for 12,544 bytes / 12.25 KiB of slot storage. Each owns completion synchronization and a large embedded request description. A small queue helper pushes/removes active slot indices.

The fixed slot count bounds resource allocations and outstanding title-level work independently of the number of logical requests. It also makes completion processing deterministic: the manager can pump a small set, publish finished resources, and refill free slots.

Evidence: `gta4_recomp.46.cpp:17032` and the surrounding initialization, plus queue functions `sub_82678DC8` and `sub_82678E20`.

### 8.2 Issue path (`sub_82679610`)

For a chosen global entry, the issue path performs approximately this sequence:

1. Clear the slot’s `0x604`-byte request payload and record the entry index/state.
2. Format `stream:\<index>` and call `pgStreamer::Open` through `sub_82678D38`.
3. Decode raw versus paged-RSC size and ask the per-type/memory layer to allocate the final guest resource regions.
4. Optionally register the read with the disk-cache layer.
5. Submit the read to `pgStreamer` with the slot’s ranges, callback, and completion context.
6. Enqueue the active slot and transition the registry entry from requested to loading.

If any stage fails, it closes the handle, releases partial allocation/accounting, and does not publish the asset. Evidence: `gta4_recomp.46.cpp:18111-18538`.

### 8.3 Completion and publication (`sub_82679140`)

I/O completion only makes the bytes available. The title completion path then:

1. closes the `pgStreamer` handle;
2. verifies that required dependencies are resident;
3. copies the saved request description into a stable local work area;
4. invokes the per-type construction/fixup callback;
5. invokes the per-type publication/registration callback;
6. transitions the central entry to loaded and adjusts dependency/accounting state;
7. cleans the slot for reuse.

On construction failure it reverses allocation and accounting instead. This two-stage “read, then construct/publish” boundary is essential for RSC resources containing guest pointers, GPU fetch descriptors, or type-specific registration data.

Evidence: `gta4_recomp.46.cpp:17424-17804`.

### 8.4 Pump behavior

`sub_82679908` advances individual slot states; `sub_826799E0` waits for or drains completions; `sub_82679A90` performs a time-budgeted completion pump; `sub_82679B98` dispatches new requests into free slots. The manager can thus do limited work during normal frames or explicitly wait/drain when a script or loading transition requires it.

## 9. `pgStreamer`: low-level queued bulk I/O

### 9.1 Handles

`pgStreamer::Open` is recovered at `sub_8284CB80`; close is `sub_8284CE38`. Open resolves the mounted `fiDevice`, validates resource metadata/version where applicable, opens the backing device handle, and takes an entry from a compact free-list table. Each handle record is 28 bytes. The error strings distinguish missing files, exhausted handles, invalid closes, and files that are not valid resources.

Initialization in `sub_8284CFD8` rounds the configured handle count to a power of two for mask-based lookup, allocates the table, builds its free list, and creates workers named `[RAGE] pgStreamer::Worker`. Each worker is created with a 64 KiB stack. Evidence: `gta4_recomp.61.cpp:12075,12471,12707` and `all_strings_with_addrs.txt:12278`.

### 9.2 Per-worker request ring

Each worker owns a ring with 16 physical descriptor positions. A descriptor is `0x614` bytes (1,556 bytes), so the backing descriptor array is 24,896 bytes / 24.3125 KiB per worker. The complete queue bank is `0x6170` bytes (24,944 bytes), leaving 48 bytes for bookkeeping. The enqueue code refuses another request when the queued count reaches 15, leaving one ring position unused so full and empty states remain distinguishable; usable queued payload is therefore 23,340 bytes / 22.792969 KiB.

Submission through `sub_8284C888` chooses a worker and copies request/range/callback data into its ring. `sub_8284C540` performs the bounded enqueue and signals the worker. `sub_8284C5E8` waits, dequeues, and calls the core worker `sub_8284BF50`. A global `synchronous` option can bypass queueing and execute the same request core directly, which is a retail diagnostic/policy path distinct from LibertyRecomp’s currently synchronous host file implementation.

Evidence: `gta4_recomp.61.cpp:10305,11162,11263,11654`.

### 9.3 Core worker and 32 KiB chunks

`sub_8284BF50` obtains a bulk handle, distinguishes paged/packed resources from ordinary reads, builds the destination scatter/page mapping for resources, and performs the actual device reads. The uncompressed path reads each scatter destination directly and requires the exact requested byte count. The packed/compressed path initializes a stream transform/decompressor, reads compressed source data in units no larger than `0x8000` (32 KiB), feeds the transform, and emits output across the final scatter destinations. As chunks complete it schedules completion work, signals client events/callbacks, closes temporary bulk handles, and finally invokes the caller’s completion callback.

The worker reads directly into the resource’s intended guest destinations. It does not load a whole archive member into an unrelated staging blob and then let the high-level manager copy it. That minimizes memory duplication and fits Microsoft’s contemporary guidance for asynchronous optical reads.

### 9.4 Concurrency contract

There are two independent queue limits:

- the title layer has eight resource-construction slots;
- each `pgStreamer` worker has a 16-position/15-queued request ring.

This separation lets the generic I/O service accept work from systems other than the central asset manager while the title layer separately bounds expensive RSC allocations and type publication. `sub_8284C3D8` provides a drain/wait operation that loops until the global active count reaches zero.

### 9.5 `pgBasicScheduler` and other streamables

RTTI also identifies `rage::pgBasicScheduler`, an eight-vfunc priority scheduler above `pgStreamer` for `pgStreamable` objects. It is related infrastructure, not the central 24-byte asset registry. The scheduler tracks the best pending streamable by floating-point priority, obtains/allocates streamed data, advances pending work to active callbacks, and supports unscheduling warnings.

Its streamable state alternates between two `0x604`-byte staging buffers—a total of 3,080 bytes / 3.007812 KiB—so one buffer can be consumed while the other is prepared. `sub_8286CB30` opens/schedules through `pgStreamer` and reports allocation failures; the method at `0x8286CF98` selects a buffer and flips the buffer bit. Evidence: `classes_with_methods.txt:33689-33697`, `default_decrypted.xex.asm:2874047-2874511`, and `gta4_recomp.62.cpp:13344,14000`.

## 10. `fiDiskCache`: optional DVD-to-HDD acceleration

### 10.1 Validation and routing

`sub_8284D440` probes `cache:\valid.txt` and `cache1:\valid.txt`. Status strings distinguish a cache that is current and directly usable from one that is absent or stale and must be copied. `fiCachedDevice` then wraps the source device and routes reads to the valid cache when possible.

The two cache partitions, the `nohdcaching` option, and the `cleardiskcache` command show that caching is explicitly optional and administratively invalidatable. A missing HDD/cache cannot invalidate the base DVD path.

### 10.2 Copy worker

`sub_8284D730` is the cache-copy worker. It has a 128-position work ring limited to 127 queued entries so empty and full remain distinguishable. It opens a bulk source and bulk destination, copies in 32 KiB chunks, updates 64-bit offsets, and periodically yields after 1 MiB of copied data so caching does not monopolize execution. On completion it closes the destination, writes/updates validation metadata, reopens as required, closes the source, and logs `fiDiskCache::Worker - copy of '%s' completed.` `sub_8284DA58` creates the `[RAGE] DiskCache` thread.

Evidence: `gta4_recomp.61.cpp:13351,13799,14269`; strings at `all_strings_with_addrs.txt:12281-12289`.

### 10.3 What the cache changes—and what it does not

The disk cache changes the physical device supplying bytes and reduces optical seeks. It does not change global asset identity, dependency order, memory budgets, state publication, or unload policy. Those remain above the file-device boundary.

## 11. Asset-family behavior

The generic manager deliberately avoids knowing how every payload becomes a usable object. The 100-byte per-type records provide callbacks that specialize at least these phases:

- enumerate/request dependencies;
- calculate or reserve memory;
- construct/fix up a freshly read resource;
- publish/register the result with its owning subsystem;
- unload/destruct and release child references;
- report/account size or eligibility.

This is how a common registry can service drawables, texture dictionaries, collision bounds, animation data, world sectors, navigation/AI data, script resources, and other families without flattening their lifetime rules. Exact type-index-to-class mappings are only partially recovered, so this document does not invent a complete enumeration.

Audio also contains substantial streaming machinery and shares the file/device layer, but radio/wave streaming has its own slot, timing, and decoder logic. It should not be mistaken for the central world-asset manager described here. The same warning applies to network streaming terminology and renderer cache terminology.

## 12. Active LibertyRecomp execution path

### 12.1 What source is active

The active runtime and native renderer live under `glue/rexglue-sdk-main`. `GTA4App::OnFinalizePaths` selects the game/update roots, and if no explicit graphics system or plugin is configured, it selects `gta4-native` (`gta4-recomp/src/gta4_app.cpp:178-240`). This document does not use the obsolete `LibertyRecomp/` directory.

### 12.2 Host VFS mapping

`Runtime::SetupVfs` mounts the chosen host game directory as `\Device\Harddisk0\Partition1`, then maps `game:` and `d:` to it. An update directory, when present, is mounted at `\Device\Harddisk0\PartitionUpdate` and linked as `update:`. Raw `Partition0`, `Cache0`, and `Cache1` paths are handled by a `NullDevice`. Its `NullFile` reads and writes return success while transferring no data, so raw cache access is neutralized rather than persisted (`src/filesystem/devices/null_file.cpp:25-39`). No `cache:` device is mounted; the code intentionally lets those paths fail cleanly because the game tolerates “device not found” better than certain device errors.

Evidence: `glue/rexglue-sdk-main/src/system/runtime.cpp:299-360`.

Consequently, the guest’s retail archive/device/streaming logic still runs, but its lowest storage layer usually resolves to ordinary host files in the configured game root. The original persistent DVD-to-HDD cache is not reproduced as a host content cache in this path.

The host VFS exposes only the underlying `game:`, `d:`, and optional `update:` roots; guest RAGE code establishes logical mounts such as `common:`, `platform:`, and `audio:` above that layer. One narrow host exception is button-prompt substitution: when prepared, the runtime mounts a shadow archive directory and redirects `game:\xbox360.rpf` and `d:\xbox360.rpf` (`gta4_app.cpp:280-284`; `rpf_button_prompts.cpp:572-587`). This is an archive overlay, not a second general asset streamer.

### 12.3 Reads complete on the calling host path

`XFile::ReadInternal` validates the guest destination range, translates virtual or physical guest memory, and calls the host file’s `ReadSync` directly. When the destination is guest physical memory it bypasses protected host-page callbacks during the write, then triggers the physical-heap callbacks after the bytes are safely present. This is crucial because streamed texture and vertex data may be written directly into GPU-visible guest memory.

`ReadScatter` divides the destination into 4 KiB segments and repeatedly calls `ReadInternal`, then sends I/O completion notification and signals the file event. Evidence: `src/system/xfile.cpp:135-221,224-283`.

At the Xbox kernel ABI, `NtReadFile` and `NtReadFileScatter` currently enter an unconditional `if (true || file->is_synchronous())` branch. Thus even handles the guest treats as asynchronous are serviced immediately on the current host thread. The runtime still writes the I/O status block, signals the event, queues the requested APC, and returns `STATUS_PENDING` for a guest-asynchronous handle, preserving much of the observable completion protocol while not preserving retail I/O latency or overlap. The unreachable asynchronous branch remains a TODO. Evidence: `src/kernel/xboxkrnl/xboxkrnl_io.cpp:187-296,299-345`.

This distinction matters when interpreting behavior: the guest `pgStreamer` workers, queues, states, and callbacks still exist, but host storage access within a worker is synchronous and normally much faster and less seek-sensitive than the original DVD path.

## 13. Boundary with the GTA IV native renderer

### 13.1 The renderer is a consumer, not the asset scheduler

The native renderer does not decide which world models or texture dictionaries should load, does not traverse streaming dependencies, and does not update the 24-byte streaming records. The guest title manager remains the authority. The renderer becomes involved when GTA IV binds, locks/unlocks, invalidates, or releases GPU resources, and when a draw causes their current bytes to be snapshotted.

Hooks in `gta4-recomp/src/gta4_native_hooks.cpp` submit commands for vertex streams, index buffers, textures, texture invalidation/lock, resolves, and resource release. `RetireNativeBoundResource` preserves the live title resource fence when replacing a binding. `D3DResource_Release` first runs the original guest implementation and submits a renderer release only when the return value shows that the guest reference count reached zero and destruction occurred (`gta4_native_hooks.cpp:2033-2041,3158-3172`). This lifecycle bridge prevents a host cache entry from silently outliving or ignoring mutation of its guest resource.

### 13.2 Lazy buffer capture

Binding commands only update the native pipeline’s bound handles, offsets, and strides. During draw-command validation—before the command enters the dedicated render-worker queue—the renderer copies the complete guest device snapshot and captures every bound vertex buffer, index buffer, and texture (`graphics_system.cpp:2115-2199`). Pending draws therefore own stable host snapshots even if the game subsequently unloads or reuses the guest memory.

For a vertex or index buffer, `CaptureBufferResource`:

1. translates the guest D3D resource object;
2. reads and endian-corrects its flags, data address, and size;
3. translates the guest payload;
4. copies the full payload into a host-owned snapshot;
5. hashes it with XXH3;
6. reuses an existing snapshot only when metadata, hash, and payload are compatible.

Evidence: `src/graphics/gta4_native/graphics_system.cpp:2277-2365`.

This is a renderer snapshot/cache. It is downstream of asset residency and should not be counted as a second implementation of GTA IV’s streaming policy. Physical-memory callbacks triggered by `XFile` exist in the generic runtime, but the active `gta4-native` directory does not register one; its resource coherence comes from these explicit hooks plus draw-time copy/hash validation.

Persistent logical buffer snapshots live in `buffer_resources_`, but vertex and index conversion/upload allocations are held in frame-local `NativeFrameResources` (`graphics_system.cpp:8209-8274,13035-13063`). They are renderer work products, not persistent guest asset residency.

### 13.3 Texture capture, untiling, and endian conversion

`CaptureTextureResource` decodes the Xbox 360 texture-fetch descriptor, validates the dimension, format, base address, mip count and extents, and walks each mip/layer. For every subresource it obtains the Xenos guest layout, translates the guest physical memory, untile-addresses tiled blocks, applies the appropriate endian conversion, and appends a host-linear payload. It records per-mip copy geometry and hashes the result. A compatible GPU-produced texture may follow a render-target lineage rather than a CPU payload upload.

Evidence begins at `graphics_system.cpp:2366`; the produced resource structures are declared in `graphics_system.h:168-260`. The exact-layout and payload checks are important: a reused handle alone is not considered proof that bytes are unchanged.

### 13.4 Vulkan materialization

`GetOrCreateTextureImage` is keyed by the captured resource generation. It maps the Xenos format to a Vulkan format, creates a device-local image/view, allocates upload memory for CPU-originated resources, copies the host-linear payload into the upload buffer, emits one `VkBufferImageCopy` per mip/layer group, executes `vkCmdCopyBufferToImage`, and transitions the image to shader-read layout.

Evidence: `graphics_system.cpp:6354-6648`, especially upload and copy at `6548-6619`.

This materialization is lazy: a streamed texture can be resident in guest memory before a draw causes a host Vulkan image to exist. Conversely, a Vulkan image may remain cached briefly after the guest handle stops appearing in current draw commands.

### 13.5 Renderer-side eviction

`ReleaseUnusedTextureImages` constructs the set of generations referenced by logical texture resources and the current frame, records last-use frames, and destroys Vulkan images that exceed `kNativeTextureEvictionGraceFrames`. The current grace constant is 240 frames (`native_frame_scheduling.h:11`; eviction at `graphics_system.cpp:7192-7239`).

Invalidation first marks a logical texture handle dirty; a release removes matching logical texture/buffer entries during command validation. Already queued/current draws retain shared ownership of the captured generation, so logical removal does not invalidate a pending draw. Only after a generation is absent from both logical maps and current-frame references does the Vulkan grace test apply (`graphics_system.cpp:1989-2031,7192-7250`).

This 240-frame rule is **not** GTA IV’s asset eviction rule. It governs only redundant host Vulkan materializations. The guest streaming manager may unload an asset for memory/dependency reasons, the release/invalidation hook retires the logical renderer view, and eventual Vulkan destruction is a third lifetime boundary.

## 14. Runtime trace findings

The active `gta4_physics_trace.cpp` instruments central state changes, completion, and unload calls. A Python analysis of the 21 rotated `Liberty Recompiled_576*.log` files in the current macOS release bundle found:

| Observation | Count/value |
|---|---:|
| Matching streaming trace lines | 72,901 |
| `stream-unload` calls | 36,442 |
| `stream-unload-done` results | 36,442 |
| Unique indices seen by unload tracing | 1,052 |
| Unload result `0` | 4,536 |
| Unload result `1` | 31,906 |
| Sampled `stream-state` lines | 16 |
| Sampled `stream-complete` lines | 1 |
| Tick range | 4,912–5,276 (span 364) |
| Matching-line timestamp range | 2026-08-22 14:41:03.217–14:41:27.323 |

Sampled state edges included six unloaded-to-requested transitions, one loaded-to-unloaded transition, and nine requested-to-unloaded transitions. The single sampled completion was entry 36,957. Unload tracing covered indices 27 through 47,965, not a contiguous set. All matching streaming lines carried emulated thread identifier 7,262,416.

These counts must be interpreted narrowly. The instrumentation rate-limits state/completion logging, so 16 state lines do not mean only 16 transitions occurred. The very large unload-call count includes eligibility attempts; the meaning of return values `0` and `1` should not be assigned without a proven caller contract. One emulated thread identifier in this capture describes this run rather than proving the retail scheduler used only one hardware thread.

The same run confirms the active host path: `Liberty Recompiled_577.log:9` records the `gta4-native` plugin load and line 81 records the host game root mounted at `\Device\Harddisk0\Partition1`. A renderer diagnostic at `Liberty Recompiled_576.14.log:390` reports frame 5,880 with 24,925 requested/captured/bound texture bindings and 59,638 live native texture-image generations. The latter is `native_texture_images_.size()` (`graphics_system.cpp:17353-17372`), not a GTA streaming-entry count, unique asset count, or statement of current guest residency.

The trace nevertheless confirms that the recompiled retail manager is active, its compact global indices span tens of thousands of entries, request cancellation/unload edges occur, and completion reaches the instrumented publication path.

## 15. End-to-end lifecycle of one resource

The most defensible complete lifecycle is:

1. **Demand:** a scene, script, or owning asset requests a global index, with flags/priority.
2. **Graph expansion:** `sub_825120E8` invokes the type dependency callback and recursively acquires required children.
3. **Queueing:** the entry is linked into an intrusive request list and becomes requested.
4. **Selection:** `sub_825132D0` considers list order, flags, dependency state, and memory feasibility; it may request evictions.
5. **Slot reservation:** `sub_82679B98` finds one of eight free title slots; `sub_826793F0` calculates/reserves the required raw or RSC memory.
6. **Synthetic open:** the loader formats `stream:\<index>`; `fiStreamingDevice` resolves that identity to backing device/archive metadata.
7. **Low-level enqueue:** `pgStreamer::Open` allocates a handle and `sub_8284C888` enqueues the bulk/scatter request on a worker ring.
8. **Read:** the worker reads in chunks of at most 32 KiB, from `fiCachedDevice` when a valid HDD cache exists or from the source/packfile device otherwise, directly into final guest destinations.
9. **I/O notification:** events/callbacks mark the slot ready for title completion.
10. **Construction:** `sub_82679140` closes the stream handle and calls the type-specific fixup/constructor over the resident bytes.
11. **Publication:** the type-specific publication callback registers the object; the registry becomes loaded; dependents can resolve its address.
12. **Consumption:** gameplay systems use the object. If it is a GPU resource, native-renderer hooks later capture, convert, hash, and materialize it in Vulkan when a draw binds it.
13. **Release:** as scene/owner demand disappears, reference/dependency counts fall. `sub_82513A10` calls the type destructor/unpublish callback, adjusts both memory accounting and dependencies, unlinks the entry, and sets it unloaded.
14. **Host cache retirement:** guest release or mutation commands invalidate renderer snapshots; otherwise unused Vulkan images age out after the renderer-only grace window.

## 16. Function atlas

Names in the “role” column are research labels, not recovered retail symbols unless explicitly stated.

| Address | Retail/generated function | Established role |
|---:|---|---|
| `0x821CFD10` | `sub_821CFD10` | Parse `platform:/stream.ini` virtual/physical budget keys. |
| `0x822D30B8` | `sub_822D30B8` | Initialize manager, title loader, allocation callback, and disk-cache layer. |
| `0x822D3190` | `sub_822D3190` | Size/initialize the global entry database. |
| `0x82511890` | `sub_82511890` | Write entry state; coordinate dependent/reference transitions. |
| `0x82511A10` | `sub_82511A10` | Resolve a loaded entry address through packed store/type metadata. |
| `0x82511A80` | `sub_82511A80` | Convert lower entry size/data field to 2 KiB-rounded units. |
| `0x825120E8` | `sub_825120E8` | Central request, dependency acquisition, list insertion, counters. |
| `0x82512DC0` | `sub_82512DC0` | Initialize/mount `fiStreamingDevice` at `stream:\`. |
| `0x82512E70`–`0x82513250` | recovered vfuncs | `fiStreamingDevice` file-device operations. |
| `0x825132A0` | `sub_825132A0` | Manager construction; two 50 MiB defaults; streaming-device init. |
| `0x825132D0` | `sub_825132D0` | Select an issueable request; consider dependencies/space/unloads. |
| `0x82513A10` | `sub_82513A10` | Central unload/destruction/accounting path. |
| `0x82678D38` | `sub_82678D38` | Format `stream:\%u` and call `pgStreamer::Open`. |
| `0x82678EB0` | `sub_82678EB0` | Initialize eight title load slots. |
| `0x82679140` | `sub_82679140` | Complete, construct/fix up, publish, or roll back a load. |
| `0x826793F0` | `sub_826793F0` | Calculate/reserve raw or RSC memory for a slot. |
| `0x82679610` | `sub_82679610` | Issue one title asset load to `pgStreamer`. |
| `0x82679908` | `sub_82679908` | Advance an individual title slot. |
| `0x826799E0` | `sub_826799E0` | Wait/drain title completions. |
| `0x82679A90` | `sub_82679A90` | Time-budgeted completion pump. |
| `0x82679B98` | `sub_82679B98` | Fill free title slots from manager requests. |
| `0x8284BF50` | `sub_8284BF50` | `pgStreamer` worker core and chunk/scatter reads. |
| `0x8284C540` | `sub_8284C540` | Bounded worker-ring enqueue. |
| `0x8284C5E8` | `sub_8284C5E8` | Worker wait/dequeue loop. |
| `0x8284C888` | `sub_8284C888` | Submit a request to a `pgStreamer` worker. |
| `0x8284CB80` | `sub_8284CB80` | `pgStreamer::Open`. |
| `0x8284CE38` | `sub_8284CE38` | `pgStreamer::Close`. |
| `0x8284CFD8` | `sub_8284CFD8` | Initialize handles, masks, worker threads, and queues. |
| `0x8284D440` | `sub_8284D440` | Probe/validate cache partitions. |
| `0x8284D730` | `sub_8284D730` | Disk-cache copy worker. |
| `0x8284DA58` | `sub_8284DA58` | Start `[RAGE] DiskCache` thread. |
| `0x8284DAD8` | `sub_8284DAD8` | Validate cache partitions and initialize caching policy/wrapper. |
| `0x8286CB30` | `sub_8286CB30` | Acquire/allocate and schedule `pgStreamable` data. |
| `0x8286CF98` | recovered scheduler vfunc | Select/flip a `pgBasicScheduler` staging buffer. |

Generated bodies are in `glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.33.cpp` (registry/manager), `.46.cpp` (title loader), and `.61.cpp` (`pgStreamer`/disk cache). Address registration is independently listed in `generated/gta4_register.cpp:12531-12570,18914-18929,27716-27750`.

## 17. Known unknowns

The evidence supports the architecture above, but not these stronger claims:

- Exact retail class/member names for the central manager and every offset in its object.
- A complete map from the one-byte type index to every GTA IV asset family.
- Formal names and ordering guarantees for every intrusive list and request flag.
- The exact priority scoring formula used by all scene/camera producers.
- The full streaming-memory defragmentation movement protocol, including all GPU/physics synchronization.
- The configured retail `pgStreamer` worker count for every build/profile; the initializer accepts a count, but a universal value is not established here.
- Exact install/cache population policy for every Xbox 360 SKU and title update.
- The semantic contract of unload return values observed in the current trace.
- Retail wall-clock overlap/latency characteristics, which current LibertyRecomp synchronous host reads cannot reproduce.

These are intentionally preserved as unknowns instead of being filled with terminology from GTA V/FiveM or assumptions based on modern engines.

## 18. Primary evidence index

### Local retail and generated evidence

- `gta_iv/xex_excavation_retail/default_decrypted.xex.asm` — retail PPC assembly and string cross-references.
- `gta_iv/xex_excavation_retail/call_graph.txt` — retail function-call graph.
- `gta_iv/xex_excavation_retail/flirt_labeled_trusted.txt` — trusted library matches, including Xbox streaming-memory copy helpers.
- `gta_iv/xex_excavation_retail/all_strings_with_addrs.txt` and `string_xrefs.txt` — mounted devices, streaming natives, `pgStreamer`, cache, and fragmentation diagnostics.
- `gta_iv/xex_excavation_retail/rtti_classes.txt`, `classes_with_methods.txt`, and `function_class_map.txt` — `fiPackfile`, `fiCachedDevice`, and `fiStreamingDevice` recovery.
- `glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.33.cpp` — central registry/request/unload code.
- `glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.46.cpp` — eight-slot title loader.
- `glue/rexglue-sdk-main/gta4-recomp/generated/gta4_recomp.61.cpp` — `pgStreamer` and disk-cache code.
- `glue/rexglue-sdk-main/gta4-recomp/src/gta4_physics_trace.cpp` — active state/index instrumentation.
- `glue/rexglue-sdk-main/src/system/runtime.cpp`, `xfile.cpp`, and `src/kernel/xboxkrnl/xboxkrnl_io.cpp` — current VFS and host-read semantics.
- `glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp` and `src/graphics/gta4_native/graphics_system.cpp` — active native-renderer bridge and resource materialization.

### External primary and source-level corroboration

- Microsoft/IEEE: [Xbox 360 System Architecture](https://doi.org/10.1109/MM.2006.45).
- Microsoft: [Xbox 360 System Architecture Hot Chips presentation](https://www.cs.wustl.edu/~roger/569M/HC17.S8T4.pdf).
- Microsoft: [Xbox 360 launch pricing and SKU announcement](https://news.microsoft.com/source/2005/08/17/microsoft-announces-xbox-360-price-for-europe-and-north-america-starts-at-299-99-u-s-e299-99209-99/).
- Microsoft: [Optimizing DVD Performance for Windows Games](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/optimizing-dvd-performance-for-windows-games).
- Rockstar North developer interview: [Inside Rockstar North, Part 3: The Tech](https://mcvuk.com/development-news/inside-rockstar-north-part-3-the-tech/).
- CitizenFX source: [`VFSRagePackfile.cpp`](https://github.com/citizenfx/fivem/blob/master/code/components/vfs-core/src/VFSRagePackfile.cpp).
- CitizenFX source: [`pgBase.cpp` RSC5 reconstruction](https://github.com/citizenfx/fivem/blob/master/code/components/rage-formats-x/src/pgBase.cpp).
- CitizenFX later-RAGE comparison: [`Streaming.h`](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/include/Streaming.h).
- Swage source documentation: [RAGE RPF formats](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rpf.md).

The later CitizenFX streaming header is useful only as corroboration for enduring RAGE concepts—compact streaming entries, dependencies, request lists, and a `pgStreamer` boundary. It is not used to assign unverified GTA IV field layouts or behavior.
