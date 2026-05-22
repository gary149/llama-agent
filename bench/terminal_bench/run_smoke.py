#!/usr/bin/env python3
"""
Minimal bench smoke runner for llama-agent.

Spawns `llama-agent --headless --profile <name> -p <task-prompt>` in a per-task
temp dir, captures stdout/stderr, runs the task's check script, writes a
JSONL result line.

This is the Phase 0 smoke harness for the small-model optimization plan.
A full Terminal-Bench 2.0 adapter with n=5 sweeps + 10-task subset comes
later; this proves the bench plumbing.

Usage:
  python run_smoke.py --agent /path/to/llama-agent \
                      --model /path/to/qwen.gguf \
                      --profile qwen3.6-27b-mtp-bench \
                      --task hello-bash \
                      --results results.jsonl
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def load_task(task_dir: Path) -> dict:
    """Load task.yaml (minimal subset — we don't depend on PyYAML)."""
    import re
    text = (task_dir / "task.yaml").read_text()
    # Trivial YAML subset: support `key: |` block scalars + `key: value` scalars.
    out = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.lstrip().startswith("#"):
            i += 1
            continue
        m = re.match(r"^([a-zA-Z_][a-zA-Z0-9_]*):\s*\|?\s*(#.*)?$", line)
        if not m and ":" in line:
            key, _, val = line.partition(":")
            out[key.strip()] = val.split("#")[0].strip()
            i += 1
            continue
        if not m:
            i += 1
            continue
        key = m.group(1)
        # Block scalar — read indented lines
        i += 1
        block_lines = []
        while i < len(lines) and (lines[i].startswith("  ") or not lines[i].strip()):
            if lines[i].strip():
                block_lines.append(lines[i][2:])
            else:
                block_lines.append("")
            i += 1
        out[key] = "\n".join(block_lines).strip()
    return out


def run_one_task(
    agent_path: Path,
    model_path: Path,
    profile: str,
    task_dir: Path,
    extra_agent_args: list,
    trial: int,
) -> dict:
    task = load_task(task_dir)
    task_name = task_dir.name
    prompt = task["prompt"]
    check_script = task.get("check", "true")

    # Per-task sandbox — fresh tmpdir, agent runs with cwd here.
    with tempfile.TemporaryDirectory(prefix=f"bench-{task_name}-") as sandbox:
        sandbox_path = Path(sandbox)
        cmd = [
            str(agent_path),
            "--headless",
            "--profile", profile,
            "-m", str(model_path),
            "-p", prompt,
        ] + extra_agent_args

        start = time.time()
        try:
            proc = subprocess.run(
                cmd,
                cwd=str(sandbox_path),
                capture_output=True,
                text=True,
                timeout=600,  # 10 min hard cap per task
            )
            elapsed = time.time() - start
            agent_exit = proc.returncode
            agent_stdout = proc.stdout
            agent_stderr = proc.stderr
            timed_out = False
        except subprocess.TimeoutExpired as e:
            elapsed = time.time() - start
            agent_exit = -1
            agent_stdout = (e.stdout or b"").decode("utf-8", errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            agent_stderr = (e.stderr or b"").decode("utf-8", errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
            timed_out = True

        # Run the check script in the sandbox
        check_proc = subprocess.run(
            ["bash", "-c", check_script],
            cwd=str(sandbox_path),
            capture_output=True,
            text=True,
            timeout=60,
        )
        passed = check_proc.returncode == 0

        return {
            "task": task_name,
            "trial": trial,
            "profile": profile,
            "passed": passed,
            "timed_out": timed_out,
            "agent_exit": agent_exit,
            "check_exit": check_proc.returncode,
            "elapsed_sec": round(elapsed, 2),
            "agent_stdout_tail": agent_stdout[-2000:],
            "agent_stderr_tail": agent_stderr[-2000:],
            "check_stdout": check_proc.stdout[-500:],
        }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent", required=True, help="path to llama-agent binary")
    ap.add_argument("--model", required=True, help="path to .gguf model file")
    ap.add_argument("--profile", default="qwen3.6-27b-mtp-bench",
                    help="profile name (loaded from built-in or path)")
    ap.add_argument("--task", default="hello-bash",
                    help="task name (subdir of bench/terminal_bench/tasks/) — or 'all'")
    ap.add_argument("--trials", type=int, default=1,
                    help="trials per task (n=5 for sign-off sweeps)")
    ap.add_argument("--results", default="results.jsonl",
                    help="JSONL output file")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra arg to forward to llama-agent (single argv token; repeat for multiple)")
    ap.add_argument("--llama-args", default="",
                    help="shell-quoted string of extra args; shlex-split and forwarded to llama-agent "
                         "(more ergonomic than chaining --extra-arg). Example: "
                         "--llama-args=\"--jinja --spec-type draft-mtp --spec-draft-n-max 2 -ngl 99\"")
    args = ap.parse_args()
    if args.llama_args:
        args.extra_arg = args.extra_arg + shlex.split(args.llama_args)

    repo_root = Path(__file__).resolve().parent
    tasks_dir = repo_root / "tasks"

    if args.task == "all":
        task_dirs = sorted([p for p in tasks_dir.iterdir() if p.is_dir()])
    else:
        task_dirs = [tasks_dir / args.task]

    results_path = Path(args.results)
    n_pass = 0
    n_total = 0
    with results_path.open("a") as f:
        for td in task_dirs:
            for trial in range(1, args.trials + 1):
                print(f"=== {td.name} trial {trial}/{args.trials} ===", flush=True)
                result = run_one_task(
                    Path(args.agent), Path(args.model), args.profile, td,
                    args.extra_arg, trial,
                )
                f.write(json.dumps(result) + "\n")
                f.flush()
                status = "PASS" if result["passed"] else "FAIL"
                tag = " (timeout)" if result["timed_out"] else ""
                print(f"  {status}{tag}  exit={result['agent_exit']}  {result['elapsed_sec']}s",
                      flush=True)
                n_total += 1
                if result["passed"]:
                    n_pass += 1

    print(f"\nSUMMARY: {n_pass}/{n_total} passed", flush=True)
    sys.exit(0 if n_pass == n_total else 1)


if __name__ == "__main__":
    main()
