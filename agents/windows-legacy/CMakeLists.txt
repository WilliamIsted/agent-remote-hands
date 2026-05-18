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

# ---------------------------------------------------------------------------
# ldoc-115 quirk 1: Static CRT MUST be set before project().
#
# project() triggers the MSBuild compiler-detection step, which hardcodes /MDd
# in its internal test template. If CMAKE_MSVC_RUNTIME_LIBRARY is set after
# project(), that detection step has already referenced ucrtd.lib → LNK1104.
# Setting it here pre-empts the detection step entirely.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ldoc-115 quirk 2: Compiler-detection bypass.
#
# CMake will inherit the correct cl.exe from the -T v141_xp toolset flag
# passed at configure time. FORCED+WORKS tells CMake not to run its own
# compiler-ID test (which also uses /MDd internally and conflicts with /MT).
set(CMAKE_CXX_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

# ldoc-115 quirk 3: Win10 SDK 19041 UCRT injection.
#
# v141 MSVC headers reference UCRT unconditionally; SDK 7.1A (the XP-era SDK)
# does not ship UCRT. We inject Win10 SDK 19041 UCRT headers + static lib so
# the compiler can find UCRT declarations and the linker can satisfy them
# without a runtime dependency.
set(WINSDK_UCRT_INC    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/ucrt")
set(WINSDK_UM_INC      "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/um")
set(WINSDK_SHARED_INC  "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/shared")
set(WINSDK_WINRT_INC   "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/winrt")
set(WINSDK_UCRT_LIB_X64 "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/ucrt/x64")
set(WINSDK_UM_LIB_X64   "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/um/x64")

foreach(_path WINSDK_UCRT_INC WINSDK_UM_INC WINSDK_SHARED_INC WINSDK_WINRT_INC
              WINSDK_UCRT_LIB_X64 WINSDK_UM_LIB_X64)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR
            "Win10 SDK 19041 path not found:\n  ${${_path}}\n"
            "Install the Windows 10 SDK 10.0.19041.0 and retry.")
    endif()
endforeach()

# ---------------------------------------------------------------------------

project(remote-hands-legacy
    VERSION 0.3.0
    DESCRIPTION "Agent Remote Hands - windows-legacy agent (Protocol 2.1)"
    HOMEPAGE_URL "https://github.com/WilliamIsted/agent-remote-hands"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "windows-legacy target requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WIN32_LEAN_AND_MEAN is defined per-source-file before `#include <windows.h>`
# rather than at compile-line scope (avoids C4005 redefinition warnings).
# NOMINMAX must be at compile-line scope — windows.h min/max macros otherwise
# leak into <algorithm> std::min/std::max usages.
add_compile_definitions(
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0501       # Target Windows XP SP3 / Server 2003
    _WINSOCK_DEPRECATED_NO_WARNINGS  # inet_addr used intentionally in net_parse (XP compat)
)

if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        $<$<CONFIG:Release>:/Zi>
    )
    add_link_options(
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

# ---------------------------------------------------------------------------
# Core library

add_library(remote-hands-legacy-core STATIC)

target_sources(remote-hands-legacy-core PRIVATE
    # Shared sources (platform-agnostic logic — identical to modern)
    ../shared/config.cpp
    ../shared/server.cpp
    ../shared/protocol.cpp
    ../shared/connection.cpp
    ../shared/token.cpp
    ../shared/capabilities.cpp
    ../shared/sysinfo.cpp
    ../shared/uipi.cpp
    ../shared/element_table.cpp
    ../shared/screen_capture.cpp
    ../shared/image_encode.cpp
    ../shared/subscription.cpp
    ../shared/crash_check.cpp
    ../shared/mdns.cpp
    # verbs (one file per namespace)
    ../shared/verbs/system.cpp
    ../shared/verbs/window.cpp
    ../shared/verbs/input.cpp
    ../shared/verbs/clipboard.cpp
    ../shared/verbs/registry.cpp
    ../shared/verbs/process.cpp
    ../shared/verbs/file.cpp
    ../shared/verbs/element.cpp
    ../shared/verbs/screen.cpp
    ../shared/verbs/watch.cpp
    ../shared/verbs/vision.cpp
    # Legacy platform implementations (stubs for W6; real impls in W7)
    platform/rng.cpp
    platform/integrity.cpp
    platform/image_encode.cpp
    platform/net_parse.cpp
    platform/known_folder.cpp
    platform/ocr.cpp
)

target_include_directories(remote-hands-legacy-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

# Win10 SDK header injection (ldoc-115 quirk 3 + WRL/UIAutomation headers).
#
# Include order is significant:
#
# 1. shim/: provides wrl/client.h (minimal ComPtr) BEFORE the SDK winrt/ so the
#    compiler picks up our version-check-free shim instead of WRL's gated header.
#    WRL's wrl/def.h hard-errors when NTDDI_VERSION < NTDDI_VISTA; sdkddkver.h
#    enforces NTDDI_VERSION>>16 == _WIN32_WINNT, making them mutually exclusive
#    for XP targets. The shim sidesteps this by not including wrl/def.h at all.
#
# 2. Win10 SDK UCRT/UM/shared: come before v7.1A Platform SDK (toolset-injected)
#    so Win10 SDK's objbase.h is used — v7.1A's objbase.h(239) generates C2760
#    with the v141 compiler + UCRT injection. All Vista+ declarations in Win10 SDK
#    headers are still guarded by _WIN32_WINNT=0x0501, so no new API surface opens.
#
# 3. winrt/: supplies UIAutomation WRL headers (other than wrl/client.h).
# shim/ is PRIVATE: only the core TUs need the WRL ComPtr shim; main.cpp does not.
target_include_directories(remote-hands-legacy-core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
)
# Win10 SDK headers are PUBLIC: inherited by the remote-hands exe so main.cpp
# also gets the fixed objbase.h / winsock2.h instead of v7.1A's versions.
target_include_directories(remote-hands-legacy-core PUBLIC
    "${WINSDK_UCRT_INC}"
    "${WINSDK_UM_INC}"
    "${WINSDK_SHARED_INC}"
    "${WINSDK_WINRT_INC}"
)

target_link_libraries(remote-hands-legacy-core PUBLIC
    ws2_32              # Winsock
    iphlpapi            # GetAdaptersAddresses (mDNS)
    user32              # input + window APIs
    gdi32               # BitBlt capture path
    gdiplus             # GDI+ PNG encoding (legacy image_encode seam)
    advapi32            # registry, security; CryptGenRandom (legacy rng seam)
    shell32             # ShellExecuteEx
    ole32               # COM core
    oleaut32            # COM extensions
    "${WINSDK_UM_LIB_X64}/UIAutomationCore.lib"  # IUIAutomation COM interface
    powrprof            # SetSuspendState (system.hibernate / sleep)
    shlwapi             # PathMatchSpec (watch.file); SHDeleteKeyW (registry.key.delete)
    "${WINSDK_UCRT_LIB_X64}/libucrt.lib"   # static UCRT (ldoc-115 quirk 3)
    # OMITTED vs modern: bcrypt (legacy uses CryptGenRandom via advapi32)
    #                    runtimeobject (WinRT loaded dynamically in ocr seam)
    #                    windowscodecs (WIC — GDI+ used instead)
    # NOTE: UIAutomation is accessed entirely via COM (CoCreateInstance), so
    # uiautomationcore.dll generates NO hard PE imports — the binary loads on XP
    # without /DELAYLOAD. Capability gating in verb_enabled() ensures element.*
    # verbs are not advertised on XP where uiautomationcore.dll is absent.
)

# ---------------------------------------------------------------------------
# Agent executable

add_executable(remote-hands src/main.cpp)
target_link_libraries(remote-hands PRIVATE remote-hands-legacy-core)

set_target_properties(remote-hands PROPERTIES
    OUTPUT_NAME "rha-win.legacy.x64"
    SUFFIX ".exe"
)

if(MSVC)
    set_target_properties(remote-hands PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup"
    )
endif()
