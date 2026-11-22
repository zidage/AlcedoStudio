// VX1 probe: do the shared headers compile as Slang?
#include "color/color_encoding_math.h"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/film/film_print_math.h"

struct Push {
  FilmKernelParams* params;
  float*            values;
  float*            out;
};
[[vk::push_constant]] Push push;

[shader("compute")]
[numthreads(64, 1, 1)]
void probe(uint3 id: SV_DispatchThreadID) {
  const float      v = CeAcesccEncode(float(id.x) * 0.01f);
  const AcesRgcRgb c = AcesReferenceGamutCompress(v, 0.5f * v, -0.1f);
  const FilmKernelRgb q = FilmKernelLayerExposure(push.params, push.values, c.r, c.g, c.b);
  push.out[id.x] = q.r + q.g + q.b;
}
