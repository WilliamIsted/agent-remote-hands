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

add_executable(remote-hands)

target_sources(remote-hands PRIVATE
    src/main.cpp
    src/config.cpp
    src/server.cpp
    src/protocol.cpp
    src/connection.cpp
    src/token.cpp
    src/capabilities.cpp
    src/sysinfo.cpp
    # verbs (one file per namespace)
    src/verbs/system.cpp
    src/verbs/window.cpp
    # Future namespaces: screen, input, element, file, process, registry,
    # clipboard, watch.
)

target_include_directories(remote-hands PRIVATE src)

target_link_libraries(remote-hands PRIVATE
    ws2_32              # Winsock
    iphlpapi            # GetAdaptersAddresses (mDNS)
    user32              # input + window APIs
    gdi32               # BitBlt fallback capture
    advapi32            # registry, security
    shell32             # ShellExecuteEx
    ole32               # COM core
    oleaut32            # COM extensions
    uiautomationcore    # IUIAutomation
    bcrypt              # BCryptGenRandom (token entropy)
    powrprof            # SetSuspendState (system.hibernate / sleep)
)

set_target_properties(remote-hands PROPERTIES
    OUTPUT_NAME "remote-hands"
    SUFFIX ".exe"
)

# Use the wide-character entry point (wmain).
if(MSVC)
    set_target_properties(remote-hands PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup"
    )
endif()
