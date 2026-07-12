# cagent

[English](README.md)

`cagent` 是一个使用 C 语言实现的 Claude Code-like 命令行编程 Agent。

它是一个本地 Runtime：可以索引当前工作区、连接 LLM Provider、解析结构化工具调用、执行文件 / 命令 / Git 工具，并在高风险操作前向用户确认。

项目目前仍处于 MVP 阶段，但核心 Runtime 已经可以用于本地实验和迭代。

## 当前能力

- 通过 `cagent` 进入交互式 REPL
- 通过 `cagent "你的需求"` 执行单次 Prompt
- 支持从 stdin 读取 Prompt
- 支持默认路径配置文件和 `--config <path>`
- 支持 `cagent config init` 初始化默认配置
- 自动索引当前 workspace，并检测常见项目类型
- 支持 fake LLM provider，用于离线验证 Agent Loop
- 支持 OpenAI-compatible LLM provider
- 支持结构化 Tool Protocol action
- 支持读取、搜索、创建、编辑、patch、追加 workspace 文件
- 支持写入前 diff 预览
- 支持受保护的 shell 命令执行
- 支持本地 Git 查看与操作
- 支持 SAFE / ASK / DANGEROUS 权限等级
- 对大 payload 做边界限制，并保持工具输出为合法 JSON

## 当前状态

已实现：

- CLI / REPL
- 配置系统
- 项目索引器
- Tool Registry / Tool Executor
- JSON utilities
- Permission Manager
- Fake LLM Agent Loop
- OpenAI-compatible provider
- 只读文件工具
- 写入文件工具
- Diff preview
- 精确字符串编辑工具
- 严格 unified patch 工具
- 受保护的命令执行
- Git 工具
- LLM action JSON 提取与 retry
- bounded large payload handling

暂未实现：

- Gemini 原生 provider
- Claude 原生 provider
- Session memory
- 多模型路由
- 流式输出
- delete-file 工具
- 打包与发布自动化

## 构建

依赖：

- CMake 3.16+
- Ninja
- C17 编译器
- libcurl

Windows 下目前主要使用 MSYS2 UCRT64 / MinGW 和 PowerShell 测试。

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

运行：

```powershell
.\build\cagent.exe --version
.\build\cagent.exe --help
```

小 payload 构建：

```powershell
cmake -S . -B build-small -G Ninja -DCAGENT_ENABLE_LARGE_PAYLOAD=OFF
cmake --build build-small
```

可选 CMake 开关：

```text
CAGENT_ENABLE_SHELL=ON|OFF
CAGENT_ENABLE_GIT=ON|OFF
CAGENT_ENABLE_LARGE_PAYLOAD=ON|OFF
```

## 快速开始

初始化配置：

```powershell
.\build\cagent.exe config init
```

进入 REPL：

```powershell
.\build\cagent.exe
```

执行单次 Prompt：

```powershell
.\build\cagent.exe "读取 README.md 并总结"
```

从 stdin 读取：

```powershell
"分析这个项目" | .\build\cagent.exe
```

查看已注册工具：

```powershell
.\build\cagent.exe /tools
```

查看项目摘要：

```powershell
.\build\cagent.exe /project
```

查看当前配置：

```powershell
.\build\cagent.exe /config
```

## 配置

默认配置路径：

- Windows: `%USERPROFILE%\.cagent\config.json`
- Linux/macOS: `~/.cagent/config.json`

创建默认配置模板：

```powershell
.\build\cagent.exe config init
```

覆盖已有配置：

```powershell
.\build\cagent.exe config init --force
```

使用指定配置文件：

```powershell
.\build\cagent.exe --config .\config.json "hello"
```

配置示例：

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

API Key 不会写入 `config.json`。请通过环境变量提供：

```powershell
$env:NEWAPI_API_KEY="your_key"
```

## CLI 命令

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

Slash commands：

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

## 工具示例

读取文件：

```powershell
.\build\cagent.exe /tool-test read_file '{"path":"README.md"}'
```

搜索代码：

```powershell
.\build\cagent.exe /tool-test search_code '{"query":"ca_config","max_results":5}'
```

预览修改，不写入磁盘：

```powershell
.\build\cagent.exe /tool-test preview_file_change '{"path":"README.md","new_content":"hello\n"}'
```

创建文件，会触发确认：

```powershell
.\build\cagent.exe /tool-test create_file '{"path":"tmp_note.txt","content":"hello\n"}'
```

执行受保护命令，会触发确认：

```powershell
.\build\cagent.exe /tool-test execute_command '{"command":"cmake --build build","timeout_ms":120000}'
```

查看 Git 状态：

```powershell
.\build\cagent.exe /tool-test git_status '{}'
```

## 已注册工具

测试工具：

- `noop`
- `echo`
- `ask_test`
- `danger_test`

项目与只读文件工具：

- `get_project_summary`
- `list_directory`
- `read_file`
- `read_file_range`
- `search_files`
- `search_code`

写入与编辑工具：

- `create_file`
- `write_file`
- `append_file`
- `preview_file_change`
- `edit_file`
- `apply_patch`

命令与 Git 工具：

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

## 权限模型

每个工具都有一个权限等级：

- `SAFE`：直接执行
- `ASK`：执行前询问 `y/N`
- `DANGEROUS`：需要输入 `YES` 强确认

示例：

- 读取和搜索工具是 `SAFE`
- 创建、写入、追加、编辑、patch 工具是 `ASK`
- shell 命令执行是 `ASK`
- `git_restore_file` 是 `DANGEROUS`

Tool Executor 会在权限确认前先执行 preflight 校验。像 `../outside.txt`、`.git/config`、`build/output.txt` 这样的非法路径会在询问用户前被拒绝。

## Workspace 安全边界

文件工具被限制在当前 workspace 内：

- 拒绝空路径
- 拒绝 `..` 路径穿越
- 拒绝访问 workspace 外的绝对路径
- 保护 `.git/` 和 `build/`
- 拒绝读取二进制文件内容
- 限制大文件读取和工具输出大小
- 工具结果保持 bounded JSON

这不是完整的操作系统级 sandbox。shell 和 Git 工具是带确认提示的本地开发辅助能力，不应被视为安全边界。

## Agent Loop

自然语言输入会进入 Agent Loop：

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

模型应输出 Tool Protocol JSON：

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

或者：

```json
{
  "type": "final_answer",
  "content": "Done."
}
```

Runtime 可以从 markdown code fence 或带少量解释文字的模型输出中提取 JSON action。如果 action 非法，会把 observation 反馈给模型并进行有限 retry。

## 项目结构

```text
include/              公共头文件
src/agent/            Agent Loop、action parser、prompt builder
src/cli/              CLI 和 REPL
src/config/           配置加载和 config init
src/core/             通用 Runtime helper
src/json/             JSON utilities
src/llm/              Fake 和 OpenAI-compatible providers
src/permission/       权限提示和权限策略
src/platform/         Console、argv、process 抽象
src/project/          Workspace indexer 和项目类型检测
src/tool/             Tool registry 和 executor
src/tools/            文件、编辑、patch、命令、Git 工具
```

## 开发说明

本项目刻意保持核心 Runtime 为 C 实现：

- 不使用 Python helper runtime
- 不引入大型 JSON 库
- 使用 C17
- 使用 bounded buffers 和明确错误码
- shell、Git、大 payload 支持均可通过 feature flag 控制

标注：详细阶段规划、Prompt 草稿和示例流程属于本地开发资料，不包含在当前公开仓库快照中。

## License

见 `LICENSE`。
