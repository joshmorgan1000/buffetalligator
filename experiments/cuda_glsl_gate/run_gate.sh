#!/usr/bin/env bash
set -euo pipefail
gate_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gate_artifacts="${1:-${gate_root}/build/cuda-glsl-gate}"
gate_compiler="${SLANGC:-slangc}"
mkdir -p "${gate_artifacts}"
printf 'Slang compiler: %s\n' "$(command -v "${gate_compiler}")"
"${gate_compiler}" -version
"${gate_compiler}" -nvrtc-version
uname -a
perl -0777 -e '
    use strict;
    use warnings;
    my $destination = shift @ARGV;
    my @sources;
    for my $path (@ARGV) {
        open my $input, "<", $path or die "Cannot read $path: $!";
        push @sources, scalar <$input>;
    }
    my ($core) = $sources[0] =~ /VULKAN_GLSL_KERNEL_CORE\s*=\s*R"glsl\((.*?)\)glsl"/s;
    my ($jobs) = $sources[0] =~ /VULKAN_GLSL_KERNEL_TAIL\s*=\s*R"glsl\((.*?)\)glsl"/s;
    my ($public) = $sources[1] =~ /PUBLIC_GLSL_TAIL\s*=\s*R"glsl\((.*?)\)glsl"/s;
    my ($body) = $sources[2] =~ /const std::string body\s*=\s*R"glsl\((.*?)\)glsl"/s;
    die "Current production prelude or experimental kernel body is missing\n"
        unless defined $core && defined $jobs && defined $public && defined $body;
    my @formats = (
        ["public-list", $public, "vulkan_count()", "vulkan_slice(vulkan_index())"],
        ["job-table", $jobs, "vulkan_job_count()", "vulkan_job(vulkan_job_index())"]);
    for my $format (@formats) {
        open my $output, ">", "$destination/$format->[0].glsl" or die $!;
        print $output "#version 450\n", $core, $format->[1],
            "uint gate_count() { return $format->[2]; }\n", $body,
            "void main() { alligator_main($format->[3]); }\n";
    }
' "${gate_artifacts}" "${gate_root}/include/alligator/easyvulkan.hpp" \
    "${gate_root}/src/vulkan/vulkan.cpp" \
    "${gate_root}/experiments/metal_glsl_gate/metal_glsl_gate.mm"
printf '#version 450\nlayout(local_size_x=16,local_size_y=4,local_size_z=1) in;\nvoid main() {}\n' \
    > "${gate_artifacts}/language-probe.glsl"
gate_failed=0
for gate_spelling in lang-glsl allow-glsl; do
    gate_options=(-lang glsl)
    if [[ "${gate_spelling}" == allow-glsl ]]; then gate_options+=(-allow-glsl); fi
    for gate_source in language-probe public-list job-table; do
        for gate_target in cuda ptx; do
            gate_command=("${gate_compiler}" "${gate_options[@]}"
                "${gate_artifacts}/${gate_source}.glsl" -entry main -stage compute
                -target "${gate_target}" -o "${gate_artifacts}/${gate_source}.${gate_spelling}.${gate_target}")
            printf '\nCommand:'
            printf ' %q' "${gate_command[@]}"
            printf '\n'
            gate_status=0
            "${gate_command[@]}" 2>&1 | tee \
                "${gate_artifacts}/${gate_source}.${gate_spelling}.${gate_target}.diagnostics.txt" \
                || gate_status=$?
            printf 'Compiler exit status: %s\n' "${gate_status}"
            printf '%s\n' "${gate_status}" \
                > "${gate_artifacts}/${gate_source}.${gate_spelling}.${gate_target}.status.txt"
            if [[ "${gate_status}" != 0 ]]; then gate_failed=1; fi
        done
    done
done
printf '\nCompiler experiments finished; NVIDIA execution remains a separate required gate.\n'
exit "${gate_failed}"
