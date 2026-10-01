struct KernelPush { uint64_t table; uint64_t directory; };
struct GPUBufRef { uint64_t address; uint size; uint offset; };
struct Slice { uint id; };
[[vk::push_constant]] ConstantBuffer<KernelPush> push;
uint64_t slice_address(Slice slice) {
    uint64_t* directory = (uint64_t*)push.directory;
    GPUBufRef* entries = (GPUBufRef*)directory[(slice.id >> 3) & 63];
    GPUBufRef entry = entries[slice.id >> 9];
    return entry.address + (uint64_t(entry.offset) << 6);
}
[numthreads(16,4,1)]
void main(uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    if (local != 0) return;
    uint* list_id = (uint*)push.table;
    Slice list = {list_id[0]};
    uint* ids = (uint*)slice_address(list);
    Slice output_slice = {ids[group.y]};
    uint64_t* destination = (uint64_t*)slice_address(output_slice);
    destination[0] = slice_address(output_slice);
}
