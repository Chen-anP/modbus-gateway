# Modbus 多通道网关（中控）

STM32H563 + FreeRTOS。上行 USB 虚拟串口做 Modbus RTU 从机，下行两路 RS485 做 Modbus RTU 主机，轮询传感器；支持点表下发和转发固件升级。

> 开发中，当前进度：第 3 步（主站轮询）。完成后补充功能说明、实测数据和演示。

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
firmware/central/       中控固件（CubeMX 生成 + Keil MDK 工程）
  App/                  自己写的模块
    log.c/.h              带等级的日志：队列 + 低优先级任务，中断里不打印
    mb_rtu.c/.h           Modbus RTU 协议层：CRC、按功能码判断帧长、从机处理、主机组帧与解析
    port_uart.c/.h        串口收发：DMA + 空闲中断接收、流缓冲区、DMA 发送
    mb_master.c/.h        主机请求层：帧间隔、回显处理、超时、重试、统计；通过函数指针 mb_io_t 收发
    dev_health.c/.h       设备在线状态：连续失败 3 次判定掉线，掉线后每 2 s 重试一次
    poll.c/.h             主站轮询：每个设备按自己的周期读，记录在线状态和最近的值
    poll_task.c/.h        两路 RS485 各一个轮询任务、设备表
  Core/ Drivers/ Middlewares/   CubeMX 生成
  central.ioc           CubeMX 配置
tests/                  PC 端单元测试
  run_tests.bat         Windows 一键编译运行（需要 gcc）
  stubs/                FreeRTOS / HAL 的替身头文件
  fake_bus.h            模拟 RS485 总线和从机（假时间），给 mb_master、poll 的测试用
docs/                   调试记录 debug-log.md；设计文档、测试报告（陆续补充）
```

## 编译

Keil MDK 5（AC6）打开 `firmware/central/MDK-ARM/central.uvprojx`，F7 编译。

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
| `test_poll.c` | 29 |

## 许可

`App/` 和 `tests/` 下自己编写的文件采用 MIT 许可；CubeMX 生成的文件、HAL、CMSIS、FreeRTOS 保持各自原有许可。
