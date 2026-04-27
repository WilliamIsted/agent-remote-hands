@echo off
REM Build the Agent Remote Hands agent for the windows-nt target under VC6.
REM
REM Tier-1 size optimisations applied:
REM   /O1 /Os         optimise for size, not speed (agent is I/O-bound)
REM   /Gy             function-level linking — each fn its own COMDAT
REM   /GS-            no stack-cookie buffer-overrun checks
REM   /OPT:REF        drop unreferenced functions
REM   /OPT:ICF        fold identical functions (do_mdown/do_mup, etc.)
REM   /MERGE          collapse .rdata into .text — kills section alignment slop
REM   /SUBSYSTEM:...,4.00   PE subsystem 4.00 = loadable from NT 4.0 onwards
REM
REM Output: remote-hands.exe (statically linked, ~20 KB, no runtime deps).

call "C:\Program Files\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul

cl /nologo /MT /O1 /Os /Gy /GS- /W3 /D_WIN32_WINNT=0x0500 ^
   agent.c ^
   /link wsock32.lib user32.lib gdi32.lib advapi32.lib ole32.lib ^
   /OPT:REF /OPT:ICF /MERGE:.rdata=.text ^
   /OUT:remote-hands.exe /SUBSYSTEM:CONSOLE,4.00

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

del agent.obj 2>nul

echo.
echo Built remote-hands.exe. Run it to start listening on port 8765:
echo     remote-hands.exe
echo.
echo Override port: remote-hands.exe 9999  (or set REMOTE_HANDS_PORT env var)
echo Install as service:   remote-hands.exe --install   (run elevated)
echo Remove service:       remote-hands.exe --uninstall (run elevated)
