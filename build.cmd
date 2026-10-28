@echo off
rem Builds qcedid.exe for Windows on ARM64 with the clang that ships with Visual Studio 2022
rem ("C++ Clang tools for Windows" component) or any clang on PATH.
setlocal
cd /d "%~dp0"
set CLANG=clang
where clang >nul 2>nul || set CLANG="%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\ARM64\bin\clang.exe"
%CLANG% --target=aarch64-pc-windows-msvc -O2 -Wall -D_CRT_SECURE_NO_WARNINGS -o qcedid.exe qcedid.c -lgdi32 -luser32 -ladvapi32
