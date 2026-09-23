# ROM 库与 Flash 打包说明

## 公开仓库的默认行为

仓库不包含商业游戏 ROM。默认清单 `roms/manifest.csv` 只引用项目原创的 `assets/test.nes`，因此全新克隆可以完成编译和烧录验证。

`assets/test.nes` 可由下面的命令确定性重新生成：

```powershell
python tools/make_test_rom.py
```

## 使用本地 ROM

本地 ROM 建议放入已被 `.gitignore` 排除的 `nesrom-master/`，然后创建同样被忽略的 `roms/manifest.local.csv`：

```csv
display_name,source_file
MY GAME,My Game.nes
ANOTHER GAME,subdir/Another Game.nes
```

`display_name` 必须是 1～39 字节 ASCII；`source_file` 相对于 ROM 根目录。构建前设置：

```powershell
$env:NES_ROM_MANIFEST = "roms/manifest.local.csv"
$env:NES_ROM_ROOT = "nesrom-master"
pio run
```

不设置这两个环境变量时，构建恢复为公开测试 ROM。

请只使用你有权使用的 ROM，不要将商业 ROM、下载合集、`roms.bin` 或合并固件提交到仓库或 GitHub Release。

## 打包校验

`tools/build_rom_pack.py` 在构建期间检查：

- 清单包含 1～60 个条目；
- 文件具有有效的 iNES 头；
- PRG/CHR 数据没有截断；
- mapper 已在当前 Nofrendo 构建中启用；
- 打包结果没有超出 ROM 分区。

这些静态检查不代表游戏一定兼容。部分改版 ROM、特殊 mapper 行为和存档功能仍需实机验证。

## Flash 布局

`partitions_16mb_no_ota.csv` 使用 16 MB Flash：

| 分区 | Offset | 大小 | 用途 |
| --- | ---: | ---: | --- |
| NVS | `0x9000` | `0x5000` | 设置存储 |
| app0 | `0x10000` | `0x180000` | 固件 |
| roms | `0x190000` | `0xE70000` | ROM 包 |

`tools/pio_rom_pack.py` 在 PlatformIO 构建前生成 `.pio/build/esp32-s3-devkitc-1/roms.bin`，并将它作为 `0x190000` 的附加烧录镜像。

## 仅更新 app0

当 ROM 包和分区表未变化时，可只写 app0，避免重复烧录整个 ROM 分区：

```powershell
pio pkg exec --package tool-esptoolpy -- esptool `
  --chip esp32s3 --port <PORT> --baud 921600 write-flash `
  0x10000 .pio/build/esp32-s3-devkitc-1/firmware.bin
```

更换 ROM、修改分区表或首次烧录时应使用标准 `pio run -t upload`。
