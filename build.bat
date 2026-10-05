@echo off
rem Build nccl-cc on Windows using MinGW-w64 gcc.
gcc -std=gnu11 -Wall -Wextra -O2 -o ncclcc.exe main.c tokenize.c parse.c codegen.c
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
echo built ncclcc.exe
