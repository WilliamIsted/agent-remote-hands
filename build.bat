@echo off
REM Build the VM control agent under VC6.
REM Output: vm_agent.exe (statically linked, ~50KB, no runtime deps).

call "C:\Program Files\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul

cl /nologo /MT /O2 /W3 /D_WIN32_WINNT=0x0500 ^
   agent.c ^
   /link wsock32.lib user32.lib gdi32.lib advapi32.lib ^
   /OUT:vm_agent.exe /SUBSYSTEM:CONSOLE

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

del agent.obj 2>nul

echo.
echo Built vm_agent.exe. Run it to start listening on port 8765:
echo     vm_agent.exe
echo.
echo Override port: vm_agent.exe 9999  (or set VM_AGENT_PORT env var)
