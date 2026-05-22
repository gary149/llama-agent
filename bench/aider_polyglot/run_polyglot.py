#!/usr/bin/env python3
"""
Aider Polyglot runner for llama-agent.

Drives `llama-agent --headless` against the 225-exercise polyglot-benchmark
dataset (Python / Rust / Go / C++ / JavaScript / Java). Per-exercise:

  1. Copy exercise to a fresh tempdir (excluding .meta/ which holds
     the reference solution).
  2. Spawn `llama-agent --headless ... -p <prompt>` with the tempdir as cwd.
  3. Run the language's test suite. Pass = exit 0.
  4. (Optional retry) On failure, spawn the agent again with --resume
     against the saved session + a "tests failed, fix it" prompt. Re-run
     tests. This is the paper's 2-attempt protocol.
  5. Record result to JSON.

The runner is restartable — re-running with --resume skips exercises whose
status is pass_1 or pass_2.

Adapted from little-coder/benchmarks/aider_polyglot.py (PiRpc → subprocess).
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BENCHMARK_ROOT = Path(os.environ.get(
    "POLYGLOT_BENCHMARK_ROOT", "/workspace/polyglot-benchmark"))
REPO_ROOT = Path(__file__).resolve().parent.parent.parent  # llama-agent repo


# ── Per-language descriptors ──────────────────────────────────────────────

def _copy_exercise(src: Path, dst: Path):
    """Copy exercise tree, excluding .meta/ (solutions live there)."""
    def _ignore(_dir, names):
        return [".meta"] if ".meta" in names else []
    shutil.copytree(src, dst, ignore=_ignore)


# ── Python ─────────────────────────────────────────────────────────────────
def _prepare_python(src: Path, work: Path):
    _copy_exercise(src, work)
    stubs = [p for p in work.glob("*.py") if not p.name.endswith("_test.py")]
    tests = list(work.glob("*_test.py"))
    return stubs, tests


def _run_python(work: Path, timeout: int):
    try:
        r = subprocess.run(
            ["python3", "-m", "pytest", "-x", "-q"],
            cwd=work, capture_output=True, text=True, timeout=timeout, errors="replace",
        )
        return r.returncode == 0, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


# ── Rust ───────────────────────────────────────────────────────────────────
def _prepare_rust(src: Path, work: Path):
    _copy_exercise(src, work)
    # Strip `#[ignore]` from test files so all tests run by default
    for test_file in (work / "tests").glob("*.rs"):
        text = test_file.read_text()
        text = text.replace("#[ignore]\n", "")
        test_file.write_text(text)
    stubs = list((work / "src").glob("*.rs"))
    tests = list((work / "tests").glob("*.rs"))
    return stubs, tests


def _run_rust(work: Path, timeout: int):
    try:
        # --include-ignored is a libtest flag, not a cargo flag — goes after `--`.
        r = subprocess.run(
            ["cargo", "test", "--", "--include-ignored", "--test-threads=1"],
            cwd=work, capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "CARGO_NET_OFFLINE": "false"},
            errors="replace",
        )
        return r.returncode == 0, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


# ── Go ─────────────────────────────────────────────────────────────────────
def _prepare_go(src: Path, work: Path):
    _copy_exercise(src, work)
    stubs = [p for p in work.glob("*.go") if not p.name.endswith("_test.go")]
    tests = list(work.glob("*_test.go"))
    return stubs, tests


def _run_go(work: Path, timeout: int):
    try:
        r = subprocess.run(
            ["go", "test", "./..."],
            cwd=work, capture_output=True, text=True, timeout=timeout, errors="replace",
        )
        return r.returncode == 0, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


# ── C++ ────────────────────────────────────────────────────────────────────
def _prepare_cpp(src: Path, work: Path):
    _copy_exercise(src, work)
    stubs = [p for p in work.glob("*.cpp") if not p.name.endswith("_test.cpp")]
    stubs += list(work.glob("*.h"))
    tests = list(work.glob("*_test.cpp"))
    return stubs, tests


def _run_cpp(work: Path, timeout: int):
    try:
        build = work / "build"
        build.mkdir(exist_ok=True)
        r1 = subprocess.run(
            ["cmake", ".."],
            cwd=build, capture_output=True, text=True, timeout=timeout // 2, errors="replace",
        )
        if r1.returncode != 0:
            return False, "cmake failed:\n" + r1.stdout + r1.stderr
        r2 = subprocess.run(
            ["cmake", "--build", "."],
            cwd=build, capture_output=True, text=True, timeout=timeout // 2, errors="replace",
        )
        if r2.returncode != 0:
            return False, "build failed:\n" + r2.stdout + r2.stderr
        # The test binary follows Exercism CMake convention
        bins = [p for p in build.iterdir() if p.is_file() and os.access(p, os.X_OK)]
        if not bins:
            return False, "no test binary produced"
        r3 = subprocess.run(
            [str(bins[0])],
            cwd=build, capture_output=True, text=True, timeout=timeout // 2,
        )
        return r3.returncode == 0, r3.stdout + r3.stderr
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


# ── JavaScript ─────────────────────────────────────────────────────────────
def _prepare_js(src: Path, work: Path):
    _copy_exercise(src, work)
    stubs = [p for p in work.glob("*.js")
             if not p.name.endswith(".spec.js") and p.name not in ("babel.config.js",)]
    tests = list(work.glob("*.spec.js"))
    # `xit(` is xit-style "skip this test". Enable all to mirror the paper.
    for test_file in tests:
        text = test_file.read_text()
        text = text.replace("xit(", "it(")
        test_file.write_text(text)
    return stubs, tests


def _run_js(work: Path, timeout: int):
    try:
        # npm install if needed (per-exercise package.json)
        if (work / "package.json").exists() and not (work / "node_modules").exists():
            r0 = subprocess.run(
                ["npm", "install", "--silent"],
                cwd=work, capture_output=True, text=True, timeout=timeout // 2, errors="replace",
            )
            if r0.returncode != 0:
                return False, "npm install failed:\n" + r0.stdout + r0.stderr
        r = subprocess.run(
            ["npm", "test", "--silent"],
            cwd=work, capture_output=True, text=True, timeout=timeout, errors="replace",
        )
        return r.returncode == 0, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


# ── Java ───────────────────────────────────────────────────────────────────
def _prepare_java(src: Path, work: Path):
    _copy_exercise(src, work)
    # Strip @Disabled / @Ignore so all tests run
    for test_file in (work / "src" / "test" / "java").rglob("*.java"):
        text = test_file.read_text()
        text = text.replace("@Disabled\n", "").replace("@Ignore\n", "")
        test_file.write_text(text)
    stubs = list((work / "src" / "main" / "java").rglob("*.java"))
    tests = list((work / "src" / "test" / "java").rglob("*.java"))
    return stubs, tests


def _run_java(work: Path, timeout: int):
    try:
        r = subprocess.run(
            ["./gradlew", "test", "--no-daemon", "--quiet"],
            cwd=work, capture_output=True, text=True, timeout=timeout, errors="replace",
        )
        return r.returncode == 0, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return False, f"timed out after {timeout}s"


LANG_DESCRIPTORS = {
    "python": {
        "practice_dir": BENCHMARK_ROOT / "python" / "exercises" / "practice",
        "prepare": _prepare_python, "run_tests": _run_python,
        "syntax_hint": "Use Python 3. Run tests with `python -m pytest -x -q`.",
        "timeout_s": 90,
    },
    "rust": {
        "practice_dir": BENCHMARK_ROOT / "rust" / "exercises" / "practice",
        "prepare": _prepare_rust, "run_tests": _run_rust,
        "syntax_hint": "Use Rust 2021 edition. Edit src/lib.rs. Run tests with `cargo test --include-ignored`.",
        "timeout_s": 180,
    },
    "go": {
        "practice_dir": BENCHMARK_ROOT / "go" / "exercises" / "practice",
        "prepare": _prepare_go, "run_tests": _run_go,
        "syntax_hint": "Use Go modules. Run tests with `go test ./...`.",
        "timeout_s": 90,
    },
    "cpp": {
        "practice_dir": BENCHMARK_ROOT / "cpp" / "exercises" / "practice",
        "prepare": _prepare_cpp, "run_tests": _run_cpp,
        "syntax_hint": "Use C++17. Build with `cmake .. && cmake --build .` from a build/ dir, then run the produced test binary.",
        "timeout_s": 180,
    },
    "javascript": {
        "practice_dir": BENCHMARK_ROOT / "javascript" / "exercises" / "practice",
        "prepare": _prepare_js, "run_tests": _run_js,
        "syntax_hint": "Use modern JavaScript (ES modules where the .spec uses them). Run tests with `npm test`.",
        "timeout_s": 120,
    },
    "java": {
        "practice_dir": BENCHMARK_ROOT / "java" / "exercises" / "practice",
        "prepare": _prepare_java, "run_tests": _run_java,
        "syntax_hint": "Use Java 17. Edit files under src/main/java. Run tests with `./gradlew test`.",
        "timeout_s": 240,
    },
}


# ── Prompt + agent invocation ─────────────────────────────────────────────

def _build_prompt(exercise: str, stubs, tests, syntax_hint: str) -> str:
    stubs_list = "\n".join(f"  - {p.name}" for p in stubs)
    tests_list = "\n".join(f"  - {p.name}" for p in tests)
    return (
        f"Implement the Exercism exercise `{exercise}`.\n\n"
        f"Stub file(s) to implement:\n{stubs_list}\n\n"
        f"Test file(s) (for reference only — DO NOT edit):\n{tests_list}\n\n"
        f"{syntax_hint}\n\n"
        "Read the stubs + any `.docs/instructions.md` in the working dir, "
        "then implement the solution. When you believe the code is correct, "
        "stop calling tools."
    )


def _spawn_agent(agent_path: str, model_path: str, prompt: str, work: Path,
                 profile: str, headless: bool, llama_args: list,
                 session_path: Path, resume: bool, timeout: int) -> tuple:
    cmd = [agent_path]
    pipe_prompt = False
    if headless:
        cmd.append("--headless")
        cmd += ["-p", prompt]   # --headless explicitly sets single_turn
    else:
        # Baseline (no --headless): use --yolo for permissions, pipe the
        # prompt via stdin so the non-TTY branch in agent.cpp sets
        # params.single_turn = true. With `-p` instead, the agent would
        # enter readline after the first turn and hang.
        cmd.append("--yolo")
        pipe_prompt = True
    if profile:
        cmd += ["--profile", profile]
    if session_path:
        cmd += ["--session", str(session_path)]
        if resume:
            cmd.append("--resume")
    cmd += ["-m", model_path]
    cmd += llama_args
    try:
        r = subprocess.run(
            cmd, cwd=str(work),
            input=(prompt if pipe_prompt else None),
            capture_output=True, text=True, timeout=timeout,
            errors="replace",   # some agent stderr can contain non-UTF-8 bytes
        )
        return r.returncode, r.stdout, r.stderr, False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = (e.stderr or b"").decode("utf-8", errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        return -1, out, err, True


# ── Per-exercise loop ──────────────────────────────────────────────────────

def run_exercise(lang: str, ex_name: str, agent_path: str, model_path: str,
                 profile: str, headless: bool, llama_args: list,
                 do_retry: bool, agent_timeout: int) -> dict:
    desc = LANG_DESCRIPTORS.get(lang)
    if desc is None:
        return {"status": "skipped", "reason": f"no descriptor for {lang}"}
    src = desc["practice_dir"] / ex_name
    if not src.exists():
        return {"status": "error", "reason": f"exercise not found: {src}"}

    t0 = time.time()
    with tempfile.TemporaryDirectory(prefix=f"poly-{lang}-{ex_name}-") as tmp:
        work = Path(tmp) / ex_name
        stubs, tests = desc["prepare"](src, work)
        prompt = _build_prompt(ex_name, stubs, tests, desc["syntax_hint"])
        session_path = work.parent / "session.json"

        # Attempt 1
        exit1, stdout1, stderr1, t_out_1 = _spawn_agent(
            agent_path, model_path, prompt, work, profile, headless,
            llama_args, session_path, resume=False, timeout=agent_timeout,
        )
        if t_out_1:
            return {
                "status": "timeout", "attempt": "agent_timeout_1",
                "elapsed_s": round(time.time() - t0, 2),
                "agent_stderr_tail": stderr1[-1500:],
            }

        passed, test_out = desc["run_tests"](work, desc["timeout_s"])
        if passed:
            return {
                "status": "pass_1",
                "elapsed_s": round(time.time() - t0, 2),
            }

        if not do_retry:
            return {
                "status": "fail",
                "elapsed_s": round(time.time() - t0, 2),
                "test_output_tail": test_out[-1500:],
            }

        # Attempt 2 — retry with the test output as a fix prompt
        retry_prompt = (
            "The tests failed. Output:\n\n```\n" + test_out[-4000:]
            + "\n```\n\nFix the implementation and try again."
        )
        exit2, stdout2, stderr2, t_out_2 = _spawn_agent(
            agent_path, model_path, retry_prompt, work, profile, headless,
            llama_args, session_path, resume=True, timeout=agent_timeout,
        )
        if t_out_2:
            return {
                "status": "timeout", "attempt": "agent_timeout_2",
                "elapsed_s": round(time.time() - t0, 2),
            }
        passed2, test_out2 = desc["run_tests"](work, desc["timeout_s"])
        if passed2:
            return {
                "status": "pass_2",
                "elapsed_s": round(time.time() - t0, 2),
            }
        return {
            "status": "fail",
            "elapsed_s": round(time.time() - t0, 2),
            "test_output_tail": test_out2[-1500:],
        }


# ── Driver ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent", required=True, help="path to llama-agent binary")
    ap.add_argument("--model", required=True, help="path to .gguf model")
    ap.add_argument("--profile", default="", help="profile name (omit for baseline)")
    ap.add_argument("--headless", action="store_true",
                    help="pass --headless (vs baseline mode which uses --yolo + -p only)")
    ap.add_argument("--llama-args", default="",
                    help="shell-quoted extra args to forward to llama-agent")
    ap.add_argument("--languages", default="python,rust,go,cpp,javascript,java",
                    help="comma-separated language subset")
    ap.add_argument("--exercise", default="", help="run only this exercise name (any language)")
    ap.add_argument("--limit", type=int, default=0, help="run at most N exercises per language")
    ap.add_argument("--results", required=True, help="output JSON path (incremental)")
    ap.add_argument("--resume", action="store_true",
                    help="skip exercises whose status is pass_1 or pass_2")
    ap.add_argument("--no-retry", action="store_true",
                    help="skip the second attempt on test failure (paper protocol = retry)")
    ap.add_argument("--agent-timeout", type=int, default=900,
                    help="hard timeout per agent spawn in seconds")
    args = ap.parse_args()

    import shlex
    llama_args = shlex.split(args.llama_args) if args.llama_args else []
    languages = [l.strip() for l in args.languages.split(",") if l.strip()]

    results_path = Path(args.results)
    results_path.parent.mkdir(parents=True, exist_ok=True)
    if args.resume and results_path.exists():
        try:
            data = json.loads(results_path.read_text())
        except Exception:
            data = {"meta": {}, "exercises": {}}
    else:
        data = {"meta": {}, "exercises": {}}

    data["meta"].update({
        "agent": args.agent,
        "model": args.model,
        "profile": args.profile,
        "headless": args.headless,
        "started_at": datetime.datetime.now().isoformat(),
        "do_retry": not args.no_retry,
    })

    def save():
        tmp = results_path.with_suffix(results_path.suffix + ".tmp")
        tmp.write_text(json.dumps(data, indent=2))
        tmp.replace(results_path)

    total = 0
    n_pass = 0
    for lang in languages:
        desc = LANG_DESCRIPTORS.get(lang)
        if desc is None:
            print(f"[skip] unknown language: {lang}", flush=True)
            continue
        names = sorted(p.name for p in desc["practice_dir"].iterdir() if p.is_dir())
        if args.exercise:
            names = [n for n in names if n == args.exercise]
        if args.limit:
            names = names[:args.limit]
        for name in names:
            key = f"{lang}/{name}"
            if args.resume and data["exercises"].get(key, {}).get("status") in ("pass_1", "pass_2"):
                total += 1
                n_pass += 1
                continue
            print(f"=== {key} ===", flush=True)
            t_start = time.time()
            r = run_exercise(
                lang, name, args.agent, args.model,
                args.profile, args.headless, llama_args,
                do_retry=not args.no_retry, agent_timeout=args.agent_timeout,
            )
            data["exercises"][key] = r
            save()
            total += 1
            if r["status"] in ("pass_1", "pass_2"):
                n_pass += 1
            status = r["status"]
            print(f"  → {status}  {time.time() - t_start:.1f}s  ({n_pass}/{total} passed)",
                  flush=True)

    data["meta"]["finished_at"] = datetime.datetime.now().isoformat()
    data["meta"]["pass_rate"] = round(n_pass / max(1, total), 4)
    data["meta"]["n_pass"] = n_pass
    data["meta"]["n_total"] = total
    save()
    print(f"\n=== DONE: {n_pass}/{total} = {n_pass/max(1,total)*100:.2f}% ===")


if __name__ == "__main__":
    main()
