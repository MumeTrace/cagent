# cagent — CMake 构建文件 / CMake build script
# C17 标准，编译 src/ 下所有 .c 文件，链接成单个可执行文件
# C17 standard, compiles all .c under src/, links into one executable.
cmake_minimum_required(VERSION 3.16)

project(cagent
    VERSION 0.1.0
    DESCRIPTION "C Claude-Code-like CLI Agent"
    LANGUAGES C
)

# 强制 C17，禁用编译器扩展 / require C17, no compiler extensions
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 可执行文件 / executable
add_executable(cagent
    src/main.c
    src/cli/repl.c
    src/config/config.c
    src/config/env.c
    src/project/ignore_rules.c
    src/project/project_detector.c
    src/project/project_indexer.c
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
