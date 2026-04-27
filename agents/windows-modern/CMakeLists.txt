cmake_minimum_required(VERSION 3.20)
project(remote-hands-modern LANGUAGES CXX)

# Modern target: Vista+ in theory, Windows 10 1903+ in practice (DPI v2,
# long-path support, UTF-8 ACP). We don't try to support pre-Vista from
# this build — that's what agents/windows-nt/ is for.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "windows-modern requires MSVC (cl.exe). Use VS 2019+ or Build Tools.")
endif()

# Static CRT — single .exe, no msvcr*.dll on the target.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# libwebp via FetchContent. Zero external setup — first configure downloads
# and builds the encoder; subsequent configures use the cache. Only the
# encoder is needed; turn off all the bundled CLI tools and demos to keep
# build time and binary size down.
include(FetchContent)
set(WEBP_BUILD_ANIM_UTILS    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP         OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP         OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP         OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST      OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC         ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG        v1.4.0
)
FetchContent_MakeAvailable(libwebp)

add_executable(remote-hands
    agent.cpp
    wgc.cpp
    uia.cpp
    discovery.cpp
)

target_compile_definitions(remote-hands PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00       # Windows 10 / Server 2016+
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
)

target_compile_options(remote-hands PRIVATE
    /W4 /permissive-
    $<$<CONFIG:Release>:/O2 /Os /Gy /GS->
)

target_link_libraries(remote-hands PRIVATE
    ws2_32        # winsock 2
    user32        # SendInput, windowing, clipboard
    gdi32         # BitBlt
    advapi32      # privileges, GetUserName
    shell32       # CommandLineToArgvW
    ole32         # COM init for Task Scheduler; CreateStreamOnHGlobal for GDI+ PNG
    oleaut32      # SAFEARRAY / VARIANT for Task Scheduler
    taskschd      # ITaskService
    gdiplus       # PNG encoding via Gdiplus::Bitmap
    webp          # WebP encoding via libwebp (bundled via FetchContent)
    d3d11             # WGC capture readback path
    dxgi              # WGC: DXGI device interop
    windowsapp        # C++/WinRT runtime activation
    uiautomationcore  # IUIAutomation (UIA tree, patterns)
    iphlpapi          # GetAdaptersAddresses for mDNS interface enumeration
)

target_link_options(remote-hands PRIVATE
    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    "/MANIFEST:EMBED"
    "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml"
)
