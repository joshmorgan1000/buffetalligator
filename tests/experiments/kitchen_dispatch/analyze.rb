require 'csv'
require 'json'
require 'time'

CASES = %w[latency-hot latency-idle read-compute read-storage burst-single-1
  burst-single-multi burst-bulk-1 burst-bulk-multi isolation-shared
  isolation-separate isolation-coroutine].freeze
EXECUTORS = %w[kitchen blocking sleeping spinning].freeze
CONFIGURATION = %w[endpoint work path workers rounds release_delay_ms pipe_reads depth
  requests file_bytes block_bytes hardware_threads producers batch tasks].freeze
def statistics(values)
  sorted = values.sort
  middle = sorted.length / 2
  median = sorted.length.odd? ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0
  { count: sorted.length, median: median, min: sorted.first, max: sorted.last }
end
def number(value)
  result = Float(value)
  raise 'Non-finite numeric CSV value' unless result.finite?
  result
end
def median(group, metric)
  group[:metrics].fetch(metric, {})[:median]
end
def formatted(value, digits = 3)
  value.nil? ? '-' : format("%.#{digits}f", value)
end
def range_text(statistic)
  return '-' unless statistic
  "#{formatted(statistic[:median])} [#{formatted(statistic[:min])},#{formatted(statistic[:max])}]"
end
directory = ARGV.fetch(0) { abort 'Usage: ruby analyze.rb RESULTS_DIRECTORY' }
abort "Results directory does not exist: #{directory}" unless File.directory?(directory)
expected = CASES.product([1, 2], EXECUTORS).map { |name, round, executor| "#{name}-#{round}-#{executor}" }
groups = {}
successful = []
failed = []
pending = []
invalid = []
expected.each do |label|
  json_path = File.join(directory, label + '.json')
  csv_path = File.join(directory, label + '.csv')
  unless File.file?(json_path)
    pending << label
    next
  end
  begin
    process = JSON.parse(File.read(json_path))
    unless process['exitstatus'] == 0 && !process['timed_out'] && process['termsig'].nil?
      failed << { label: label, exitstatus: process['exitstatus'], termsig: process['termsig'],
        timed_out: process['timed_out'] }
      next
    end
    match = /\A(.+)-(\d+)-(kitchen|blocking|sleeping|spinning)\z/.match(label)
    name, round, executor = match.captures
    isolation = name.start_with?('isolation-')
    rows = CSV.read(csv_path, headers: true).map(&:to_h)
    raise "Expected #{isolation ? 1 : 3} measured CSV rows, found #{rows.size}" unless
      rows.size == (isolation ? 1 : 3)
    raise 'Process label does not match its filename' unless process['label'] == label
    metrics = Hash.new { |hash, key| hash[key] = [] }
    configurations = []
    rows.each do |row|
      actual_executor = row['executor'] || row.fetch('endpoint').split('_').first
      raise 'CSV executor does not match its filename' unless actual_executor == executor
      configurations << row.select { |key, _value| CONFIGURATION.include?(key) }
      row.each do |key, value|
        next unless key == 'seconds' || key == 'cpu_percent' || key.end_with?('_seconds', '_per_second', '_us')
        metrics[key] << number(value)
      end
      wall = number(row['seconds'] || row.fetch('wall_seconds'))
      raise 'Nonpositive sample wall time' unless wall > 0
      metrics['cpu_wall_ratio'] << number(row.fetch('cpu_seconds')) / wall
    end
    raise 'CSV workload configuration changed within the process' unless configurations.uniq.size == 1
    key = [name, executor]
    group = groups[key] ||= { case: name, executor: executor, processes: [],
      configuration: configurations.first, values: Hash.new { |hash, metric| hash[metric] = [] },
      latency_aggregation: isolation ? 'median_of_process_quantiles' : 'median_of_sample_quantiles' }
    raise 'CSV workload configuration changed between processes' unless group[:configuration] == configurations.first
    metrics.each { |metric, values| group[:values][metric].concat(values) }
    group[:processes] << { label: label, round: round.to_i, samples: rows.size,
      process_seconds: number(process.fetch('process_seconds')), csv: csv_path, json: json_path }
    successful << label
  rescue JSON::ParserError, CSV::MalformedCSVError, Errno::ENOENT, KeyError, ArgumentError, RuntimeError => error
    invalid << { label: label, reason: error.message }
  end
end
results = groups.values.sort_by { |group| [CASES.index(group[:case]), EXECUTORS.index(group[:executor])] }
results.each do |group|
  group[:metrics] = group.delete(:values).transform_values { |values| statistics(values) }
  group[:process_wall_seconds] = statistics(group[:processes].map { |process| process[:process_seconds] })
  group[:process_count] = group[:processes].size
  group[:sample_count] = group[:processes].sum { |process| process[:samples] }
  group[:complete] = group[:process_count] == 2
  rate = group[:metrics].key?('tasks_per_second') ? 'tasks_per_second' : 'requests_per_second'
  group[:rate_metric] = rate if group[:metrics].key?(rate)
  group[:rate_unit] = rate == 'tasks_per_second' ? 'tasks/s' :
    (group[:configuration]['work'] == 'read' ? 'reads/s' : 'callbacks/s') if group[:rate_metric]
end
results.each do |group|
  baseline = groups[[group[:case], 'kitchen']]
  next unless baseline
  comparison = {}
  if group[:rate_metric]
    comparison[:rate_ratio] = median(group, group[:rate_metric]) / median(baseline, group[:rate_metric])
  end
  %w[entry_p50_us entry_p99_us cpu_wall_ratio].each do |metric|
    next unless median(group, metric) && median(baseline, metric)
    comparison[metric + '_ratio'] = median(group, metric) / median(baseline, metric)
  end
  group[:versus_kitchen] = comparison
end
complete = successful.size == expected.size && failed.empty? && pending.empty? && invalid.empty?
summary = { generated_at: Time.now.utc.iso8601, directory: File.expand_path(directory),
  complete: complete, expected_processes: expected.size, successful_processes: successful.size,
  failed_processes: failed, pending_processes: pending, invalid_processes: invalid,
  notes: ['Statistics are medians and min/max of measured CSV rows, never pooled request quantiles.',
    'Ordinary cases have six samples across two successful fresh processes when complete.',
    'Isolation has two process-level summaries, each covering 101 controlled blocking-read rounds.',
    'CPU/wall is measured process CPU seconds divided by the same sample wall interval; 1.0 is one core.',
    'Process wall includes initialization, warmup, measured samples, and shutdown; it is polled every 50 ms.',
    'Read throughput is buffered cache-friendly file traffic; isolation uses controlled pipes, not disk.',
    'Rate and latency comparisons use only the same case against its Kitchen baseline.'], groups: results }
File.write(File.join(directory, 'summary.json'), JSON.pretty_generate(summary) + "\n")
lines = ["Processes: #{successful.size}/#{expected.size} successful and validated; " \
  "#{failed.size} failed, #{pending.size} pending, #{invalid.size} invalid; complete=#{complete}",
  '', 'All ranges show median [minimum,maximum] across measured CSV rows.',
  'Isolation p99 is the median of two process p99 values, each from 101 rounds.',
  'CPU/wall 1.0 means one fully occupied CPU core; process wall includes warmup and shutdown.', '']
CASES.each do |name|
  selected = results.select { |group| group[:case] == name }
  next if selected.empty?
  rate_unit = selected.first[:rate_unit]
  lines << "#{name}#{rate_unit ? ' — rate in ' + rate_unit : ' — controlled-pipe compute probe'}"
  lines << 'executor | processes/samples | rate median [min,max] | rate/Kitchen | entry p50 us | entry p99 us [min,max] | CPU/wall [min,max] | process wall s [min,max]'
  selected.each do |group|
    lines << [group[:executor], "#{group[:process_count]}/#{group[:sample_count]}",
      range_text(group[:metrics][group[:rate_metric]]),
      formatted(group.fetch(:versus_kitchen, {})[:rate_ratio]),
      formatted(median(group, 'entry_p50_us')), range_text(group[:metrics]['entry_p99_us']),
      range_text(group[:metrics]['cpu_wall_ratio']), range_text(group[:process_wall_seconds])].join(' | ')
  end
  lines << ''
end
lines << 'Invalid results: ' + invalid.to_json unless invalid.empty?
lines << 'Failed processes: ' + failed.to_json unless failed.empty?
report = lines.join("\n") + "\n"
File.write(File.join(directory, 'summary.txt'), report)
puts report
