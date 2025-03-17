# Agent Dev Kit DEMO

[English](./readme.md)

## 一、软件环境准备

### 1.1 IDF 版本

- **版本**: 5.3

- **下载链接**: [IDF v5.3.2](https://github.com/espressif/esp-idf/tree/v5.3.2)

- **克隆命令**:

  ```
  git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
  ```

### 1.2 ADF 版本

- **版本**: 2.7

- **下载链接**: [ESP-ADF v2.7](https://github.com/espressif/esp-adf/tree/v2.7)

- **克隆命令**:

  ```
  git clone -b v2.7 --recursive https://github.com/espressif/esp-adf.git
  ```

### 1.3 QMSD-ESP32-BSP

- **下载链接**: [QMSD-ESP32-BSP](https://github.com/smartpanle/QMSD-ESP32-BSP)

- **克隆命令**:

  ```
  git clone -b v5.3.2 --recursive https://github.com/smartpanle/QMSD-ESP32-BSP.git
  ```

## 二、环境配置

### 2.1 IDF 环境配置

参考 `esp-idf` 中的 `README.md` 文件进行配置。

### 2.2 ADF 环境配置

1. **下载 ESP-ADF**:

   使用克隆命令下载 ESP-ADF 到本地。

2. **打补丁**:

   进入 `esp-adf/idf_patches` 目录，按照 `README.md` 文件中的说明打上对应 esp-idf 版本的补丁。

3. **添加 `ADF_PATH` 环境变量**:

   - **方法一**: 按照 `esp-adf` 中的 `README.md` 文件进行添加。

     ```linux
     export ADF_PATH="esp-adf的路径"
     如 export ADF_PATH=~/esp/esp-adf
     ```

   - **方法二**: 修改 `CMakeLists.txt` 文件中的路径。

     ```cmake
     if(NOT DEFINED ENV{ADF_PATH})
         set(ENV{ADF_PATH} "esp-adf的路径")
     endif()
     ```

### 2.3 QMSD-ESP32-BSP 环境配置

工程中已附带 QMSD-ESP32-BSP，参考 `QMSD-ESP32-BSP` 中的 `README.md` 文件进行配置。

## 三、常见问题

### 3.1 lcd_periph_signals 报错

**解决方法**: 修改 `QMSD-ESP32-BSP/components/qmsd_screen/rgb_panel/qmsd_lcd_panel_rgb.c` 文件，更改判断 esp-idf 版本。

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 2)
#define lcd_periph_signals lcd_periph_rgb_signals
#endif
```

### 3.2 没有声音

**修改方法**：请将`esp-adf`版本更改为 2.7，具体安装方式参考2.2章节ADF 环境配置。

## 四、说明

- **`esp_peripherals`**: 来源于 `esp-adf/components/esp_peripherals`，主要修改 `i2c_bus.c` 和 `i2c_bus.h` 文件，适配屏幕使用的 I2C 库。
- **`board_to_adf`**: 初始化驱动 `ES7210` 和 `ES8311`，如有疑问请参考 `ESP-ADF`。
- **`QM-Y1091-4832`**: 屏幕驱动配置部分。