# 全局图标 + 音频替换工具

统一 Windows 的图标与提示音：把全部文件类型的图标换成同一个 `.ico`，把全部系统音效换成同一个
WAV，两者都可完整还原。单文件 C++ 实现，只写 `HKCU`，无需管理员权限。

仓库自带两个可以直接用的样品：图标是蒋结实（`OIP-C_256x256.ico`），音效是
`我不明白.wav`（48 kHz 立体声，1.5 秒）。

## 原理

文件图标不由文件本身决定，而由注册表的「文件类型关联」决定。程序在
`HKCU\Software\Classes` 下为每种类型写入 `DefaultIcon`；用户级注册优先于系统级，
因此不动 HKLM、不改文件内容、可完整还原。

系统音效走另一套：每个「程序 + 事件」在
`HKCU\AppEvents\Schemes\Apps\<程序>\<事件>\.Current` 下存一个 WAV 路径，
`.Current` 是子键、路径写在它的默认值里。把全部事件的这个值指向同一个文件，整机提示音就统一了。

## 用法

```bat
IconAudioReplacer.exe apply   <icon.ico>    :: 应用图标
IconAudioReplacer.exe restore               :: 还原
IconAudioReplacer.exe status                :: 当前状态
IconAudioReplacer.exe list                  :: 列出将被修改的类型
IconAudioReplacer.exe lnktest <icon.ico>    :: 自检快捷方式能否改写（改完立即还原）
IconAudioReplacer.exe lnkstrip <某个.lnk>   :: 自检单个快捷方式的图标位置
IconAudioReplacer.exe sound   <sound.wav>   :: 把所有系统音效换成这个 WAV
IconAudioReplacer.exe soundoff              :: 只还原系统音效
```

双击 exe 进交互菜单；把 `.ico` 或 `.wav` 拖到 exe 上直接应用；双击 `一键恢复.bat` 直接还原。

用仓库自带的样品试一遍：

```bat
IconAudioReplacer.exe apply OIP-C_256x256.ico
IconAudioReplacer.exe sound 我不明白.wav
```

| 选项 | 作用 |
|---|---|
| `--dry-run` | 只统计不写入 |
| `--no-folders` | 不改文件夹与驱动器 |
| `--no-refresh` / `--no-restart` | 不刷新缓存 / 刷新但不重启资源管理器 |
| `--install` | 把图标复制到本机固定目录后再引用 |
| `--index N` | 用 .ico 内的第 N 个图像 |
| `--exclude .ext` | 排除扩展名，可多个 |
| `--yes` | 跳过确认 |
| `--no-lnk` | 不改写快捷方式 |
| `--lnk-dir <目录>` | 追加扫描快捷方式的目录 |
| `--keep-thumbnails` | 保留缩略图 |

## 覆盖情况

- **常规类型**：写入 `DefaultIcon`，覆盖上千种扩展名与关联类型。
- **桌面系统图标**：此电脑、回收站、用户文件夹、网络、控制面板、库不属于文件关联，
  单独写 `Explorer\CLSID\{GUID}\DefaultIcon`，同样只碰 `HKCU`。
- **`.lnk`**：图标存在每个快捷方式文件内部，注册表管不到，改为逐个改写文件本身
  （`IShellLink::SetIconLocation`），原值留档。默认扫桌面、公共桌面、开始菜单、快速启动、
  任务栏固定。
  写完后会摘掉 shell 自动塞进来的 `IconEnvironmentDataBlock`——那个块把路径折算成
  `%SystemDrive%/...` 另存一份，ANSI 字段只有 260 字节，路径一长或代码页表示不了就断，
  图标随即退回原样，是"明明改了却没变"的主要来源。
  只读或占用的文件先清只读再写，写完恢复属性；改不动的会连原因一起列出来。
- **`.ico` 等图片**：系统按图片自身内容显示缩略图，会盖掉 `DefaultIcon`，因此同时关闭缩略图。
  想保留预览用 `--keep-thumbnails`。
- **`.exe`**：写入 `exefile` 的 `DefaultIcon`。shell 在部分视图下仍直接从文件提取原图标，
  注册表层面到此为止。
- **系统音效**：不属于图标，单独一条路。`sound` 把 80 多个「程序 + 事件」的
  `.Current` 统一指向同一个 WAV，改完试听一次确认真的能播。
  系统放音走 `PlaySound`、不做解码，所以只收 WAV——传 mp3 会当场拒绝，而不是改完才发现没声音。
  有些程序会把声音缓存在自己进程里，改完重启该程序才生效。

## 还原

`restore` 还原五样：注册表 `DefaultIcon`（含系统图标）、快捷方式的原图标、缩略图的原设置、
系统音效，随后重启资源管理器。音效还原按事件逐个回到原本的状态——原本有值的写回原值，
原本只有空子键的清掉值，原本连子键都没有的整键删除。
每次 `apply` 会在程序目录 `_restore\` 留一份还原记录副本，
`%LOCALAPPDATA%` 下那份丢失时自动取回。

## 编译

```bat
g++ -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
  -o IconAudioReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32 -lwinmm
```

## 文件

| 文件 | 说明 |
|---|---|
| `IconAudioReplacer.exe` | 主程序，静态单文件，64 位 Windows 7 及以上 |
| `icon_replacer.cpp` | 源码，单文件，无第三方库 |
| `OIP-C_256x256.ico` | 示例图标（蒋结实） |
| `我不明白.wav` | 示例音效，`sound` 可直接用 |
| `一键恢复.bat` | 双击还原 |
| `使用说明.txt` | 详细中文说明 |
