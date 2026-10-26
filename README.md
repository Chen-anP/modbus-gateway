# Modbus 多通道网关（中控）

STM32H563 + FreeRTOS。上行 USB 虚拟串口做 Modbus RTU 从机，下行两路 RS485 做 Modbus RTU 主机，轮询传感器；支持点表下发和转发固件升级。

> 开发中，当前进度：第 8 步（看门狗、防"启动就死机"、通过中控升级传感器）。完成后补充功能说明、实测数据和演示。

功能需求参考了百问网课程中"中控"的功能；本仓库的设计和代码为重新编写，不使用课程代码和 libmodbus。

## 硬件

| 项目 | 内容 |
| --- | --- |
| 芯片 | STM32H563RIV6，Cortex-M33，250 MHz |
| RS485 CH1 | USART2（PA2 / PA3），115200 8N1，DMA |
| RS485 CH2 | UART4（PA0 / PA1），115200 8N1，DMA |
| 日志 | USART3（PC10 / PC11），115200 |
| 上行 | USB 全速设备（PA11 / PA12） |

## 目录

```
firmware/boot/          Bootloader（0x08000000，256 KB，Keil 工程 MDK-ARM/boot.uvprojx）：
                          开机判断跳不跳 App；留下时通过 USB 收新固件写进 App 区。共用 central 的 HAL、TinyUSB 和 App/ 里的协议模块
  App/                    app_flash.c（新固件写进 App 区，断电安全）、boot_proto.c（Bootloader 的系统区和 0x15）
  Core/                   boot_main.c（判断、跳转、主循环）、boot_usb.c（裸机 TinyUSB）、boot_log.c、boot_it.c
firmware/central/       中控固件（App，0x08040000；CubeMX 生成 + Keil MDK 工程）
  App/                  自己写的模块
    log.c/.h              带等级的日志：队列 + 低优先级任务，中断里不打印
    mb_rtu.c/.h           Modbus RTU 协议层：CRC、按功能码判断帧长、从机处理、主机组帧与解析
    port_uart.c/.h        串口收发：DMA + 空闲中断接收、流缓冲区、DMA 发送
    mb_master.c/.h        主机请求层：帧间隔、回显处理、超时、重试、统计；通过函数指针 mb_io_t 收发
    dev_health.c/.h       设备在线状态：连续失败 3 次判定掉线，掉线后每 2 s 重试一次
    point_table.c/.h      点表：文件解析、校验、排序、合并读块；default_points.c 是内置默认表
    reg_space.c/.h        中控寄存器空间：映射区（点表决定）+ 系统区；"待写入"标记
    point_sync.c/.h       点同步：每轮先下发待写入的点，再按读块读回；掉线降频
    crc32.c/.h            CRC-32（和 zlib 相同）
    file_rx.c/.h          0x15 文件接收：文件头 + 224 字节分包，重发 / 跳包 / CRC32
    table_loader.c/.h     点表下发：收文件 → 校验 → 解析到临时表
    gw_data.c/.h          共享数据（点表、寄存器空间、系统区）、互斥锁、换表用的轮锁；开机选点表、换表后存 Flash
    flash_layout.h        Flash 分区：Bootloader / App / 点表区 / 配置区
    flash_io.h            擦写 Flash 的函数指针（板子上用 HAL，PC 测试用假 Flash）
    flash_if.c/.h         HAL 擦写（8 KB 扇区、16 字节写），只允许擦写指定窗口
    pt_store.c/.h         点表存 Flash：先写数据最后写头，断电只会得到完整的表或"没有"
    boot_cfg.c/.h         配置区（App 长度、CRC、留在 Bootloader 标志）和 Bootloader 开机判断
    boot_guard.c/.h       启动次数（连续 3 次没稳定就留在 Bootloader）、复位原因；guard_hw.h 是备份寄存器和 IWDG
    wd.c/.h               任务看门狗的判断：所有任务都按时报到才喂狗
    wdog.c/.h             App 的看门狗任务、故障测试命令
    fwd.c/.h              转发给传感器：命令寄存器、0x15 固件（扣住文件头查向量表、按序号去重、最后一包前核对 CRC）
    poll_task.c/.h        两路 RS485 各一个通道任务
    port_usb.c/.h         USB 虚拟串口（CDC）收发，基于 TinyUSB；usbd 任务、中断交接
    usb_descriptors.c     USB 描述符；tusb_config.h 是 TinyUSB 配置
    sys_regs.c/.h         系统区寄存器（4x 0xF000 起）：版本、命令、点表来源、每通道统计
    up_link.c/.h          上行任务：USB 收一帧 → 从机处理 → 回一帧（中控是从机，地址 1）
  Middlewares/Third_Party/tinyusb/   TinyUSB 0.20.0（MIT），只保留用到的部分
  Core/ Drivers/ Middlewares/   CubeMX 生成
  central.ioc           CubeMX 配置
tools/                  上位机工具 gw_tool.py（pyserial）：读写寄存器、下发 CSV 点表、系统命令、升级中控（upgrade）、升级传感器（upgrade-sensor）
tests/                  PC 端单元测试
  run_tests.bat         Windows 一键编译运行（需要 gcc）
  stubs/                FreeRTOS / HAL 的替身头文件
  fake_bus.h            模拟 RS485 总线和从机（假时间），给 mb_master、point_sync 的测试用
  fake_flash.h          模拟 Flash（扇区擦除、16 字节写、不能写两次、模拟断电）
docs/                   调试记录 debug-log.md；设计文档、测试报告（陆续补充）
```

## 编译和下载

Keil MDK 5（AC6）：

1. `firmware/boot/MDK-ARM/boot.uvprojx`：F7、F8（Bootloader，只需要烧一次）
2. `firmware/central/MDK-ARM/central.uvprojx`：F7、F8（App）。编译后生成 `MDK-ARM/central/central.bin`

之后可以不用调试器，通过 USB 升级 App：

```
python tools/gw_tool.py COM9 upgrade firmware/central/MDK-ARM/central/central.bin
```

## 测试

```
tests\run_tests.bat
```

或手动（以 mb_rtu 为例）：

```
gcc -std=c99 -Wall -Wextra -I firmware/central/App firmware/central/App/mb_rtu.c tests/test_mb_rtu.c -o test_mb_rtu
./test_mb_rtu
```

| 测试 | 用例数 |
| --- | --- |
| `test_mb_rtu.c` | 74 |
| `test_log.c` | 14（含 `log_hex`，编译时加 `-DTEST_LOG_HEX`） |
| `test_dev_health.c` | 19 |
| `test_mb_master.c` | 31 |
| `test_sys_regs.c` | 45 |
| `test_point_table.c` | 36 |
| `test_reg_space.c` | 34 |
| `test_point_sync.c` | 29 |
| `test_file_rx.c` | 20 |
| `test_table_loader.c` | 23 |
| `test_pt_store.c` | 31（含每一步断电） |
| `test_boot_cfg.c` | 33 |
| `test_app_flash.c` | 24（每一次 Flash 操作前断电） |
| `test_boot_proto.c` | 31（真实请求帧走完整升级） |
| `test_boot_guard.c` | 33 |
| `test_wd.c` | 31 |
| `test_fwd.c` | 35（模拟课程 F030 Bootloader 收固件的方式） |

## 许可

`App/` 和 `tests/` 下自己编写的文件采用 MIT 许可；CubeMX 生成的文件、HAL、CMSIS、FreeRTOS、TinyUSB 保持各自原有许可。
