# 全局图标替换工具

把 Windows 上所有文件类型的图标统一替换为一个指定的 `.ico`，并可完整还原。
单文件 C++ 实现，只写 `HKCU`，无需管理员权限。

仓库自带的示例图标为蒋结实（`OIP-C_256x256.ico`），可直接试用。

## 原理

文件图标不由文件本身决定，而由注册表的「文件类型关联」决定。程序在
`HKCU\Software\Classes` 下为每种类型写入 `DefaultIcon`；用户级注册优先于系统级，
因此不动 HKLM、不改文件内容、可完整还原。

## 用法

```bat
IconReplacer.exe apply   <icon.ico>    :: 应用图标
IconReplacer.exe restore               :: 还原
IconReplacer.exe status                :: 当前状态
IconReplacer.exe list                  :: 列出将被修改的类型
IconReplacer.exe lnktest <icon.ico>    :: 自检快捷方式能否改写（改完立即还原）
IconReplacer.exe lnkstrip <某个.lnk>   :: 自检单个快捷方式的图标位置
```

双击 exe 进交互菜单；把 `.ico` 拖到 exe 上直接应用；双击 `一键恢复.bat` 直接还原。

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

## 还原

`restore` 还原四样：注册表 `DefaultIcon`（含系统图标）、快捷方式的原图标、缩略图的原设置，
随后重启资源管理器。每次 `apply` 会在程序目录 `_restore\` 留一份还原记录副本，
`%LOCALAPPDATA%` 下那份丢失时自动取回。

## 编译

```bat
g++ -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
  -o IconReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32
```

## 文件

| 文件 | 说明 |
|---|---|
| `IconReplacer.exe` | 主程序，静态单文件，64 位 Windows 7 及以上 |
| `icon_replacer.cpp` | 源码，单文件，无第三方库 |
| `OIP-C_256x256.ico` | 示例图标（蒋结实） |
| `一键恢复.bat` | 双击还原 |
| `使用说明.txt` | 详细中文说明 |
