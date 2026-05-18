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

REM -- Adjust these two paths for your VS6 / Platform SDK installation -----
set MSVC6=C:\Program Files\Microsoft Visual Studio\VC98
set PSDK=C:\Program Files\Microsoft Platform SDK
REM ----------------------------------------------------------------------

set SRCS=src\main.c src\server.c src\connection.c src\protocol.c src\json.c src\token.c ^
 src\verbs\system.c src\verbs\common.c src\verbs\screen.c src\verbs\window.c ^
 src\verbs\input.c src\verbs\file.c src\verbs\directory.c src\verbs\process.c ^
 src\verbs\registry.c src\verbs\clipboard.c src\verbs\watch.c

"%MSVC6%\Bin\cl.exe" /nologo /MT /W3 /O2 ^
  /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" ^
  /I "%MSVC6%\Include" /I "%PSDK%\Include" ^
  %SRCS% ^
  /link /SUBSYSTEM:CONSOLE ^
        /LIBPATH:"%MSVC6%\Lib" /LIBPATH:"%PSDK%\Lib" ^
        /OUT:rha-win.classic.x86.exe ^
  wsock32.lib advapi32.lib kernel32.lib user32.lib gdi32.lib shell32.lib

if %ERRORLEVEL% EQU 0 (
  echo BUILD OK: rha-win.classic.x86.exe
) else (
  echo BUILD FAILED
  endlocal
  exit /b 1
)

endlocal
