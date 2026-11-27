// A kernel group of two kernels that reads a value from an included header, for
// VulkanSlangBuildRuleTest: a change of the header must build the module again.

#include "fill_value.slangh"

[shader("compute")]
[numthreads(64, 1, 1)]
void fill_header_value(uint3 id: SV_DispatchThreadID, uniform uint* output) {
  output[id.x] = kFillValue;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void fill_header_value_twice(uint3 id: SV_DispatchThreadID, uniform uint* output) {
  output[id.x] = 2 * kFillValue;
}
