cmake_minimum_required(VERSION 3.16)

project(cagent
    VERSION 0.1.0
    DESCRIPTION "C Claude-Code-like CLI Agent"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(cagent
    src/main.c
    src/cli/repl.c
    src/config/config.c
    src/config/env.c
)

target_include_directories(cagent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(cagent
    PRIVATE
        CAGENT_VERSION="${PROJECT_VERSION}"
)
