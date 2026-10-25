# TinyUSB（第三方，MIT 许可）

- 来源：https://github.com/hathach/tinyusb ，版本 **0.20.0**（tag `0.20.0`）
- 许可：MIT，见同目录 `LICENSE`。文件内容未修改
- 只保留了本项目用到的部分：设备栈（`device/`）、CDC 类（`class/cdc/` 的设备端）、FreeRTOS 适配（`osal/`）、
  公共代码（`common/`）、STM32 全速设备控制器驱动（`portable/st/stm32_fsdev/`）
- 本项目的配置和描述符不在这里：`App/tusb_config.h`、`App/usb_descriptors.c`

升级 TinyUSB 时：用新版本的同名目录整体替换，再检查 `App/tusb_config.h` 里的宏有没有改名。
