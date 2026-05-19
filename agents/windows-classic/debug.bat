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

REM Debug build wrapper for windows-classic.
REM
REM Enables RH_DEBUG wire-trace logging and delegates to build.bat.
REM Produces rha-win.classic.x86.debug.exe alongside the release binary.
REM
REM Usage: debug.bat  (run from agents\windows-classic\ on the VS6 VM)

set RH_DEBUG=1
call "%~dp0build.bat"
