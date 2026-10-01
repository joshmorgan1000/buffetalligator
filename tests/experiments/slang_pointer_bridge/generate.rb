require 'fileutils'
require 'digest'
require 'json'

root = File.expand_path('../../..', __dir__)
out = ARGV[0] || File.join(root, 'build/slang-pointer-bridge')
FileUtils.mkdir_p(out)
header = File.read(File.join(root, 'include/alligator/easyvulkan.hpp'))
source = File.read(File.join(root, 'src/gpu/shader_source.cpp'))
gate = File.read(File.join(root, 'tests/experiments/metal_glsl_gate/metal_glsl_gate.mm'))
core = header[/VULKAN_GLSL_KERNEL_CORE\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
jobs = header[/VULKAN_GLSL_KERNEL_TAIL\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
public_tail = source[/PUBLIC_GLSL_TAIL\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
body = gate[/const std::string body\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
abort 'Missing current production input' unless core && jobs && public_tail && body
native = core.sub('#extension GL_EXT_buffer_reference : require', '')
native.sub!(/layout\(push_constant\) uniform AlligatorPush \{(.*?)\} vulkan_push;/m,
    "struct AlligatorPush {\\1};\nstatic AlligatorPush vulkan_push;")
native.gsub!(/layout\(buffer_reference,[^\n]*\)\s*(?:readonly\s+)?buffer\s+(\w+)\s*\{\s*([^}]+)\};/m) do
    name, fields = Regexp.last_match(1), Regexp.last_match(2).strip
    if fields =~ /(\w+)\s+(\w+)\[\];/
        type, field = Regexp.last_match(1), Regexp.last_match(2)
        "struct #{name} { #{type}* #{field}; __init(uint64_t address) { #{field} = (#{type}*)address; } };"
    elsif name == 'SliceRef' && fields == 'uint32_t id;'
        "struct #{name} { uint32_t id; __init(uint64_t address) { id = *((uint32_t*)address); } };"
    else
        abort "Unrecognized current buffer-reference fields: #{name}"
    end
end
abort 'Buffer references remain' if native.include?('layout(buffer_reference')
native.gsub!('atomicAnd(', 'InterlockedAnd(')
native.gsub!('atomicOr(', 'InterlockedOr(')
native.gsub!('unpackFloat2x16(', 'alligator_unpack_half2(')
native.sub!('f16vec4 fp8x4_to_f16x4',
    "f16vec2 alligator_unpack_half2(uint packed) { return f16vec2(unpackHalf2x16(packed)); }\nf16vec4 fp8x4_to_f16x4")
native_jobs = jobs.gsub('barrier();', 'GroupMemoryBarrierWithGroupSync();')
File.write(File.join(out, 'native_core.glsl'), native)
File.write(File.join(out, 'unchanged_body.glsl'), body)
File.write(File.join(out, 'body.sha256'), Digest::SHA256.hexdigest(body) + "\n")
manifest = []
[['public-list', public_tail, 'vulkan_count()', 'vulkan_slice(vulkan_index())'],
 ['job-table', native_jobs + "\nlayout(local_size_x=16,local_size_y=4,local_size_z=1) in;\n", 'vulkan_job_count()', 'vulkan_job(vulkan_job_index())']].each do |name, tail, count, argument|
    generated = "#version 450\n" + native + tail + "\nuint gate_count() { return #{count}; }\n" + body + "\nvoid main(uniform AlligatorPush parameters) { vulkan_push = parameters; alligator_main(#{argument}); }\n"
    File.write(File.join(out, name + '.glsl'), generated)
    manifest << {'name' => name, 'body_sha256' => Digest::SHA256.hexdigest(body)}
end
interop = File.read(File.join(root, 'tests/vulkan_interop_functional_test.cpp'))
['TYPED_SHADER', 'BOUNDARY_SHADER'].each do |constant|
    application = interop[/#{constant}\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
    abort "Missing #{constant}" unless application
    name = constant.downcase
    File.write(File.join(out, name + '.glsl'), "#version 450\n" + native + public_tail + application + "\nvoid main(uniform AlligatorPush parameters) { vulkan_push = parameters; alligator_main(vulkan_slice(vulkan_index())); }\n")
    File.write(File.join(out, name + '.body.sha256'), Digest::SHA256.hexdigest(application) + "\n")
    manifest << {'name' => name, 'body_sha256' => Digest::SHA256.hexdigest(application)}
end
l2 = header[/VULKAN_GLSL_L2_EXAMPLE\s*=\s*R"glsl\((.*?)\)glsl"/m, 1]
abort 'Missing production reduction example' unless l2
File.write(File.join(out, 'l2-reduction.glsl'), "#version 450\n" + native + native_jobs + "\nlayout(local_size_x=16,local_size_y=4,local_size_z=1) in;\n#define main alligator_application_main\n" + l2 + "\n#undef main\nvoid main(uniform AlligatorPush parameters) { vulkan_push = parameters; alligator_application_main(); }\n")
manifest << {'name' => 'l2-reduction', 'body_sha256' => Digest::SHA256.hexdigest(l2)}
helpers = File.read(File.join(__dir__, 'all_helpers.glsl'))
File.write(File.join(out, 'all-helpers.glsl'), "#version 450\n" + native + public_tail + helpers + "\nvoid main(uniform AlligatorPush parameters) { vulkan_push = parameters; alligator_main(vulkan_slice(vulkan_index())); }\n")
manifest << {'name' => 'all-helpers', 'body_sha256' => Digest::SHA256.hexdigest(helpers)}
reference_section = source[/std::string reference_shader_source\(.*?return source;/m]
reference_parts = reference_section.scan(/R"glsl\((.*?)\)glsl"/m).flatten
reference_test = File.read(File.join(root, 'tests/shader_slice_test.cpp'))
reference_body = reference_test[/static void reference_dispatch\(\).*?R"glsl\((.*?)\)glsl"/m, 1]
abort 'Missing reference ABI or application' unless reference_parts.size == 2 && reference_body
reference_entry = reference_parts[1].sub('void main() {',
    'void main(uniform AlligatorPush parameters) { vulkan_push = parameters;')
File.write(File.join(out, 'references.glsl'), "#version 450\n" + native + reference_parts[0] + reference_body + reference_entry)
manifest << {'name' => 'references', 'body_sha256' => Digest::SHA256.hexdigest(reference_body)}
File.write(File.join(out, 'original-public-list.glsl'), "#version 450\n" + core + public_tail + "\nuint gate_count() { return vulkan_count(); }\n" + body + "\nvoid main() { alligator_main(vulkan_slice(vulkan_index())); }\n")
File.write(File.join(out, 'manifest.json'), JSON.pretty_generate(manifest) + "\n")
