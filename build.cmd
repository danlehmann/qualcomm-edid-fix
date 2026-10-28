@echo off
rem Builds qcedid.exe (command line) and qcedid-gui.exe for Windows on ARM64 with the clang
rem that ships with Visual Studio 2022 ("C++ Clang tools for Windows") or any clang on PATH.
setlocal
cd /d "%~dp0"
set CLANG=clang
where clang >nul 2>nul || set CLANG="%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\ARM64\bin\clang.exe"
set FLAGS=--target=aarch64-pc-windows-msvc -O2 -Wall -Wno-missing-field-initializers -D_CRT_SECURE_NO_WARNINGS
%CLANG% %FLAGS% -o qcedid.exe qcedid.c qcedid_core.c -lgdi32 -luser32 -ladvapi32 || exit /b 1
%CLANG% %FLAGS% -o qcedid-gui.exe qcedid-gui.c qcedid_core.c -lgdi32 -luser32 -ladvapi32 -lcomctl32 -lcomdlg32 ^
    -Wl,/subsystem:windows -Wl,/manifest:embed "-Wl,/manifestuac:level='requireAdministrator' uiAccess='false'" || exit /b 1
