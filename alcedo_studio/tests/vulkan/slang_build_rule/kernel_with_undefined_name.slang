// A kernel with an undefined name on line 7, for VulkanSlangBuildRuleTest: the build must stop
// with this file and this line.

[shader("compute")]
[numthreads(64, 1, 1)]
void kernel_with_undefined_name(uint3 id: SV_DispatchThreadID, uniform uint* output) {
  output[id.x] = undefined_value;
}
