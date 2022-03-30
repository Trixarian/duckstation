// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_sw_rasterizer.h"
#include "gpu.h"

#include "common/assert.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/string_util.h"

#include "cpuinfo.h"

Log_SetChannel(GPU_SW_Rasterizer);

namespace GPU_SW_Rasterizer {
constinit const DitherLUT g_dither_lut = []() constexpr {
  DitherLUT lut = {};
  for (u32 i = 0; i < DITHER_MATRIX_SIZE; i++)
  {
    for (u32 j = 0; j < DITHER_MATRIX_SIZE; j++)
    {
      for (u32 value = 0; value < DITHER_LUT_SIZE; value++)
      {
        const s32 dithered_value = (static_cast<s32>(value) + DITHER_MATRIX[i][j]) >> 3;
        lut[i][j][value] = static_cast<u8>((dithered_value < 0) ? 0 : ((dithered_value > 31) ? 31 : dithered_value));
      }
    }
  }
  return lut;
}();

Common::Rectangle<u32> g_drawing_area = {};
} // namespace GPU_SW_Rasterizer

// Default implementation definitions.
namespace GPU_SW_Rasterizer {
#include "gpu_sw_rasterizer.inl"
}

// Initialize with default implementation.
namespace GPU_SW_Rasterizer {
const DrawRectangleFunctionTable* SelectedDrawRectangleFunctions = &DrawRectangleFunctions;
const DrawTriangleFunctionTable* SelectedDrawTriangleFunctions = &DrawTriangleFunctions;
const DrawLineFunctionTable* SelectedDrawLineFunctions = &DrawLineFunctions;
} // namespace GPU_SW_Rasterizer

// Declare alternative implementations.
void GPU_SW_Rasterizer::SelectImplementation()
{
  static bool selected = false;
  if (selected)
    return;

  selected = true;

#define SELECT_ALTERNATIVE_RASTERIZER(isa)                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    Log_InfoPrint("* Using " #isa " software rasterizer implementation.");                                             \
    SelectedDrawRectangleFunctions = &isa::DrawRectangleFunctions;                                                     \
    SelectedDrawTriangleFunctions = &isa::DrawTriangleFunctions;                                                       \
    SelectedDrawLineFunctions = &isa::DrawLineFunctions;                                                               \
  } while (0)

#if defined(CPU_ARCH_SSE)
  const char* use_isa = std::getenv("SW_USE_ISA");

  if (!cpuinfo_initialize())
  {
    Log_ErrorPrint("cpuinfo_initialize() failed, using default implementation");
    return;
  }

  if (cpuinfo_has_x86_avx2() && (!use_isa || StringUtil::Strcasecmp(use_isa, "AVX2") == 0))
  {
    SELECT_ALTERNATIVE_RASTERIZER(AVX2);
    return;
  }

  if (cpuinfo_has_x86_sse4_1() && (!use_isa || StringUtil::Strcasecmp(use_isa, "SSE4") == 0))
  {
    SELECT_ALTERNATIVE_RASTERIZER(SSE4);
    return;
  }
#endif

  Log_InfoPrint("* Using default software rasterizer implementation.");

#undef SELECT_ALTERNATIVE_RASTERIZER
}
