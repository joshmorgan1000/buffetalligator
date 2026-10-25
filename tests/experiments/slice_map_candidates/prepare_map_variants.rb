#!/usr/bin/env ruby
# Emits isolated SliceMap candidates from the current working source.
require 'digest'
require 'fileutils'
require 'json'

def replace_exact(source, original, replacement, label)
    occurrences = source.scan(Regexp.new(Regexp.escape(original))).length
    unless occurrences == 1
        abort "#{label}: expected one source match, found #{occurrences}; inspect current source."
    end
    source.sub(original, replacement)
end
abort "Usage: ruby #{File.basename(__FILE__)} OUTPUT_DIRECTORY" unless ARGV.length == 1
repository_root = File.expand_path('../../..', __dir__)
source_path = File.join(repository_root, 'src/containers/slicemap.cpp')
output_directory = File.expand_path(ARGV.fetch(0))
source = File.binread(source_path)
original_lookup = <<~'CPP'
Slice SliceMap::get_slice_internal(int64_t identifier) const {
    MapHazard state_hazard(0), payload_hazard(1);
    State* state = state_hazard.protect(state_);
    State::Node* node = state->find(State::hash(identifier));
    if (!node) return Slice();
    State::Payload* payload = payload_hazard.protect(node->current);
    return payload->slice;
}
CPP
deferred_lookup = <<~'CPP'
Slice SliceMap::get_slice_internal(int64_t identifier) const {
    MapHazard state_hazard(0);
    State* state = state_hazard.protect(state_);
    State::Node* node = state->find(State::hash(identifier));
    if (!node) return Slice();
    MapHazard payload_hazard(1);
    State::Payload* payload = payload_hazard.protect(node->current);
    return payload->slice;
}
CPP
original_counter =
    "            positions_.fetch_sub(kInflightStep, std::memory_order_release);\n" \
    "            const size_t position = positions_.fetch_add(1, std::memory_order_relaxed)\n" \
    "                & kPopulationMask;\n"
combined_counter =
    "            const size_t position = positions_.fetch_add(size_t(1) - kInflightStep,\n" \
    "                std::memory_order_release) & kPopulationMask;\n"
variants = {
    'baseline' => source,
    'deferred_payload_hazard' => replace_exact(source, original_lookup, deferred_lookup,
        'deferred_payload_hazard'),
    'combined_publication_counter' => replace_exact(source, original_counter, combined_counter,
        'combined_publication_counter')
}
variants['map_bookkeeping'] = replace_exact(variants.fetch('deferred_payload_hazard'),
    original_counter, combined_counter, 'map_bookkeeping')
manifest = {
    'source' => source_path,
    'source_sha256' => Digest::SHA256.hexdigest(source),
    'variants' => variants.map do |name, contents|
        { 'name' => name, 'source' => "#{name}/slicemap.cpp",
          'sha256' => Digest::SHA256.hexdigest(contents) }
    end,
    'omitted_variants' => {
        'direct_publication_node' => 'Requires changing the private declaration in the public header.'
    }
}
variants.each do |name, contents|
    directory = File.join(output_directory, name)
    FileUtils.mkdir_p(directory)
    File.binwrite(File.join(directory, 'slicemap.cpp'), contents)
end
File.write(File.join(output_directory, 'manifest.json'), JSON.pretty_generate(manifest) + "\n")
puts "Prepared #{variants.length} isolated SliceMap sources in #{output_directory}."
puts "Current source SHA-256: #{manifest.fetch('source_sha256')}"
