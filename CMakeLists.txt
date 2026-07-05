# cagent — CMake 构建文件 / CMake build script
# C17 标准，编译 src/ 下所有 .c 文件，链接成单个可执行文件
# C17 standard, compiles all .c under src/, links into one executable.
cmake_minimum_required(VERSION 3.16)

project(cagent
    VERSION 0.1.0
    DESCRIPTION "C Claude-Code-like CLI Agent"
    LANGUAGES C
)

find_package(CURL REQUIRED)

# 强制 C17，禁用编译器扩展 / require C17, no compiler extensions
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 可执行文件 / executable
add_executable(cagent
    src/main.c
    src/agent/action_parser.c
    src/agent/agent.c
    src/agent/prompt_builder.c
    src/cli/repl.c
    src/config/config.c
    src/config/env.c
    src/json/json.c
    src/llm/fake_provider.c
    src/llm/http_client.c
    src/llm/openai_compat_provider.c
    src/platform/console.c
    src/platform/windows_args.c
    src/permission/confirm_prompt.c
    src/permission/permission.c
    src/project/ignore_rules.c
    src/project/project_detector.c
    src/project/project_indexer.c
    src/tool/builtin_tools.c
    src/tool/tool_executor.c
    src/tool/tool_registry.c
    src/tools/file_tools.c
    src/tools/write_tools.c
)

# 头文件搜索路径 / include path
target_include_directories(cagent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 把 CMake 的 PROJECT_VERSION 传给 C 代码，在 repl.c 里用 / pass version to C code
target_compile_definitions(cagent
    PRIVATE
        CAGENT_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_link_libraries(cagent PRIVATE shell32)
endif()

target_link_libraries(cagent PRIVATE CURL::libcurl)
