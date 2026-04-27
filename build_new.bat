@echo off
REM Build a SEPARATE binary (vm_agent_new.exe) so the running vm_agent.exe
REM remains undisturbed. Used for hot-swap testing.

call "C:\Program Files\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul

cl /nologo /MT /O2 /W3 /D_WIN32_WINNT=0x0500 ^
   agent.c ^
   /link wsock32.lib user32.lib gdi32.lib advapi32.lib ^
   /OUT:vm_agent_new.exe /SUBSYSTEM:CONSOLE

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

del agent.obj 2>nul
echo Built vm_agent_new.exe.
