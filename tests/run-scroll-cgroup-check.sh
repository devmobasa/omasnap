#!/usr/bin/env bash
set -euo pipefail

benchmark=${1:?benchmark executable is required}
unit="omasnap-scroll-cgroup-${RANDOM}-$$"

for required_command in systemd-run systemctl; do
  if ! command -v "$required_command" >/dev/null 2>&1; then
    echo "scroll cgroup check: $required_command is unavailable" >&2
    exit 77
  fi
done
if ! systemctl --user show-environment >/dev/null 2>&1; then
  echo "scroll cgroup check: no usable systemd user manager" >&2
  exit 77
fi
if [[ ! -r /sys/fs/cgroup/cgroup.controllers ]] ||
   ! grep -qw memory /sys/fs/cgroup/cgroup.controllers; then
  echo "scroll cgroup check: cgroup v2 memory controller is unavailable" >&2
  exit 77
fi

baseline_bytes=$("$benchmark" --baseline-rss-bytes)
if [[ ! "$baseline_bytes" =~ ^[0-9]+$ ]]; then
  echo "scroll cgroup check: could not measure benchmark baseline RSS" >&2
  exit 1
fi
# Two 512 MiB stitch budgets, plus this host's measured Qt/process baseline
# and the fixture's one 64 MiB producer frame. Swap is disabled so success is
# resident-memory proof rather than spill-to-swap.
limit_bytes=$((2 * 512 * 1024 * 1024 + baseline_bytes + 64 * 1024 * 1024))
output=$(systemd-run --user --quiet --pipe --wait --collect \
  --unit="$unit" \
  --property="MemoryMax=${limit_bytes}" \
  --property="MemorySwapMax=0" \
  "$benchmark" --maximum-budget-fixture)
printf '%s\n' "$output"
peak_bytes=$(sed -n 's/.*cgroup_peak_bytes=\([0-9][0-9]*\).*/\1/p' <<<"$output")
if [[ -z "$peak_bytes" ]] || ((peak_bytes > limit_bytes)); then
  echo "scroll cgroup check: missing or excessive memory.peak ($peak_bytes / $limit_bytes)" >&2
  exit 1
fi
printf 'cgroup_limit_bytes=%s measured_baseline_bytes=%s verified_peak_bytes=%s\n' \
  "$limit_bytes" "$baseline_bytes" "$peak_bytes"
