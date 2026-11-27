#!/usr/bin/env bash
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#
# The file management fuzz on the scheduled C++ CI (#1193). It uses the build tree of the job
# (alcedo_main, alcedo-cli, and StorageInvariantCheck) and builds nothing.
#
#   bash scripts/file_fuzz/ci_run.sh <build tree>
#
# Environment (all optional; workflow_dispatch fills them from its inputs):
#   FUZZ_SEED            the seed of every phase; empty: a random 64-bit seed
#   FUZZ_STEPS           the most steps of each phase (the time budget ends a phase first)
#   FUZZ_MODE            all (watcher-off, watcher-on, and the volume sequence), or one of
#                        watcher-off, watcher-on, concurrent, volumes
#   FUZZ_INJECT_FAILURE  test only: the first operation of this kind fails
#   FUZZ_BUDGET_SECONDS  the time budget of all phases together (default 1200)
#
# The seed and the replay commands go to the step summary. After a failure the script shrinks
# the log, replays the shrunk log once to confirm the failure, and exits 1; the workflow uploads
# <build tree>/file-fuzz as an artifact. Exit 0: every phase passed; 2: a setup error.

# No -u: the bash 3.2 of macOS treats an empty array as unset.
set -o pipefail

build_dir="${1:?usage: ci_run.sh <build tree>}"
out="${build_dir}/file-fuzz"
driver="scripts/file_fuzz/file_fuzz.py"
seed="${FUZZ_SEED:-}"
if [ -z "${seed}" ]; then
  seed="$(python3 -c 'import secrets; print(secrets.randbits(64))')"
fi
steps="${FUZZ_STEPS:-2000}"
mode="${FUZZ_MODE:-all}"
inject="${FUZZ_INJECT_FAILURE:-}"
budget="${FUZZ_BUDGET_SECONDS:-1200}"
summary="${GITHUB_STEP_SUMMARY:-/dev/stdout}"

rm -rf "${out}"
mkdir -p "${out}"

# The volume sequence: hdiutil disk images (test 42 to 44) with the mounts and disk changes that
# use them.
volume_weights=(--areas M1,M2,M5 --weight volume_create=6 --weight volume_detach=5
                --weight volume_attach=6 --weight volume_rename=3 --weight mount=6
                --weight sync=6)

# Each phase: name, mode, share of the budget in percent, extra arguments.
phases=()
case "${mode}" in
  all) phases=("watcher-off:watcher-off:45" "watcher-on:watcher-on:35" "volumes:watcher-off:20") ;;
  watcher-off | watcher-on | concurrent) phases=("${mode}:${mode}:100") ;;
  volumes) phases=("volumes:watcher-off:100") ;;
  *) echo "unknown FUZZ_MODE ${mode}" >&2; exit 2 ;;
esac

{
  echo "## File management fuzz"
  echo
  echo "Seed \`${seed}\`, mode \`${mode}\`, at most ${steps} steps for each phase, ${budget} s in all."
  if [ -n "${inject}" ]; then
    echo
    echo "Test-only failure injected at the first \`${inject}\` operation."
  fi
  echo
  echo "| Phase | Mode | Steps | Result | Reproduce |"
  echo "| --- | --- | --- | --- | --- |"
} >> "${summary}"

failed=0
for phase in "${phases[@]}"; do
  IFS=: read -r name phase_mode share <<< "${phase}"
  limit=$(( budget * share / 100 ))
  extra=()
  if [ "${name}" = "volumes" ]; then
    extra=("${volume_weights[@]}")
  fi
  if [ -n "${inject}" ]; then
    extra+=(--inject-failure "${inject}")
  fi
  run_dir="${out}/${name}"
  echo "::group::File management fuzz: ${name}"
  python3 "${driver}" run --seed "${seed}" --steps "${steps}" --mode "${phase_mode}" \
    --out "${run_dir}" --build-dir "${build_dir}" --time-limit "${limit}" "${extra[@]}"
  code=$?
  echo "::endgroup::"
  ran="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["steps"])' \
    "${run_dir}/summary.json" 2>/dev/null || echo "?")"
  reproduce="\`python3 ${driver} run --seed ${seed} --steps ${ran} --mode ${phase_mode} ${extra[*]}\`"
  if [ "${code}" -eq 0 ]; then
    echo "| ${name} | ${phase_mode} | ${ran} | passed | ${reproduce} |" >> "${summary}"
    continue
  fi
  failed=1
  if [ "${code}" -ne 1 ]; then
    echo "| ${name} | ${phase_mode} | ${ran} | setup error (exit ${code}) | ${reproduce} |" >> "${summary}"
    continue
  fi
  check="$(python3 -c 'import json,sys; f=json.load(open(sys.argv[1])); print("step %s (%s): %s" % (f["step"], f["op"], f["check"]))' \
    "${run_dir}/failure.json" 2>/dev/null || echo "failure")"
  ran="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["step"])' \
    "${run_dir}/failure.json" 2>/dev/null || echo "${ran}")"
  reproduce="\`python3 ${driver} run --seed ${seed} --steps ${ran} --mode ${phase_mode} ${extra[*]}\`"
  echo "::group::Shrink and confirm: ${name}"
  python3 "${driver}" shrink "${run_dir}/ops.jsonl" --out "${run_dir}/shrink" \
    --build-dir "${build_dir}" --time-limit 600
  confirmed="the shrunk log was not written"
  if [ -f "${run_dir}/shrink/shrunk.jsonl" ]; then
    python3 "${driver}" replay "${run_dir}/shrink/shrunk.jsonl" --out "${run_dir}/confirm" \
      --build-dir "${build_dir}"
    replay_code=$?
    shrunk_ops=$(( $(wc -l < "${run_dir}/shrink/shrunk.jsonl") - 1 ))
    if [ "${replay_code}" -eq 1 ]; then
      confirmed="the shrunk log (${shrunk_ops} operations) fails again"
    else
      confirmed="the shrunk log (${shrunk_ops} operations) passed its replay (exit ${replay_code})"
    fi
  fi
  echo "::endgroup::"
  echo "| ${name} | ${phase_mode} | ${ran} | **failed** at ${check}; ${confirmed} | ${reproduce}, or \`python3 ${driver} replay ${name}/shrink/shrunk.jsonl\` from the artifact |" >> "${summary}"
done

{
  echo
  if [ "${failed}" -ne 0 ]; then
    echo "The artifact \`file-fuzz-failure\` holds ops.jsonl, the shrunk log, the project, and the session logs of each failed phase. Open an issue for each failure; its fix adds a regression test in the suite of the owner."
  fi
} >> "${summary}"
exit "${failed}"
