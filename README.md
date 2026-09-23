# ESP32-S3 NES Handheld

面向 ESP32-S3-N16R8 的开源 NES 掌机固件，驱动 800×480 RGB565 屏幕、GT911 触摸、Xbox USB 手柄和 MAX98357A I2S 功放。

The firmware turns an ESP32-S3-N16R8 board into a small NES handheld with an 800×480 RGB display, GT911 touch input, an Xbox USB controller, and MAX98357A audio.

> 仓库不包含商业 NES 游戏。默认只打包项目自行生成的 `assets/test.nes` 测试 ROM。请仅使用你有权使用和分发的 ROM。

## 功能

- 1～60 个 ROM 的分页菜单，每页最多 10 个条目。
- Xbox 十字键选择，左右翻页，`A` 或 `Menu/Start` 启动游戏。
- 游戏内完整 NES A/B/Select/Start/方向键映射。
- `View + Menu` 或屏幕底部中央上滑可无重启返回菜单。
- 左右触摸条分别调节背光和音量。
- ROM 从 Flash 复制到可写 PSRAM 后运行，兼容会修改 bank 指针的旧版 Nofrendo mapper。
- Core 0 异步缩放并提交 512×480 游戏画面；模拟器运行在 Core 1。

## 硬件与接线

目标主控为带 16 MB Flash、8 MB OPI PSRAM 的 `ESP32-S3-N16R8`。不同开发板的供电、USB OTG 和启动脚设计可能不同，请先核对原理图。

### RGB 屏幕

| 信号 | ESP32-S3 GPIO |
| --- | ---: |
| PCLK | 1 |
| HSYNC | 39 |
| VSYNC | 40 |
| DE | 41 |
| RGB D0～D14 | 4～18 |
| RGB D15 | 21 |
| BL / LED_EN | 42 |

屏幕参数为 800×480、RGB565、PCLK 16 MHz、下降沿采样；水平时序 48/40/8，垂直时序 2/40/6。GPIO42 输出 20 kHz 高电平有效 PWM，只能连接背光使能或驱动电路，不能直接给背光 LED 供电。

### GT911 触摸

| 信号 | ESP32-S3 GPIO |
| --- | ---: |
| SDA | 47 |
| SCL | 48 |
| INT | 2 |

当前面板使用 800×480 原始坐标，不交换或镜像坐标轴。

### MAX98357A 音频

| 信号 | ESP32-S3 GPIO |
| --- | ---: |
| BCLK | 3 |
| LRC / WS | 45 |
| DIN | 46 |

功放必须与 ESP32-S3 共地。模块 VIN 是否支持 5 V 取决于具体模块。GPIO3、45、46 与启动配置有关，不要添加会在上电时强拉电平的外部上下拉。绝不能把两个主控的 BCLK、LRC、DIN 推挽输出直接并接；共享功放必须使用可靠的 break-before-make 切换或三态隔离。

### USB Host

| 信号 | ESP32-S3 GPIO |
| --- | ---: |
| USB D- | 19 |
| USB D+ | 20 |

USB 手柄的 5 V 供电能力取决于开发板和电源设计，请勿仅凭 GPIO 接线表假定开发板能够安全给外设供电。

## 构建

要求：

- Python 3
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
- 支持 USB OTG 的 ESP32-S3-N16R8 硬件

```powershell
pio run
```

默认构建使用 PioArduino 官方预编译库、`roms/manifest.csv` 和原创测试 ROM，可在全新克隆中直接编译。生成文件位于：

```text
.pio/build/esp32-s3-devkitc-1/
```

- `firmware.bin`：app0 固件。
- `roms.bin`：ROM 数据分区镜像。
- `firmware.factory.bin`：若构建环境生成该文件，则为完整合并镜像。

实机性能测试使用的 `PERF + 64B Cache Line + PSRAM XIP` 配置保存在项目内，可按需构建：

```powershell
pio run -e esp32-s3-performance -j 1
```

该环境会从源码重编译完整 Arduino/ESP-IDF 框架，首次构建耗时明显更长并需要联网获取 Espressif 托管组件。下文记录的实机帧率来自该性能环境；默认环境用于可移植构建，不应直接视为同等性能结论。

烧录并打开串口：

```powershell
pio run -t upload --upload-port <PORT>
pio device monitor --port <PORT> --baud 115200
```

## 使用自己的 ROM

不要将第三方 ROM 提交到 Git。复制示例清单并填写 ASCII 显示名与 ROM 相对路径：

```powershell
Copy-Item roms/manifest.csv roms/manifest.local.csv
$env:NES_ROM_MANIFEST = "roms/manifest.local.csv"
$env:NES_ROM_ROOT = "nesrom-master"
pio run
```

例如本地文件为 `nesrom-master/My Game.nes` 时：

```csv
display_name,source_file
MY GAME,My Game.nes
```

清单支持 1～60 个条目。打包器会检查 iNES 文件头、文件长度、mapper 和 ROM 分区容量。更详细的格式见 [`ROM_LIBRARY_CN.md`](ROM_LIBRARY_CN.md)。

## 已验证状态

- Xbox `VID:PID 045E:0B12` 的连接、菜单操作、游戏输入和组合键返回已在实机验证。
- GT911 菜单触摸、侧边调节和游戏内上滑返回已验证。
- PCLK 16 MHz 时连续采集超过 4320 个显示帧，稳定约 28.6～28.9 FPS；18 MHz 没有继续提升，因此当前保持 16 MHz。
- 音频使用 48 kHz 和 12×512 DMA；软件统计达到零丢弃、零错误，长期主观听感仍欢迎更多测试。
- `SUPER CONTRA` 曾连续运行至少 1080 帧，无 `Cache error`；这不代表所有 ROM 或 mapper 均已完整验证。

## 已知限制

- 不支持电池存档持久化。
- 未逐一验证所有 ROM 和改版游戏。
- 分区表没有 core dump 分区，启动日志中的 `No core dump partition found` 为已知提示。
- USB 手柄热插拔时偶尔可能出现 root port reset 警告。
- 这是针对一套具体硬件接线的项目，不是任意 ESP32-S3 开发板的即插即用固件。

## 项目结构

```text
src/                 硬件驱动、菜单、输入、音频和模拟器适配
lib/nofrendo/        修改后的 Nofrendo 核心
assets/test.nes      项目原创的最小测试 ROM
roms/manifest.csv    可公开构建的测试清单
tools/               ROM 生成与打包脚本
```

深入的实现和调试记录见 [`PROJECT_CONTEXT_CN.md`](PROJECT_CONTEXT_CN.md)。

## 许可证

本项目以 GNU General Public License v2.0 only 发布，详见 [`LICENSE`](LICENSE)。第三方来源和版权信息见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
