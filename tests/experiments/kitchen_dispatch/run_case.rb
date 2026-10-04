require 'json'
require 'fileutils'
require 'time'
directory, label, *command = ARGV
raise 'Expected directory, label, and command' if command.empty?
log_path = File.join(directory, label + '.log')
started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
started_at = Time.now.utc.iso8601
pid = Process.spawn({'ALLIGATOR_GPU_BACKEND' => 'cpu'}, *command,
  out: log_path, err: [:child, :out], pgroup: true)
next_progress = started
status = nil
timed_out = false
loop do
  completed = Process.waitpid2(pid, Process::WNOHANG)
  if completed
    status = completed[1]
    break
  end
  now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  if now >= next_progress
    puts "#{label}: running (#{(now - started).round(1)}s)"
    STDOUT.flush
    next_progress = now + 1.0
  end
  if now - started > 120
    timed_out = true
    Process.kill('TERM', -pid)
    sleep 1
    begin
      Process.kill('KILL', -pid)
    rescue Errno::ESRCH
    end
    status = Process.waitpid2(pid)[1]
    break
  end
  sleep 0.05
end
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
result = {label: label, command: command, started_at: started_at,
  process_seconds: elapsed, exitstatus: status.exitstatus,
  termsig: status.termsig, timed_out: timed_out}
File.write(File.join(directory, label + '.json'), JSON.pretty_generate(result))
puts JSON.generate(result)
exit(status.success? && !timed_out ? 0 : 1)
