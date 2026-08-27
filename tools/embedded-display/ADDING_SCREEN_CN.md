# 新增屏幕驱动与 Profile 技术指南

本文档说明如何在 OP Embedded Studio 中接入新的显示屏模组，包括复用已有 LCD 控制器、新增控制器驱动、注册屏幕 Profile、编译验证，以及生成可供 Studio 烧录的固件。

## 1. 接入模型

工程使用两层结构管理屏幕差异：

- **LCD 控制器驱动**负责初始化命令、地址窗口、像素写入、镜像和反色等底层行为，代码位于 `main/`。
- **屏幕 Profile**负责具体模组的分辨率、偏移、方向、颜色顺序、总线、GPIO、接线说明和固件配置，位于 `screen_profiles/`。

因此，新增屏幕前先判断控制器是否已受支持：

| 情况 | 所需工作 |
| --- | --- |
| 控制器已支持，初始化协议兼容 | 新建 defaults 并注册 Profile |
| 控制器名称相同，但厂商初始化序列不同 | 扩展或新增控制器驱动，再注册 Profile |
| 全新的控制器或总线协议 | 接入 Kconfig、驱动、工厂和总线，再注册 Profile |

当前控制器列表以 `main/Kconfig.projbuild` 中的 `EXAMPLE_LCD_CONTROLLER` 为准。控制器名称相同不代表不同模组一定兼容，必须对照规格书或供应商示例确认初始化命令。

## 2. 接入前准备

从屏幕规格书、原厂示例和原理图中确认以下参数：

| 参数 | 说明 |
| --- | --- |
| 模组与控制器型号 | 记录完整料号，不只记录控制器系列名 |
| 总线 | 4-wire SPI、QSPI 或 I80，以及命令/参数传输格式 |
| 逻辑分辨率 | Studio 编码和固件帧缓冲使用的宽高 |
| 显存尺寸与偏移 | 确认 `X_GAP`、`Y_GAP`，尤其是裁切屏和圆屏 |
| 像素格式 | 当前内容通路以 RGB565 为主，确认 RGB/BGR 和字节顺序 |
| 显示方向 | 是否交换 X/Y、镜像 X 或镜像 Y |
| 反色 | 是否需要发送 display inversion 命令 |
| 时钟 | 规格上限和飞线调试时的稳定起始频率 |
| GPIO | 时钟、数据、CS、DC、RESET、背光及并行数据线 |
| 电气要求 | IO 电平、供电、背光电流、复位时序和电源时序 |
| 初始化序列 | 原厂命令表、延时和特殊寄存器页切换 |

背光不要直接由普通 GPIO 承担超出额定值的电流，应使用合适的限流和驱动电路。ESP32-S3 使用原生 USB 时通常应避开 GPIO19、GPIO20。

## 3. 复用已有控制器

以下步骤适用于控制器及初始化协议已经受支持的屏幕。

### 3.1 新建 defaults

在 `screen_profiles/` 中复制最接近的现有配置，并使用稳定、可读的 ID 命名：

```text
<controller>_<module>.defaults
```

例如：

```text
st7789_my_screen.defaults
```

典型 SPI 配置如下，实际值必须以目标屏幕为准：

```ini
# Screen profile: MY-SCREEN, 240x320, ST7789 controller.
CONFIG_EXAMPLE_LCD_CONTROLLER_ST7789=y
# CONFIG_EXAMPLE_LCD_CONTROLLER_ST7735 is not set
# CONFIG_EXAMPLE_LCD_CONTROLLER_GC9D01N is not set
# CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01 is not set
# CONFIG_EXAMPLE_LCD_CONTROLLER_ST77916 is not set
# CONFIG_EXAMPLE_LCD_CONTROLLER_CO5300 is not set
# CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9342 is not set

CONFIG_EXAMPLE_LCD_BUS_SPI=y
# CONFIG_EXAMPLE_LCD_BUS_QSPI is not set
# CONFIG_EXAMPLE_LCD_BUS_I80 is not set

CONFIG_EXAMPLE_LCD_H_RES=240
CONFIG_EXAMPLE_LCD_V_RES=320
CONFIG_EXAMPLE_LCD_X_GAP=0
CONFIG_EXAMPLE_LCD_Y_GAP=0
CONFIG_EXAMPLE_LCD_PIXEL_CLOCK_HZ=10000000

CONFIG_EXAMPLE_LCD_RGB_ORDER_RGB=y
# CONFIG_EXAMPLE_LCD_RGB_ORDER_BGR is not set
CONFIG_EXAMPLE_LCD_INVERT_COLOR=y
# CONFIG_EXAMPLE_LCD_MIRROR_X is not set
# CONFIG_EXAMPLE_LCD_MIRROR_Y is not set
# CONFIG_EXAMPLE_LCD_SWAP_XY is not set
```

开发板公共参数来自 `screen_profiles/base.defaults`。如果新模组使用不同 GPIO，可在模组 defaults 中覆盖相应的 `CONFIG_EXAMPLE_PIN_NUM_*`。不要为了单个模组修改公共 GPIO，除非所有 Profile 都应同步改变。

### 3.2 注册 Profile

在 `screen_profiles/profiles.json` 的 `profiles` 数组中增加条目。至少保证以下信息准确：

```json
{
  "id": "st7789_my_screen",
  "displayName": "MY-SCREEN 240x320 ST7789",
  "displayNameZh": "MY-SCREEN 240x320 ST7789 屏",
  "screenType": "rectangle_tft",
  "module": "MY-SCREEN",
  "driverIc": "ST7789",
  "controller": "ST7789",
  "interface": "4-wire SPI",
  "logicalResolution": {
    "width": 240,
    "height": 320
  },
  "visibleArea": {
    "shape": "rectangle",
    "description": "Full 240 x 320 logical frame is visible."
  },
  "verified": false,
  "defaultsFile": "screen_profiles/st7789_my_screen.defaults",
  "menuconfig": {
    "CONFIG_EXAMPLE_LCD_CONTROLLER_ST7789": true,
    "CONFIG_EXAMPLE_LCD_H_RES": 240,
    "CONFIG_EXAMPLE_LCD_V_RES": 320,
    "CONFIG_EXAMPLE_LCD_X_GAP": 0,
    "CONFIG_EXAMPLE_LCD_Y_GAP": 0,
    "CONFIG_EXAMPLE_LCD_INVERT_COLOR": true,
    "CONFIG_EXAMPLE_LCD_RGB_ORDER_RGB": true,
    "CONFIG_EXAMPLE_LCD_PIXEL_CLOCK_HZ": 10000000
  },
  "wiring": []
}
```

应按现有条目的结构补全真实 `wiring`、物理尺寸、说明和注意事项。`profiles.json` 是 Studio 设备目录、构建服务和接线提示的共同数据源，不要在前端另建重复的内置屏幕列表。

真机验证完成前保持 `verified: false`。

### 3.3 校验 Profile 注册表

在仓库根目录执行：

```bash
python3 tools/embedded-display/server/build_server.py --check
```

该命令会检查 Profile ID、defaults 引用和路径等注册表约束。

### 3.4 使用独立目录编译

先加载 ESP-IDF 环境，再从仓库根目录构建：

```bash
. /Users/fengqihao/esp-idf/export.sh

idf.py -C tools/embedded-display \
  -B build/profile_st7789_my_screen \
  -DSDKCONFIG=build/profile_st7789_my_screen/sdkconfig \
  "-DSDKCONFIG_DEFAULTS=screen_profiles/base.defaults;screen_profiles/st7789_my_screen.defaults" \
  build
```

每个 Profile 使用独立的 build 目录和 `SDKCONFIG`。修改 defaults 后应使用新 build 目录，或清理该 Profile 已生成的 `sdkconfig`，否则 ESP-IDF 可能继续使用旧值。

也可以启动本地构建服务，通过 Profile API 执行构建；接口说明见 `server/API_CN.md`。

## 4. 接入全新控制器

如果控制器尚不受支持，仅添加 defaults 和 JSON 不会产生可用驱动。需要完成以下接入点。

### 4.1 增加 Kconfig 选项

在 `main/Kconfig.projbuild` 的 `EXAMPLE_LCD_CONTROLLER` choice 中增加控制器选项，并按需要设置默认总线、分辨率或时钟：

```kconfig
config EXAMPLE_LCD_CONTROLLER_NEW
    bool "NEW"
```

### 4.2 实现 Panel 驱动

在 `main/` 的控制器域中增加实现和头文件，例如：

```text
new_panel.c
new_panel.h
```

优先遵循现有 `esp_lcd_panel_t` 驱动结构。通常需要处理：

- 硬件复位和软件复位；
- 厂商初始化命令及规定延时；
- sleep out、display on/off；
- 显示窗口和像素数据写入；
- X/Y 镜像、X/Y 交换和显示偏移；
- RGB/BGR、反色和 RGB565 字节顺序；
- 参数长度、DMA 缓冲及错误返回。

初始化表应有明确来源，并把模组专属差异限制在合适层级。若同一控制器的多个模组仅命令表不同，应避免把某一块屏的行为无条件应用到全部 Profile。

### 4.3 注册源文件和驱动工厂

将新源文件加入 `main/CMakeLists.txt` 的 `OPENPENCIL_SRCS`。

然后修改 `main/lcd_panel_factory.c`：

1. 引入驱动头文件；
2. 在 `example_lcd_controller_name()` 返回控制器名称；
3. 在 `example_lcd_panel_needs_rgb565_byte_swap()` 配置字节交换策略；
4. 在 `example_lcd_new_panel()` 创建对应 Panel。

### 4.4 接入特殊总线

普通 4-wire SPI 通常可复用现有总线初始化。若控制器使用特殊 QSPI 命令前缀、非标准命令/数据阶段或新的并行接口，需要在 `main/st7789_simple_main.c` 及相应独立 IO 模块中接入。

可参考现有 ST77916 的 `st77916_qspi_io.c`，但不要假设不同 QSPI 控制器具有相同传输格式。

底层驱动可独立初始化并显示测试图后，再按第 3 节新增 Profile。

## 5. 真机调试与验收

建议从低速、静态测试图开始，按以下顺序排查：

1. 使用 1–5 MHz 的保守时钟验证供电、复位和初始化。
2. 显示纯红、纯绿、纯蓝，检查 RGB/BGR 和 RGB565 字节顺序。
3. 显示黑白区域，确认反色配置。
4. 显示四角标记和 1 像素边框，检查分辨率及 X/Y 偏移。
5. 显示带方向文字的图案，检查 X/Y 交换和镜像。
6. 连续刷新，逐步提高时钟并观察花屏、撕裂和偶发错误。
7. 分别验证 Studio 预览、USB 内容部署以及计划支持的 BLE、Wi-Fi 或交互模式。

验收记录至少应包含：

| 检查项 | 通过标准 |
| --- | --- |
| 初始化 | 冷启动、复位和重复烧录后均稳定点亮 |
| 几何 | 边框完整，四角方向和可视区域正确 |
| 颜色 | 红绿蓝、黑白及渐变显示正确 |
| 刷新 | 目标时钟下连续运行无花屏或异常重启 |
| 内容通路 | 编码尺寸、设备报告尺寸和逻辑分辨率一致 |
| 接线文档 | Profile 中的 GPIO 与实际硬件一致 |

全部验证完成后才将 Profile 的 `verified` 改为 `true`。

## 6. 预编译固件与 Studio 烧录

Profile 注册后，Studio 可以识别屏幕、按目标尺寸预览并编码内容。但如果 `prebuilt-firmware/` 中没有匹配产物，用户不能直接使用依赖预编译固件的烧录方式。

根据需要分别生成并验证对应构建模式，例如：

- 普通 USB 内容部署；
- USB 快速单帧；
- USB 交互或动画交互；
- BLE 内容传输；
- Wi-Fi 或实时镜像。

不要假设一次普通构建会自动覆盖全部模式。构建服务会按 Profile 和 build mode 隔离产物；浏览器烧录清单可通过以下接口检查：

```text
GET /api/artifacts/<profileId>/manifest.json
```

发布前确认 Studio 能找到目标 Profile 的固件文件，并在真实设备上至少走通计划公开支持的每种部署路径。

## 7. 常见故障

| 现象 | 优先检查 |
| --- | --- |
| 背光亮但无图像 | RESET、CS、DC、初始化命令、sleep out/display on 和总线模式 |
| 全屏颜色相反 | `CONFIG_EXAMPLE_LCD_INVERT_COLOR` |
| 红蓝互换 | RGB/BGR 配置 |
| 颜色呈随机条纹 | RGB565 字节序、DMA 数据长度、SPI mode 和时钟 |
| 图像整体偏移或裁切 | `X_GAP`、`Y_GAP`、逻辑分辨率和显存尺寸 |
| 图像旋转或镜像 | `SWAP_XY`、`MIRROR_X`、`MIRROR_Y` |
| 低速正常、高速花屏 | 接线长度、信号完整性、供电、时钟和 DMA 配置 |
| 编译后配置未变化 | 旧 build 目录中的 `sdkconfig` |
| Studio 能选屏但不能烧录 | 缺少对应 Profile/build mode 的预编译固件 |

## 8. 提交前检查

- defaults、`profiles.json` 和实际硬件参数一致；
- 新控制器已接入 Kconfig、CMake 和 Panel 工厂；
- `python3 tools/embedded-display/server/build_server.py --check` 通过；
- 使用独立 build 目录完成固件编译；
- 真机完成几何、颜色、稳定性和目标部署通路验证；
- 更新 `screen_profiles/README_CN.md` 和 `OPERATION_GUIDE_CN.md` 的支持列表；
- 若属于用户可见能力，在根目录 `CHANGELOG.md` 的 Unreleased 区域记录结果；
- 最后执行与改动范围相符的仓库质量检查。

