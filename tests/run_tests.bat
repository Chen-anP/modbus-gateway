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
gcc %CF% -DTEST_LOG_HEX -I tests/stubs -I %APP% %APP%/log.c tests/test_log.c -o tests/out/test_log.exe || set FAIL=1
if exist tests\out\test_log.exe tests\out\test_log.exe || set FAIL=1

echo.
echo ===== 3. dev_health 在线状态 =====
gcc %CF% -I %APP% %APP%/dev_health.c tests/test_dev_health.c -o tests/out/test_dev_health.exe || set FAIL=1
if exist tests\out\test_dev_health.exe tests\out\test_dev_health.exe || set FAIL=1

echo.
echo ===== 4. mb_master 请求层 =====
gcc %CF% -I %APP% -I tests %APP%/mb_rtu.c %APP%/mb_master.c tests/test_mb_master.c -o tests/out/test_mb_master.exe || set FAIL=1
if exist tests\out\test_mb_master.exe tests\out\test_mb_master.exe || set FAIL=1

echo.
echo ===== 5. sys_regs 系统区寄存器 =====
gcc %CF% -I %APP% %APP%/mb_rtu.c %APP%/sys_regs.c tests/test_sys_regs.c -o tests/out/test_sys_regs.exe || set FAIL=1
if exist tests\out\test_sys_regs.exe tests\out\test_sys_regs.exe || set FAIL=1

echo.
echo ===== 6. point_table 点表 =====
gcc %CF% -I %APP% %APP%/point_table.c %APP%/default_points.c tests/test_point_table.c -o tests/out/test_point_table.exe || set FAIL=1
if exist tests\out\test_point_table.exe tests\out\test_point_table.exe || set FAIL=1

echo.
echo ===== 7. reg_space 寄存器空间 =====
gcc %CF% -I %APP% %APP%/mb_rtu.c %APP%/sys_regs.c %APP%/point_table.c %APP%/default_points.c %APP%/reg_space.c tests/test_reg_space.c -o tests/out/test_reg_space.exe || set FAIL=1
if exist tests\out\test_reg_space.exe tests\out\test_reg_space.exe || set FAIL=1

echo.
echo ===== 8. point_sync 点同步 =====
gcc %CF% -I %APP% -I tests %APP%/mb_rtu.c %APP%/mb_master.c %APP%/dev_health.c %APP%/sys_regs.c %APP%/point_table.c %APP%/default_points.c %APP%/reg_space.c %APP%/point_sync.c tests/test_point_sync.c -o tests/out/test_point_sync.exe || set FAIL=1
if exist tests\out\test_point_sync.exe tests\out\test_point_sync.exe || set FAIL=1

echo.
echo ===== 9. file_rx 文件接收 =====
gcc %CF% -I %APP% %APP%/crc32.c %APP%/file_rx.c tests/test_file_rx.c -o tests/out/test_file_rx.exe || set FAIL=1
if exist tests\out\test_file_rx.exe tests\out\test_file_rx.exe || set FAIL=1

echo.
echo ===== 10. table_loader 点表下发 =====
gcc %CF% -I %APP% %APP%/mb_rtu.c %APP%/crc32.c %APP%/file_rx.c %APP%/point_table.c %APP%/default_points.c %APP%/sys_regs.c %APP%/reg_space.c %APP%/table_loader.c tests/test_table_loader.c -o tests/out/test_table_loader.exe || set FAIL=1
if exist tests\out\test_table_loader.exe tests\out\test_table_loader.exe || set FAIL=1

echo.
echo ===== 11. pt_store 点表存 Flash（含模拟断电） =====
gcc %CF% -I %APP% -I tests %APP%/crc32.c %APP%/pt_store.c tests/test_pt_store.c -o tests/out/test_pt_store.exe || set FAIL=1
if exist tests\out\test_pt_store.exe tests\out\test_pt_store.exe || set FAIL=1

echo.
echo ===== 12. boot_cfg 配置区和开机判断 =====
gcc %CF% -I %APP% -I tests %APP%/crc32.c %APP%/boot_cfg.c tests/test_boot_cfg.c -o tests/out/test_boot_cfg.exe || set FAIL=1
if exist tests\out\test_boot_cfg.exe tests\out\test_boot_cfg.exe || set FAIL=1

echo.
if %FAIL%==0 (echo 全部通过) else (echo 有失败，往上翻看 FAIL 那一行)
del /q tests\out\*.exe >nul 2>&1
pause
