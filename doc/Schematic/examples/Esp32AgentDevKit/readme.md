# Esp32AgentDevKit

[Chinese](./README_CN.md)

## 1. Software Environment Preparation

### 11.1 IDF Version

- **Version**: 5.3

- **Download Link**: [IDF v5.3.2](https://github.com/espressif/esp-idf/tree/v5.3.2)

- **Clone Command**:

  ```
  git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
  ```

### ADF Version

- **Version**: 2.7

- **Download Link**: [ESP-ADF v2.7](https://github.com/espressif/esp-adf/tree/v2.7)

- **Clone Command**:

  ```
  git clone -b v2.7 --recursive https://github.com/espressif/esp-adf.git
  ```

### 1.3 QMSD-ESP32-BSP

- **Download Link**: [QMSD-ESP32-BSP](https://github.com/smartpanle/QMSD-ESP32-BSP)

- **Clone Command**:

  ```
  git clone -b v5.3.2 --recursive https://github.com/smartpanle/QMSD-ESP32-BSP.git
  ```

## 2. Environment Configuration

### 2.1 IDF Environment Configuration

Refer to the `README.md` file in  `esp-idf` for configuration instructions.

### 2.2 ADF Environment Configuration

1. **Download ESP-ADF**:

   Use the clone command to download ESP-ADF locally.

2. **Apply Patches**:

 Navigate to the  `esp-adf/idf_patches ` directory and follow the instructions in the  `README.md ` file to apply the patches corresponding to the esp-idf version.

3. **Add `ADF_PATH` Environment Variable**:

   - **Method 1**: Follow the instructions in the `README.md` file of `esp-adf` to add the variable.

     ```linux
     export ADF_PATH="esp-adf path"
     eg: export ADF_PATH=~/esp/esp-adf
     ```

   - **Method 2**: Modify the path in the `CMakeLists.txt` file.

     ```cmake
     if(NOT DEFINED ENV{ADF_PATH})
         set(ENV{ADF_PATH} "esp-adf path")
     endif()
     ```

### 2.3 QMSD-ESP32-BSP Environment Configuration

The project already includes `QMSD-ESP32-BSP.` Refer to the `README.md` file in `QMSD-ESP32-BSP` for configuration instructions.

## 三、Common Issues

### 3.1 lcd_periph_signals Error

**Solution**: Modify the `QMSD-ESP32-BSP/components/qmsd_screen/rgb_panel/qmsd_lcd_panel_rgb.c` file，change the esp-idf version check.

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 2)
#define lcd_periph_signals lcd_periph_rgb_signals
#endif
```

### 3.2 No Sound

**Solution**： Change the`esp-adf`version to 2.7. Refer to section 2.2 for the installation method.

## 4. Notes

- **`esp_peripherals`**: Sourced from `esp-adf/components/esp_peripherals`，mainly modified the `i2c_bus.c` and `i2c_bus.h` files to adapt to the I2C library used by the screen.
- **`board_to_adf`**: Initializes the drivers for `ES7210` and `ES8311`，For any questions, refer to `ESP-ADF`。
- **`QM-Y1091-4832`**: Screen driver configuration section.