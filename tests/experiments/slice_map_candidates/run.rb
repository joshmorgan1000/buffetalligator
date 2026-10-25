#!/usr/bin/env ruby
# Measures isolated candidates sequentially with rotated process order and retained-result checks.
require 'digest'
require 'fileutils'
require 'json'
require 'open3'
require 'time'

def competing_processes(excluded)
    output, status = Open3.capture2e('ps', '-axo', 'pid,comm,args')
    abort 'Cannot check competing processes; rerun with process inspection access.' unless status.success?
    output.lines.select do |line|
        next false if excluded.include?(line.split.first.to_i)
        line.match?(%r{(?:^|/)(?:clang(?:\+\+)?|clang-[0-9]+|ninja|ctest|cc1)(?:\s|$)}) ||
            line.include?('cmake --build') || line.match?(/\bbash .*run_build\.sh(?:\s|$)/) ||
            line.match?(%r{/(?:map_candidate_[a-z_]+|buffetalligator_[a-z_]*benchmark)(?:\s|$)})
    end
end
abort "Usage: ruby #{File.basename(__FILE__)} BUILD_DIRECTORY RESULTS_DIRECTORY" unless ARGV.length == 2
build_directory = File.expand_path(ARGV.fetch(0))
results_directory = File.expand_path(ARGV.fetch(1))
abort 'Results directory already exists; choose a fresh directory to preserve prior measurements.' if File.exist?(results_directory)
root = File.expand_path('../../..', __dir__)
candidates = %w[baseline deferred_payload_hazard combined_publication_counter map_bookkeeping fused_metadata_clone]
configurations = [[4096, 'single'], [65536, 'single'], [1048576, 'single'], [65536, 'threaded8p8c']]
repetitions = 15
series_count = 3
executables = candidates.to_h do |candidate|
    executable = File.join(build_directory, "map_candidate_#{candidate}")
    abort "Missing candidate executable: #{executable}" unless File.executable?(executable)
    [candidate, executable]
end
build_log = File.read(File.join(build_directory, 'build_buffetalligator.log'))
abort 'Candidate build must complete its registered tests before measurement.' unless
    build_log.include?('100% tests passed, 0 tests failed') && !build_log.include?('No tests were found')
test_log = File.read(File.join(build_directory, 'Testing/Temporary/LastTest.log'))
candidates.each do |candidate|
    %w[slice_map_test slice_map_concurrency_test slice_core_regression_test global_slice_lifetime_test growth].each do |check|
        abort "Missing candidate correctness check: #{candidate}/#{check}" unless
            test_log.include?("Test: check_#{candidate}_#{check}\n")
    end
end
inputs = %w[src/containers/slicemap.cpp src/memory/slice.cpp src/memory/alligator.cpp
    src/memory/chainbuffet.cpp include/alligator.hpp include/alligator/containers.hpp
    tests/benchmarks/slice_map_benchmark.cpp tests/benchmarks/benchmark_support.hpp]
source_hashes = inputs.to_h { |path| [path, Digest::SHA256.file(File.join(root, path)).hexdigest] }
{'manifest.json' => 'src/containers/slicemap.cpp', 'slice_variants.json' => 'src/memory/slice.cpp'}.each do |name, source|
    generated = JSON.parse(File.read(File.join(build_directory, 'map-candidates', name)))
    abort "Rebuild candidates after source changes: #{source}" unless generated.fetch('source_sha256') == source_hashes.fetch(source)
end
FileUtils.mkdir_p(results_directory)
manifest = {
    'started_at' => Time.now.iso8601,
    'platform' => RUBY_PLATFORM,
    'build_directory' => build_directory,
    'source_sha256' => source_hashes,
    'executables' => executables.transform_values { |path| {'path' => path, 'sha256' => Digest::SHA256.file(path).hexdigest} },
    'configurations' => configurations,
    'warmups' => 2,
    'repetitions' => repetitions,
    'series' => series_count,
    'measurements' => []
}
manifest_path = File.join(results_directory, 'run.json')
File.write(manifest_path, JSON.pretty_generate(manifest) + "\n")
FileUtils.cp(File.join(build_directory, 'CMakeCache.txt'), results_directory)
FileUtils.cp(File.join(build_directory, 'compile_commands.json'), results_directory)
FileUtils.cp(File.join(build_directory, 'Testing/Temporary/LastTest.log'), File.join(results_directory, 'correctness.log'))
%w[manifest.json slice_variants.json].each do |name|
    FileUtils.cp(File.join(build_directory, 'map-candidates', name), File.join(results_directory, name))
end
%w[uname sw_vers pmset].each do |command|
    arguments = {'uname' => ['-a'], 'sw_vers' => [], 'pmset' => ['-g', 'batt']}.fetch(command)
    next if RUBY_PLATFORM !~ /darwin/ && command != 'uname'
    output, status = Open3.capture2e(command, *arguments)
    File.write(File.join(results_directory, "host-#{command}.txt"), output)
    abort "Host metadata command failed: #{command}" unless status.success?
end
puts "Running #{configurations.length * candidates.length * series_count} benchmark processes sequentially."
$stdout.sync = true
series_count.times do |series_index|
    configurations.rotate(series_index).each do |items, shape|
        configuration_index = configurations.index([items, shape])
        candidates.rotate(series_index + configuration_index).each do |candidate|
            competing = competing_processes([Process.pid])
            unless competing.empty?
                File.write(File.join(results_directory, 'competing-processes.txt'), competing.join)
                abort 'Competing build/test activity detected; measurement stopped without changing those processes.'
            end
            stem = "#{candidate}__#{items}__#{shape}__#{series_index + 1}"
            csv_path = File.join(results_directory, "#{stem}.csv")
            log_path = File.join(results_directory, "#{stem}.log")
            command = [executables.fetch(candidate), '--items', items.to_s,
                '--lookups', [items, 262144].max.to_s, '--warmup', '2',
                '--repetitions', repetitions.to_s, '--timeout', '300', '--csv', csv_path]
            command.concat(shape == 'single' ? ['--single-thread'] : ['--producers', '8', '--consumers', '8'])
            puts "Starting #{stem}: identical insertion, retained hits and misses."
            started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
            process = Process.spawn({'ALLIGATOR_GPU_BACKEND' => 'cpu'}, *command, out: log_path, err: [:child, :out])
            waiter = Process.detach(process)
            elapsed = 0
            contamination = []
            until waiter.join(1)
                elapsed += 1
                puts "#{stem}: working (#{elapsed}s)."
                contamination.concat(competing_processes([Process.pid, process]))
            end
            unless waiter.value.success?
                warn File.readlines(log_path).last(30).join
                abort "Candidate failed correctness or execution: #{stem}; see #{log_path}"
            end
            unless contamination.empty?
                File.write(File.join(results_directory, 'competing-processes.txt'), contamination.uniq.join)
                abort 'Competing activity began during measurement; stopped after the current process.'
            end
            duration = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
            manifest['measurements'] << {'candidate' => candidate, 'items' => items,
                'shape' => shape, 'series' => series_index + 1, 'command' => command,
                'elapsed_seconds' => duration, 'csv' => File.basename(csv_path),
                'log' => File.basename(log_path)}
            File.write(manifest_path, JSON.pretty_generate(manifest) + "\n")
            puts "Finished #{stem} in #{duration.round(2)}s; output checks passed."
        end
    end
end
source_hashes.each do |path, expected|
    abort "Source changed during measurement: #{path}" unless Digest::SHA256.file(File.join(root, path)).hexdigest == expected
end
manifest['finished_at'] = Time.now.iso8601
File.write(manifest_path, JSON.pretty_generate(manifest) + "\n")
puts "Completed all measurements; results saved in #{results_directory}."
