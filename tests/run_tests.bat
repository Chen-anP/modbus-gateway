@echo off
rem 一键编译并运行全部 PC 测试。双击运行，或在命令行里执行 tests\run_tests.bat
rem -D__USE_MINGW_ANSI_STDIO=1：让 MinGW 的 printf / vsnprintf 按 C99 标准工作（和板子上的 C 库一致）
chcp 65001 >nul
cd /d "%~dp0.."
if not exist tests\out mkdir tests\out
set CF=-std=c99 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1
set APP=firmware/central/App
set FAIL=0

echo.
echo ===== 1. mb_rtu 协议层 =====
gcc %CF% -I %APP% %APP%/mb_rtu.c tests/test_mb_rtu.c -o tests/out/test_mb_rtu.exe || set FAIL=1
if exist tests\out\test_mb_rtu.exe tests\out\test_mb_rtu.exe || set FAIL=1

echo.
echo ===== 2. log 日志模块 =====
gcc %CF% -I tests/stubs -I %APP% %APP%/log.c tests/test_log.c -o tests/out/test_log.exe || set FAIL=1
if exist tests\out\test_log.exe tests\out\test_log.exe || set FAIL=1

echo.
echo ===== 3. bus_probe 请求-回应 =====
gcc %CF% -I tests/stubs -I %APP% %APP%/mb_rtu.c tests/test_bus_probe.c -o tests/out/test_bus_probe.exe || set FAIL=1
if exist tests\out\test_bus_probe.exe tests\out\test_bus_probe.exe || set FAIL=1

echo.
if %FAIL%==0 (echo 全部通过) else (echo 有失败，往上翻看 FAIL 那一行)
del /q tests\out\*.exe >nul 2>&1
pause
