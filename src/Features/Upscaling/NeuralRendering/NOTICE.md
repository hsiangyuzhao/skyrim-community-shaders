# Provenance

`Runtime`, `D3D12Interop` and `Renderer` in this directory, together with
`features/Upscaling/Shaders/Upscaling/NeuralRendering/CopyDepthGuideCS.hlsl`, are ported from

> https://github.com/YtzyFvra/skyrim-community-shaders
> branch `feature/dlssnr-vr`
> `src/Features/Upscaling/NeuralRendering/`

which is an experimental DLSS Neural Rendering integration built on the Open Shaders dev branch.
Open Shaders and this tree are both forks of Community Shaders and share its licence, so the code
carries over unchanged in that respect. It is not an official release of Open Shaders or
Community Shaders, and their maintainers do not support it.

## Changes made during the port

- `Renderer.cpp`
  - dropped `#include "GpuPass.h"`, which was included but never referenced
  - `Util::LazyShader<ID3D11ComputeShader>` has no counterpart here, so it is replaced by a
    local `LazyComputeShader` with the same contract expressed through `Util::CompileShader`
  - `CS_GPU_PASS(...)` replaced by `Util::CpuPassScope`, this tree's scoped pass marker
- `Runtime`, `D3D12Interop`, `CopyDepthGuideCS.hlsl` are unmodified
- `Integration` is **not** ported. The upstream version is built around Open Shaders' foveated
  rendering and HDR display features, neither of which exists here, so this tree has its own,
  which places the pass in `Upscaling::MenuManagerDrawInterfaceStartHook` and gates it on this
  tree's own upscaler and frame-generation state.

## What is deliberately absent

`nvngx_dlssnr.dll` is not shipped and must not be. NVIDIA publishes no build of it: their
own runtime reached the public only inside a game's early-access files, and it contains CUDA
kernels for Blackwell alone, so running it on anything older needs a community recompile.
Neither is ours to redistribute, and the community builds carry no NVIDIA signature, so the
provenance check that applies to every other binary in this package cannot be applied to them.

The feature therefore looks for the runtime at
`Data/Shaders/Upscaling/Streamline/nvngx_dlssnr.dll`, reports plainly when it is absent, and
does nothing.
