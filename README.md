# cagent

[中文说明](README.zh-CN.md)

`cagent` is a Claude Code-like command line coding agent implemented in C.

It is a local runtime that can index a workspace, talk to an LLM provider, parse structured tool calls, execute file/system/Git tools, and ask for confirmation before risky operations.

The project is still an MVP, but the core runtime is already usable for local experiments.

## What It Can Do

- Start an interactive REPL with `cagent`
- Run a single prompt with `cagent "your request"`
- Read prompt text from stdin
- Load JSON config from the default config path or `--config <path>`
- Initialize a default config file with `cagent config init`
- Index the current workspace and detect common project types
- Use a fake LLM provider for offline Agent Loop testing
- Use an OpenAI-compatible LLM provider
- Execute structured Tool Protocol actions
- Read, search, create, edit, patch, and append workspace files
- Preview file diffs before writing
- Run guarded shell commands
- Inspect and operate on local Git repositories
- Enforce SAFE / ASK / DANGEROUS permission levels
- Keep tool outputs bounded and JSON-valid for large payloads

## Current Status

Implemented:

- CLI / REPL
- Config system
- Project indexer
- Tool Registry / Tool Executor
- JSON utilities
- Permission Manager
- Fake LLM Agent Loop
- OpenAI-compatible provider
- Read-only file tools
- Write file tools
- Diff preview
- Exact edit tool
- Strict unified patch tool
- Guarded command execution
- Git tools
- Robust LLM action JSON extraction and retry
- Bounded large payload handling

Not implemented yet:

- Native Gemini provider
- Native Claude provider
- Session memory
- Multi-model routing
- Streaming output
- Delete-file tool
- Packaging / release automation

## Build

Requirements:

- CMake 3.16+
- Ninja
- C17 compiler
- libcurl

On Windows, the project is currently tested with MSYS2 UCRT64 / MinGW and PowerShell.

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

Run:

```powershell
.\build\cagent.exe --version
.\build\cagent.exe --help
```

Small-payload build:

```powershell
cmake -S . -B build-small -G Ninja -DCAGENT_ENABLE_LARGE_PAYLOAD=OFF
cmake --build build-small
```

Optional CMake flags:

```text
CAGENT_ENABLE_SHELL=ON|OFF
CAGENT_ENABLE_GIT=ON|OFF
CAGENT_ENABLE_LARGE_PAYLOAD=ON|OFF
```

## Quick Start

Initialize config:

```powershell
.\build\cagent.exe config init
```

Start REPL:

```powershell
.\build\cagent.exe
```

Run a single prompt:

```powershell
.\build\cagent.exe "读取 README.md 并总结"
```

Read from stdin:

```powershell
"分析这个项目" | .\build\cagent.exe
```

Show registered tools:

```powershell
.\build\cagent.exe /tools
```

Show project summary:

```powershell
.\build\cagent.exe /project
```

Show current config:

```powershell
.\build\cagent.exe /config
```

## Config

Default config path:

- Windows: `%USERPROFILE%\.cagent\config.json`
- Linux/macOS: `~/.cagent/config.json`

Create the default template:

```powershell
.\build\cagent.exe config init
```

Overwrite existing config:

```powershell
.\build\cagent.exe config init --force
```

Use a custom config:

```powershell
.\build\cagent.exe --config .\config.json "hello"
```

Example config:

```json
{
  "default_provider": "fake_local",
  "providers": {
    "fake_local": {
      "type": "fake",
      "compat_profile": "generic",
      "base_url": "",
      "api_key_env": "",
      "model": "fake"
    },
    "newapi_main": {
      "type": "openai_compat",
      "compat_profile": "newapi",
      "base_url": "https://your-newapi.example.com/v1",
      "api_key_env": "NEWAPI_API_KEY",
      "model": "gpt-4.1"
    },
    "deepseek_main": {
      "type": "openai_compat",
      "compat_profile": "deepseek",
      "base_url": "https://api.deepseek.com",
      "api_key_env": "DEEPSEEK_API_KEY",
      "model": "deepseek-chat"
    }
  },
  "agent": {
    "max_steps": 8,
    "temperature": 0.2,
    "stream": false
  },
  "permission": {
    "default_write": "ask",
    "default_shell": "ask"
  }
}
```

API keys are not stored in `config.json`. Put them in environment variables:

```powershell
$env:NEWAPI_API_KEY="your_key"
```

## CLI Commands

```text
cagent
cagent "prompt"
echo "prompt" | cagent
cagent --help
cagent --version
cagent --config <path> "prompt"
cagent config init
cagent config init --force
```

Slash commands:

```text
/help
/exit
/quit
/config
/config init
/project
/tools
/tool-test <tool_name> [arguments_json]
```

## Tool Examples

Read a file:

```powershell
.\build\cagent.exe /tool-test read_file '{"path":"README.md"}'
```

Search code:

```powershell
.\build\cagent.exe /tool-test search_code '{"query":"ca_config","max_results":5}'
```

Preview a change without writing:

```powershell
.\build\cagent.exe /tool-test preview_file_change '{"path":"README.md","new_content":"hello\n"}'
```

Create a file. This asks for confirmation:

```powershell
.\build\cagent.exe /tool-test create_file '{"path":"tmp_note.txt","content":"hello\n"}'
```

Run a guarded command. This asks for confirmation:

```powershell
.\build\cagent.exe /tool-test execute_command '{"command":"cmake --build build","timeout_ms":120000}'
```

Show Git status:

```powershell
.\build\cagent.exe /tool-test git_status '{}'
```

## Registered Tools

Test tools:

- `noop`
- `echo`
- `ask_test`
- `danger_test`

Project and read-only file tools:

- `get_project_summary`
- `list_directory`
- `read_file`
- `read_file_range`
- `search_files`
- `search_code`

Write and edit tools:

- `create_file`
- `write_file`
- `append_file`
- `preview_file_change`
- `edit_file`
- `apply_patch`

Command and Git tools:

- `execute_command`
- `git_status`
- `git_diff`
- `git_log`
- `git_show`
- `git_branch`
- `git_add`
- `git_unstage`
- `git_commit`
- `git_create_branch`
- `git_restore_file`

## Permission Model

Every tool has a permission level:

- `SAFE`: runs directly
- `ASK`: asks for `y/N`
- `DANGEROUS`: requires typing `YES`

Examples:

- Read/search tools are `SAFE`
- Create/write/append/edit/patch tools are `ASK`
- Shell command execution is `ASK`
- `git_restore_file` is `DANGEROUS`

The Tool Executor runs validation before permission prompts when a tool provides preflight checks. Invalid paths such as `../outside.txt`, `.git/config`, or `build/output.txt` are rejected before asking the user.

## Workspace Safety

File tools are sandboxed to the current workspace:

- Empty paths are rejected
- `..` traversal is rejected
- Absolute paths outside the workspace are rejected
- `.git/` and `build/` are protected
- Binary file reads are rejected
- Large reads and outputs are capped
- Tool results are JSON and bounded

This is not a complete operating-system sandbox. Treat shell and Git tools as local developer conveniences guarded by prompts, not as a security boundary.

## Agent Loop

Natural language input goes through the Agent Loop:

```text
user input
-> prompt builder
-> LLM provider
-> JSON action parser
-> Tool Executor
-> Permission Manager
-> tool result observation
-> next LLM step
-> final answer
```

The model is expected to output Tool Protocol JSON:

```json
{
  "type": "tool_call",
  "tool": "read_file",
  "arguments": {
    "path": "README.md"
  },
  "reason": "Need to inspect the project README."
}
```

Or:

```json
{
  "type": "final_answer",
  "content": "Done."
}
```

The runtime can extract JSON actions from fenced code blocks or model output with surrounding text, then retry with an observation if the action is invalid.

## Project Structure

```text
include/              Public headers
src/agent/            Agent Loop, action parser, prompt builder
src/cli/              CLI and REPL
src/config/           Config loading and config init
src/core/             Shared runtime helpers
src/json/             JSON utilities
src/llm/              Fake and OpenAI-compatible providers
src/permission/       Permission prompts and policy
src/platform/         Console, argv, process abstraction
src/project/          Workspace indexer and project detector
src/tool/             Tool registry and executor
src/tools/            File, edit, patch, command, Git tools
```

## Development Notes

This repository intentionally keeps the core runtime in C:

- No Python helper runtime
- No large JSON library dependency
- C17 only
- Bounded buffers and explicit status codes
- Feature flags for shell, Git, and large payload support

Note: detailed phase plans, prompt drafts, and example flows are kept as local development materials and are not part of the public repository snapshot.

## License

See `LICENSE`.
