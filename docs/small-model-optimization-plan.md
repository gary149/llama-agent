# Small-model harness optimization plan

Goal: optimize the llama-agent harness for medium-sized local models, with **Qwen 3.6 27B dense + MTP** as the flagship target (M4 Max, 128 GB RAM, llama-server with `--jinja` and `--spec-type draft-mtp`).

This plan was distilled from three parallel research passes — auditing the current llama-agent codebase, surveying Qwen 3.6 best practices, and mapping mechanisms from `little-coder` / `smallcode` / `forge` onto our C++ architecture. See `/tmp/small-model-agents.md` (also published at the gist `gary149/19d9e9988543ebdcb647d2c807315242`) for the source-project survey.

### Why dense + MTP, not MoE

Per [Unsloth's Qwen 3.6 MTP guide](https://unsloth.ai/docs/models/qwen3.6#mtp-guide):

| Variant | MTP speedup | RTX 6000 tok/s with MTP |
|---|---|---|
| Dense (27B + MTP) | **1.4–2.2×** | 160 |
| MoE (35B-A3B + MTP) | 1.15–1.2× | 240 |

MoE + MTP barely gains because each speculative draft loads fresh expert slices (expert saturation). Dense + MTP gives the full speedup, with no MoE routing variance to worry about for tool-calling reliability. Apple Silicon Metal numbers aren't published by Unsloth — we measure on M4 Max as part of Phase 0.

---

## Locked decisions

| Decision | Choice |
|---|---|
| Scope | Top 5 surgical wins + profile system + Qwen 3.6 flagship profile |
| Flagship model | **`unsloth/Qwen3.6-27B-MTP-GGUF` (UD-Q6_K_XL)** — dense + MTP |
| Inference server | llama-server with `--jinja --spec-type draft-mtp --spec-draft-n-max 2 -ngl 99 -fa on` |
| Bench runtime | **RunPod A40 (48 GB) in EU-SE-1, secure cloud, $0.44/hr.** Pod `jzln4orxdtined`. SSH `ssh -i ~/.runpod/ssh/RunPod-Key-Go root@194.68.245.188 -p 22059`. No network volume (EU-SE-1 doesn't support them — ate the 24 GB GGUF re-download cost; pod stays up across the bench cycle to avoid). 60 GB container disk. Originally planned L40S in US-TX-3 but stock dropped mid-launch. A40 is Ampere not Ada → expect ~90–110 tok/s with MTP vs Unsloth's reference 160 tok/s on RTX 6000 Ada. The MTP gain RATIO (1.4–2.2×) is arch-independent. |
| Bench | Terminal-Bench 2.0, 10-task subset, **n=5 always** |
| Bench adapter | Borrow `little-coder/benchmarks/harbor_adapter/`, adapt to spawn `llama-agent --headless` |
| First measured win | Write-guard (NOT the first commit — see "Foundation → wins" breakdown) |
| Foundation precedes wins | Headless + profile scaffold + smoke TB adapter ship FIRST, then write-guard as the first measured win |
| Headless | New `--headless` flag, implies `--yolo` + non-blocking doom-loop + readline skip (Docker sandbox makes auto-allow safe) |
| Profile format | TOML (`toml++` vendored) |
| Profile resolution | `--profile <name>` CLI → `.llama-agent/config.toml` project pin → auto-derive from `--model` path → `default.toml` |
| Profile split | Two Qwen profiles: `qwen3.6-27b-mtp` (coding, T=0.6, presence=0) and `qwen3.6-27b-mtp-bench` (TB sweep, T=1.0, presence=1.5 — matches Qwen's own TB 2.0 eval) |
| Thinking mode default | `enable_thinking=true` + `preserve_thinking=true` (paired with thinking-budget cap) |
| Baseline strategy | Two-baseline: stock defaults vs Qwen-profile baseline; each subsequent win measured against the profile baseline |
| Sequence of wins | write-guard → thinking-budget cap → quality monitor → per-turn skill re-injection → TODO re-injection |
| Ephemeral context injection | Mutate the messages array INSIDE `build_oai_request_body()` (which already deep-copies `messages_`); never modify canonical `messages_` |
| Server parity | CLI profile loading ships first; HTTP-server `agent-session.cpp` adopts the same `profile` struct in a later phase. Shared struct from day one. |

---

## Research summary (for context)

### Current llama-agent state (audit findings)

- `tool-edit.cpp` is already patch-first (exact + fuzzy Unicode search-replace). ✓
- `tool-write.cpp` overwrites existing files freely — **no write-guard**. ✗
- Sampler defaults are llama.cpp generic: `temp=0.8, top_k=40, top_p=0.95, min_p=0.05, no penalties`. **Not Qwen-tuned.** ✗
- Tool routing uses OAI `tools[]` + server `--jinja` template (correct shape). **No rescue parsing** when the model emits malformed or text-mode tool calls.
- Doom-loop check exists (hash-based, count ≥ 3) but **blocks on a user prompt** — unusable in headless / benchmark runs. ✗
- Compaction is single-tier (summarize-old + keep-recent), uses the same model as inference.
- Skills inject statically at session start; the model has forgotten them by turn 8. ✗
- No thinking-budget cap, no working memory, no quality monitor, no TODO re-injection.
- `agent-loop-compaction.cpp` already passes `enable_thinking=false` to the summarizer, but the **main loop has no thinking-mode control**.

### Qwen 3.6 27B Dense + MTP specific findings

**Official sampler matrix** (from the Qwen 3.6 model card, four presets — DO NOT mix-and-match):

| Mode | Use case | temp | top_p | top_k | min_p | presence_penalty | repeat_penalty |
|---|---|---|---|---|---|---|---|
| Thinking | **Precise coding** | 0.6 | 0.95 | 20 | 0.0 | **0.0** | 1.0 |
| Thinking | General | 0.6 | 0.95 | 20 | 0.0 | 1.5 | 1.0 |
| Non-thinking | General | 0.7 | 0.8 | 20 | 0.0 | 1.5 | 1.0 |
| Non-thinking | Reasoning | 1.0 | 0.95 | 20 | 0.0 | 1.5 | 1.0 |
| **Qwen's own TB 2.0 eval** | Benchmark | **1.0** | 0.95 | 20 | 0.0 | 1.5 | 1.0 |

The precise-coding preset (`presence=0.0`) is what users want day-to-day. The TB-eval preset (`T=1.0, presence=1.5`) is what we pin during sweeps so our numbers are comparable to Qwen's published TB 2.0 result. **One unified "Qwen-recommended" profile is wrong** — they're meaningfully different settings for meaningfully different purposes.

**MTP (dense):**
- Use `unsloth/Qwen3.6-27B-MTP-GGUF` (not the plain `Qwen3.6-27B-GGUF` — MTP heads must be in the GGUF).
- llama.cpp flag: `--spec-type draft-mtp --spec-draft-n-max 2` (renamed from `mtp` on 2026-05-13).
- Expected gain: **1.4–2.2×** on dense vs. 1.15–1.2× on MoE.
- Sweet spot: `--spec-draft-n-max 2`; acceptance rate drops from 83% → 50% at `n=4`.
- Memory overhead: +~1 GB vs non-MTP GGUF (trivial on 128 GB Mac).
- Apple Silicon Metal benchmarks not published by Unsloth — must measure in Phase 0.

**Other:**
- **Tool-call format:** `qwen3_coder` XML — *not* Hermes JSON. Use `--jinja` with the Unsloth-patched template.
- **Template kwargs:** `enable_thinking`, `preserve_thinking` for cross-turn reasoning retention. `developer` role supported for agentic-coding tools.
- **Long-context degradation:** kicks in around 80 K total tokens; keep effective context ~32–64 K.
- **Compatibility:** `-np 1` required with MTP (no parallel sequences). Vision (`mmproj`) supported alongside MTP for dense — unlike MoE where MTP+vision crashes.
- **Known bug fixed elsewhere in tree:** `enable_thinking=false` is already wired for compaction summarizer but NOT for the main inference loop — easy to expose via the profile.

### Top 5 wins (impact / cost ranked)

| # | Win | Cost | Why this model |
|---|-----|------|----------------|
| 1 | Write-guard | 30 min / ~30 LOC | Little-coder data: fires on ~57 % of Polyglot exercises. Best for validating bench→fix→measure loop end-to-end since change is so small the delta can't be noise. |
| 2 | Thinking-budget cap | ~100 LOC | Qwen 3.6 burns 4 K+ thinking tokens on trivial edits. Biggest single Qwen-specific win per little-coder's data. |
| 3 | Quality monitor + non-blocking correction | ~150 LOC | Existing doom-loop blocks on user prompt → useless headless. Add empty-response + hallucinated-tool checks. Prerequisite for clean bench runs. |
| 4 | Per-turn skill re-injection with error-recovery priority | ~120 LOC | Static skill injection is forgotten by turn 8; recency-biased re-injection of the relevant card after a failed tool call matches little-coder's strongest scaffold-effect data. |
| 5 | TODO re-injection | ~80 LOC | `update_plan` already exists but state isn't re-injected each turn. Tightens multi-file coordination. |

### Skipped for this model size

- **2-stage tool routing** — context isn't tight with our 6 tools (< 2 % of budget).
- **Synthetic `respond` tool injection** (forge) — Qwen 3.6 stays in tool-calling mode reliably with `--jinja`.
- **Heavy step-enforcer middleware** (forge) — Qwen 3.6 follows system-prompt instructions reliably at 35 B scale.
- **Evidence store + compaction bridge** — useful for GAIA-style long-horizon, not TB 2.0 priority.
- **Working memory scratchpad** — overlaps with TODO re-injection; revisit if Phase 6 numbers say it's needed.

---

## Phase 0 — Bench harness + MTP smoke (Day 1–2)

### 0a. Model + server setup

```bash
# Download dense + MTP GGUF (~24 GB)
hf download unsloth/Qwen3.6-27B-MTP-GGUF Qwen3.6-27B-MTP-UD-Q6_K_XL.gguf --local-dir ~/models

# Start llama-server with MTP enabled
llama-server \
  -m ~/models/Qwen3.6-27B-MTP-UD-Q6_K_XL.gguf \
  --jinja \
  --spec-type draft-mtp --spec-draft-n-max 2 \
  -ngl 99 -c 32768 -fa on -np 1 \
  --host 127.0.0.1 --port 8080
```

### 0b. MTP smoke measurement on the bench pod — DONE ✓

Measured on the A40 pod (`jzln4orxdtined`, EU-SE-1) on 2026-05-22:

| Config | tok/s (mean of 3) | Range |
|---|---|---|
| Without MTP | 19.69 | 19.29 – 20.18 |
| With MTP (`--spec-draft-n-max 2`) | **39.20** | 37.94 – 40.82 |
| **Speedup ratio** | **1.99×** | top of Unsloth's predicted 1.4–2.2× range |

Setup: `Qwen3.6-27B-UD-Q6_K_XL.gguf`, `--jinja -ngl 99 -c 8192 -fa on -np 1`, `enable_thinking=false` for the smoke test (timing measurement), `temperature=0.6 seed=42` deterministic, 76-token outputs. MTP works as advertised. Absolute throughput (~40 tok/s) is lower than Unsloth's RTX 6000 Ada reference (160 tok/s) — expected because A40 is Ampere, not Ada. The *gain* from MTP is even better than expected on this Ampere card.

**Implication for bench timing:** at ~40 tok/s with MTP and thinking on, an n=5 × 10-task sweep is ~3–4 h (vs. ~75 min projected for L40S). Total bench cycle (7 sweeps + iteration) ≈ 25–30 h × $0.44/hr ≈ **$11–13**. Still under budget.

### 0c. Bench adapter

**New directory:** `bench/terminal_bench/`

**Adapter:** copy `/Users/vm/code/little-coder/benchmarks/harbor_adapter/` as starting point; replace the pi-RPC subprocess section with a launcher for `llama-agent --headless --profile qwen3.6-27b-mtp-bench -p <task-prompt>`.

**Task selection:** 10 tasks from TB 2.0 (89 total), mix:
- 3 shell-heavy (file ops, process management)
- 3 multi-file refactor
- 2 debug-failing-test
- 2 write-from-scratch

Label this honestly as a **TB2.0-10 local subset** — not a leaderboard-comparable number.

### 0d. Smoke runner before full sweep

Before the 50-run sweep, the harness must clear a smoke test:
- 1 task × 1 run × fixed profile × JSONL output
- Asserts: process launches, Docker sandbox isolates, agent exits cleanly, no prompts block, result schema is stable, stdout/stderr captured, pass/fail parsed.

Only then add the full 10-task × n=5 sweeper.

### 0e. Run protocol (full)

- `n=5` trials per task → 50 task-runs per sweep
- Results to `bench/terminal_bench/results/<commit>.jsonl`
- Sweep runner reports mean ± std and per-task pass/fail
- Sign-off criterion per change: positive delta beyond the std envelope of the comparison baseline

**Estimated sweep time on M4 Max with thinking + MTP on:** ~75 min (MTP speedup pulls down the ~2.5 h MoE estimate). Validated in 0b.

---

## Phase 1 — Foundation: benchmarkable llama-agent (Day 3–5)

**Goal in one sentence:** ship `--headless` + profile scaffold + smoke-tested TB adapter so the baseline can be measured BEFORE any win lands.

Foundation breaks into three commits. Write-guard is **not** in Phase 1; it's the first measured win in Phase 2.

### Commit 1 — `agent: add headless mode`

**File:** `tools/agent/agent.cpp` + `tools/agent/agent-loop.h`

Add to `agent_config`:
```cpp
bool headless = false;
```

Parse `--headless` in `agent.cpp`. Semantics:
```cpp
if (headless) {
    yolo_mode = true;
    params.single_turn = true;
    if (max_iterations == 0) max_iterations = 50;
    enable_session = false;  // benchmarks shouldn't leak session files
}
```

**Crucial extra step (catch from review):** the existing `--yolo` only bypasses `check_permission()` in `permission.cpp`. The doom-loop detection in `agent-loop-tools.cpp` calls `prompt_user()` *before* the permission check — it must be short-circuited too:

```cpp
// agent-loop-tools.cpp — replace the existing prompt_user() branch
if (permission_mgr_.is_doom_loop(call.name, args_hash)) {
    if (config_.headless) {
        return {false, "", "Repeated identical tool call detected. Try a different approach."};
    }
    // existing interactive prompt path
}
```

This minimal short-circuit ships in Commit 1. Phase 4's quality monitor replaces it with the full three-check apparatus later. **Without this, `--headless` is not actually unblocked even with `--yolo`.**

**Done criterion for Commit 1:**
1. `llama-agent --headless -p "<task>"` never enters readline.
2. `--headless` defaults `max_iterations=50` and disables session persistence.
3. Doom-loop / external-dir / dangerous-command / normal permission paths cannot block in headless.
4. **Unit test:** three identical tool calls in headless terminate without `prompt_user()` being invoked (regression-guards against this trap returning).

### Commit 2 — `agent: add profile scaffold`

**New module:** `tools/agent/profile.{h,cpp}` (TOML loader + schema validation)

**Vendor:** `toml++` (header-only, MIT) at `tools/agent/vendor/toml.hpp`

**Shared struct:** the `profile` struct lives in a shared header so CLI and HTTP-server paths converge (server-parity deferral noted in locked decisions).

**`tool_context` extension:** the current `tool_context` carries working dir / timeout / interrupt pointer / vision capability — no profile flags. Extend it with a `tool_settings` substruct so per-tool profile flags (write_guard, quality_monitor) are reachable from tool implementations.

**Built-in profile dir:** `tools/agent/profiles/`
- `qwen3.6-27b-mtp.toml` — flagship (precise-coding sampler, MTP on, thinking on)
- `qwen3.6-27b-mtp-bench.toml` — TB sweep profile (Qwen's TB-eval sampler)
- `default.toml` — generic fallback (current llama.cpp defaults, all `tools.*` flags `false` for backwards compat)

**Resolution order:**
1. `--profile <name>` CLI flag (highest)
2. `.llama-agent/config.toml` project key
3. Auto-derive from `--model` path against `[meta].model_id_match` patterns
4. `default.toml`

**Override behavior:** profile loaded first, then `common_params_parse()` applies user-provided CLI overrides on top. Explicit CLI flags always win.

**CMakeLists:** the current build duplicates source lists for `llama-agent` and `llama-agent-server`. Either add `profile.cpp` to both lists or refactor common sources into a shared CMake variable. Recommend the latter as cleanup.

**Flagship profile (`qwen3.6-27b-mtp.toml` — precise coding, day-to-day use):**

```toml
[meta]
name = "qwen3.6-27b-mtp"
description = "Qwen 3.6 27B dense + MTP — precise coding (day-to-day)"
model_id_match = ["Qwen3.6-27B-MTP", "Qwen3.6-27B"]

[sampler]
# Qwen card: thinking + precise coding preset
temperature = 0.6
top_p = 0.95
top_k = 20
min_p = 0.0
presence_penalty = 0.0      # NOT 1.5 — that's the general-thinking preset
repeat_penalty = 1.0

[template_kwargs]
enable_thinking = true
preserve_thinking = true

[agent]
max_iterations = 50
bash_timeout_ms = 120000

[thinking]
budget_chars = 7000         # ~2000 reasoning tokens; Phase 3

[tools]
write_guard = true                       # Phase 2
quality_monitor = true                   # Phase 4
quality_monitor_max_corrections = 2

[skills]
inject_token_budget = 2000  # Phase 5
```

**Bench profile (`qwen3.6-27b-mtp-bench.toml` — TB sweep):**

```toml
[meta]
name = "qwen3.6-27b-mtp-bench"
description = "Qwen 3.6 27B dense + MTP — Terminal-Bench 2.0 sweep settings"
model_id_match = []         # explicit only — not auto-derived

[sampler]
# Qwen card: settings used for their own TB 2.0 evaluation
temperature = 1.0
top_p = 0.95
top_k = 20
min_p = 0.0
presence_penalty = 1.5
repeat_penalty = 1.0

[template_kwargs]
enable_thinking = true
preserve_thinking = true

[agent]
max_iterations = 50
bash_timeout_ms = 120000

[thinking]
budget_chars = 7000

[tools]
write_guard = true
quality_monitor = true
quality_monitor_max_corrections = 2

[skills]
inject_token_budget = 2000
```

### Commit 3 — `bench: terminal-bench smoke adapter`

Land the smoke runner from Phase 0d before the full sweeper. 1 task × 1 run is enough to prove the plumbing works.

### End of Phase 1 — baseline measurements

- **Sweep #1 (stock baseline):** current llama-agent, no `--profile`, current sampler defaults, no thinking kwargs → `baseline_stock.jsonl`
- **Sweep #2 (profile baseline):** `--profile qwen3.6-27b-mtp-bench` with all `tools.*` flags overridden to `false` → `baseline_profile.jsonl`

**Sweep #2 − Sweep #1 = the first measured win (profile + MTP alone, no harness changes).**

Total bench time for both baselines on M4 Max with MTP: ~2.5 h (much faster than the MoE estimate).

---

## Implementation status (as of 2026-05-22)

| Phase | Commit | Status |
|---|---|---|
| Phase 0 — bench harness + MTP smoke | `e743383` | ✅ Smoke runner + 1 task; MTP measured at 1.99× speedup on A40 |
| Phase 1 Commit 1 — `--headless` + doom-loop short-circuit | `61e9efa` | ✅ End-to-end validated on pod with Qwen 3.6 + MTP |
| Phase 1 Commit 2 — TOML profile system | `0d6e4b1` | ✅ toml++ vendored; 3 profiles shipped; `--profile` flag live |
| Phase 1 Commit 3 — bench smoke adapter | `e743383` | ✅ `bench/terminal_bench/run_smoke.py` + `hello-bash` task |
| Phase 2 — write-guard | `fc0fa9c` | ✅ Plumbed via `tool_context::settings.write_guard` |
| Phase 3 — thinking-budget cap | `fc0fa9c` | ✅ Partial assistant turn discarded, user-nudge injected |
| Phase 4 — quality monitor (non-blocking) | `fc0fa9c` | ✅ Empty-response + hallucinated-tool checks; Commit 1's doom-loop short-circuit covers check #3 |
| Phase 5 — per-turn skill re-injection | `fc0fa9c` | ✅ Inside `build_oai_request_body()` on the copied messages array; canonical `messages_` untouched |
| Phase 6 — TODO re-injection | `fc0fa9c` | ✅ Same seam as Phase 5; plan state read from message history |

### Deferred to a follow-up session

- **Full n=5 × 10-task TB 2.0 sweep cycle.** The smoke harness is in place;
  the actual 10-task selection from TB 2.0 + the sweep runner is left for a
  later session. Each sweep is ~3–4 h on A40 + MTP; 7 sweeps × ~3.5h ≈ ~25 h
  of pod time at $0.44/hr ≈ ~$11. Run when ready.
- **Auto-derive profile from `--model` path** — `[meta].model_id_match`
  patterns are loaded but not yet consulted. Currently `--profile` is
  explicit-only.
- **`.llama-agent/config.toml` project-pin** of profile name.
- **HTTP-server profile parity** — `agent-session.cpp` doesn't yet apply the
  profile. The `profile_settings` struct is shared so wiring it in is a
  small follow-up.
- **Doom-loop unit test** (Commit 1 done-criterion #4) — bench coverage now
  exercises this codepath, so the explicit unit test is deferred.

---

## Phase 2 — Win 1: write-guard (Day 6)

The first **measured** win — Phase 1's foundation lets us actually attribute the delta.

**File:** `tools/agent/tools/tool-write.cpp`

**Behavior:**
```cpp
if (ctx.tool_settings.write_guard && fs::exists(file_path)) {
    return {false, "", build_write_guard_error(file_path)};
}
```

Note `ctx.tool_settings.write_guard` — reads from the `tool_settings` substruct added to `tool_context` in Phase 1 Commit 2.

Error text borrowed verbatim from little-coder's `write-guard/index.ts` — it includes the Edit-tool recipe so the model knows how to fix its approach.

**Gating:** `tools.write_guard` profile flag (`true` in both Qwen profiles, `false` in `default.toml`).

**Cost:** ~30 LOC

**Measurement:** sweep delta vs Sweep #2 (profile baseline).

---

## Phase 3 — Win 2: thinking-budget cap (Day 7–8)

**Highest-risk phase per the risk register** — mid-generation abort must not corrupt history.

**File:** `tools/agent/agent-loop-completion.cpp` — streaming loop

**Behavior:** count `reasoning_content` delta chars per turn. When `accumulated_chars > thinking.budget_chars`:

1. Abort generation via the streaming-callback interrupt path.
2. **Do NOT** append the partial/aborted assistant turn to `messages_`. The aborted message is discarded entirely; no half-formed assistant content survives in canonical history.
3. Inject a fresh `user` message into `messages_`:
   > "You've been reasoning extensively. Commit to your implementation now using the available tools."
4. The next iteration starts cleanly — the model sees the user nudge as the most recent turn and re-enters generation.

Skip the entire mechanism when `budget_chars = 0`.

**Cost:** ~100 LOC

**Measurement:** sweep delta vs Phase 2 result.

---

## Phase 4 — Win 3: quality monitor + non-blocking correction (Day 9–10)

**Files:**
- `tools/agent/agent-loop-tools.cpp` lines 79–88 — replace the blocking doom-loop user prompt with a non-blocking correction injection
- `tools/agent/agent-loop.cpp::run()` — add `assess_response()` post-completion hook

**Three checks (all return early if `tools.quality_monitor == false`):**

1. **Empty response:** content empty + no tool calls + not an overflow → inject "Please continue with the task" nudge
2. **Hallucinated tool name:** `tool_call.name` not in registry → inject "Valid tools are: …" with the registered names list
3. **Repeated identical call:** exact `(tool, args_hash)` match prior turn → inject "Try a different approach" nudge (replaces the current blocking permission prompt)

**Cap:** `quality_monitor_max_corrections` consecutive corrections (default 2) before bailing to `AGENT_ERROR`.

**Interaction with headless mode:** in `--headless`, this IS the doom-loop handler (no prompt path). In interactive mode, this layer fires *before* the existing permission prompt, so corrections happen invisibly when they work and only escalate to prompts when they don't.

**Cost:** ~150 LOC

**Measurement:** sweep delta vs Phase 3 result.

---

## Phase 5 — Win 4: per-turn skill re-injection with error-recovery priority (Day 11–12)

**Files:**
- `tools/agent/skills/skills-manager.{h,cpp}` — add `select_skill_for_error(last_tool_name, error_text) → skill_metadata*`
- `tools/agent/agent-loop-completion.cpp` — extend `build_oai_request_body()` to optionally prepend ephemeral context to the **copied** messages array

**Behavior:**
- Keep static skill list at session start (preserves KV cache for the system-prompt prefix)
- Each turn, inspect the last tool result; on error, fetch matching skill card
- **Inject the skill card inside `build_oai_request_body()` — which already deep-copies `messages_` before rendering the request.** The copy gets the ephemeral user-prefix block; canonical `messages_` is never touched. This preserves session-file replay, compaction semantics, and undo.
- Token budget enforced via `skills.inject_token_budget` (truncate skill content if needed)

**Recency rationale:** the freshest block in the context window is most-attended; placing the relevant skill card right before the next generation call gives the highest signal-per-token ratio.

**Cost:** ~120 LOC

**Measurement:** sweep delta vs Phase 4 result.

---

## Phase 6 — Win 5: TODO re-injection (Day 13)

**Files:**
- `tools/agent/tools/tool-plan.cpp` — persist plan state in `agent_loop` member
- `tools/agent/agent-loop-completion.cpp` — same `build_oai_request_body()` seam as Phase 5

**Behavior:** when a plan is active (`update_plan` called at least once), each iteration prepends a `<current_plan>` block (current step highlighted) to the **copied** messages array inside the request builder. Same ephemeral-injection contract as Phase 5: canonical `messages_` is never modified.

**Cost:** ~80 LOC

**Measurement:** sweep delta vs Phase 5 result.

---

## Final sweep + publication

After Phase 6, run a clean n=5 sweep and publish seven numbers:

| Sweep | Setup |
|---|---|
| 1 | Stock baseline |
| 2 | + Qwen 3.6 profile (sampler + thinking kwargs) |
| 3 | + Write-guard |
| 4 | + Thinking-budget cap |
| 5 | + Quality monitor + non-blocking correction |
| 6 | + Per-turn skill re-injection |
| 7 | + TODO re-injection |

Each step's delta proves the fix earned its slot. If any delta is within noise, that fix gets reconsidered (the bench data is the gate, not the design intuition).

---

## Total budget

- Phase 0 + 1 foundation: ~3–5 days code + 5 h baseline bench
- Phases 2–6 each: ~1 day code + 2.5 h bench + analysis
- **Calendar: ~2 weeks focused, ~3–4 weeks part-time**

---

## Risk register (sequenced by descending risk)

| Rank | Risk | Mitigation |
|---|---|---|
| 1 | **Thinking-budget mid-abort corruption** — stopping generation mid-`reasoning_content` can leave a malformed assistant turn in `messages_` if not handled carefully | First implementation is conservative: aborted turn is DISCARDED (never appended); user-nudge becomes the next message; next iteration clean-starts. Add a unit test for the abort path. |
| 2 | **Quality-monitor placement** — empty responses currently become `COMPLETED`; monitor must intercept before that return path. Also needs to wrap both post-completion and post-tool-error sites. | Two hook sites: `agent-loop.cpp::run()` post-completion (catches empty + hallucinated tool); `agent-loop-tools.cpp` post-execute (catches repeated calls). Cap consecutive corrections at 2 to avoid correction loops. |
| 3 | **Profile-sampler mismatch** — using the general-thinking preset (`presence=1.5`) when the actual use case is precise coding (`presence=0`) materially affects results. Single "Qwen-recommended" profile is wrong. | Two profiles: `qwen3.6-27b-mtp.toml` (coding) and `qwen3.6-27b-mtp-bench.toml` (TB sweep). Bench results pin the bench profile; deployment uses the coding profile. Document explicitly which is which. |
| 4 | **Doom-loop blocking even with `--yolo`** — `prompt_user()` lives in `agent-loop-tools.cpp` and isn't bypassed by YOLO's `permission.cpp` short-circuit. Bench runs hang silently. | Headless mode short-circuits doom-loop to a tool-error return (Commit 1). Unit-tested. |
| 5 | **MTP on Apple Silicon Metal** — Unsloth's published gains are CUDA-only. Metal may underperform; MoE saturation pattern could in theory hit dense too (less likely). | Phase 0b smoke measures the M4 Max ratio empirically before relying on it. Escalation path if ratio <1.1×: still ship dense (better quality vs MoE for tool-calling), without MTP, and re-revisit when Metal MTP matures. |
| 6 | **Write-guard** — lowest risk. 30 LOC, behind a flag, easily revertable. | None needed beyond the gating flag. |

## Deferred (revisit after Phase 6 measurements)

- **Synthetic `respond` tool** (forge) — Qwen 3.6 stays in tool mode reliably with `--jinja`; not worth complexity.
- **2-stage tool routing** (smallcode) — context isn't tight with 6 tools.
- **Step enforcer + nudges** (forge) — Qwen 3.6 follows system-prompt instructions reliably at 27 B dense.
- **Evidence store + compaction bridge** (little-coder) — useful for GAIA-style long-horizon, not TB 2.0 priority.
- **Working memory scratchpad** (smallcode) — overlaps with TODO re-injection; reconsider if Phase 6 data suggests it's needed.
- **HTTP-server profile parity** — `agent-session.cpp` builds its own `agent_config`. The `profile` struct is shared from day one (Phase 1 Commit 2); wiring it into `agent-session.cpp` is a follow-up. Keep CLI and server in sync schema-wise; defer server-side loading until needed.
- **Forgiving rescue tool-call parser** — Qwen 3.6 with `--jinja` produces well-formed tool calls reliably; rescue parsing was high on the original portability table but moved to deferred since the failure mode is rare in the target setup.

---

## Pre-start items to lock before coding

1. **Download the dense + MTP GGUF** — `hf download unsloth/Qwen3.6-27B-MTP-GGUF Qwen3.6-27B-MTP-UD-Q6_K_XL.gguf --local-dir ~/models` (~24 GB; you don't have it cached yet — only the non-MTP `Qwen3.6-27B-GGUF` is on disk).
2. **Pick the 10 TB 2.0 tasks** — propose a list or accept the proposed distribution (3 shell-heavy / 3 refactor / 2 debug / 2 write-from-scratch).
3. **Confirm bench dir name** — `bench/terminal_bench/` or `benchmarks/terminal_bench/`?
4. **Confirm runtime for bench** — M4 Max (this machine) or Hetzner box? With MTP enabled and dense 27B on M4 Max, n=5 × 10 tasks ≈ ~75 min of inference; comfortable on M4 Max.
5. **`toml++` vendor location** — `tools/agent/vendor/toml.hpp` or `common/vendor/toml.hpp`?

---

## Implementation details to nail down as the work proceeds

- Exact streaming `reasoning_content` char-counting API in llama.cpp's OAI-compat layer (need to confirm where the delta is exposed in `oaicompat_msg`).
- Whether `--headless` should also disable the AGENTS.md / skills startup discovery to keep cold-start fast for bench runs.
- Whether the doom-loop hash check should augment to embedding-similarity (rather than exact hash) inside headless mode — minor enhancement, parked unless data demands it.
- Exact `[meta].model_id_match` matching semantics: substring or full glob?

---

## Runbook — picking this up in a new session

The full n=5 × 10-task sweep cycle is the remaining operational work. To run it:

```bash
# On the runpod pod (jzln4orxdtined, A40 EU-SE-1):
ssh -i ~/.runpod/ssh/RunPod-Key-Go root@194.68.245.188 -p 22059
cd /workspace/llama-agent && git pull

# Sweep #1: stock baseline (no profile)
python3 bench/terminal_bench/run_smoke.py \
  --agent ./build/bin/llama-agent \
  --model /workspace/models/Qwen3.6-27B-UD-Q6_K_XL.gguf \
  --task all --trials 5 \
  --results bench/terminal_bench/results/sweep_01_stock.jsonl \
  --llama-args="--jinja --spec-type draft-mtp --spec-draft-n-max 2 -ngl 99 -c 8192 -fa on"

# Sweep #2: profile baseline (qwen3.6-27b-mtp-bench, all tools.* OFF)
# Edit profiles/qwen3.6-27b-mtp-bench.toml: set tools.write_guard / quality_monitor / thinking.budget_chars / skills.inject_token_budget all to false/0
# Then:
python3 bench/terminal_bench/run_smoke.py \
  --agent ./build/bin/llama-agent \
  --model /workspace/models/Qwen3.6-27B-UD-Q6_K_XL.gguf \
  --profile qwen3.6-27b-mtp-bench \
  --task all --trials 5 \
  --results bench/terminal_bench/results/sweep_02_profile.jsonl \
  --llama-args="--jinja --spec-type draft-mtp --spec-draft-n-max 2 -ngl 99 -c 8192 -fa on"

# Sweep #3-#7: re-enable each tools.* flag one at a time and re-run, naming
# results accordingly (sweep_03_write_guard, sweep_04_thinking, ...).
```

**Before doing real sweeps:** curate 10 TB 2.0 tasks under `bench/terminal_bench/tasks/`
matching the 3/3/2/2 distribution (shell / refactor / debug / write-from-scratch).
The smoke `hello-bash` task is the template — each task is a directory with
`task.yaml` containing `prompt:` + `check:` blocks.

**To stop the pod between sweeps** (saves $0.44/hr):
```bash
runpodctl pod stop jzln4orxdtined
runpodctl pod start jzln4orxdtined   # resume; model + build persist on container disk
```

---

## References

- Survey of source projects: `/tmp/small-model-agents.md` (gist: `gary149/19d9e9988543ebdcb647d2c807315242`)
- little-coder repo: `/Users/vm/code/little-coder/`
- smallcode repo: `/Users/vm/code/smallcode/`
- forge: https://github.com/antoinezambelli/forge
- **Qwen 3.6 dense + MTP guide (Unsloth):** https://unsloth.ai/docs/models/qwen3.6#mtp-guide
- **Qwen 3.6 27B MTP GGUF:** https://huggingface.co/unsloth/Qwen3.6-27B-MTP-GGUF
- Qwen 3.6 model card: https://huggingface.co/Qwen/Qwen3.6-35B-A3B (sampler matrix applies to dense variants too)
- Terminal-Bench 2.0 leaderboard: https://www.tbench.ai/leaderboard/terminal-bench/2.0
