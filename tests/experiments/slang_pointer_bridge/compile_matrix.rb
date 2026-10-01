require 'json'
require 'open3'
require 'shellwords'

artifacts = File.expand_path(ARGV.fetch(0))
compiler = ENV.fetch('SLANGC', 'slangc')
cases = JSON.parse(File.read(File.join(artifacts, 'manifest.json')))
cases << {'name' => 'all-helpers-half', 'source' => 'all-helpers', 'options' => ['-DVULKAN_FLOAT16=1']}
cases << {'name' => 'module-import', 'path' => File.join(__dir__, 'module_import.glsl'),
    'options' => ['-I', __dir__]}
commands = [['version', [compiler, '-version']]]
commands << ['original-public-list', [compiler, File.join(artifacts, 'original-public-list.glsl'),
    '-lang', 'glsl', '-entry', 'main', '-stage', 'compute', '-target', 'cuda',
    '-o', File.join(artifacts, 'original-public-list.cu')]]
cases.each do |entry|
    name = entry.fetch('name')
    path = entry['path'] || File.join(artifacts, entry.fetch('source', name) + '.glsl')
    commands << [name, [compiler, path, '-lang', 'glsl', '-entry', 'main', '-stage',
        'compute', '-target', 'cuda', '-reflection-json', File.join(artifacts, name + '.json'),
        '-o', File.join(artifacts, name + '.cu')] + entry.fetch('options', [])]
end
commands << ['ptx-probe', [compiler, File.join(artifacts, 'all-helpers.glsl'),
    '-DVULKAN_FLOAT16=1', '-lang', 'glsl', '-entry', 'main', '-stage', 'compute',
    '-target', 'ptx', '-o', File.join(artifacts, 'all-helpers.ptx')]]
results = []
commands.each do |name, command|
    puts "\n#{name}: #{command.shelljoin}"
    $stdout.flush
    status = nil
    File.open(File.join(artifacts, name + '.diagnostics.txt'), 'w') do |log|
        Open3.popen2e(*command) do |input, output, process|
            input.close
            output.each do |line|
                $stdout.write(line)
                $stdout.flush
                log.write(line)
            end
            status = process.value.exitstatus
        end
    end
    puts "Compiler exit status: #{status}"
    results << {'name' => name, 'command' => command, 'status' => status}
end
File.write(File.join(artifacts, 'results.json'), JSON.pretty_generate(results) + "\n")
cases.each do |entry|
    name = entry.fetch('name')
    result = results.find { |item| item['name'] == name }
    abort "CUDA source emission failed: #{name}" unless result['status'] == 0
    reflection = JSON.parse(File.read(File.join(artifacts, name + '.json')))
    abort "Global parameter state remains: #{name}" unless reflection.fetch('parameters').empty?
    entrypoints = reflection.fetch('entryPoints')
    abort "Unexpected entry count: #{name}" unless entrypoints.size == 1
    entrypoint = entrypoints[0]
    abort "Unexpected workgroup: #{name}" unless entrypoint['threadGroupSize'] == [16, 4, 1]
    parameters = entrypoint.fetch('parameters')
    abort "Unexpected parameter count: #{name}" unless parameters.size == 1
    binding = parameters[0].fetch('binding')
    abort "Push ABI size changed: #{name}" unless binding['kind'] == 'uniform' && binding['size'] == 16
    fields = parameters[0].fetch('type').fetch('fields')
    abort "Push ABI fields changed: #{name}" unless fields.size == 2
    ['vulkan_table_address', 'vulkan_pool_address'].each_with_index do |field, index|
        value = fields[index]
        abort "Push ABI field changed: #{name}" unless value['name'] == field &&
            value['type']['scalarType'] == 'uint64' && value['binding']['offset'] == index * 8 &&
            value['binding']['size'] == 8
    end
    cuda = File.read(File.join(artifacts, name + '.cu'))
    abort "Module-global launch parameters remain: #{name}" if cuda.include?('SLANG_globalParams')
    abort "Entry parameter is not passed by value: #{name}" unless
        cuda.match?(/__global__ void main_\d+\(AlligatorPush_\d+ parameters_\d+\)/)
end
core = File.read(File.join(artifacts, 'native_core.glsl'))
helpers = core.scan(/^\w+\s+(\w+)\(/).flatten
half_cuda = File.read(File.join(artifacts, 'all-helpers-half.cu'))
missing = helpers.reject { |name| half_cuda.match?(/\b#{Regexp.escape(name)}_\d+/) }
abort "Helpers were not emitted: #{missing.join(', ')}" unless missing.empty?
puts "\nCUDA emission and reflected per-dispatch ABI passed for #{cases.size} cases and #{helpers.size} reachable helpers."
ptx = results.find { |item| item['name'] == 'ptx-probe' }
puts "PTX probe exit status: #{ptx['status']}; NVIDIA execution is a separate required qualification."
