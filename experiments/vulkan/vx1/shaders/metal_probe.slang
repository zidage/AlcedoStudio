// VX1 probe: which pointer forms does the Metal target of Slang accept?
struct Record {
  uint  count;
  float values[48];
};

struct Push {
  Record* records;
  float*  values;
  float*  out;
};
[[vk::push_constant]] Push push;

[shader("compute")]
[numthreads(64, 1, 1)]
void metal_probe(uint3 id: SV_DispatchThreadID) {
  float*  row    = push.values + 16u;           // pointer arithmetic on float*
  Record* second = push.records + 1;            // pointer arithmetic on a struct pointer
  float   v      = row[id.x % 16u] + second->values[id.x % 48u] + push.records->values[0];
#if defined(PROBE_ADDRESS_OF)
  float* element = &push.records->values[3];    // address of an element
  v += element[1];
#endif
  push.out[id.x] = v;
}
