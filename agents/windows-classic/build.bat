@echo off
REM   Copyright 2026 William Isted and contributors
REM
REM   Licensed under the Apache License, Version 2.0 (the "License");
REM   you may not use this file except in compliance with the License.
REM   You may obtain a copy of the License at
REM
REM       http://www.apache.org/licenses/LICENSE-2.0
REM
REM   Unless required by applicable law or agreed to in writing, software
REM   distributed under the License is distributed on an "AS IS" BASIS,
REM   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

REM windows-classic build (VS6 primary, per D11). C89, /MT static CRT,
REM Winsock 1.1. Run from this directory on the XP SP3 / VS6 build VM.

setlocal

REM -- Optional log capture: call with _log as the first argument -----------
REM    build.bat _log   -> compiles and writes build.log alongside the binary
if "%1"=="_log" (
    call "%~f0" > build.log 2>&1
    if %ERRORLEVEL% EQU 0 (echo LOG: build.log) else (echo LOG FAILED: build.log)
    exit /b %ERRORLEVEL%
)
REM -------------------------------------------------------------------------

REM -- Adjust these two paths for your VS6 / Platform SDK installation -----
set MSVC6=C:\Program Files\Microsoft Visual Studio\VC98
set PSDK=C:\Program Files\Microsoft Platform SDK
REM ----------------------------------------------------------------------

set SRCS=src\main.c src\server.c src\connection.c src\protocol.c src\json.c src\token.c ^
 src\debug.c src\socket_watchdog.c src\power_watcher.c ^
 src\verbs\system.c src\verbs\system_power.c src\verbs\common.c src\verbs\screen.c src\verbs\window.c ^
 src\verbs\input.c src\verbs\file.c src\verbs\directory.c src\verbs\process.c ^
 src\verbs\registry.c src\verbs\clipboard.c src\verbs\watch.c

REM cl.exe strips quotes when forwarding /LIBPATH: to link.exe, so paths with
REM spaces (Program Files) are split and misread as object files.  Set LIB and
REM INCLUDE directly -- the compiler and linker both honour these variables
REM without a quoting round-trip.
set INCLUDE=%MSVC6%\Include;%PSDK%\Include
set LIB=%MSVC6%\Lib;%PSDK%\Lib

REM Clear LINK: link.exe prepends its value to the command line, so an
REM inherited value from an outer build environment (e.g. vcvarsall.bat)
REM would be misread as extra input files.
set LINK=

REM -- Debug wire-trace flag -------------------------------------------------
REM Set RH_DEBUG=1 in the environment (or call debug.bat) to enable compile-
REM time logging: every dispatched verb (>> verb arg...) and every response
REM (<< OK / << ERR code) are printed to stderr.  Produces a separate binary
REM so release and debug builds can coexist on the VM.
set RH_DEBUG_FLAG=
set OUT_NAME=rha-win.classic.x86.exe
if defined RH_DEBUG (
    set RH_DEBUG_FLAG=/D "RH_DEBUG"
    set OUT_NAME=rha-win.classic.x86.debug.exe
)
REM -------------------------------------------------------------------------

"%MSVC6%\Bin\cl.exe" /nologo /MT /W3 /O2 ^
  /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" %RH_DEBUG_FLAG% ^
  %SRCS% ^
  /link /SUBSYSTEM:CONSOLE,4.00 /MACHINE:IX86 ^
        /OUT:%OUT_NAME% ^
  wsock32.lib advapi32.lib kernel32.lib user32.lib gdi32.lib shell32.lib

if %ERRORLEVEL% EQU 0 (
  echo BUILD OK: %OUT_NAME%
) else (
  echo BUILD FAILED
  endlocal
pause
  exit /b 1
)

endlocal
