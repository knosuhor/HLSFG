# Phase 1 design: LSFG backend (D3D12-native, bring-your-own LS)

Status: in progress (research + scaffold). This document records the measured
facts the implementation is built on and the architecture decision, which
changed after inspecting the real LS 3.2.1 binary.

## What Lossless Scaling 3.2.1 actually ships (measured, 2026-11-23)

`Lossless.dll` (v3.2.1.0, Steam install) packs **202 RT_RCDATA resources**,
ids 101-302, and every one of them is a **DXBC container** (magic `DXBC`),
compiled by the "Microsoft (R) HLSL Shader Compiler 10.1" toolchain.

This contradicts the phase plan assumption (docs/PHASES.md) inherited from
lsfg-vk: the LS 3.1-era resources were SPIR-V; **LS 3.2.x switched to DXBC**.
The RDEF strings of the blobs name the bindings of the frame generation
shader family, and the family is identical to lsfg-vk's shader ids:

| ids (raw = lsfg-vk id) | role | evidence from RDEF names |
|---|---|---|
| 255 | mipmaps | `Input1 -> Output0..Output6` (7 mip outputs, matches lsfg-vk SHADER(255, 1 storage in, 7 storage out)) |
| 256 | generate | `Sampler, Sampler1, Input1..5 -> Output1` + constants (5 sampled, 1 storage, 2 samplers + cb: exact lsfg-vk generate signature) |
| 267-270 | alpha | 1->2, 2->2, 2->4, 4->4 outputs (lsfg-vk quality/perf alpha chains) |
| 275-279 | beta | ..., 279: `2 inputs -> OutputMip0..5` (6 storage outputs; beta1) |
| 257-262 | gamma | 258: 7 sampled; 259/263: 3->4; 260/261/264/265: 4->4; 262/266: 6->1 + 2 samplers |
| 271-274 | delta tail | 2->2 pass pairs + 274: 3->1 |

Additional constants visible in RDEF: `InputOffset` (uint2) and
`InputInvDimension` (float2) buffers per shader, `const0/1` etc.
Resources 243-246 (`in_texture, coef_scale, coef_uwm, kDetectRatio...`)
are the FSR-style scaling shaders, not FG; 162-241 belong to the older
optical-flow FG surface ("InputFrame1/2, InputFlow, InputMask, InputUiMask").

## Architecture decision

**D3D12-native compute. No Vulkan, no DXBC->SPIR-V translation, no
D3D12<->VK interop.** The LSFG shaders are SM5 DXBC compute shaders and
D3D12 consumes them directly:

1. At LSFG backend init, read Lossless.dll from the user's Steam install
   (`ExtractRcData`, `src/lsfg/dll_extract.*`), never ship the resources.
2. Create `ID3D12PipelineState` objects for the FG family (255-279) from
   the raw blobs; root signatures are generated from DXBC reflection
   (registers come from `D3DReflect` via d3dcompiler_47, loaded on demand;
   SM5 => single space 0 everywhere, `b#` cbuffer, `t#` SRV, `u#` UAV,
   `s#` sampler).
3. Run the lsfg-vk execution graph on the provider's own command list with
   the same semantics (below), writing outputs into DLSSG.Output*.

The execution graph, constants and extents are carried over from
lsfg-vk **v2.0.0-dev (GPL-3.0-or-later)**, the last GPL-licensed release
(GitHub tag `v2.0.0-dev`): mip pyramid over the source pair at
`flowExtent = source >> flowScale`, per-level alpha feature passes,
beta accumulation prepass, then per generated frame: gamma refinement x7,
delta x3 (first 3 passes reuse black image), generate with
timestamp = (i+1)/(N+1) and direction alternating per real-frame parity.

## Burst-mode mapping (the measured contract, docs/CONTRACT.md)

OptiScaler calls Evaluate once per real frame with the whole output group.
LSFG interpolates between two real frames, so the backend generates the
group for the pair `(previous, current)` source frames and presents:

- slot 0 (`DLSSG.OutputReal`): copy of the current real frame
- slots 1..N-1 (`DLSSG.OutputInterpolated`): LSFG frames of the previous pair

That is exactly Lossless Scaling's own latency model (one real frame
ahead): at the moment the group must be filled, the current frame is the newest
one that exists. First frame after init falls back to a Blend-style copy.

Synchronous first implementation: copy in, run the graph, wait, copy out,
all inside Evaluate. Pipelining (fences, keep the previous pair in flight)
is a later optimization; correctness first.

## Licensing

- We ship **no Lossless Scaling resources**. Lossless.dll is read at runtime
  from the user's own installation (default: Steam library path; also
  configurable via ini). Same model as lsfg-vk.
- The execution-graph reference is lsfg-vk at tag `v2.0.0-dev`
  (GPL-3.0-or-later, SPDX headers in every file). Our project is
  GPL-3.0-or-later, so porting that orchestration is permitted with
  attribution. **Current lsfg-vk (lsfg-vk.dev, post-migration 2.0) is
  CC BY-NC-ND 4.0:** that code must NOT be copied from; the archived
  GitHub tag is the only source used.
- RT_RCDATA extraction re-implemented from the PE file format spec
  (`src/lsfg/dll_extract.cpp`), independent of lsfg-vk's GPL parser.
- Shaders stay LS's proprietary data: we only ever load them in-process
  from the user's install, we do not redistribute, extract to the repo, or
  modify them.

## Tools

- `tools/lsfg_probe.cpp` (built with `-DMFG_BUILD_TOOLS=ON`): extracts all
  resources from a Lossless.dll, reflects each shader (bindings, cbuffer
  layouts, thread group size from the disassembly), writes
  `lsfg-report/report.txt` + `.cso` + `.asm` per id. The report is the
  authoritative binding table for the root-signature generator above.

## Open items / next steps

1. Run lsfg_probe against 3.2.1, confirm thread groups + registers for
   the FG family (255-279).
2. Implement `src/lsfg/pipeline.{h,cpp}`: D3D12 device-side graph
   (transient state, UAV barriers, cbuffer upload) following the report.
3. `src/lsfg_backend.{h,cpp}` as IFGBackend (Backend=2 in the plugin tab).
4. Sizing decisions: intermediate formats (RGBA16F vs RGBA32F) per
   reflection return types; R8_UNORM for the mip chain per lsfg-vk.
5. A/B against Blend for pacing; verify the UI flicker case over
   interpolated slots (OptiScaler UIComposition, see docs/CONTRACT.md).
