struct KernelPush { uint64_t table; uint64_t directory; };
struct Chomp { uint id; };
[[vk::push_constant]] ConstantBuffer<KernelPush> push;
uint64_t chomp_address(Chomp chomp) {
    uint64_t* directory = (uint64_t*)push.directory;
    uint64_t word = ((uint64_t*)directory[chomp.id >> 16])[chomp.id & 0xFFFF];
    return directory[256 + uint(word >> 48)] + (((word >> 20) & 0x3FFFFFF) << 6);
}
[numthreads(16,4,1)]
void main(uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    if (local != 0) return;
    uint* list_id = (uint*)push.table;
    Chomp list = {list_id[0]};
    uint* ids = (uint*)chomp_address(list);
    Chomp output_chomp = {ids[group.y]};
    uint64_t* destination = (uint64_t*)chomp_address(output_chomp);
    destination[0] = chomp_address(output_chomp);
}
