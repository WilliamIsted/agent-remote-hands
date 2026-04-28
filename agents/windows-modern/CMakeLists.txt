#   Copyright 2026 William Isted and contributors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

cmake_minimum_required(VERSION 3.20)

project(remote-hands-modern
    VERSION 2.0.0
    DESCRIPTION "Agent Remote Hands - windows-modern v2 agent"
    HOMEPAGE_URL "https://github.com/WilliamIsted/agent-remote-hands"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "windows-modern target requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00         # Target Windows 10 (1809+) APIs
)

if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
endif()

option(REMOTE_HANDS_BUILD_TESTS
       "Build unit tests (fetches doctest from GitHub at configure time)" OFF)

# ---------------------------------------------------------------------------
# Core library — every translation unit except the entry point.
#
# Both the agent executable and the unit-test executable link against this so
# tests can exercise the same code paths the production binary uses.

add_library(remote-hands-core STATIC)

target_sources(remote-hands-core PRIVATE
    src/config.cpp
    src/server.cpp
    src/protocol.cpp
    src/connection.cpp
    src/token.cpp
    src/capabilities.cpp
    src/sysinfo.cpp
    src/uipi.cpp
    src/element_table.cpp
    src/screen_capture.cpp
    src/image_encode.cpp
    src/subscription.cpp
    src/crash_check.cpp
    src/mdns.cpp
    # verbs (one file per namespace)
    src/verbs/system.cpp
    src/verbs/window.cpp
    src/verbs/input.cpp
    src/verbs/clipboard.cpp
    src/verbs/registry.cpp
    src/verbs/process.cpp
    src/verbs/file.cpp
    src/verbs/element.cpp
    src/verbs/screen.cpp
    src/verbs/watch.cpp
)

target_include_directories(remote-hands-core PUBLIC src)

target_link_libraries(remote-hands-core PUBLIC
    ws2_32              # Winsock
    iphlpapi            # GetAdaptersAddresses (mDNS)
    user32              # input + window APIs
    gdi32               # BitBlt capture path
    advapi32            # registry, security
    shell32             # ShellExecuteEx
    ole32               # COM core
    oleaut32            # COM extensions
    uiautomationcore    # IUIAutomation
    windowscodecs       # WIC (PNG encoder)
    bcrypt              # BCryptGenRandom (token entropy)
    powrprof            # SetSuspendState (system.hibernate / sleep)
    shlwapi             # PathMatchSpec (watch.file)
)

# ---------------------------------------------------------------------------
# Agent executable

add_executable(remote-hands src/main.cpp)
target_link_libraries(remote-hands PRIVATE remote-hands-core)

set_target_properties(remote-hands PROPERTIES
    OUTPUT_NAME "remote-hands"
    SUFFIX ".exe"
)

if(MSVC)
    set_target_properties(remote-hands PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup"
    )
endif()

# ---------------------------------------------------------------------------
# Unit tests (doctest, opt-in)

if(REMOTE_HANDS_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
    )
    FetchContent_MakeAvailable(doctest)

    add_executable(remote-hands-tests
        tests/unit/test_main.cpp
        tests/unit/json_test.cpp
        tests/unit/errors_test.cpp
        tests/unit/tier_test.cpp
        tests/unit/wire_test.cpp
    )

    target_link_libraries(remote-hands-tests PRIVATE
        remote-hands-core
        doctest::doctest
    )

    enable_testing()
    add_test(NAME unit COMMAND remote-hands-tests)
endif()
