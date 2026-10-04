require 'json'
require 'fileutils'
require 'etc'
build, results = ARGV
abort 'Usage: ruby run.rb BUILD_DIRECTORY RESULTS_DIRECTORY' unless build && results
FileUtils.mkdir_p(results)
concurrent = (Etc.nprocessors * 2).to_s
cases = [
  ['latency-hot', 'main', %w[--mode compute --work noop --depth 1 --requests 8192 --warmup 1 --repetitions 3]],
  ['latency-idle', 'main', %w[--mode compute --work noop --depth 1 --requests 512 --warmup 1 --repetitions 3 --idle-us 2000]],
  ['read-compute', 'main', %W[--mode compute --work read --depth #{concurrent} --requests 8192 --warmup 1 --repetitions 3]],
  ['read-storage', 'main', %W[--mode storage --work read --depth #{concurrent} --requests 8192 --warmup 1 --repetitions 3]],
  ['burst-single-1', 'burst', %w[--producers 1 --batch 1 --tasks 262144 --warmup 1 --repetitions 3]],
  ['burst-single-multi', 'burst', %W[--producers #{concurrent} --batch 1 --tasks 262144 --warmup 1 --repetitions 3]],
  ['burst-bulk-1', 'burst', %w[--producers 1 --batch 256 --tasks 262144 --warmup 1 --repetitions 3]],
  ['burst-bulk-multi', 'burst', %W[--producers #{concurrent} --batch 256 --tasks 262144 --warmup 1 --repetitions 3]],
  ['isolation-shared', 'io_isolation', %w[--path shared --rounds 101]],
  ['isolation-separate', 'io_isolation', %w[--path separate --rounds 101]],
  ['isolation-coroutine', 'io_isolation', %w[--path coroutine --rounds 101]]
]
orders = [%w[kitchen blocking sleeping spinning], %w[spinning sleeping blocking kitchen]]
cases.each do |name, program, arguments|
  orders.each_with_index do |executors, round|
    executors.each do |executor|
      label = "#{name}-#{round + 1}-#{executor}"
      command = ['ruby', File.join(__dir__, 'run_case.rb'), results, label,
        File.join(build, "kitchen_dispatch_#{program}"), '--executor', executor,
        *arguments, '--csv', File.join(results, label + '.csv')]
      File.open(File.join(results, 'commands.jsonl'), 'a') { |file| file.puts JSON.generate(command) }
      abort "Experiment failed: #{label}; inspect #{File.join(results,label + '.log')}" unless system(*command)
    end
  end
end
