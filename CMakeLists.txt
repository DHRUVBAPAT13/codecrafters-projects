cmake_minimum_required(VERSION 3.13)
project(claude_code C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find required system libraries
find_package(CURL REQUIRED)
find_package(cJSON REQUIRED)

# Define source files
set(SOURCES
    src/main.c
    src/mcp.c
    src/session.c
)

# Build executable
add_executable(claude-code ${SOURCES})

# Target include directories
target_include_directories(claude-code PRIVATE
    src
    ${CURL_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(claude-code PRIVATE
    ${CURL_LIBRARIES}
    cjson
)
