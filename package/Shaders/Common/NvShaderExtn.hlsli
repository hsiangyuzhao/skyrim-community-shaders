#ifndef COMMON_NV_SHADER_EXTN_HLSLI
#define COMMON_NV_SHADER_EXTN_HLSLI

// (45c) NVIDIA HLSL shader extensions (warp vote / ballot / shuffle, special registers).
//
// NV_SHADER_EXTN is only ever defined by the C++ side (Util::Nvapi::ShaderDefines), and only after
// NvAPI_D3D11_SetNvShaderExtnSlot accepted slot u63 on the game's device and the driver reported the
// opcodes the shader needs. Without it every caller compiles its portable path, so a shader built here
// never depends on the driver unless C++ asked for it.
//
// The slot must match Util::Nvapi::kShaderExtnSlot. Nothing may bind a real UAV at u63: the driver
// rewrites every access to it into the extension instruction.
#if defined(NV_SHADER_EXTN) && NV_SHADER_EXTN
#	define NV_SHADER_EXTN_SLOT u63
#	include "Common/NVAPI/nvHLSLExtns.h"
#endif

#endif  // COMMON_NV_SHADER_EXTN_HLSLI
