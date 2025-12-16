# llama-agent

An agentic coding assistant built on [llama.cpp](https://github.com/ggml-org/llama.cpp).

## What is it?

`llama-agent` transforms a local LLM into a coding assistant that can autonomously complete programming tasks. It uses tool calling to read files, write code, run commands, and navigate codebases.

## Quick Start

```bash
# Build
cmake -B build
cmake --build build --target llama-agent

# Run (downloads model automatically)
./build/bin/llama-agent -hf unsloth/Qwen3-30B-A3B-GGUF:Q4_K_M

# Or with a local model
./build/bin/llama-agent -m model.gguf
```

## Recommended Models

Models with good tool-calling support:

| Model | Command |
|-------|---------|
| Qwen3 30B A3B | `-hf unsloth/Qwen3-30B-A3B-GGUF:Q4_K_M` |
| Qwen3 32B | `-hf unsloth/Qwen3-32B-GGUF:Q4_K_M` |

## Available Tools

| Tool | Description |
|------|-------------|
| `bash` | Execute shell commands |
| `read` | Read file contents with line numbers |
| `write` | Create or overwrite files |
| `edit` | Search and replace in files |
| `glob` | Find files matching a pattern |

## Usage Examples

```
> Find all TODO comments in src/

[Tool: bash] grep -r "TODO" src/
Found 5 TODO comments...

> Read the main.cpp file

[Tool: read] main.cpp
   1| #include <iostream>
   2| int main() {
   ...

> Fix the bug on line 42

[Tool: edit] main.cpp
Replaced "old code" with "fixed code"
```

## Commands

| Command | Description |
|---------|-------------|
| `/exit` | Exit the agent |
| `/clear` | Clear conversation history |
| `/tools` | List available tools |

## Permission System

The agent asks for confirmation before:
- Running shell commands
- Writing or editing files

When prompted: `y` (yes), `n` (no), `a` (always allow), `N` (never allow)

## License

MIT - see [LICENSE](LICENSE)
