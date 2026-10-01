#!/usr/bin/env ruby
# Measures the isolated flat-map matrix sequentially with provenance and process-load checks.
require 'digest'
require 'fileutils'
require 'json'
require 'open3'
require 'time'

class FlatMapRun
    REPEATS = 11
    WARMUPS = 2
    SERIES = 3
    QUIET_SECONDS = 10
    WAIT_BUDGET_SECONDS = 300
    MAX_CONTAMINATED_ATTEMPTS = 3
    def initialize(build_directory, results_directory)
        @root = File.expand_path('../../..', __dir__)
        @build = File.expand_path(build_directory)
        @results = File.expand_path(results_directory)
        @executable = File.join(@build, 'flat_slice_map_benchmark')
        @manifest = nil
        @waited_seconds = 0.0
        @contaminated_counts = Hash.new(0)
    end
    def require_value(condition, message)
        raise ArgumentError, message unless condition
    end
    def hash_file(path)
        Digest::SHA256.file(path).hexdigest
    end
    def configurations
        rows = [4096, 65536, 1048576].product(%w[sequential strided],
            %w[reserved growing]).map { |items, layout, reservation|
            [items, layout, reservation, 'single'] }
        rows + [65536, 1048576].map { |items| [items, 'sequential', 'reserved', 'readers8'] }
    end
    def competing_processes(excluded)
        output, status = Open3.capture2e('ps', '-axo', 'pid,comm,args')
        require_value(status.success?, 'Cannot inspect processes; enable process inspection and rerun.')
        output.lines.select do |line|
            fields = line.strip.split(/\s+/, 3)
            next false if fields.length < 3 || excluded.include?(fields.first.to_i)
            command = File.basename(fields[1])
            command.match?(/\A(?:clang(?:\+\+)?|gcc|g\+\+|cc1|ninja|make|gmake|ctest)(?:-[0-9.]+)?\z/) ||
                command.match?(/\A(?:map_candidate_[a-z_]+|flat_slice_map_benchmark)\z/) ||
                command.match?(/\Abuffetalligator_[a-z_]*benchmark\z/) ||
                command.match?(/\A(?:check_[a-z_]+|slice_[a-z_]+_test|flat_slice_map_test)\z/) ||
                fields[2].match?(/\bcmake\s+--build(?:\s|$)/) ||
                fields[2].match?(%r{(?:\A|\s)(?:[^\s]*/)?run_build\.sh(?:\s|$)})
        end
    end
    def check_correctness
        path = File.join(@build, 'Testing/Temporary/LastTest.log')
        text = File.read(path)
        %w[flat_slice_map_test slice_map_test].each do |name|
            expression = /^\d+\/\d+ Test: #{Regexp.escape(name)}\n(.*?)(?=^\d+\/\d+ Testing:|\z)/m
            matches = text.scan(expression)
            require_value(matches.length == 1 && matches.first.first.include?("\nTest Passed.\n"),
                "Run the registered #{name} check successfully before measurement; see #{path}.")
        end
        path
    end
    def source_inputs
        fixed = %w[CMakeLists.txt run_build.sh tests/benchmarks/benchmark_support.hpp
            tests/slice_map_test.cpp]
        patterns = %w[src/**/* include/**/* cmake/**/* tests/experiments/flat_slice_map/**/*]
        discovered = patterns.flat_map { |pattern| Dir.glob(File.join(@root, pattern)) }
            .select { |path| File.file?(path) }
            .map { |path| path.delete_prefix(@root + '/') }
            .reject { |path| path.end_with?('.md', '.csv', '.json', '.log') }
        (fixed + discovered).uniq.sort
    end
    def check_provenance
        path = File.join(@build, 'flat-map-sources.json')
        recorded = JSON.parse(File.read(path)).fetch('source_sha256')
        recorded.each do |source, expected|
            require_value(hash_file(File.join(@root, source)) == expected,
                "Reconfigure and rebuild after source changes: #{source}.")
        end
        source_inputs.each do |source|
            require_value(source.end_with?('.rb') || recorded.key?(source),
                "Reconfigure with complete build provenance for: #{source}.")
        end
        source_hashes = recorded.merge(source_inputs.to_h do |source|
            [source, hash_file(File.join(@root, source))]
        end)
        [path, source_hashes]
    end
    def abseil_inputs
        prefix = File.join(@root, 'deps/src/abseil')
        sources = %w[absl/base/config.h absl/container/flat_hash_map.h
            absl/container/internal/raw_hash_map.h absl/container/internal/raw_hash_set.h
            absl/container/internal/hashtable_control_bytes.h absl/container/internal/raw_hash_set.cc
            absl/hash/hash.h absl/hash/internal/hash.cc]
        config = File.read(File.join(prefix, 'absl/base/config.h'))
        release = config.match(/^#define ABSL_LTS_RELEASE_VERSION ([0-9]+)$/)
        patch = config.match(/^#define ABSL_LTS_RELEASE_PATCH_LEVEL ([0-9]+)$/)
        require_value(release && patch, 'Pinned Abseil source must identify its LTS release and patch.')
        archives = Dir.glob(File.join(@build, 'experiment-abseil/**/*.a')).sort
        require_value(archives.any? { |path| File.basename(path) == 'libabsl_raw_hash_set.a' },
            'Experiment-built Abseil archives are missing; complete the build first.')
        paths = sources.map { |path| File.join(prefix, path) }
        {'version' => "#{release[1]}.#{patch[1]}",
            'sha256' => paths.to_h { |path| [path.delete_prefix(@root + '/'), hash_file(path)] },
            'archive_sha256' => archives.to_h { |path| [path, hash_file(path)] }}
    end
    def save_manifest
        File.write(File.join(@results, 'run.json'), JSON.pretty_generate(@manifest) + "\n")
    end
    def capture_host
        commands = {'uname' => ['uname', '-a']}
        if RUBY_PLATFORM.include?('darwin')
            commands['system'] = ['sw_vers']
            commands['cpu'] = ['sysctl', 'machdep.cpu.brand_string', 'hw.ncpu', 'hw.memsize']
            commands['power'] = ['pmset', '-g', 'batt']
        else
            commands['cpu'] = ['lscpu']
        end
        commands.each do |name, command|
            output, status = Open3.capture2e(*command)
            require_value(status.success?, "Host metadata command failed: #{command.join(' ')}")
            File.write(File.join(@results, "host-#{name}.txt"), output)
        end
    end
    def wait_for_idle(require_quiet = false)
        competing = competing_processes([Process.pid])
        return if competing.empty? && !require_quiet
        started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        previous_wait = @waited_seconds
        quiet_since = competing.empty? ? started : nil
        record = {'started_at' => Time.now.iso8601, 'elapsed_seconds' => 0.0,
            'observations' => []}
        @manifest.fetch('idle_waits') << record
        loop do
            now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
            @waited_seconds = previous_wait + now - started
            @manifest['waited_seconds'] = @waited_seconds
            record['elapsed_seconds'] = now - started
            require_value(@waited_seconds <= WAIT_BUDGET_SECONDS,
                "Wait budget of #{WAIT_BUDGET_SECONDS}s exhausted; let builds/checks finish, " +
                    'then rerun with a fresh results directory; prior clean and discarded records are preserved.')
            if competing.empty?
                quiet_since ||= now
                quiet_seconds = now - quiet_since
                if quiet_seconds >= QUIET_SECONDS
                    record['finished_at'] = Time.now.iso8601
                    save_manifest
                    puts "Host was quiet for #{QUIET_SECONDS}s; continuing measurements."
                    return
                end
                puts "Waiting for quiet host: #{quiet_seconds.floor}/#{QUIET_SECONDS}s idle; " +
                    "#{@waited_seconds.round}/#{WAIT_BUDGET_SECONDS}s total wait."
            else
                quiet_since = nil
                record.fetch('observations') << {'observed_at' => Time.now.iso8601,
                    'processes' => competing}
                puts "Waiting for #{competing.length} competing processes; " +
                    "#{@waited_seconds.round}/#{WAIT_BUDGET_SECONDS}s total wait."
            end
            save_manifest
            require_value(@waited_seconds < WAIT_BUDGET_SECONDS,
                "Wait budget of #{WAIT_BUDGET_SECONDS}s exhausted; let builds/checks finish, " +
                    'then rerun with a fresh results directory; prior clean and discarded records are preserved.')
            sleep([1.0, WAIT_BUDGET_SECONDS - @waited_seconds].min)
            competing = competing_processes([Process.pid])
        end
    end
    def discard_attempt(record, stem, attempt, contamination)
        relative_directory = "discarded/#{stem}__attempt#{attempt}"
        directory = File.join(@results, relative_directory)
        FileUtils.mkdir_p(File.join(@results, 'discarded'))
        Dir.mkdir(directory)
        %w[csv log].each do |type|
            source = File.join(@results, record.fetch(type))
            next unless File.file?(source)
            destination = File.join(directory, File.basename(source))
            FileUtils.mv(source, destination)
            record[type] = File.join(relative_directory, File.basename(source))
            record["#{type}_sha256"] = hash_file(destination)
        end
        evidence = File.join(directory, 'contamination.json')
        File.write(evidence, JSON.pretty_generate(contamination) + "\n")
        record['contamination'] = contamination
        record['contamination_file'] = File.join(relative_directory, 'contamination.json')
        record['contamination_sha256'] = hash_file(evidence)
        @manifest.fetch('discarded_attempts') << record
        save_manifest
    end
    def measure_attempt(configuration, series, attempt)
        items, layout, reservation, shape = configuration
        stem = "flatmap__#{items}__#{layout}__#{reservation}__#{shape}__series#{series}"
        csv_path = File.join(@results, "#{stem}.csv")
        log_path = File.join(@results, "#{stem}.log")
        require_value(!File.exist?(csv_path) && !File.exist?(log_path),
            "Measurement files already exist for #{stem}; refusing to overwrite them.")
        command = [@executable, '--items', items.to_s, '--lookups', [262144, items].max.to_s,
            '--warmup', WARMUPS.to_s, '--repetitions', REPEATS.to_s, '--timeout', '600',
            '--csv', csv_path]
        command.concat(shape == 'single' ? ['--single-thread'] :
            ['--producers', '1', '--consumers', '8'])
        environment = {'ALLIGATOR_GPU_BACKEND' => 'cpu', 'FLAT_MAP_LAYOUT' => layout,
            'FLAT_MAP_RESERVATION' => reservation}
        puts "Starting #{stem}, attempt #{attempt}: insertion, retained lookups, misses, replacement."
        started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        process = Process.spawn(environment, *command, out: log_path, err: [:child, :out])
        waiter = Process.detach(process)
        contamination = []
        until waiter.join(1)
            elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
            puts "#{stem}: working (#{elapsed.round}s)."
            competing = competing_processes([Process.pid, process])
            unless competing.empty?
                if contamination.empty?
                    puts "#{stem}: competing activity detected; finishing this attempt before discarding it."
                end
                contamination << {'observed_at' => Time.now.iso8601,
                    'elapsed_seconds' => elapsed, 'processes' => competing}
            end
        end
        duration = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
        record = {'items' => items, 'layout' => layout, 'reservation' => reservation,
            'shape' => shape, 'series' => series, 'command' => command, 'environment' => environment,
            'attempt' => attempt, 'elapsed_seconds' => duration, 'exit_status' => waiter.value.exitstatus,
            'csv' => File.basename(csv_path), 'log' => File.basename(log_path),
            'log_sha256' => hash_file(log_path)}
        record['csv_sha256'] = hash_file(csv_path) if File.file?(csv_path)
        discard_attempt(record, stem, attempt, contamination) unless contamination.empty?
        unless waiter.value.success?
            @manifest.fetch('failed_attempts') << record if contamination.empty?
            save_manifest
            failed_log = File.join(@results, record.fetch('log'))
            warn File.readlines(failed_log).last(30).join
            raise ArgumentError, "Benchmark or its correctness checks failed: #{failed_log}"
        end
        unless contamination.empty?
            @contaminated_counts[configuration] += 1
            count = @contaminated_counts.fetch(configuration)
            puts "Discarded #{stem}, attempt #{attempt}; files and evidence are in discarded/."
            require_value(count < MAX_CONTAMINATED_ATTEMPTS,
                "#{MAX_CONTAMINATED_ATTEMPTS} contaminated attempts for #{configuration.join('/')}; " +
                    'let builds/checks finish, then rerun with a fresh results directory; ' +
                    'all discarded evidence and prior clean measurements are preserved.')
            return false
        end
        require_value(File.file?(csv_path), "Benchmark did not produce its CSV: #{csv_path}")
        @manifest.fetch('measurements') << record
        save_manifest
        puts "Finished #{stem} in #{duration.round(2)}s; process correctness checks passed."
        true
    end
    def measure(configuration, series)
        attempt = 1
        loop do
            wait_for_idle(attempt > 1)
            return if measure_attempt(configuration, series, attempt)
            attempt += 1
        end
    end
    def run
        require_value(!File.exist?(@results), 'Results directory exists; choose a fresh directory.')
        require_value(File.executable?(@executable), "Missing benchmark; build first: #{@executable}")
        correctness = check_correctness
        provenance, source_hashes = check_provenance
        cache = File.join(@build, 'CMakeCache.txt')
        commands = File.join(@build, 'compile_commands.json')
        require_value(File.read(cache).match?(/^CMAKE_BUILD_TYPE:STRING=Release$/),
            'Performance measurements require a Release build from run_build.sh.')
        require_value(File.file?(commands), "Missing compiler command record: #{commands}")
        abseil = abseil_inputs
        FileUtils.mkdir_p(@results)
        @manifest = {'status' => 'running', 'started_at' => Time.now.iso8601,
            'platform' => RUBY_PLATFORM, 'build_directory' => @build,
            'source_sha256' => source_hashes, 'abseil' => abseil,
            'executable' => {'path' => @executable, 'sha256' => hash_file(@executable)},
            'configurations' => configurations, 'warmups' => WARMUPS,
            'repetitions' => REPEATS, 'series' => SERIES, 'measurements' => [],
            'discarded_attempts' => [], 'failed_attempts' => [], 'idle_waits' => [],
            'waited_seconds' => 0.0, 'contention_policy' => {'quiet_seconds' => QUIET_SECONDS,
                'wait_budget_seconds' => WAIT_BUDGET_SECONDS,
                'max_contaminated_attempts_per_configuration' => MAX_CONTAMINATED_ATTEMPTS}}
        save_manifest
        {cache => 'CMakeCache.txt', commands => 'compile_commands.json',
            correctness => 'correctness.log', provenance => 'flat-map-sources.json'}.each do |source, name|
            FileUtils.cp(source, File.join(@results, name))
        end
        capture_host
        @manifest['artifact_sha256'] = Dir.glob(File.join(@results, '*')).select do |path|
            File.file?(path) && File.basename(path) != 'run.json'
        end.to_h { |path| [File.basename(path), hash_file(path)] }
        save_manifest
        puts "Running #{configurations.length * SERIES} processes sequentially; 2 warmups, 11 samples each."
        SERIES.times do |index|
            configurations.rotate(index * 5).each { |configuration| measure(configuration, index + 1) }
        end
        (source_hashes.merge(abseil.fetch('sha256'))).each do |path, expected|
            require_value(hash_file(File.join(@root, path)) == expected,
                "Source or dependency changed during measurement: #{path}")
        end
        abseil.fetch('archive_sha256').each do |path, expected|
            require_value(hash_file(path) == expected, "Abseil archive changed during measurement: #{path}")
        end
        require_value(hash_file(@executable) == @manifest.fetch('executable').fetch('sha256'),
            'Benchmark executable changed during measurement; discard this sweep.')
        expected = configurations.flat_map do |configuration|
            (1..SERIES).map { |series| configuration + [series] }
        end
        actual = @manifest.fetch('measurements').map do |record|
            %w[csv log].each do |type|
                require_value(hash_file(File.join(@results, record.fetch(type))) ==
                    record.fetch("#{type}_sha256"), 'A completed clean measurement was modified.')
            end
            %w[items layout reservation shape series].map { |key| record.fetch(key) }
        end
        require_value(actual.sort == expected.sort, 'The 42-process clean measurement matrix is incomplete.')
        @manifest['status'] = 'complete'
        @manifest['finished_at'] = Time.now.iso8601
        save_manifest
        puts "Completed all measurements; run analyze.rb #{@results} to validate and summarize."
    rescue StandardError => error
        if @manifest
            @manifest['status'] = 'failed'
            @manifest['failure'] = error.message
            save_manifest
        end
        raise
    end
end
begin
    unless ARGV.length == 2
        abort 'Usage: ruby run.rb BUILD_DIRECTORY RESULTS_DIRECTORY'
    end
    $stdout.sync = true
    FlatMapRun.new(*ARGV).run
rescue ArgumentError, KeyError, JSON::ParserError, SystemCallError => error
    warn "FlatSliceMap measurement stopped: #{error.message}"
    exit 1
end
