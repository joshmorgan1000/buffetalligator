require 'fileutils'
require 'digest'
require 'json'

abort 'Usage: ruby prepare_slice_variants.rb OUTPUT_DIRECTORY' unless ARGV.length == 1
root = File.expand_path('../../..', __dir__)
source_path = File.join(root, 'src/memory/slice.cpp')
source = File.binread(source_path)
output_directory = File.expand_path(ARGV.fetch(0))
original_constructor = <<~'CPP'
Slice::Slice(const Slice& other) {
    if (other.is_null()) return;
    Alligator& arena = Alligator::inst();
    id_ = arena.next_id(other.placement());
    arena.entry(*this).set(arena.entry(other).token(), static_cast<uint8_t>((id_ >> 3) & 63));
    *arena.gpubuf(*this) = *arena.gpubuf(other);
}
CPP
fused_constructor = <<~'CPP'
Slice::Slice(const Slice& other) {
    if (other.is_null()) return;
    Alligator& arena = Alligator::inst();
    id_ = arena.next_id(other.placement());
    const uint8_t source_region_index = static_cast<uint8_t>((other.id_ >> 3) & 63);
    const uint8_t destination_region_index = static_cast<uint8_t>((id_ >> 3) & 63);
    const size_t source_slot_index = other.id_ >> 9;
    const size_t destination_slot_index = id_ >> 9;
    const Region* source_region =
        arena.regions[source_region_index].load(std::memory_order_acquire);
    Region* destination_region =
        arena.regions[destination_region_index].load(std::memory_order_acquire);
    destination_region->slots[destination_slot_index].set(
        source_region->slots[source_slot_index].token(), destination_region_index
    );
    destination_region->gpu_slots[destination_slot_index] =
        source_region->gpu_slots[source_slot_index];
}
CPP
matches = source.scan(Regexp.new(Regexp.escape(original_constructor))).length
abort "Expected exactly one current Slice copy constructor, found #{matches}" unless matches == 1
variants = {
    'baseline' => source,
    'fused_metadata_clone' => source.sub(original_constructor, fused_constructor)
}
FileUtils.mkdir_p(output_directory)
manifest = {
    'source' => source_path,
    'source_sha256' => Digest::SHA256.hexdigest(source),
    'variants' => variants.map do |name, contents|
        filename = "slice_#{name}.cpp"
        File.binwrite(File.join(output_directory, filename), contents)
        {'name' => name, 'file' => filename, 'sha256' => Digest::SHA256.hexdigest(contents)}
    end
}
File.write(File.join(output_directory, 'slice_variants.json'), JSON.pretty_generate(manifest) + "\n")
warn "Prepared baseline and fused_metadata_clone Slice sources in #{output_directory}"
