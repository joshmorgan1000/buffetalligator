require 'csv'
require 'fileutils'

class MapCandidateAnalysis
    HEADERS = %w[container workload shape items operations capacity_per_producer batch sample
        seconds operations_per_second].freeze
    WORKLOADS = %w[insert lookup-hit lookup-miss].freeze
    SHAPES = {'single' => 'one-thread', 'threaded8p8c' => '8P/8C'}.freeze
    FILE_PATTERN = /\A([a-z][a-z0-9]*(?:_[a-z0-9]+)*)__([1-9][0-9]*)__
        (single|threaded8p8c)__(?:series)?([1-9][0-9]*)\.csv\z/x
    SUMMARY_HEADERS = %w[scope candidate items shape workload series series_count
        samples_per_process operations capacity_per_producer batch range_basis
        slice_median_ns_op slice_min_ns_op slice_max_ns_op
        unordered_map_median_ns_op unordered_map_min_ns_op unordered_map_max_ns_op
        slice_speedup_vs_baseline unordered_map_speedup_vs_baseline slice_speedup_vs_std
        slice_range_overlaps_baseline].freeze
    def initialize(input, output)
        @input = File.expand_path(input)
        @output = File.expand_path(output)
        @processes = {}
        @files = []
    end
    def require_value(condition, message)
        raise ArgumentError, message unless condition
    end
    def integer(value, label)
        require_value(value && value.match?(/\A[1-9][0-9]*\z/), "#{label} must be a positive integer")
        Integer(value, 10)
    end
    def positive_number(value, label)
        number = Float(value)
        require_value(number.finite? && number.positive?, "#{label} must be finite and positive")
        number
    rescue TypeError, ArgumentError
        raise ArgumentError, "#{label} must be finite and positive"
    end
    def median(values)
        ordered = values.sort
        middle = ordered.length / 2
        ordered.length.odd? ? ordered[middle] : (ordered[middle - 1] + ordered[middle]) / 2.0
    end
    def read_file(path)
        filename = File.basename(path)
        match = FILE_PATTERN.match(filename)
        require_value(match, "Unexpected CSV filename: #{filename}")
        candidate, item_text, shape, series_text = match.captures
        items = Integer(item_text, 10)
        series = Integer(series_text, 10)
        key = [candidate, items, shape, series]
        require_value(!@processes.key?(key), "Duplicate candidate/configuration/series: #{filename}")
        table = CSV.read(path, headers: true)
        require_value(table.headers == HEADERS, "Unexpected CSV headers: #{filename}")
        require_value(!table.empty?, "Empty CSV: #{filename}")
        hash_name = shape == 'single' ? 'unordered_map' : 'unordered_map+mutex'
        groups = {}
        table.each_with_index do |row, index|
            label = "#{filename}:#{index + 2}"
            require_value(row.fields.length == HEADERS.length, "Unexpected field count: #{label}")
            container = row.fetch('container')
            workload = row.fetch('workload')
            require_value(['SliceMap', hash_name].include?(container), "Unexpected container: #{label}")
            require_value(WORKLOADS.include?(workload), "Unexpected workload: #{label}")
            require_value(row.fetch('shape') == SHAPES.fetch(shape), "Shape differs from filename: #{label}")
            require_value(integer(row.fetch('items'), label) == items, "Items differ from filename: #{label}")
            operations = integer(row.fetch('operations'), "#{label} operations")
            capacity = integer(row.fetch('capacity_per_producer'), "#{label} capacity")
            batch = integer(row.fetch('batch'), "#{label} batch")
            sample = integer(row.fetch('sample'), "#{label} sample")
            seconds = positive_number(row.fetch('seconds'), "#{label} seconds")
            throughput = positive_number(row.fetch('operations_per_second'), "#{label} throughput")
            require_value((throughput * seconds / operations - 1.0).abs <= 1e-8,
                "Throughput disagrees with elapsed time: #{label}")
            require_value(workload != 'insert' || operations == items,
                "Insert operation count differs from row count: #{label}")
            group = (groups[[container, workload]] ||= {settings: [operations, capacity, batch], samples: {}})
            require_value(group.fetch(:settings) == [operations, capacity, batch],
                "Settings changed within a workload: #{label}")
            require_value(!group.fetch(:samples).key?(sample), "Duplicate sample: #{label}")
            group.fetch(:samples)[sample] = seconds * 1e9 / operations
        end
        expected_groups = ['SliceMap', hash_name].product(WORKLOADS)
        require_value(groups.keys.sort == expected_groups.sort, "Missing comparison workloads: #{filename}")
        groups.each_value do |group|
            samples = group.fetch(:samples)
            require_value(samples.keys.sort == (1..samples.length).to_a,
                "Sample IDs must be contiguous from one: #{filename}")
            values = samples.values
            group[:count] = values.length
            group[:median] = median(values)
            group[:minimum] = values.min
            group[:maximum] = values.max
        end
        counts = groups.values.map { |group| group.fetch(:count) }.uniq
        require_value(counts.length == 1, "Sample counts differ between workloads: #{filename}")
        capacity_batch = groups.values.map { |group| group.fetch(:settings)[1, 2] }.uniq
        require_value(capacity_batch.length == 1, "Capacity or batch differs between workloads: #{filename}")
        WORKLOADS.each do |workload|
            require_value(groups.fetch(['SliceMap', workload]).fetch(:settings) ==
                groups.fetch([hash_name, workload]).fetch(:settings),
                "Candidates have unequal workload settings: #{filename}")
        end
        require_value(groups.fetch(['SliceMap', 'lookup-hit']).fetch(:settings) ==
            groups.fetch(['SliceMap', 'lookup-miss']).fetch(:settings),
            "Hit and miss lookup counts differ: #{filename}")
        @processes[key] = {groups: groups, hash_name: hash_name, filename: filename}
        @files << filename
    end
    def validate_matrix
        @candidates = @processes.keys.map(&:first).uniq.sort
        require_value(@candidates.include?('baseline'), 'The matrix must include baseline')
        @candidates.delete('baseline')
        @candidates.unshift('baseline')
        reference = @processes.keys.select { |key| key.first == 'baseline' }.map { |key| key.drop(1) }.sort
        @candidates.each do |candidate|
            actual = @processes.keys.select { |key| key.first == candidate }.map { |key| key.drop(1) }.sort
            require_value(actual == reference, "Incomplete configuration/series matrix for #{candidate}")
        end
        @configurations = reference.map { |items, shape, _series| [items, shape] }.uniq.sort
        @series = reference.map(&:last).uniq.sort
        @configurations.each do |items, shape|
            actual = reference.select { |entry| entry[0, 2] == [items, shape] }.map(&:last).sort
            require_value(actual == @series, "Series matrix differs for #{items}/#{shape}")
            @candidates.each do |candidate|
                @series.each do |series|
                    process = @processes.fetch([candidate, items, shape, series])
                    baseline = @processes.fetch(['baseline', items, shape, @series.first])
                    WORKLOADS.each do |workload|
                        group = process.fetch(:groups).fetch(['SliceMap', workload])
                        control = baseline.fetch(:groups).fetch(['SliceMap', workload])
                        require_value(group.fetch(:settings) == control.fetch(:settings) &&
                            group.fetch(:count) == control.fetch(:count),
                            "Settings or sample counts differ: #{process.fetch(:filename)} #{workload}")
                    end
                end
            end
        end
    end
    def groups_for(candidate, items, shape, workload, series)
        process = @processes.fetch([candidate, items, shape, series])
        [process.fetch(:groups).fetch(['SliceMap', workload]),
            process.fetch(:groups).fetch([process.fetch(:hash_name), workload])]
    end
    def overlap?(left, right)
        left.fetch(:minimum) <= right.fetch(:maximum) && right.fetch(:minimum) <= left.fetch(:maximum)
    end
    def summary_row(scope, candidate, items, shape, workload, series, slice, standard, baseline_slice,
        baseline_standard, settings, count)
        [scope, candidate, items, shape, workload, series, scope == 'aggregate' ? @series.length : 1,
            count, *settings, scope == 'aggregate' ? 'process_medians' : 'measured_samples',
            slice.fetch(:median), slice.fetch(:minimum), slice.fetch(:maximum),
            standard.fetch(:median), standard.fetch(:minimum), standard.fetch(:maximum),
            baseline_slice.fetch(:median) / slice.fetch(:median),
            baseline_standard.fetch(:median) / standard.fetch(:median),
            standard.fetch(:median) / slice.fetch(:median), overlap?(slice, baseline_slice)]
    end
    def aggregate(groups)
        values = groups.map { |group| group.fetch(:median) }
        {median: median(values), minimum: values.min, maximum: values.max}
    end
    def build_rows
        rows = []
        @configurations.each do |items, shape|
            WORKLOADS.each do |workload|
                baseline_groups = @series.map { |series| groups_for('baseline', items, shape, workload, series) }
                baseline_slice = aggregate(baseline_groups.map(&:first))
                baseline_standard = aggregate(baseline_groups.map(&:last))
                @candidates.each do |candidate|
                    groups = @series.map { |series| groups_for(candidate, items, shape, workload, series) }
                    settings = groups.first.first.fetch(:settings)
                    count = groups.first.first.fetch(:count)
                    rows << summary_row('aggregate', candidate, items, shape, workload, 'all',
                        aggregate(groups.map(&:first)), aggregate(groups.map(&:last)), baseline_slice,
                        baseline_standard, settings, count)
                    @series.each_with_index do |series, index|
                        rows << summary_row('process', candidate, items, shape, workload, series,
                            *groups[index], *baseline_groups[index], settings, count)
                    end
                end
            end
        end
        rows.map { |row| SUMMARY_HEADERS.zip(row).to_h }
    end
    def measurement(row, prefix)
        format('%.2f [%.2f–%.2f]', row.fetch("#{prefix}_median_ns_op"),
            row.fetch("#{prefix}_min_ns_op"), row.fetch("#{prefix}_max_ns_op"))
    end
    def ratio(value)
        format('%.3fx', value)
    end
    def write_markdown(rows)
        lines = ['# SliceMap candidate measurements', '',
            "Validated #{@files.length} process CSVs, #{@candidates.length} candidates, " +
                "#{@configurations.length} row-count/topology configurations, and #{@series.length} series.", '',
            'Each process contributes its median measured sample; the aggregate is the median of those ' +
                'process medians, with equal weight for every series.', '',
            'Aggregate brackets show the minimum and maximum process medians across series; per-series ' +
                'brackets show the minimum and maximum individual measured samples within that process. ' +
                'These observed ranges are not confidence intervals. Overlap or small differences do not ' +
                'establish a performance winner, and non-overlap is not a statistical significance test.', '',
            'All times are aggregate wall-clock nanoseconds per operation, not individual request latency. ' +
                'Speedups above 1x favor the candidate; comparisons use ratios of the displayed medians. ' +
                'Baseline SliceMap speedup compares against unmodified SliceMap, std speedup compares the ' +
                "candidate's std control against baseline's std control, and SliceMap/std compares " +
                "SliceMap against the std control in that candidate's process.", '',
            'The std control is plain unordered_map for single and unordered_map plus one mutex for ' +
                'threaded8p8c. Insertions, retained successful lookups, and misses run in separate phases; ' +
                '8P/8C has eight active writers followed by eight active readers. It does not measure ' +
                'overlapping reads and writes or the best possible lock-free read-only std map baseline.', '',
            'Successful lookups include creation and retention of owned Slice handles until phase end. ' +
                'Both containers use the arena, so changes to Slice metadata or ownership can affect both ' +
                'columns; std timings are shown for every variant to expose that effect. Payload creation, ' +
                'reset, release of retained results, and correctness validation occur outside timing.', '',
            'The analyzer validates names, workload settings, sample IDs/counts, finite positive timing, ' +
                'reported throughput, and a complete candidate/configuration/series matrix. The raw CSV ' +
                'does not record hardware, compiler flags, warmups, launch order, concurrent system load, ' +
                'or source provenance; those must be checked against the run logs. These results are an ' +
                'experiment, not CI qualification or supported-platform performance qualification.', '']
        @configurations.each do |items, shape|
            WORKLOADS.each do |workload|
                selected = rows.select { |row| row.fetch('items') == items && row.fetch('shape') == shape &&
                    row.fetch('workload') == workload }
                first = selected.first
                lines.concat(["## #{items} rows · #{shape} · #{workload}", '',
                    "#{first.fetch('operations')} operations per process sample; " +
                        "#{first.fetch('samples_per_process')} measured samples per process.", '',
                    '| Candidate | SliceMap ns/op [series range] | std ns/op [series range] | ' +
                        'SliceMap speedup vs baseline | std speedup vs baseline std | ' +
                        'SliceMap speedup vs std | Baseline range overlap |',
                    '| --- | ---: | ---: | ---: | ---: | ---: | --- |'])
                selected.select { |row| row.fetch('scope') == 'aggregate' }.each do |row|
                    overlap = row.fetch('candidate') == 'baseline' ? 'reference' :
                        (row.fetch('slice_range_overlaps_baseline') ? 'yes; unresolved' : 'no; descriptive only')
                    lines << "| #{row.fetch('candidate')} | #{measurement(row, 'slice')} | " +
                        "#{measurement(row, 'unordered_map')} | #{ratio(row.fetch('slice_speedup_vs_baseline'))} | " +
                        "#{ratio(row.fetch('unordered_map_speedup_vs_baseline'))} | " +
                        "#{ratio(row.fetch('slice_speedup_vs_std'))} | #{overlap} |"
                end
                lines.concat(['', 'Per-series process results:', '',
                    '| Candidate | Series | SliceMap ns/op [sample range] | std ns/op [sample range] | ' +
                        'SliceMap speedup vs baseline | std speedup vs baseline std | SliceMap speedup vs std |',
                    '| --- | ---: | ---: | ---: | ---: | ---: | ---: |'])
                selected.select { |row| row.fetch('scope') == 'process' }.each do |row|
                    lines << "| #{row.fetch('candidate')} | #{row.fetch('series')} | " +
                        "#{measurement(row, 'slice')} | #{measurement(row, 'unordered_map')} | " +
                        "#{ratio(row.fetch('slice_speedup_vs_baseline'))} | " +
                        "#{ratio(row.fetch('unordered_map_speedup_vs_baseline'))} | " +
                        "#{ratio(row.fetch('slice_speedup_vs_std'))} |"
                end
                lines << ''
            end
        end
        lines.concat(['## Inputs', '', '<details><summary>Validated raw CSV files</summary>', ''])
        @files.sort.each { |filename| lines << "- `#{filename}`" }
        lines.concat(['', '</details>', ''])
        File.write(File.join(@output, 'RESULTS.md'), lines.join("\n"))
    end
    def run
        require_value(File.directory?(@input), "Input directory does not exist: #{@input}")
        paths = Dir.glob(File.join(@input, '*.csv')).reject { |path| File.basename(path) == 'summary.csv' }.sort
        require_value(!paths.empty?, "No raw CSV files found in #{@input}")
        paths.each { |path| read_file(path) }
        validate_matrix
        rows = build_rows
        FileUtils.mkdir_p(@output)
        CSV.open(File.join(@output, 'summary.csv'), 'w') do |csv|
            csv << SUMMARY_HEADERS
            rows.each { |row| csv << SUMMARY_HEADERS.map { |header| row.fetch(header) } }
        end
        write_markdown(rows)
        puts "Validated #{@files.length} process CSVs; wrote summary.csv and RESULTS.md to #{@output}"
    end
end
begin
    unless (1..2).cover?(ARGV.length)
        abort 'Usage: ruby analyze.rb INPUT_DIR [OUTPUT_DIR]'
    end
    MapCandidateAnalysis.new(ARGV.fetch(0), ARGV.fetch(1, ARGV.fetch(0))).run
rescue ArgumentError, CSV::MalformedCSVError, SystemCallError => error
    warn "Map candidate analysis failed: #{error.message}"
    exit 1
end
