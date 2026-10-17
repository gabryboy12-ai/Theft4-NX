# XeniOS direct host resolve shaders

These 14 Vulkan utility compute shaders are from
[XeniOS](https://github.com/xenios-jp/XeniOS) commit
`87b176a078c316fde3adf67a217f0c44615b0e0d`. They are public renderer code;
this package contains no game shaders or game data. The upstream BSD license is
retained in `LICENSE` and the source copyright notices are unchanged.

The Switch build uses the pre-generated headers in `include/`; no host shader
compiler is required to build the NRO. Enable
`THEFT4_XENIOS_DIRECT_RESOLVE_SHADERS` and add `include/` to the renderer's include
paths. The existing renderer also requires `THEFT4_DIRECT_RESOLVE=1` at runtime.
Unsupported formats, sample counts, scaling, or incomplete render-target
coverage continue through its existing resolve fallback. This is a performance
experiment: lower EDRAM dump counts alone do not establish an FPS improvement.

`generated-manifest.json` records every module's definitions, SHA-256, descriptor
bindings, constant offsets, source hashes, compiler source pins, and optimization
passes. The checked-in modules passed `spirv-val --target-env vulkan1.0`; all use
SPIR-V 1.0, 8×8×1 workgroups, a 44-byte uniform block, and 20-byte push constants.
`pinned-tree.json` is the immutable upstream Git tree used to verify the sources.
`source/` retains the complete transitive shader include set and the upstream
build definitions that establish the variants and compiler options. The original
manifest describes a larger fetched source set; unrelated shaders were omitted
from this small package.

To reproduce the headers with Python 3 and the pinned native tools:

```sh
python3 regenerate.py \
  --glslang /path/to/glslangValidator \
  --optimizer /path/to/spirv-opt \
  --validator /path/to/spirv-val
```

The tool commits are glslang `f4f1d8a352ca1908943aea2ad8c54b39b4879080`,
SPIRV-Tools `04d0b166dcd62e29509bf2aac3ca0c5ccdcb6929`, and SPIRV-Headers
`04f10f650d514df88b76d25e83db360142c7b174`. The generator uses Vulkan 1.0,
the GLSL extension prologue in upstream `tools/build/shader_cc.cc`, and the
upstream optimizer sequence `-O -O --canonicalize-ids`.

Optional arguments are `--source-root` for a different pinned XeniOS source
checkout, `--tree` for its pinned Git-tree JSON, and `--output` for a different
include root. `--sdk-source` points to a renderer SDK checkout containing
`thirdparty/{glslang,spirv-tools,spirv-headers}`; when supplied, the generator also
checks those Git commits against the manifest. All required source Git blobs and
SHA-256 hashes are checked regardless of this option. Every regenerated module
must pass validation, ABI checks, and match the recorded bytecode SHA-256 before
any headers are replaced. The existing manifest is never rewritten.
