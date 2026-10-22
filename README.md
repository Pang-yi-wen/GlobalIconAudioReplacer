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
- **不改动、不读取、不移动你的任何文件**
- **随时可完整还原**

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

---

## 仓库内容

| 文件 | 说明 |
|---|---|
| `IconReplacer.exe` | 编译好的主程序，静态单文件，无需任何运行环境 |
| `icon_replacer.cpp` | 源码，单文件、无第三方库 |
| `OIP-C_256x256.ico` | 自带的图标示例，图案为蒋结实 |
| `使用说明.txt` | 详细的中文使用说明 |

---

## 自己编译

```bat
"C:\Program Files (x86)\Embarcadero\Dev-Cpp\TDM-GCC-64\bin\g++.exe" ^
  -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
  -o IconReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32
```

（`^` 是 cmd 的换行符；PowerShell 里换成 `` ` `` 反引号。也可以在 Dev-C++ 里新建工程、把 `.cpp` 加进去直接编译。）

---

## 已知例外（不是 bug）

- `.lnk` 快捷方式的图标由它指向的目标决定，通常覆盖不了
- `.ico` 图标文件默认按自身内容显示
- `.exe` 图标内嵌在程序里，部分视图下仍显示原图标
- 任务栏 / 开始菜单 / 桌面快捷方式属于 `.lnk`，不受影响
- 改完后需要重启资源管理器才会立刻生效，程序默认会自动做这一步
  （会关闭你已打开的所有文件夹窗口，不影响文件）

---

## 卸载

先运行一次 `restore` 还原图标，然后把整个文件夹删掉即可。
无残留：注册表里只有 `HKCU\Software\Classes` 下本程序写入的 `DefaultIcon` 项，`restore` 会全部清除。

---

环境要求：64 位 Windows 7 及以上。中文路径完全支持——程序全程用 Unicode（宽字符）读写注册表和文件，
控制台输入输出也不经过 cmd 的代码页。
