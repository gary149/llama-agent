#pragma once

// Destroy all subagent runners (joining any remaining background subagent
// threads). Call this at process exit while the parent agent_loop's objects
// (inference backend, session_stats) are still alive: if left to static
// destruction, ~subagent_runner would run after main()'s stack has unwound
// and a still-running subagent thread could touch dangling parent members.
void subagent_shutdown_runners();
