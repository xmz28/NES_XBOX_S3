# NES_XBOX_S3 工程接手说明

> 更新日期：2026-08-26
> 用途：供本人查阅，也供 Codex 新聊天窗口快速恢复工程上下文。结论中会明确区分编译、烧录、串口日志验证和人工体验。

## 1. 工程目标和当前结论

本工程把 ESP32-S3-N16R8 做成一台 NES 掌机，硬件包括 800×480 RGB 屏、GT911 触摸、Xbox USB 手柄和 MAX98357A。当前版本取消 OTA，保持 app0 的 `0x10000` Offset 不变，在 16 MB Flash 中提供最多 60 款游戏的 ROM 分区。公开仓库默认只打包原创测试 ROM。

GPIO48 现在专用于 GT911 SCL，不能同时作为状态灯或其他推挽输出。

当前主链路已经打通：开机菜单 → 选择游戏 → 从 Flash ROM 分区读入可写 PSRAM → Nofrendo 运行 → Xbox 完整按键状态逐帧送入模拟器 → `View + Menu` 安全停止模拟器并直接返回菜单。

最近修复的关键问题是“进入游戏后方向像乱跳/卡住”。它包含两个独立风险：

1. 旧版 CPU/mapper 代码可能通过 ROM bank 指针写数据，直接把 ROM 映射到只读 XIP Flash 会触发 `Guru Meditation Error: Cache error`。
2. 只发送按下/松开的边沿事件，丢失一次松开事件就可能让方向长期保持。

现在游戏 ROM 会完整复制到可写 PSRAM，手柄则每个模拟器帧覆盖一次完整状态。修复后已通过实机串口日志验证，但不同 ROM 仍需分别验证。

## 2. 代码入口和关键文件

| 文件 | 作用 |
|---|---|
| `platformio.ini` | 16 MB Flash、8 MB OPI PSRAM、构建目录和 ROM 预构建脚本 |
| `partitions_16mb_no_ota.csv` | 无 OTA 分区表，app0 固定在 `0x10000` |
| `src/main.cpp` | 启动顺序、菜单循环、手柄/触摸事件和进入游戏前清屏 |
| `src/game_menu.cpp/.h` | 最多 60 款游戏的菜单、分页、手柄长按连发和触摸点击/滑页手势 |
| `src/rom_catalog.cpp/.h` | 读取 Flash 目录，把所选 ROM 复制到 PSRAM并释放 |
| `src/nes_runtime.cpp/.h` | Nofrendo 任务、画面输出、音频回调和逐帧手柄状态 |
| `lib/nofrendo/src/event.c/.h` | `event_set_joypad1()`，一次替换完整 Joypad 1 状态 |
| `src/display_panel.cpp` | 800×480 RGB565 三缓冲、切帧同步和背光 PWM |
| `src/touch_input.cpp` | GT911 探测、坐标转换和触摸轮询 |
| `src/usb_xbox_controller.cpp` | 严格识别 Xbox `045E:0B12` 并解析 USB 输入报告 |
| `src/audio_output.cpp` | MAX98357A 48 kHz I2S、音频队列、静音预填和丢块统计 |
| `roms/manifest.csv` | 可公开构建的原创测试 ROM 清单 |
| `tools/build_rom_pack.py` | 校验清单、iNES、mapper、容量并生成 `roms.bin` |
| `tools/pio_rom_pack.py` | PlatformIO 预构建与 `0x190000` 附加烧录 |
| `ROM_LIBRARY_CN.md` | 游戏库和分区的专项说明 |

## 3. Flash 和内存布局

| 区域 | Offset | 大小 | 说明 |
|---|---:|---:|---|
| NVS | `0x9000` | `0x5000` | 亮度、音量等设置 |
| boot_app0 保留区 | `0xE000` | `0x2000` | Arduino/PlatformIO 启动辅助和对齐 |
| app0 | `0x10000` | `0x180000` | 单 factory 应用，最大 1.5 MiB |
| roms | `0x190000` | `0xE70000` | 自定义 subtype `0x40`，一直到 16 MB Flash 末尾 |

没有 OTA，也没有 core dump 分区。`app0` Offset 必须继续保持 `0x10000`。当前 `roms.bin` 为 13,054,816 字节，分区余量约 1.99 MiB。

运行游戏时，不再使用 `esp_partition_mmap()` 让模拟器直接访问只读 Flash。`rom_catalog` 使用带 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` 的内存分配，再用 `esp_partition_read()` 把当前 ROM 复制进去。退出/失败路径必须调用 `unmapGame()` 释放这块 PSRAM；函数名保留了历史命名，但行为已经是“释放复制缓冲区”。

## 4. 启动和运行流程

1. PlatformIO 构建前执行 `tools/pio_rom_pack.py`，从 `roms/manifest.csv` 和本地 `nesrom-master` 生成 `roms.bin`。
2. 设备启动显示屏、三帧缓冲、背光、音频、GT911 和 USB Host。
3. 从 `roms` 分区读取 `NESPACK1` 目录，显示每页 10 款的菜单。
4. 用户选择后，所选 ROM 从 Flash 复制进可写 PSRAM。
5. 清空并提交全部三个显示帧缓冲，避免 512×480 游戏区域之外残留菜单文字。
6. Nofrendo 在 Core 1 的独立任务运行；每次需要显示时先把 256×240 索引画面快照到已有驱动暂存区，再由 Core 0 异步进行 2 倍最近邻缩放并提交到屏幕中间 512×480 区域。若上一帧仍在渲染，则只丢弃过期显示帧，不阻塞模拟器。
7. 每个模拟器帧都由 `osd_getinput()` 调用 `event_set_joypad1(gButtons)`，完整覆盖 Joypad 1 状态；输入不再等到低帧率 LCD 提交后才更新。
8. `View + Menu` 或游戏画面底部中央上滑提出退出请求；Nofrendo 在自己的帧边界停止，关闭异步渲染、音频和定时器、释放 ROM PSRAM，再由主循环直接恢复菜单。ESP32-S3、显示驱动、USB Host 和 Xbox 连接均不重启。
9. 组合键返回后，如果手柄仍有任意键处于按下状态，菜单输入保持屏蔽；收到一次全零报告后才恢复，避免仍按着 `Menu/Start` 导致刚回菜单又启动游戏。

旧版 Nofrendo 还保留了这些重要适配，后续不要无故回退：

- `NES_VISIBLE_HEIGHT=240`，避免以 224 行缓冲渲染 240 扫描线造成越界。
- 修正 iNES Trainer/数据指针偏移和 ROM/VROM 所有权处理。
- 避免 Nofrendo 日志符号与 Arduino 日志宏冲突。
- Nofrendo 固定在 Core 1、优先级 1；提高优先级可能饿死 Arduino `loop()`，使 GT911 和 Xbox 事件不再处理。

## 5. 菜单、手柄和触摸操作

菜单操作：

- 十字键上/下：移动选择；持续按住约 400 ms 后，每约 90 ms 连续移动一次。
- 十字键左/右：前后翻 10 款。
- `A` 或 `Menu/Start`：启动选中游戏。
- 菜单会绘制左右亮度/音量条，触摸两侧可直接调整并保存。
- 轻触中间 `x=150..649` 的有效列表行：松手后启动；手指移动超过点击阈值时不会误启动。
- 在菜单中间上滑至少 60 像素进入下一页，下滑至少 60 像素返回上一页。

游戏内映射：

| Xbox | NES |
|---|---|
| A | A |
| B | B |
| View | Select |
| Menu | Start |
| 十字键 | 上、下、左、右 |
| View + Menu | 安全停止当前游戏并直接返回菜单；松开全部按键后菜单才重新接收输入 |

注意：Xbox 报告层使用位掩码，NES `kb_input.data` 也使用状态位。不要恢复为仅发送 `event_joypad1_up/down` 边沿事件，否则 USB 报告或任务调度丢掉松开边沿后，方向可能卡住。`osd_getinput()` 必须在每个模拟器帧执行，不能只跟随 `vid_flush()`，否则十几 FPS 的显示会直接带来明显按键延迟。相反方向同时出现时由 Nofrendo 原逻辑处理，不应凭菜单表现推断游戏输入正确。

游戏触屏返回手势必须从底部中央开始：起点 `y>=400`、`x=144..655`，向上移动至少 100 像素并松手后请求返回菜单。退出必须经过 `main_requestquit()`，等模拟器任务完成 `nes_poweroff → main_eject → osd_shutdown` 后才能释放 ROM PSRAM；不能由 Arduino 主循环强杀任务或提前释放内存。

## 6. 硬件接口

屏幕为 16 位并行 RGB565：

- PCLK：GPIO1
- HSYNC：GPIO39
- VSYNC：GPIO40
- DE：GPIO41
- RGB D0～D15：GPIO4～GPIO18、GPIO21
- 背光 PWM：GPIO42

屏幕参数为 800×480、PCLK 16 MHz、下降沿采样；水平时序 48/40/8，垂直时序 2/40/6。驱动使用 3 个完整 PSRAM 帧缓冲、30 行 bounce buffer 和 64 字节 DMA burst。背光为 20 kHz、11 bit、高电平有效 PWM；GPIO42 只能接 BL/LED_EN 逻辑端，不能直接给背光 LED 供电。

按总时序 896×528 计算，12 MHz理论上限约25.38 FPS，16 MHz理论上限约33.82 FPS。16 MHz实机稳定约28.6～28.9 FPS，说明当前还存在渲染调度和帧缓冲同步开销；不要把降低800×480输出分辨率作为默认方案。

2026-08-26 已启用第一轮环境A/B：直接测试 `PERF + 64B Cache Line + PSRAM XIP` 完整组合。公开版本把
相关设置保存在项目内的 `sdkconfig.performance.defaults`，不依赖开发者电脑上的全局 Core 或其他工程路径。
默认 `esp32-s3-devkitc-1` 环境使用 PioArduino 官方预编译库，便于全新克隆直接构建；需要复现实机性能配置时使用
`pio run -e esp32-s3-performance -j 1`。性能环境首次构建会从源码重编译完整 Arduino/ESP-IDF 框架，耗时较长且需要联网获取托管组件。

这仍是性能试验，不代表XIP一定有利。PERF和64B Cache Line可能提高模拟器及LCD内存路径性能，但
NES ROM、完整帧缓冲和运行数据已经占用PSRAM，XIP还会把指令和只读数据放入PSRAM，可能增加带宽
竞争。必须记录实际FPS、渲染耗时、输入延迟、画面错位/闪屏和音频稳定性，并与当前12 MHz稳定版
对比。若最终需要关闭 XIP 或修改其他 sdkconfig，应同步记录实机性能和稳定性变化。

NES 原始 256×240 画面以最近邻 2 倍放大到 512×480，位于 `x=144..655`。左右各 144 像素保留给亮度和音量条；亮度范围 5%～100%、默认 100%，音量范围 0%～100%、默认 35%。触摸松开后写入 NVS 命名空间 `nes-ui`，键为 `bright` 和 `volume`。

GT911：

- SDA：GPIO47
- SCL：GPIO48
- INT：GPIO2
- 实机启动时可能探测到地址 `0x5D` 或 `0x14`，产品号为 `911`，两种地址均已兼容。

触摸坐标按 800×480 直接使用，没有交换或镜像。

MAX98357A：

- BCLK：GPIO3
- LRC：GPIO45
- DIN：GPIO46

功放必须与 ESP32-S3 共地，模块 VIN 是否可接 5 V 以具体模块规格为准。GPIO3、GPIO45、GPIO46 与启动配置有关，不要外加会在上电时强拉电平的上拉/下拉。

MAX98357A 的 LRCLK 使用其明确支持的 48 kHz，每个 NES 60 Hz 模拟器帧生成 800 个采样。音频推进位于 Nofrendo 的 `system_video()`，包括被 LCD 跳过的帧；不能再移回 `videoBlit()`，否则真实显示只有约 12～14 FPS 时，I2S 会长期欠载并产生刺啦声。

音频保持原始双声道 PCM，不启用 EQ；使用显式 12×512 DMA、欠载自动清零、启动静音预填、深度 6 队列和最多 25 ms 等待。旧 `ESP_I2S` 路径实测音频任务偶尔会有 45.8～58.8 ms 写入调度间隙，而默认约 30 ms DMA 储备不足，背景音乐中会表现为每隔数秒偶发刺啦；当前约 128 ms DMA 储备可以覆盖这些间隙。

本次“先有轻微电流声、十几秒后加重、一分钟后几乎无声”以及后续完全静音的真实根因，是两块 ESP32 同时连接并驱动同一块 MAX98357A。BCLK、LRC 和 DIN 都是主控侧推挽输出，不能直接把两套输出并接。硬件只能保留一个 I2S 主控；如确需两个主控共享功放，必须用能保证 break-before-make 的数字开关/总线复用器或三态缓冲，并确保未选中的主控与三根信号线高阻隔离。

诊断格式为 `audioQ=排队数/实际写入数 pending=等待数 drop=丢块数 err=写入错误数 peak=处理后峰值`。正常情况下前两个计数应同步增长，`pending/drop/err` 应持续为 0；`peak=0` 只表示该时段游戏本身静音。若 `peak` 非零且扬声器仍随时间逐渐无声，应优先检查功放供电压降、过热保护、SD/MODE 脚、扬声器接法和电源去耦，而不是继续扩大软件队列。

Xbox 手柄使用 ESP32-S3 USB Host，当前严格匹配 `VID:PID 045E:0B12`。若换手柄型号，需要先抓真实描述符和报告格式，不能仅放宽名称或做模糊匹配。

- USB D-：GPIO19
- USB D+：GPIO20
- USB 口必须有可靠的 5 V VBUS 供电能力；D+/D- 初始化成功不代表手柄已经得到供电。

## 7. ROM 打包规则

公开版本默认使用 `assets/test.nes`。个人 ROM 可放入项目下被忽略的 `nesrom-master`，并通过 `NES_ROM_MANIFEST` 和 `NES_ROM_ROOT` 环境变量选择本地清单及根目录。清单中每行提供 ASCII 显示名和源文件相对路径。构建脚本会检查：

- 游戏数必须是 1～60。
- 显示名必须是 1～39 字节 ASCII。
- 文件存在且具有有效 iNES 头。
- mapper 已由当前 Nofrendo 构建启用。
- 打包后不超过 `0xE70000`。

包格式以 `NESPACK1` 开头，后面是定长目录和对齐后的 ROM 数据。运行时只解析目录并读取选中项，不把全部 ROM 同时装入 PSRAM。默认清单为 `roms/manifest.csv`；本地覆盖方式见 `ROM_LIBRARY_CN.md`。

## 8. 构建、烧录和串口

标准命令：

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
pio run
pio run -t upload --upload-port <PORT>
pio device monitor --port <PORT> --baud 115200
```

复现实机性能配置：

```powershell
pio run -e esp32-s3-performance -j 1
```

正常 `pio run -t upload` 会同时写入 ROM 包，因为 `tools/pio_rom_pack.py` 将 `roms.bin` 以 `FLASH_EXTRA_IMAGES` 注册到标准上传目标。当前 ROM 原始大小为 13,054,816 字节、压缩后约 6.92 MB；仅这一段在 921600 波特率下实测约 95 秒，所以完整上传通常需要约两分钟。这不是编译环境变慢，也不是 16/18 MHz 或触控修改导致的。构建目录已恢复为项目内默认 `.pio`：

```text
.pio\build\esp32-s3-devkitc-1
```

确认 ROM 分区已经存在且只改了程序时，可只写 app0：

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
pio pkg exec --package tool-esptoolpy -- esptool --chip esp32s3 --port <PORT> --baud 921600 write-flash 0x10000 .pio\build\esp32-s3-devkitc-1\firmware.bin
```

不要省略 UTF-8 环境变量：Windows 下 esptool 的 Unicode 进度条可能在擦除后因控制台编码报错。若只写 app0，必须先确认设备上的分区表和 `roms.bin` 与当前工程匹配。

## 9. 2026-08-26 已验证结果

以下是当前源码的实际结果，不是预期描述：

- `PERF + 64B + XIP` 完整 PlatformIO 编译成功：RAM 120,660 / 327,680 字节（36.8%），app 529,493 / 1,572,864 字节（33.7%），`firmware.bin` 为 536,864 字节。
- 本地 60 款 ROM 曾打包成功：13,054,816 字节，约 12.45 MiB；这些 ROM 不随公开仓库分发。
- 实机已完成整包烧录和 app-only 烧录，两次均通过写入哈希校验。
- `SUPER CONTRA` 已从 Flash 复制 253,968 字节到 PSRAM，mapper 4，连续运行至少 1080 帧，没有再次出现 `Cache error`。
- 串口确认手柄状态出现 B=`0x02`、A+B=`0x03`、右=`0x80`，松开后回到 `0x00`；`View + Menu` 能回到菜单。
- 最新清屏版本烧录后运行至少 240 帧，没有崩溃。
- 优化前真实游戏实测约 12.4～14.2 FPS。同步行展开优化后约 17.9 FPS；最终异步版本在《超级马里奥》稳定段实测 25.3～25.5 FPS，快照约 1.9～2.3 ms、缩放渲染约 21.4～22.9 ms，已基本达到当前面板时序上限，显示丢帧很少。最终实现复用原有 256×240 驱动暂存区，不额外占用 61 KiB 内部 RAM。
- 同游戏/12 MHz下启用 `PERF + 64B + XIP` 后连续采集到1680个显示帧，FPS稳定25.3～25.4；缩放渲染约17.5～20.2 ms，较原21.4～22.9 ms明显下降，但FPS已被约25.38的面板时序上限限制。采集期间音频保持 `pending=0`、`drop=0`、`err=0`、`gap=0`；用户现场确认画面正常，没有闪烁、变色或水平错位。
- 将PCLK提高到16 MHz后，用户确认画面正常、不闪屏。串口连续采集超过4320个显示帧，稳定段约28.6～28.9 FPS，偶发低点约27.2～27.4 FPS，缩放渲染约17.0～19.1 ms；相对12 MHz稳定段提升约13%。音频继续保持 `pending=0`、`drop=0`、`err=0`、`gap=0`。
- 18 MHz 已完成实机试验：连续采集超过8600个显示帧，静态段约27.8～28.1 FPS，未高于16 MHz稳定段；音频仍为零丢弃、零错误。ESP32-S3可从160 MHz源准确分数分频得到18 MHz，结果不佳主要说明瓶颈已转移到约16.7～18.2 ms的缩放渲染、PSRAM带宽和帧提交调度，而不是时钟取整。源码及实机已回退到16 MHz稳定配置。
- GT911 使用 Core 0 独立轮询任务、单元素最新状态队列、I2C 互斥、三次读取重试，以及连续 5 次失败后重启 I2C 并重新探测。任务优先级为 0，避免抢占同核游戏渲染。长时间轮询仍可能极偶发底层 `ESP_ERR_INVALID_STATE`，现有重试可吸收且实测触控仍能工作；若要完全消除底层日志，应评估直接使用 ESP-IDF I2C 驱动或 GT911 中断触发读取。
- 串口监视器现固定 `monitor_dtr = 0`、`monitor_rts = 0`，防止关闭监视器后DTR/RTS经开发板自动下载电路持续影响EN/BOOT。实测关闭并重新打开监视器后程序连续运行、没有重新启动日志。
- 输入采样已从“每个显示帧一次”改为“每个模拟器帧一次”，因此不会再被约 12～25 FPS 的 LCD 提交周期限制；主观延迟仍需用户现场复核。
- Xbox `045E:0B12`、GT911 菜单/侧边触摸和屏幕显示均有实机日志或现场画面证据。
- 菜单上滑下一页、下滑上一页已多次产生正确串口日志。无重启退出版本已在 `SUPER MARIO BROS 1/2/3`、`SUPER CONTRA` 中反复进出十余次，手柄组合键和底部上滑均成功，未再出现白屏、堆损坏或 Xbox 断连。最新版异步渲染下也连续完成多次组合键退出/重新进入；每次串口均先出现 `controller released; menu input enabled`，确认菜单在组合键全部释放前保持屏蔽。
- 旧音频路径测得最大写入间隙 45.8～58.8 ms 且疑似欠载计数持续增加；12×512 DMA 版本在相同最大间隙下保持 `gap=0`、`pending=0`、`audioDrop=0`、`audioErr=0`。软件侧验证通过，背景音乐最终听感等待用户确认。

仍未验证或仅部分验证：

- 不同 ROM 没有逐一启动和通关；输入手感仍应由用户结合具体游戏现场确认。
- MAX98357A 的真实输出已确认；最新深 DMA 版本的软件连续性已验证，背景音乐刺啦是否完全消失仍待用户最终确认，且尚未进行数小时级压力测试。
- 电池存档未实现，RPG 关机存档不能依赖。
- USB 多次拔插的长期稳定性未完成压力测试。

## 10. 已知风险和排查提示

- 若再出现 `Guru Meditation Error: Cache error`，先确认串口中的 ROM 地址来自 PSRAM，而不是 `0x3C...` 的 Flash 映射地址；再检查是否有其他只读指针被 mapper 写入。
- 若人物持续向某方向移动，打印 `gButtons` 和 `kb_input.data`，必须能在松开后同帧归零；不要先调整摇杆死区，因为当前使用的是十字键位。
- `No core dump partition found` 是无 core dump 分区导致的已知提示，不是本次崩溃根因。诊断崩溃依赖串口回溯。
- mapper 静态检查通过不等于游戏完全兼容，部分中文改版、Hack 或特殊存储行为仍可能失败。
- app 超过 1.5 MiB 会在构建阶段失败。若未来确需扩大 app，应从 ROM 分区头部向后挪，不能改变 `app0=0x10000`，并同步脚本中的 ROM Offset 和容量。
- 标准 `upload` 每次都会重写约12.45 MiB ROM分区，即使只改了一行程序；不要把约95秒的ROM写入误判为编译或独立Core变慢。ROM和分区表未变化时优先使用第8节的app-only命令；只有更换游戏、修改分区表或首次烧录时才需要完整上传。
- Nofrendo 代码带 GPL 许可；发布固件或产品前需要复核许可证义务。

## 11. 新聊天窗口建议接手顺序

1. 先读本文件、`README.md`、`ROM_LIBRARY_CN.md`。
2. 执行 `git status --short` 和 `git log -5 --oneline`，确认是否有用户未提交修改。
3. 查看 `platformio.ini`、`partitions_16mb_no_ota.csv` 和 `roms/manifest.csv`，确认构建目标与 Flash 内容。
4. 输入问题优先看 `src/main.cpp`、`src/nes_runtime.cpp`、`lib/nofrendo/src/event.c` 和 `src/usb_xbox_controller.cpp`。
5. ROM 崩溃优先看 `src/rom_catalog.cpp` 以及串口中的 mapper、PSRAM 地址和 Guru Meditation 回溯。
6. 修改后先 `pio run`；需要实机结论时再烧录并保留串口日志。编译成功不能表述成实机已经正常。
7. 不要提交 `nesrom-master` 下下载的游戏文件；只提交清单、工具、源码和文档。
