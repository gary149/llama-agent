# bench/terminal_bench

Minimal bench harness for llama-agent — the Phase 0 smoke scaffolding from
[`docs/small-model-optimization-plan.md`](../../docs/small-model-optimization-plan.md).

This is **not** Terminal-Bench 2.0 leaderboard parity. It's the smallest
viable scaffold to validate the bench→fix→measure loop end-to-end, run
locally or on a single RunPod pod (no Docker isolation). Labelled as
**"TB2.0-10 local subset"** in any published numbers.

## Layout

```
bench/terminal_bench/
├── README.md         (this file)
├── run_smoke.py      (the runner)
├── tasks/
│   └── hello-bash/   (the smoke task)
│       └── task.yaml
└── results/          (JSONL output, gitignored)
```

## Task format

Each task is a directory under `tasks/` containing `task.yaml`:

```yaml
prompt: |
  Free-form prompt for the agent.

check: |
  # bash script run AFTER the agent exits, in the task sandbox.
  # exit 0 = pass, anything else = fail.
  [ -f out.txt ]

category: shell        # optional
difficulty: trivial    # optional
```

The sandbox is a fresh `tempfile.TemporaryDirectory()` per task per trial.
The agent runs with the sandbox as its `cwd`. After the agent exits the
check script runs in the same dir.

## Run

```bash
# Smoke — one task, one trial
python bench/terminal_bench/run_smoke.py \
  --agent ./build/bin/llama-agent \
  --model ~/models/Qwen3.6-27B-UD-Q6_K_XL.gguf \
  --profile qwen3.6-27b-mtp-bench \
  --task hello-bash \
  --results bench/terminal_bench/results/smoke.jsonl
```

For MTP, forward the llama.cpp flags via `--extra-arg`:

```bash
python bench/terminal_bench/run_smoke.py \
  --agent ./build/bin/llama-agent \
  --model ~/models/Qwen3.6-27B-UD-Q6_K_XL.gguf \
  --profile qwen3.6-27b-mtp-bench \
  --task hello-bash \
  --extra-arg=--jinja \
  --extra-arg=--spec-type=draft-mtp \
  --extra-arg=--spec-draft-n-max=2 \
  --extra-arg=-ngl=99 \
  --extra-arg=-c=8192 \
  --extra-arg=-fa \
  --extra-arg=on
```

(The `--extra-arg` chain forwards individual tokens to the agent's argv.)

For an n=5 sweep:

```bash
python bench/terminal_bench/run_smoke.py ... --trials 5 --task all
```

## What's deferred

- Real Terminal-Bench 2.0 task definitions (10-task subset). The 10
  selected tasks live in the plan doc; importing/curating them is the
  next bench-side commit.
- Multiple profile pinning per sweep (would script via shell wrapper).
- Sweep aggregator + per-task pass/fail report generator.
- Per-task baseline vs measurement comparison (the plan's two-baseline
  + per-fix delta protocol).

## Output schema

Each line of the JSONL results file is one trial:

```json
{
  "task": "hello-bash",
  "trial": 1,
  "profile": "qwen3.6-27b-mtp-bench",
  "passed": true,
  "timed_out": false,
  "agent_exit": 0,
  "check_exit": 0,
  "elapsed_sec": 6.83,
  "agent_stdout_tail": "...",
  "agent_stderr_tail": "...",
  "check_stdout": ""
}
```
