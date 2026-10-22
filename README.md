# 全局图标替换工具 (Global Icon Replacer)

一个 Windows 单文件小工具：把电脑上**所有文件类型**的图标，统一换成你指定的一个 `.ico` 图标，
包括各种扩展名的文件、文件夹和磁盘驱动器，并且随时可以一键还原。

> **自带的图标示例为蒋结实。**
> 仓库内附带的 `OIP-C_256x256.ico` 就是这个示例图标，下载后可直接拿来试用，无需另外准备图标文件。

---

## 它怎么做到的

Windows 里文件的图标并不保存在文件自身，而是由注册表中的「文件类型关联」决定的。
本程序在当前用户的注册表 `HKCU\Software\Classes` 下，为每一种类型写入 `DefaultIcon` 指向你的图标。
用户级注册优先于系统级，因此：

- **不需要管理员权限**
- **不改动系统原有配置**（HKLM 一个字都没碰）
- **不改动、不读取、不移动你的任何文件**（快捷方式 `.lnk` 除外，见下）
- **随时可完整还原**

连三类原本改不动的「硬骨头」也能统一：

| 类型 | 难点 | 现在的做法 |
|---|---|---|
| `.lnk` 快捷方式 | 图标写在每个 `.lnk` 文件内部，注册表的 `DefaultIcon` 管不到 | 逐个用 `IShellLink::SetIconLocation` 改写文件本身，原值留档用于还原 |
| `.ico` 等图片 | 系统用缩略图处理器按图片自身内容显示，把图标盖掉 | 除写 `DefaultIcon` 外，关闭资源管理器缩略图（`IconsOnly`） |
| `.exe` | shell 直接从文件资源提取图标 | 写入 `exefile` 的 `DefaultIcon`，尽力而为 |

---

## 用法

### 方式 A：双击 `IconReplacer.exe`

```
1  应用图标（把所有文件类型换成同一个 .ico）
2  还原为系统原始图标
3  查看当前状态
4  列出将被修改的类型
0  退出
```

### 方式 B：拖放

把 `.ico` 文件拖到 `IconReplacer.exe` 的图标上，松手即应用。

### 方式 C：命令行

```bat
IconReplacer.exe apply   D:\my.ico      :: 应用图标
IconReplacer.exe restore                 :: 还原为系统原始图标
IconReplacer.exe status                  :: 查看当前状态
IconReplacer.exe list                    :: 列出将被修改的类型
IconReplacer.exe lnktest <ico>           :: 自检快捷方式能否改写（改完立即还原）
IconReplacer.exe help                    :: 帮助
```

可选开关：

| 开关 | 作用 |
|---|---|
| `--dry-run` | 只统计不写入，先看看会改多少项 |
| `--no-folders` | 只改文件，不改文件夹和驱动器 |
| `--no-refresh` | 不刷新图标缓存（手动重启资源管理器后才生效） |
| `--no-restart` | 刷新缓存但不重启资源管理器 |
| `--install` | 把图标复制到本机固定目录后再引用（换电脑也不失效） |
| `--index N` | 用 `.ico` 里的第 N 个图像，默认 0 |
| `--exclude .lnk` | 排除某个扩展名，可写多个 |
| `--yes` | 跳过确认，直接执行 |
| `--no-lnk` | 不改写快捷方式（`.lnk`）文件 |
| `--lnk-dir <目录>` | 额外扫描该目录下的快捷方式，可重复写 |
| `--keep-thumbnails` | 不关闭缩略图（图片仍按自身内容显示） |

### 快捷方式扫描范围

默认扫：桌面、公共桌面、开始菜单、快速启动、任务栏固定（User Pinned\TaskBar）。
其它目录用 `--lnk-dir` 追加。每个被改写的快捷方式，其原始图标位置都记在
`%LOCALAPPDATA%\GlobalIconReplacer\applied_lnk.txt`，`restore` 时逐个写回。

不确定能不能改？先跑自检——它把每个快捷方式「改一次再还原」，不动注册表：

```bat
IconReplacer.exe lnktest OIP-C_256x256.ico
```

---

## 仓库内容

| 文件 | 说明 |
|---|---|
| `IconReplacer.exe` | 编译好的主程序，静态单文件，无需任何运行环境 |
| `一键恢复.bat` | 双击即可完整还原（等同于 `restore`） |
| `icon_replacer.cpp` | 源码，单文件、无第三方库 |
| `OIP-C_256x256.ico` | 自带的图标示例，图案为蒋结实 |
| `使用说明.txt` | 详细的中文使用说明 |

运行时会在程序目录生成 `_restore\`，存放还原记录的副本——别删它。

---

## 自己编译

```bat
"C:\Program Files (x86)\Embarcadero\Dev-Cpp\TDM-GCC-64\bin\g++.exe" ^
  -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
  -o IconReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32
```

（`^` 是 cmd 的换行符；PowerShell 里换成 `` ` `` 反引号。也可以在 Dev-C++ 里新建工程、把 `.cpp` 加进去直接编译。）

---

## 一键恢复

三种方式，效果完全一样：

1. **双击 `一键恢复.bat`** —— 最省事
2. 双击 `IconReplacer.exe` 后菜单选 **2**
3. 命令行 `IconReplacer.exe restore`

`restore` 一次还原三样东西：注册表里的 `DefaultIcon`、被改写过的快捷方式图标（逐个写回原值）、
被关掉的缩略图设置（恢复成你原来的样子）。之后会重启资源管理器，图标立刻回到系统原始状态。

**记录丢了也能恢复**：每次 `apply` 都会在程序目录的 `_restore\` 里另存一份还原记录。
如果 `%LOCALAPPDATA%` 下那份被清理工具删掉了，`restore` 会自动从这里取回——所以别删 `_restore\` 文件夹。

## 三类硬骨头的处理细节

**`.lnk`** —— 图标存在快捷方式文件内部，注册表管不到，所以只能逐个改写文件本身
（`IShellLink::SetIconLocation`）。原值留档，`restore` 写回。
若某个快捷方式只读或正被占用会跳过，状态里能看到失败数。

**`.ico`** —— 系统会用缩略图处理器按图片内容显示，把 `DefaultIcon` 盖掉，
所以顺手关掉缩略图（等于勾选「始终显示图标，从不显示缩略图」）。
代价：图片/视频不再预览。想保留预览就加 `--keep-thumbnails`。

**`.exe`** —— 已写入 `exefile` 的 `DefaultIcon`。shell 在部分视图下仍直接从文件资源
提取原图标，这是系统行为，注册表层面到此为止。

另外：任务栏 / 开始菜单 / 桌面快捷方式属于 `.lnk`，现在同样会被改写；
改完后需要重启资源管理器才立刻生效，程序默认自动做（会关掉已打开的文件夹窗口，不影响文件）。

---

## 卸载

先运行一次 `restore` 还原图标，然后把整个文件夹删掉即可。
无残留：`HKCU\Software\Classes` 下写入的 `DefaultIcon`、`Explorer\Advanced` 下的缩略图开关、
以及改写过的快捷方式，`restore` 会全部清除或还原。

---

环境要求：64 位 Windows 7 及以上。中文路径完全支持——程序全程用 Unicode（宽字符）读写注册表和文件，
控制台输入输出也不经过 cmd 的代码页。
