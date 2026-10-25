#!/usr/bin/env ruby
# Validates the complete flat-map matrix and summarizes equally weighted process medians.
require 'csv'
require 'digest'
require 'fileutils'
require 'json'

class FlatMapAnalysis
    HEADERS = %w[container workload shape items operations capacity_per_producer batch sample
        seconds operations_per_second].freeze
    CONTAINERS = %w[SliceMap unordered_map FlatSliceMap].freeze
    WORKLOADS = %w[insert lookup-hit lookup-miss replace].freeze
    SHAPES = {'single' => 'one-thread', 'readers8' => '1P/8C'}.freeze
    FILE_PATTERN = /\Aflatmap__([1-9][0-9]*)__
        (sequential|strided)__(reserved|growing)__(single|readers8)__series([1-3])\.csv\z/x
    SUMMARY_HEADERS = %w[scope items layout reservation shape workload series series_count
        samples_per_process operations capacity_per_producer batch range_basis
        slicemap_median_ns_op slicemap_min_ns_op slicemap_max_ns_op
        unordered_map_median_ns_op unordered_map_min_ns_op unordered_map_max_ns_op
        flat_median_ns_op flat_min_ns_op flat_max_ns_op
        unordered_map_over_flat slicemap_over_flat].freeze
    def initialize(input, output)
        @input = File.expand_path(input)
        @output = File.expand_path(output)
        @processes = {}
    end
    def require_value(condition, message)
        raise ArgumentError, message unless condition
    end
    def integer(value, label, zero = false)
        pattern = zero ? /\A(?:0|[1-9][0-9]*)\z/ : /\A[1-9][0-9]*\z/
        require_value(value && value.match?(pattern), "Invalid integer: #{label}")
        Integer(value, 10)
    end
    def positive_number(value, label)
        number = Float(value)
        require_value(number.finite? && number.positive?, "Invalid positive number: #{label}")
        number
    rescue TypeError, ArgumentError
        raise ArgumentError, "Invalid positive number: #{label}"
    end
    def median(values)
        ordered = values.sort
        middle = ordered.length / 2
        ordered.length.odd? ? ordered[middle] : (ordered[middle - 1] + ordered[middle]) / 2.0
    end
    def configurations
        entries = [4096, 65536, 1048576].product(%w[sequential strided],
            %w[reserved growing]).map { |items, layout, reservation|
            [items, layout, reservation, 'single'] }
        entries + [65536, 1048576].map { |items| [items, 'sequential', 'reserved', 'readers8'] }
    end
    def expected_keys
        configurations.flat_map { |configuration| (1..3).map { |series| configuration + [series] } }
    end
    def check_file_hash(filename, expected)
        require_value(File.basename(filename) == filename, "Unexpected artifact path: #{filename}")
        require_value(expected.is_a?(String) && expected.match?(/\A[0-9a-f]{64}\z/),
            "Missing SHA-256: #{filename}")
        require_value(Digest::SHA256.file(File.join(@input, filename)).hexdigest == expected,
            "Artifact differs from recorded measurement: #{filename}")
    end
    def validate_manifest
        @manifest = JSON.parse(File.read(File.join(@input, 'run.json')))
        require_value(@manifest.fetch('status') == 'complete' && @manifest.key?('finished_at'),
            'The runner did not finish successfully; partial or contaminated results are rejected.')
        require_value(@manifest.fetch('configurations').sort == configurations.sort &&
            @manifest.fetch('series') == 3 && @manifest.fetch('repetitions') == 11 &&
            @manifest.fetch('warmups') == 2, 'Run settings differ from the required matrix.')
        %w[source_sha256 artifact_sha256].each do |name|
            require_value(!@manifest.fetch(name).empty?, "Missing provenance: #{name}")
        end
        require_value(!@manifest.fetch('abseil').fetch('sha256').empty?, 'Missing Abseil provenance.')
        @manifest.fetch('artifact_sha256').each { |name, digest| check_file_hash(name, digest) }
        @records = {}
        @manifest.fetch('measurements').each do |record|
            key = %w[items layout reservation shape series].map { |field| record.fetch(field) }
            require_value(!@records.key?(key), "Duplicate process record: #{key.inspect}")
            require_value(record.fetch('exit_status') == 0, "Unsuccessful process: #{key.inspect}")
            environment = record.fetch('environment')
            require_value(environment.fetch('ALLIGATOR_GPU_BACKEND') == 'cpu' &&
                environment.fetch('FLAT_MAP_LAYOUT') == key[1] &&
                environment.fetch('FLAT_MAP_RESERVATION') == key[2],
                "Recorded environment differs from configuration: #{key.inspect}")
            %w[csv log].each do |type|
                check_file_hash(record.fetch(type), record.fetch("#{type}_sha256"))
            end
            @records[key] = record
        end
        require_value(@records.keys.sort == expected_keys.sort, 'Incomplete process manifest matrix.')
    end
    def read_file(path)
        filename = File.basename(path)
        matched = FILE_PATTERN.match(filename)
        require_value(matched, "Unexpected raw CSV filename: #{filename}")
        item_text, layout, reservation, shape, series_text = matched.captures
        items = Integer(item_text, 10)
        key = [items, layout, reservation, shape, Integer(series_text, 10)]
        require_value(!@processes.key?(key), "Duplicate process CSV: #{filename}")
        require_value(@records.fetch(key).fetch('csv') == filename,
            "CSV filename differs from process manifest: #{filename}")
        table = CSV.read(path, headers: true)
        require_value(table.headers == HEADERS && !table.empty?, "Invalid CSV headers: #{filename}")
        groups = {}
        table.each_with_index do |row, index|
            label = "#{filename}:#{index + 2}"
            require_value(row.fields.length == HEADERS.length, "Invalid field count: #{label}")
            container, workload = row.fetch('container'), row.fetch('workload')
            require_value(CONTAINERS.include?(container), "Unexpected container: #{label}")
            require_value(WORKLOADS.include?(workload), "Unexpected workload: #{label}")
            require_value(row.fetch('shape') == SHAPES.fetch(shape), "Unexpected shape: #{label}")
            require_value(integer(row.fetch('items'), label) == items, "Unexpected row count: #{label}")
            operations = integer(row.fetch('operations'), "#{label} operations")
            expected_operations = %w[insert replace].include?(workload) ? items : [262144, items].max
            require_value(operations == expected_operations, "Unexpected operation count: #{label}")
            capacity = integer(row.fetch('capacity_per_producer'), "#{label} capacity", true)
            batch = integer(row.fetch('batch'), "#{label} batch")
            require_value(capacity == (reservation == 'growing' ? 16 : items) && batch == 1,
                "Unexpected initial reservation or batch size: #{label}")
            sample = integer(row.fetch('sample'), "#{label} sample")
            seconds = positive_number(row.fetch('seconds'), "#{label} seconds")
            throughput = positive_number(row.fetch('operations_per_second'), "#{label} throughput")
            require_value((throughput * seconds / operations - 1.0).abs <= 1e-8,
                "Throughput differs from operations and elapsed time: #{label}")
            group = (groups[[container, workload]] ||= {settings: [operations, capacity, batch], samples: {}})
            require_value(group.fetch(:settings) == [operations, capacity, batch],
                "Workload settings changed between samples: #{label}")
            require_value(!group.fetch(:samples).key?(sample), "Duplicate sample: #{label}")
            group.fetch(:samples)[sample] = seconds * 1e9 / operations
        end
        require_value(groups.keys.sort == CONTAINERS.product(WORKLOADS).sort,
            "Missing container/workload combinations: #{filename}")
        groups.each_value do |group|
            require_value(group.fetch(:samples).keys.sort == (1..11).to_a,
                "Expected exactly samples 1 through 11: #{filename}")
            values = group.fetch(:samples).values
            group[:median] = median(values)
            group[:minimum] = values.min
            group[:maximum] = values.max
        end
        WORKLOADS.each do |workload|
            settings = CONTAINERS.map { |container| groups.fetch([container, workload]).fetch(:settings) }
            require_value(settings.uniq.length == 1, "Unequal container settings: #{filename}/#{workload}")
        end
        require_value(groups.values.map { |group| group.fetch(:settings).drop(1) }.uniq.length == 1,
            "Capacity or batch changed between workloads: #{filename}")
        @processes[key] = groups
    end
    def aggregate(groups)
        values = groups.map { |group| group.fetch(:median) }
        {median: median(values), minimum: values.min, maximum: values.max}
    end
    def summary_row(scope, configuration, workload, series, groups, settings)
        values = [scope, *configuration, workload, series, scope == 'aggregate' ? 3 : 1, 11,
            *settings, scope == 'aggregate' ? 'process_medians' : 'measured_samples']
        groups.each { |group| values.concat(group.values_at(:median, :minimum, :maximum)) }
        values.concat([groups[1].fetch(:median) / groups[2].fetch(:median),
            groups[0].fetch(:median) / groups[2].fetch(:median)])
        SUMMARY_HEADERS.zip(values).to_h
    end
    def build_rows
        rows = []
        configurations.each do |configuration|
            WORKLOADS.each do |workload|
                groups = (1..3).map do |series|
                    process = @processes.fetch(configuration + [series])
                    CONTAINERS.map { |container| process.fetch([container, workload]) }
                end
                settings = groups.first.first.fetch(:settings)
                require_value(groups.flatten.map { |group| group.fetch(:settings) }.uniq == [settings],
                    "Settings differ between process series: #{configuration.inspect}/#{workload}")
                rows << summary_row('aggregate', configuration, workload, 'all',
                    (0..2).map { |index| aggregate(groups.map { |group| group[index] }) }, settings)
                groups.each_with_index do |process_groups, index|
                    rows << summary_row('process', configuration, workload, index + 1,
                        process_groups, settings)
                end
            end
        end
        rows
    end
    def measurement(row, prefix)
        format('%.2f [%.2f–%.2f]', row.fetch("#{prefix}_median_ns_op"),
            row.fetch("#{prefix}_min_ns_op"), row.fetch("#{prefix}_max_ns_op"))
    end
    def write_markdown(rows)
        lines = ['# FlatSliceMap experiment results', '',
            'Validated 42 processes: 14 configurations, three process series, two warmups, and ' +
                '11 measured samples for each container/workload pair.', '',
            'Each cell reports the median of three process medians in aggregate nanoseconds per ' +
                'operation, followed by the minimum and maximum process medians. Lower is faster; ' +
                'these ranges are descriptive and are not confidence intervals. Ratios above 1x ' +
                'favor FlatSliceMap and are calculated from the displayed medians.', '',
            'All three containers return independently owned Slice handles on successful lookups. ' +
                'Payload preparation, reset, releasing returned handles, and correctness checks ' +
                'occur outside timing. The 1P/8C topology has one writer followed by eight readers ' +
                'of an unchanged map; it does not overlap mutation with reads. Neither standard ' +
                'unordered_map nor FlatSliceMap has a benchmark mutex in that read phase.', '',
            'The matrix covers deterministic sequential and strided keys with reserved and growing ' +
                'tables; growing tables start at 16 reserved rows and growth is timed. Every sample ' +
                'constructs a fresh map and makes its initial reservation outside timing. A separate flat map does not ' +
                "implement SliceMap's stable positions, publication hooks, waits, or concurrent reset.", '',
            "Recorded Abseil version: `#{@manifest.fetch('abseil').fetch('version')}`. " +
                'Source/dependency hashes, binary hash, compiler commands, build flags, host details, ' +
                'correctness output, commands, and per-process logs are preserved beside this file.', '',
            'The runner checked for competing builds/tests/benchmarks before processes and at ' +
                'one-second heartbeats. This does not control thermals, scheduling, other applications, ' +
                'or shorter-lived activity. Results remain specific to the recorded host and build.', '']
        configurations.each do |items, layout, reservation, shape|
            lines.concat(["## #{items} rows · #{layout} · #{reservation} · #{SHAPES.fetch(shape)}", '',
                '| Workload | SliceMap ns/op [process range] | std ns/op [process range] | ' +
                    'FlatSliceMap ns/op [process range] | std / flat | SliceMap / flat |',
                '| --- | ---: | ---: | ---: | ---: | ---: |'])
            rows.select { |row| row.fetch('scope') == 'aggregate' &&
                row.values_at('items', 'layout', 'reservation', 'shape') ==
                    [items, layout, reservation, shape] }.each do |row|
                lines << "| #{row.fetch('workload')} | #{measurement(row, 'slicemap')} | " +
                    "#{measurement(row, 'unordered_map')} | #{measurement(row, 'flat')} | " +
                    format('%.3fx | %.3fx |', row.fetch('unordered_map_over_flat'),
                        row.fetch('slicemap_over_flat'))
            end
            lines << ''
        end
        lines.concat(['Per-process medians, sample ranges, aggregate process ranges, operation counts, ' +
            'and ratios are in `summary.csv`; full measured samples remain in the 42 raw CSVs.', ''])
        File.write(File.join(@output, 'RESULTS.md'), lines.join("\n"))
    end
    def run
        validate_manifest
        paths = Dir.glob(File.join(@input, '*.csv')).reject { |path| File.basename(path) == 'summary.csv' }
        paths.sort.each { |path| read_file(path) }
        require_value(@processes.keys.sort == expected_keys.sort, 'Incomplete CSV configuration matrix.')
        rows = build_rows
        FileUtils.mkdir_p(@output)
        CSV.open(File.join(@output, 'summary.csv'), 'w') do |csv|
            csv << SUMMARY_HEADERS
            rows.each { |row| csv << SUMMARY_HEADERS.map { |header| row.fetch(header) } }
        end
        write_markdown(rows)
        puts "Validated 42 process CSVs; wrote summary.csv and RESULTS.md to #{@output}."
    end
end
begin
    unless (1..2).cover?(ARGV.length)
        abort 'Usage: ruby analyze.rb INPUT_DIRECTORY [OUTPUT_DIRECTORY]'
    end
    FlatMapAnalysis.new(ARGV.fetch(0), ARGV.fetch(1, ARGV.fetch(0))).run
rescue ArgumentError, KeyError, JSON::ParserError, CSV::MalformedCSVError, SystemCallError => error
    warn "FlatSliceMap analysis failed: #{error.message}"
    exit 1
end
