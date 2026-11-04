@echo off
rem Builds dsound.dll (32 bit) into ..\_release\Binaries. Needs zig on PATH.
rem Compile and link are separate: the headers come with libc, the link leaves the C runtime out, so the DLL only
rem depends on kernel32 and user32. zig writes .lib and .pdb beside its output, so it builds in _build first.
setlocal
pushd "%~dp0"
if not exist _build mkdir _build
zig cc -target x86-windows-gnu -c -O2 -mstackrealign -fno-stack-protector -fno-builtin -Wall -Wextra dsound.c -o _build\dsound.obj
if errorlevel 1 goto failed
zig cc -target x86-windows-gnu -shared -nostdlib -Wl,-e,DllMain@12 _build\dsound.obj dsound.def -o _build\dsound.dll -lkernel32 -luser32
if errorlevel 1 goto failed
if not exist ..\_release\Binaries mkdir ..\_release\Binaries
copy /y _build\dsound.dll ..\_release\Binaries\ >nul
if errorlevel 1 goto failed
popd
echo built: %~dp0..\_release\Binaries\dsound.dll
exit /b 0

:failed
popd
echo build FAILED
exit /b 1
