// 全局图标替换工具 (Global Icon Replacer) — C++ 单文件版
//
// 自带的图标示例为蒋结实（即仓库内附带的 OIP-C_256x256.ico，可直接拿来试用）。
//
// 编译: g++ -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
//         -o IconReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32 -lwinmm
//
// 原理: Windows 的文件图标不由文件决定, 而由注册表中的"文件类型关联"决定。
//       本程序在 HKCU\Software\Classes 下为每种类型写入 DefaultIcon,
//       使其统一指向指定 .ico。用户级注册优先于系统级, 无需管理员权限。
//
// 三种"硬骨头"类型的处理:
//   .lnk  快捷方式的图标写在每个 .lnk 文件内部, 注册表的 DefaultIcon 管不到它,
//         所以逐个用 IShellLink::SetIconLocation 改写文件本身, 并把原图标位置
//         记进状态文件, restore 时原样写回。
//   .ico  系统会用缩略图处理器按图片自身内容显示, 把 DefaultIcon 盖掉,
//         所以除写 DefaultIcon 外还要关掉资源管理器缩略图 (IconsOnly)。
//   .exe  写入 exefile 的 DefaultIcon (尽力而为); shell 在部分视图下仍会直接
//         从文件资源里提取原图标, 这是系统行为, 注册表层面无法再进一步。
//
// 编码: 全程 UTF-16 宽字符。控制台输入输出走 ReadConsoleW/WriteConsoleW,
//       不经过控制台代码页, 因此中文路径在 GBK(936) 的 cmd 下也不会损坏;
//       状态文件以 UTF-16LE 存储, 不做任何编码往返。
//
// 用法:
//   IconReplacer.exe                  无参数 -> 交互菜单
//   IconReplacer.exe <icon.ico>       把 .ico 拖到 exe 上 -> 直接应用
//   IconReplacer.exe apply <ico> [选项]
//   IconReplacer.exe restore [--no-refresh]
//   IconReplacer.exe status
//   IconReplacer.exe list
//
// 选项:
//   --dry-run              只统计不写入
//   --no-folders           不改文件夹/驱动器
//   --no-refresh           不刷新图标缓存
//   --no-restart           刷新但不重启资源管理器
//   --install              把图标复制到本机固定目录后再引用
//   --index N              图标索引, 默认 0
//   --exclude .a .b ...    排除指定扩展名
//   --yes                  跳过确认
//   --no-lnk               不改写快捷方式 (.lnk) 文件
//   --lnk-dir <目录>       额外扫描该目录下的快捷方式, 可写多次
//   --keep-thumbnails      不关闭缩略图 (图片仍按自身内容显示)

#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

static const wchar_t* HKCU_CLASSES = L"Software\\Classes";
static const wchar_t* APP_NAME     = L"GlobalIconReplacer";

// ------------------------------------------------------------ 系统图标
//
// 桌面上的"此电脑""回收站"之类不是 .lnk, 也不是文件类型关联, 改文件关联
// 对它们毫无作用。它们各自有个 CLSID, 图标写在
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{GUID}\DefaultIcon
// 这里用 "@SHELL:{GUID}" 当作类型名, 复用同一套写入/还原/记录流程。
static const wchar_t* SHELL_PREFIX = L"@SHELL:";

struct ShellIconDef { const wchar_t* guid; const wchar_t* label; };
static const ShellIconDef SHELL_ICONS[] = {
    {L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"此电脑"},
    {L"{645FF040-5081-101B-9F08-00AA002F954E}", L"回收站"},
    {L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}", L"用户文件夹"},
    {L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"网络"},
    {L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}", L"控制面板"},
    {L"{031E4825-7B94-4DC3-B131-E946B44C8DD5}", L"库"},
};
static const int SHELL_ICON_COUNT = (int)(sizeof(SHELL_ICONS) / sizeof(SHELL_ICONS[0]));

static bool isShellClass(const std::wstring& cls) {
    return cls.compare(0, wcslen(SHELL_PREFIX), SHELL_PREFIX) == 0;
}

static std::wstring regSubForClass(const std::wstring& cls) {
    if (isShellClass(cls))
        return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID\\" +
               cls.substr(wcslen(SHELL_PREFIX)) + L"\\DefaultIcon";
    return std::wstring(HKCU_CLASSES) + L"\\" + cls + L"\\DefaultIcon";
}

// ------------------------------------------------------------ 编码辅助

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}

static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower((wint_t)c);
    return r;
}

static bool isConsole(HANDLE h) {
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

static bool& bomWritten() { static bool b = false; return b; }

// 直接写宽字符到控制台, 完全绕开代码页
static void outRaw(const std::wstring& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (isConsole(h)) {
        DWORD w = 0;
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &w, NULL);
    } else {
        // 输出被重定向: 写 UTF-8; 若重定向到真实文件, 补 BOM 方便记事本识别
        if (!bomWritten()) {
            if (GetFileType(h) == FILE_TYPE_DISK) fwrite("\xEF\xBB\xBF", 1, 3, stdout);
            bomWritten() = true;
        }
        fputs(utf8(s).c_str(), stdout);
        fflush(stdout);
    }
}

static void P() { outRaw(L"\n"); }
static void P(const wchar_t* s) { outRaw(s); outRaw(L"\n"); }
static void P(const std::wstring& s) { outRaw(s); outRaw(L"\n"); }
static void P(const char* s) { outRaw(fromUtf8(s)); outRaw(L"\n"); }
static void P(const std::string& s) { outRaw(fromUtf8(s)); outRaw(L"\n"); }

static void hr(const wchar_t* title = 0) {
    if (title) {
        outRaw(L"\n==========================================================\n  ");
        outRaw(title);
        outRaw(L"\n==========================================================\n");
    } else {
        outRaw(L"----------------------------------------------------------\n");
    }
}

static std::wstring W(int n) { return std::to_wstring(n); }

// 读一行: 控制台用 ReadConsoleW (宽字符), 管道/重定向时按 UTF-8 读
static std::wstring readLine(const std::wstring& prompt) {
    outRaw(prompt);
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (isConsole(h)) {
        std::wstring line;
        wchar_t ch = 0;
        DWORD n = 0;
        while (ReadConsoleW(h, &ch, 1, &n, NULL) && n == 1) {
            if (ch == L'\r') continue;
            if (ch == L'\n') break;
            line += ch;
            if (line.size() > 4000) break;
        }
        // 去掉首尾空格与包围的引号 (拖放/粘贴路径时常见)
        while (!line.empty() && (line.front() == L' ' || line.front() == L'"')) line.erase(line.begin());
        while (!line.empty() && (line.back() == L' ' || line.back() == L'"')) line.pop_back();
        return line;
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) { outRaw(L"\n"); return L""; }
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '"')) s.pop_back();
    outRaw(L"\n");
    return fromUtf8(s);
}

// ------------------------------------------------------------ 文件 (UTF-16)

static bool writeTextUtf16(const std::wstring& path, const std::wstring& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring body = L"\xFEFF" + content;      // BOM
    DWORD written = 0;
    BOOL ok = WriteFile(h, body.c_str(), (DWORD)(body.size() * sizeof(wchar_t)), &written, NULL);
    CloseHandle(h);
    return ok;
}

static bool readTextUtf16(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size < 2) { CloseHandle(h); return false; }
    std::vector<char> buf((size_t)size);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf.data(), size, &rd, NULL);
    CloseHandle(h);
    if (!ok || rd < 2) return false;
    const wchar_t* w = (const wchar_t*)buf.data();
    size_t n = (size_t)rd / sizeof(wchar_t);
    if (w[0] == 0xFEFF) out.assign(w + 1, n - 1);
    else out.assign(w, n);
    return true;
}

// ------------------------------------------------------------ 路径 / 状态

static std::wstring appDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\Windows\\Temp");
    std::wstring dir = base + L"\\" + APP_NAME;
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static std::wstring stateFile() { return appDir() + L"\\applied.txt"; }
static std::wstring installedIcon() { return appDir() + L"\\icon.ico"; }

static bool fileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring toAbs(const std::wstring& p) {
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, full, NULL);
    if (n > 0 && n < MAX_PATH * 4) return std::wstring(full);
    return p;
}

// ------------------------------------------------------------ 注册表

static bool regReadDefault(HKEY root, const std::wstring& sub, std::wstring& out) {
    wchar_t buf[1024];
    DWORD sz = sizeof(buf);
    LONG r = RegGetValueW(root, sub.c_str(), NULL, RRF_RT_REG_SZ, NULL, buf, &sz);
    if (r != ERROR_SUCCESS) return false;
    out.assign(buf, sz / sizeof(wchar_t));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return true;
}

static bool hkcuKeyExists(const std::wstring& sub) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

// 写入 HKCU\Software\Classes\<cls>\DefaultIcon
// created: 本次是否新建该键; hadOld: 原本是否已有默认值
static bool writeDefaultIcon(const std::wstring& cls, const std::wstring& value,
                             bool& created, bool& hadOld, std::wstring& old) {
    std::wstring sub = regSubForClass(cls);
    created = !hkcuKeyExists(sub);
    old.clear();
    hadOld = false;
    if (!created) hadOld = regReadDefault(HKEY_CURRENT_USER, sub, old);

    HKEY h;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, NULL, 0,
                             KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL);
    if (r != ERROR_SUCCESS) return false;
    r = RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static bool deleteDefaultIcon(const std::wstring& cls, const std::wstring* restore,
                              bool deleteValueOnly) {
    std::wstring sub = regSubForClass(cls);
    bool shell = isShellClass(cls);

    // 仅删除默认值, 保留键本身
    if (deleteValueOnly) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS)
            return false;
        LONG r = RegDeleteValueW(h, NULL);
        RegCloseKey(h);
        return r == ERROR_SUCCESS;
    }

    // 恢复原本的默认值
    if (restore) {
        HKEY h;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, NULL, 0,
                            KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL) == ERROR_SUCCESS) {
            LONG r = RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE*)restore->c_str(),
                                    (DWORD)((restore->size() + 1) * sizeof(wchar_t)));
            RegCloseKey(h);
            return r == ERROR_SUCCESS;
        }
        return false;
    }

    // 本次新建的键: 整个删掉
    if (RegDeleteKeyExW(HKEY_CURRENT_USER, sub.c_str(), KEY_WOW64_64KEY, 0) != ERROR_SUCCESS)
        return false;

    // 系统图标的上级是 Explorer\CLSID, 不能往回收
    if (shell) return true;

    std::wstring parent = std::wstring(HKCU_CLASSES) + L"\\" + cls;
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, parent.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        DWORD nSub = 0, nVal = 0;
        RegQueryInfoKeyW(h, NULL, NULL, NULL, &nSub, NULL, NULL, &nVal, NULL, NULL, NULL, NULL);
        std::wstring def;
        bool hasDef = regReadDefault(HKEY_CURRENT_USER, parent, def);
        RegCloseKey(h);
        if (nSub == 0 && nVal == 0 && !hasDef)
            RegDeleteKeyExW(HKEY_CURRENT_USER, parent.c_str(), KEY_WOW64_64KEY, 0);
    }
    return true;
}

static std::vector<std::wstring> enumExtensions() {
    std::vector<std::wstring> out;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, NULL, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD nsz = 512;
        FILETIME ft;
        LONG r = RegEnumKeyExW(root, i, name, &nsz, NULL, NULL, NULL, &ft);
        if (r != ERROR_SUCCESS) break;
        std::wstring n(name, nsz);
        if (n.size() > 1 && n[0] == L'.' && n.find(L'\\') == std::wstring::npos)
            out.push_back(n);
    }
    RegCloseKey(root);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static std::wstring resolveProgId(const std::wstring& ext) {
    std::wstring v;
    if (!regReadDefault(HKEY_CLASSES_ROOT, ext, v)) return L"";
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    if (v.empty() || v[0] == L'.') return L"";
    return v;
}

// ------------------------------------------------------------ 目标构建

struct Target {
    std::wstring cls;
    int kind;   // 0=ext 1=progid 2=generic
};

static std::vector<Target> buildTargets(bool includeFolders,
                                        const std::set<std::wstring>& exclude) {
    std::vector<Target> t;
    std::set<std::wstring> seen;
    std::vector<std::wstring> exts = enumExtensions();

    for (auto& e : exts) {
        if (exclude.count(lower(e))) continue;
        if (!seen.count(e)) { t.push_back({e, 0}); seen.insert(e); }
        std::wstring p = resolveProgId(e);
        if (!p.empty() && !seen.count(p)) { t.push_back({p, 1}); seen.insert(p); }
    }

    if (includeFolders) {
        const wchar_t* gs[] = {L"Folder", L"Directory", L"Drive", L"Unknown",
                               L"AllFileSystemObjects", L"*"};
        for (auto g : gs)
            if (!seen.count(g)) { t.push_back({g, 2}); seen.insert(g); }
    }

    // 桌面上的系统图标 (此电脑 / 回收站 / 用户文件夹 / 网络 / 控制面板 / 库)
    for (int i = 0; i < SHELL_ICON_COUNT; i++) {
        std::wstring c = std::wstring(SHELL_PREFIX) + SHELL_ICONS[i].guid;
        if (!seen.count(c)) { t.push_back({c, 3}); seen.insert(c); }
    }
    return t;
}

// ------------------------------------------------------------ 图标校验

static bool validateIco(const std::wstring& path, int& count) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char h[8] = {0};
    size_t n = fread(h, 1, 8, f);
    fclose(f);
    if (n < 6) return false;
    if (h[0] != 0 || h[1] != 0 || h[2] != 1 || h[3] != 0) return false;
    count = h[4] | (h[5] << 8);
    return count > 0;
}

static std::wstring iconValue(const std::wstring& path, int index) {
    std::wstring tail = L"," + W(index);
    if (path.find(L',') != std::wstring::npos)
        return L"\"" + path + L"\"" + tail;
    return path + tail;
}

// ------------------------------------------------------------ 状态读写

struct Entry {
    std::wstring cls;
    bool created;    // 本次新建
    bool hadOld;     // 原本已有默认值
    std::wstring oldValue;
};

static std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> r;
    size_t st = 0;
    for (;;) {
        size_t p = s.find(sep, st);
        if (p == std::wstring::npos) { r.push_back(s.substr(st)); break; }
        r.push_back(s.substr(st, p - st));
        st = p + 1;
    }
    return r;
}

static bool saveState(const std::wstring& icon, const std::vector<Entry>& es) {
    std::wstring c = L"GIR1\n" + icon + L"\n";
    for (auto& e : es)
        c += W(e.created ? 1 : 0) + L"\t" + W(e.hadOld ? 1 : 0) + L"\t"
           + e.cls + L"\t" + e.oldValue + L"\n";
    return writeTextUtf16(stateFile(), c);
}

static bool loadState(std::wstring& icon, std::vector<Entry>& es) {
    std::wstring c;
    if (!readTextUtf16(stateFile(), c)) return false;
    std::vector<std::wstring> lines = split(c, L'\n');
    size_t i = 0;
    while (i < lines.size() && lines[i].empty()) i++;
    if (i >= lines.size()) return false;
    std::wstring head = lines[i];
    while (!head.empty() && head.back() == L'\r') head.pop_back();
    if (head != L"GIR1") return false;
    i++;
    if (i >= lines.size()) return false;
    icon = lines[i];
    while (!icon.empty() && icon.back() == L'\r') icon.pop_back();
    i++;
    for (; i < lines.size(); ++i) {
        std::wstring ln = lines[i];
        while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (ln.empty()) continue;
        std::vector<std::wstring> f = split(ln, L'\t');
        if (f.size() < 4) continue;
        Entry e;
        e.created = (f[0] == L"1");
        e.hadOld  = (f[1] == L"1");
        e.cls     = f[2];
        e.oldValue = f[3];
        es.push_back(e);
    }
    return true;
}

// ------------------------------------------------------------ 还原记录备份
//
// 状态文件默认只躺在 %LOCALAPPDATA% 里, 一旦被清理工具删掉就再也还原不回去。
// 所以每次 apply 之后往程序目录留一份副本; restore 时若本机那份没了, 自动取回。

static std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4) return L"";
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? L"" : p.substr(0, s);
}

static std::wstring backupDir() {
    std::wstring d = exeDir();
    if (d.empty()) return L"";
    std::wstring b = d + L"\\_restore";
    CreateDirectoryW(b.c_str(), NULL);
    return b;
}

static const wchar_t* STATE_NAMES[] = {L"applied.txt", L"applied_lnk.txt",
                                       L"applied_thumb.txt", L"applied_sound.txt"};

static void backupState() {
    std::wstring b = backupDir();
    if (b.empty()) return;
    for (auto n : STATE_NAMES) {
        std::wstring src = appDir() + L"\\" + n;
        if (fileExists(src)) CopyFileW(src.c_str(), (b + L"\\" + n).c_str(), FALSE);
    }
}

// 本机记录丢了就从备份取回; 返回是否取回了任何一份
static bool restoreStateFromBackup() {
    std::wstring b = backupDir();
    if (b.empty()) return false;
    bool any = false;
    for (auto n : STATE_NAMES) {
        std::wstring src = b + L"\\" + n;
        std::wstring dst = appDir() + L"\\" + n;
        if (fileExists(src) && !fileExists(dst))
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) any = true;
    }
    return any;
}

// ------------------------------------------------------------ 快捷方式 (.lnk)
//
// .lnk 的图标不在注册表里, 而是写在每个快捷方式文件内部 (IShellLink 的图标位置),
// 所以注册表的 DefaultIcon 对它无效, 只能逐个改写文件本身。
// 改写前先读出原图标位置存进状态文件, restore 时原样写回。

struct LnkEntry {
    std::wstring path;
    std::wstring oldIcon;
    int oldIndex = 0;
};

static std::wstring lnkStateFile() { return appDir() + L"\\applied_lnk.txt"; }

static bool comReady() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

// 用字符串现取 GUID, 省掉链接 uuid 库的麻烦
//
// forWrite 为真时不回退到 STGM_READ: 以只读方式载入的 IPersistFile 调 Save 必然失败,
// 旧实现在这里静默吞掉错误, 于是"看起来改了、实际一个字都没落盘"。
static bool lnkOpen(const std::wstring& lnk, IShellLinkW*& sl, IPersistFile*& pf,
                    bool forWrite = false, DWORD* loadErr = nullptr) {
    CLSID clsid; IID iidSL, iidPF;
    if (FAILED(CLSIDFromString(L"{00021401-0000-0000-C000-000000000046}", &clsid))) return false;
    if (FAILED(IIDFromString(L"{000214F9-0000-0000-C000-000000000046}", &iidSL))) return false;
    if (FAILED(IIDFromString(L"{0000010B-0000-0000-C000-000000000046}", &iidPF))) return false;
    sl = NULL; pf = NULL;
    if (FAILED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, iidSL, (void**)&sl)) || !sl) return false;
    if (FAILED(sl->QueryInterface(iidPF, (void**)&pf)) || !pf) { sl->Release(); sl = NULL; return false; }

    HRESULT hr = pf->Load(lnk.c_str(), STGM_READWRITE);
    if (FAILED(hr) && !forWrite) hr = pf->Load(lnk.c_str(), STGM_READ);
    if (FAILED(hr)) {
        if (loadErr) *loadErr = (DWORD)hr;
        pf->Release(); sl->Release(); sl = NULL; pf = NULL; return false;
    }
    return true;
}

static bool lnkReadIcon(const std::wstring& lnk, std::wstring& icon, int& index) {
    IShellLinkW* sl; IPersistFile* pf;
    if (!lnkOpen(lnk, sl, pf)) return false;
    wchar_t buf[MAX_PATH * 4] = {0};
    int idx = 0;
    bool ok = SUCCEEDED(sl->GetIconLocation(buf, MAX_PATH * 4, &idx));
    pf->Release(); sl->Release();
    if (ok) { icon = buf; index = idx; }
    return ok;
}

// ------------------------------------------------------------ .lnk 尾部整理
//
// IShellLink::SetIconLocation 在保存时会把 "C:\Users\..." 反展开成
// "%SystemDrive%/Users/..." 塞进 IconEnvironmentDataBlock, 顺手把 LinkFlags 的
// HasExpIcon 位置起来。此后 shell 读图标改走环境变量那条路, 而该块的 ANSI 字段
// 只有 260 字节、且路径必须能被当前代码页表示 —— 一旦不满足, 展开失败, 图标就
// 静默退回 target 自己的图标, 表现正是"这个软件怎么都换不掉"。
//
// 这里把这层副作用抹掉: 清标志位 + 从文件里整块摘除 IconEnvironmentDataBlock。
// 删块只影响 ExtraData 段内后续块的偏移, 前面的 IDList/LinkInfo/StringData 偏移
// 保持不变, 所以是安全的。

// 按 MS-SHLLINK 顺序走一遍, 求出 ExtraData 段的起始下标; 解析不了返回 0
static size_t lnkExtraDataOffset(const std::vector<unsigned char>& d) {
    if (d.size() < 0x4C) return 0;
    DWORD flags = 0, hdr = 0;
    memcpy(&hdr, &d[0], 4);
    memcpy(&flags, &d[0x14], 4);
    if (hdr < 0x4C || hdr > d.size()) return 0;
    size_t p = hdr;

    if (flags & 0x01) {                       // HasLinkTargetIDList
        if (p + 2 > d.size()) return 0;
        WORD n = 0; memcpy(&n, &d[p], 2);
        p += 2 + (size_t)n;
    }
    if (flags & 0x02) {                       // HasLinkInfo
        if (p + 4 > d.size()) return 0;
        DWORD n = 0; memcpy(&n, &d[p], 4);
        if (n < 4) return 0;
        p += n;
    }
    bool uni = (flags & 0x80) != 0;
    auto skipStr = [&](size_t& q) -> bool {
        if (uni) {
            if (q + 2 > d.size()) return false;
            WORD n = 0; memcpy(&n, &d[q], 2);
            q += 2 + (size_t)n * 2;
        } else {
            while (q < d.size() && d[q]) q++;
            q += 1;
        }
        return q <= d.size();
    };
    if (flags & 0x04) if (!skipStr(p)) return 0;   // NAME
    if (flags & 0x08) if (!skipStr(p)) return 0;   // RELATIVE_PATH
    if (flags & 0x10) if (!skipStr(p)) return 0;   // WORKING_DIR
    if (flags & 0x20) if (!skipStr(p)) return 0;   // ARGUMENTS
    if (flags & 0x40) if (!skipStr(p)) return 0;   // ICON_LOCATION
    return (p <= d.size()) ? p : 0;
}

static bool fileReadAll(const std::wstring& path, std::vector<unsigned char>& d) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h); return false;
    }
    d.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, d.data(), (DWORD)d.size(), &got, NULL) && got == d.size();
    CloseHandle(h);
    if (!ok) d.clear();
    return ok;
}

static bool fileWriteAll(const std::wstring& path, const std::vector<unsigned char>& d) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &put, NULL) && put == d.size();
    CloseHandle(h);
    return ok;
}

// 在 ExtraData 段内找 IconEnvironmentDataBlock(0xA0000007), 命中则给出偏移与块长
static bool findExpIconBlock(const std::vector<unsigned char>& d, size_t& off, size_t& len) {
    off = 0; len = 0;
    size_t xoff = lnkExtraDataOffset(d);
    if (!xoff || xoff >= d.size()) return false;
    size_t p = xoff;
    while (p + 4 <= d.size()) {
        DWORD bsz = 0;
        memcpy(&bsz, &d[p], 4);
        if (bsz < 4 || p + bsz > d.size()) break;      // 末尾 4 字节 0 是正常终止
        if (bsz >= 8) {
            DWORD sig = 0;
            memcpy(&sig, &d[p + 4], 4);
            if (sig == 0xA0000007) { off = p; len = bsz; return true; }
        }
        p += bsz;
    }
    return false;
}

// 清掉 HasExpIcon 并摘除 IconEnvironmentDataBlock。返回是否动过文件。
static bool lnkStripExpIcon(const std::wstring& lnk) {
    std::vector<unsigned char> d;
    if (!fileReadAll(lnk, d)) return false;
    if (d.size() < 0x4C) return false;

    DWORD flags = 0;
    memcpy(&flags, &d[0x14], 4);
    bool hadFlag = (flags & 0x4000) != 0;

    size_t found = 0, foundLen = 0;
    bool hasBlock = findExpIconBlock(d, found, foundLen);

    if (!hadFlag && !hasBlock) return false;           // 本来干干净净

    std::vector<unsigned char> out;
    out.reserve(d.size());
    if (hasBlock) {
        out.insert(out.end(), d.begin(), d.begin() + (ptrdiff_t)found);
        out.insert(out.end(), d.begin() + (ptrdiff_t)(found + foundLen), d.end());
    } else {
        out = d;
    }

    DWORD nf = flags & ~0x4000u;
    memcpy(&out[0x14], &nf, 4);

    return fileWriteAll(lnk, out);
}

// 写入快捷方式图标。
//   · 先摘掉只读属性 (公共桌面的 lnk 常带只读, 直接写会失败)
//   · Save 传 NULL 而不是路径: 传路径会让 IPersistFile 重算相对路径,
//     实测把 "..\AppData\..." 算成 "..\Users\...\AppData\...", 平白改坏内容
//   · 写完回读校验, 不一致就当失败, 不再报"成功"
//   · stripExp 为真时抹掉 shell 顺手写进去的 IconEnvironmentDataBlock。
//     还原时传假: 那个块本来就是某些软件自己写的, 不该替它删掉。
static bool lnkWriteIcon(const std::wstring& lnk, const std::wstring& icon, int index,
                         std::wstring* why = nullptr, bool stripExp = true) {
    auto fail = [&](const wchar_t* m) {
        if (why) *why = m;
        return false;
    };

    DWORD attr = GetFileAttributesW(lnk.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return fail(L"文件不存在或无法访问");
    bool wasReadOnly = (attr & FILE_ATTRIBUTE_READONLY) != 0;
    if (wasReadOnly) {
        SetFileAttributesW(lnk.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        if (GetFileAttributesW(lnk.c_str()) & FILE_ATTRIBUTE_READONLY)
            return fail(L"只读属性无法清除");
    }

    bool ok = false;
    {
        IShellLinkW* sl = NULL; IPersistFile* pf = NULL;
        DWORD loadErr = 0;
        if (!lnkOpen(lnk, sl, pf, true, &loadErr)) {
            if (wasReadOnly) SetFileAttributesW(lnk.c_str(), attr);
            // 0x80070005 = E_ACCESSDENIED
            return fail(loadErr == 0x80070005 ? L"没有写入权限 (可能需要管理员)"
                                              : L"无法以可写方式打开");
        }
        ok = SUCCEEDED(sl->SetIconLocation(icon.c_str(), index));
        if (ok) ok = SUCCEEDED(pf->Save(NULL, TRUE));
        pf->Release(); sl->Release();
    }

    if (!ok) {
        if (wasReadOnly) SetFileAttributesW(lnk.c_str(), attr);
        return fail(L"写入被系统拒绝 (文件可能被占用或受保护)");
    }

    if (stripExp) lnkStripExpIcon(lnk);

    // 回读校验: 只认真正落到磁盘上的值
    std::wstring back; int backIdx = 0;
    if (!lnkReadIcon(lnk, back, backIdx) || lower(back) != lower(icon)) {
        if (wasReadOnly) SetFileAttributesW(lnk.c_str(), attr);
        return fail(L"写入后回读不一致");
    }

    if (wasReadOnly) SetFileAttributesW(lnk.c_str(), attr);
    return true;
}

static void collectLnk(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName); }
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring n(fd.cFileName);
                if (n == L"." || n == L"..") continue;
                collectLnk(dir + L"\\" + n, out, depth + 1);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static std::vector<std::wstring> defaultLnkDirs() {
    std::vector<std::wstring> v;
    wchar_t b[MAX_PATH];
    auto add = [&](const std::wstring& d) {
        if (!d.empty() && fileExists(d)) {
            bool dup = false;
            for (auto& x : v) if (lower(x) == lower(d)) dup = true;
            if (!dup) v.push_back(d);
        }
    };
    if (GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH)) {
        std::wstring u(b);
        add(u + L"\\Desktop");
        add(u + L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu");
        add(u + L"\\AppData\\Roaming\\Microsoft\\Internet Explorer\\Quick Launch");
        add(u + L"\\AppData\\Roaming\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar");
    }
    if (GetEnvironmentVariableW(L"PUBLIC", b, MAX_PATH)) add(std::wstring(b) + L"\\Desktop");
    else add(L"C:\\Users\\Public\\Desktop");
    if (GetEnvironmentVariableW(L"ProgramData", b, MAX_PATH)) add(std::wstring(b) + L"\\Microsoft\\Windows\\Start Menu");
    else add(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu");
    return v;
}

static std::vector<std::wstring> scanLnks(const std::vector<std::wstring>& extra) {
    std::vector<std::wstring> dirs = defaultLnkDirs();
    for (auto& d : extra) dirs.push_back(d);
    std::vector<std::wstring> lnks;
    for (auto& d : dirs) collectLnk(d, lnks);
    std::sort(lnks.begin(), lnks.end());
    lnks.erase(std::unique(lnks.begin(), lnks.end()), lnks.end());
    return lnks;
}

static bool saveLnkState(const std::vector<LnkEntry>& es) {
    std::wstring c = L"GIRL1\n";
    for (auto& e : es) c += e.path + L"\t" + e.oldIcon + L"\t" + W(e.oldIndex) + L"\n";
    return writeTextUtf16(lnkStateFile(), c);
}

static bool loadLnkState(std::vector<LnkEntry>& es) {
    std::wstring c;
    if (!readTextUtf16(lnkStateFile(), c)) return false;
    std::vector<std::wstring> lines = split(c, L'\n');
    size_t i = 0;
    while (i < lines.size() && lines[i].empty()) i++;
    if (i >= lines.size()) return false;
    std::wstring head = lines[i];
    while (!head.empty() && head.back() == L'\r') head.pop_back();
    if (head != L"GIRL1") return false;
    for (++i; i < lines.size(); ++i) {
        std::wstring ln = lines[i];
        while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (ln.empty()) continue;
        std::vector<std::wstring> f = split(ln, L'\t');
        if (f.size() < 3) continue;
        LnkEntry e;
        e.path = f[0]; e.oldIcon = f[1]; e.oldIndex = _wtoi(f[2].c_str());
        es.push_back(e);
    }
    return true;
}

// ------------------------------------------------------------ 缩略图开关
//
// .ico/.jpg/.png 这类图片类型, 系统会用缩略图处理器按图片自身内容显示,
// 把 DefaultIcon 整个盖掉, 想让它们也统一显示图标就必须关掉缩略图。
// 等价于在资源管理器里勾选"始终显示图标, 从不显示缩略图"。

struct ThumbEntry {
    std::wstring name;
    bool hadOld = false;
    DWORD oldValue = 0;
};

static const wchar_t* EXPLORER_ADV = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static std::wstring thumbStateFile() { return appDir() + L"\\applied_thumb.txt"; }

static bool regGetDword(const std::wstring& sub, const wchar_t* name, DWORD& out) {
    DWORD v = 0, sz = sizeof(v);
    LONG r = RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_DWORD, NULL, &v, &sz);
    if (r != ERROR_SUCCESS) return false;
    out = v;
    return true;
}

static void thumbApply() {
    const wchar_t* names[] = {L"IconsOnly", L"DisableThumbnails"};
    std::wstring c = L"GIRT1\n";
    for (auto n : names) {
        ThumbEntry e; e.name = n;
        e.hadOld = regGetDword(EXPLORER_ADV, n, e.oldValue);
        HKEY h;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, EXPLORER_ADV, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS) {
            DWORD one = 1;
            RegSetValueExW(h, n, 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            RegCloseKey(h);
        }
        c += e.name + L"\t" + W(e.hadOld ? 1 : 0) + L"\t" + W((int)e.oldValue) + L"\n";
    }
    writeTextUtf16(thumbStateFile(), c);
}

static void thumbRestore() {
    std::wstring c;
    if (!readTextUtf16(thumbStateFile(), c)) return;
    std::vector<std::wstring> lines = split(c, L'\n');
    size_t i = 0;
    while (i < lines.size() && lines[i].empty()) i++;
    if (i >= lines.size()) return;
    std::wstring head = lines[i];
    while (!head.empty() && head.back() == L'\r') head.pop_back();
    if (head != L"GIRT1") return;
    for (++i; i < lines.size(); ++i) {
        std::wstring ln = lines[i];
        while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (ln.empty()) continue;
        std::vector<std::wstring> f = split(ln, L'\t');
        if (f.size() < 3) continue;
        std::wstring name = f[0];
        bool hadOld = (f[1] == L"1");
        DWORD oldValue = (DWORD)_wtoi(f[2].c_str());
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, EXPLORER_ADV, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) continue;
        if (hadOld) RegSetValueExW(h, name.c_str(), 0, REG_DWORD, (const BYTE*)&oldValue, sizeof(oldValue));
        else RegDeleteValueW(h, name.c_str());
        RegCloseKey(h);
    }
    DeleteFileW(thumbStateFile().c_str());
}

// ------------------------------------------------------------ 刷新

// ------------------------------------------------------------ 系统音效
//
// 结构是三层子键, 不是"值名":
//   HKCU\AppEvents\Schemes\Apps\<程序>\<事件>\.Current\(默认) = WAV 完整路径
// 同级还有个 .Default 子键存该方案的出厂值。只改 .Current, 系统放音优先读它。
//
// 系统放音走 PlaySound, 不做解码, 所以必须是 WAV —— 换成 mp3 之类会直接静音。
// 这里就先拦住, 不让人改完才发现没声音。

struct SoundEntry {
    std::wstring app;
    std::wstring evt;
    bool hadKey = false;      // 原本是否存在 .Current 子键
    bool hadOld = false;      // 该子键下原本是否有值
    DWORD oldType = REG_SZ;
    std::wstring oldValue;
};

static const wchar_t* SOUND_APPS = L"AppEvents\\Schemes\\Apps";

static std::wstring soundStateFile() { return appDir() + L"\\applied_sound.txt"; }

static bool regReadStr(HKEY k, const wchar_t* name, std::wstring& out, DWORD* type = nullptr) {
    wchar_t buf[2048];
    DWORD sz = sizeof(buf), t = 0;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE*)buf, &sz) != ERROR_SUCCESS) return false;
    if (t != REG_SZ && t != REG_EXPAND_SZ) return false;
    out.assign(buf, sz / sizeof(wchar_t));
    while (!out.empty() && !out.back()) out.pop_back();
    if (type) *type = t;
    return true;
}

static std::wstring soundKeyPath(const SoundEntry& e) {
    return std::wstring(SOUND_APPS) + L"\\" + e.app + L"\\" + e.evt;
}

// 读 <事件>\.Current 的默认值。子键不存在, 或子键下没有值, 都算"没设过"
static bool readSoundCurrent(const std::wstring& evtKey, SoundEntry& e) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (evtKey + L"\\.Current").c_str(), 0,
                      KEY_READ, &h) != ERROR_SUCCESS) return false;
    e.hadKey = true;
    e.hadOld = regReadStr(h, NULL, e.oldValue, &e.oldType);
    RegCloseKey(h);
    return true;
}

static bool enumSoundEvents(std::vector<SoundEntry>& out) {
    HKEY hApps;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SOUND_APPS, 0, KEY_READ, &hApps) != ERROR_SUCCESS)
        return false;
    for (DWORD i = 0;; ++i) {
        wchar_t app[256];
        DWORD n = 256;
        if (RegEnumKeyExW(hApps, i, app, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hApp;
        if (RegOpenKeyExW(hApps, app, 0, KEY_READ, &hApp) != ERROR_SUCCESS) continue;
        for (DWORD j = 0;; ++j) {
            wchar_t evt[256];
            DWORD m = 256;
            if (RegEnumKeyExW(hApp, j, evt, &m, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            SoundEntry e;
            e.app = app;
            e.evt = evt;
            readSoundCurrent(soundKeyPath(e), e);
            out.push_back(e);
        }
        RegCloseKey(hApp);
    }
    RegCloseKey(hApps);
    return true;
}

static bool setSoundEvent(const SoundEntry& e, const std::wstring& value) {
    std::wstring sub = soundKeyPath(e) + L"\\.Current";
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, NULL, 0, KEY_WRITE,
                        NULL, &h, NULL) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

// 原本什么状态就还原成什么状态:
//   连 .Current 子键都没有 -> 整个删掉
//   有子键但没有值        -> 只删值, 子键留着
//   有值                   -> 按原类型写回
static bool restoreSoundEvent(const SoundEntry& e) {
    std::wstring sub = soundKeyPath(e) + L"\\.Current";
    if (!e.hadKey) {
        LONG r = RegDeleteKeyExW(HKEY_CURRENT_USER, sub.c_str(), KEY_WOW64_64KEY, 0);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (e.hadOld) {
        r = RegSetValueExW(h, NULL, 0, e.oldType, (const BYTE*)e.oldValue.c_str(),
                           (DWORD)((e.oldValue.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(h, NULL);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static bool validateWav(const std::wstring& p, std::wstring& why) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { why = L"打不开文件"; return false; }
    unsigned char hd[12] = {0};
    DWORD got = 0;
    bool ok = ReadFile(h, hd, 12, &got, NULL) && got == 12;
    CloseHandle(h);
    if (!ok) { why = L"文件太小, 不是有效的 WAV"; return false; }
    if (memcmp(hd, "RIFF", 4) != 0 || memcmp(hd + 8, "WAVE", 4) != 0) {
        why = L"不是 WAV 格式。系统放音只认 WAV, 换成 mp3/flac 会直接没声音";
        return false;
    }
    return true;
}

static bool saveSoundState(const std::wstring& wav, const std::vector<SoundEntry>& es) {
    std::wstring c = L"GIRS1\n" + wav + L"\n";
    for (auto& e : es)
        c += W(e.hadKey ? 1 : 0) + L"\t" + W(e.hadOld ? 1 : 0) + L"\t"
           + W((int)e.oldType) + L"\t" + e.app + L"\t" + e.evt + L"\t" + e.oldValue + L"\n";
    return writeTextUtf16(soundStateFile(), c);
}

static bool loadSoundState(std::wstring& wav, std::vector<SoundEntry>& es) {
    std::wstring c;
    if (!readTextUtf16(soundStateFile(), c)) return false;
    std::vector<std::wstring> lines = split(c, L'\n');
    size_t i = 0;
    while (i < lines.size() && lines[i].empty()) i++;
    if (i >= lines.size()) return false;
    std::wstring head = lines[i];
    while (!head.empty() && head.back() == L'\r') head.pop_back();
    if (head != L"GIRS1") return false;
    i++;
    if (i >= lines.size()) return false;
    wav = lines[i];
    while (!wav.empty() && wav.back() == L'\r') wav.pop_back();
    i++;
    for (; i < lines.size(); ++i) {
        std::wstring ln = lines[i];
        while (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (ln.empty()) continue;
        std::vector<std::wstring> f = split(ln, L'\t');
        if (f.size() < 6) continue;
        SoundEntry e;
        e.hadKey   = (f[0] == L"1");
        e.hadOld   = (f[1] == L"1");
        e.oldType  = (DWORD)_wtoi(f[2].c_str());
        e.app      = f[3];
        e.evt      = f[4];
        e.oldValue = f[5];
        es.push_back(e);
    }
    return !es.empty();
}

// 把记录里面的原值写回。返回还原成功的条数。
static int soundRestoreAll(const std::vector<SoundEntry>& es) {
    int n = 0;
    for (auto& e : es) if (restoreSoundEvent(e)) n++;
    return n;
}

static int cmdSound(const std::wstring& pathArg) {
    hr(L"系统音效");
    std::wstring wav = toAbs(pathArg);
    if (!fileExists(wav)) { P(L"[错误] 文件不存在: " + wav); return 2; }
    std::wstring why;
    if (!validateWav(wav, why)) { P(L"[错误] " + why); return 2; }
    P(L"[ok] 声音文件: " + wav);

    // 已有记录说明之前改过一次, 先把原值写回去, 否则会把"改后的声音"当成原值存下来
    std::wstring prevWav;
    std::vector<SoundEntry> prevEs;
    if (loadSoundState(prevWav, prevEs)) {
        soundRestoreAll(prevEs);
        DeleteFileW(soundStateFile().c_str());
        P(L"[info] 检测到上一次的音效记录, 已先还原为原始音效");
    }

    std::vector<SoundEntry> es;
    if (!enumSoundEvents(es) || es.empty()) {
        P(L"[错误] 读不到系统音效设置 (HKCU\\AppEvents\\Schemes\\Apps)");
        return 2;
    }

    std::vector<SoundEntry> saved;
    int done = 0, failed = 0;
    for (auto& e : es) {
        if (setSoundEvent(e, wav)) { saved.push_back(e); done++; }
        else failed++;
    }

    saveSoundState(wav, saved);
    P(L"\n[ok] 已替换 " + W(done) + L" 个音效事件" +
      (failed ? std::wstring(L", 失败 ") + W(failed) : std::wstring(L"")));
    P(L"[ok] 还原记录: " + soundStateFile());

    std::wstring bk = backupDir();
    if (!bk.empty()) { backupState(); P(L"[ok] 还原记录另存一份: " + bk); }

    // 试听一下, 顺便确认文件真的能播
    P(L"\n试听中 ...");
    PlaySoundW(wav.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    Sleep(1200);
    PlaySoundW(NULL, NULL, 0);
    P(L"系统放音走 PlaySound, 只吃 WAV。个别程序若缓存了旧声音, 重启该程序即可。");
    return 0;
}

static int cmdSoundOff() {
    hr(L"还原系统音效");
    std::wstring wav;
    std::vector<SoundEntry> es;
    if (!loadSoundState(wav, es)) {
        P(L"[info] 没有音效记录, 当前应该已是系统原始音效。");
        return 0;
    }
    P(L"原音效记录: " + wav);
    int n = soundRestoreAll(es);
    P(L"[ok] 已还原 " + W(n) + L"/" + W((int)es.size()) + L" 个音效事件");
    DeleteFileW(soundStateFile().c_str());
    return 0;
}

// ------------------------------------------------------------ 进程控制
// 按进程名结束进程并等它真正退出。旧实现用 system("taskkill"), 拿不到结果,
// 也等不到进程死透, 后面删图标缓存时句柄还占着, 于是"删了 0 个"却毫不知情。
static int killProcessByName(const wchar_t* name) {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) != 0) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!h) continue;
            if (TerminateProcess(h, 0)) {
                WaitForSingleObject(h, 5000);
                n++;
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

static bool startExplorer() {
    wchar_t sys[MAX_PATH] = {0};
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    std::wstring exe = std::wstring(sys) + L"\\explorer.exe";
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::wstring cmd = exe;
    BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    return ok != FALSE;
}

// ------------------------------------------------------------ 刷新

// lnks: 本轮实际改动过的快捷方式。逐个发 SHCNE_UPDATEITEM —— 这一步是必须的,
// SHCNE_ASSOCCHANGED 只让"文件类型关联"重新读, 对已经渲染过的 .lnk 图标不起作用,
// 那些一律沿用 iconcache 里的旧图, 这正是"文件夹和 txt 都变了、就钉钉不动"的成因。
static void refreshIcons(bool restartExplorer,
                         const std::vector<std::wstring>* lnks = nullptr) {
    hr(L"刷新图标");

    if (lnks && !lnks->empty()) {
        for (auto& f : *lnks)
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, f.c_str(), NULL);
        P(L"[ok] 已逐个通知 " + W((int)lnks->size()) + L" 个快捷方式重取图标");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    P(L"[ok] 已通知系统关联发生变化");

    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring local = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L"";

    if (restartExplorer) {
        P(L"正在重启资源管理器以重建图标缓存 ...");
        int killed = killProcessByName(L"explorer.exe");
        if (killed) P(L"[ok] 已结束 " + W(killed) + L" 个资源管理器进程");
        else P(L"[warn] 未能结束资源管理器 (可能被安全软件保护), 改为仅通知刷新");
        Sleep(300);
    }

    int removed = 0, tried = 0;
    std::wstring main = local + L"\\IconCache.db";
    if (fileExists(main)) { tried++; if (DeleteFileW(main.c_str())) removed++; }

    std::wstring dir = local + L"\\Microsoft\\Windows\\Explorer";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\iconcache_*.db").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            tried++;
            std::wstring f = dir + L"\\" + fd.cFileName;
            // 被占用时先挪走再删, 挪不动才算失败
            if (DeleteFileW(f.c_str()) || MoveFileExW(f.c_str(), (f + L".old").c_str(),
                                                      MOVEFILE_REPLACE_EXISTING))
                removed++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (removed) P(L"[ok] 已清除 " + W(removed) + L" 个图标缓存文件");
    else if (tried) P(L"[warn] " + W(tried) + L" 个图标缓存文件都被占用, 未能清除 (下次登录自动重建)");
    else P(L"[info] 未找到图标缓存文件 (系统可能尚未生成)");

    if (restartExplorer) {
        if (startExplorer()) {
            Sleep(800);
            P(L"[ok] 资源管理器已重启");
        } else {
            P(L"[warn] 资源管理器启动失败, 请手动按 Win+E 或重新登录");
        }
    } else {
        P(L"[info] 未重启资源管理器, 手动重启或注销后生效");
    }
}

// ------------------------------------------------------------ 命令

struct Opts {
    bool dryRun = false;
    bool noFolders = false;
    bool noRefresh = false;
    bool noRestart = false;
    bool install = false;
    bool yes = false;
    bool noLnk = false;          // 不改写快捷方式文件
    bool keepThumbs = false;     // 保留缩略图
    int index = 0;
    std::set<std::wstring> exclude;
    std::vector<std::wstring> lnkDirs;   // 额外扫描的快捷方式目录
};

static int cmdRestore(bool doRefresh, bool restart);

static int cmdApply(const std::wstring& srcArg, const Opts& o) {
    std::wstring src = toAbs(srcArg);   // 注册表里必须是绝对路径
    if (!fileExists(src)) {
        P(L"[错误] 图标文件不存在: " + src);
        return 2;
    }
    int cnt = 0;
    if (!validateIco(src, cnt)) {
        P(L"[错误] 不是有效的 .ico 文件: " + src);
        return 2;
    }
    P(L"[ok] 源图标: " + src + L" (内含 " + W(cnt) + L" 个尺寸)");

    std::wstring icon = src;
    if (o.install) {
        std::wstring dest = installedIcon();
        if (CopyFileW(src.c_str(), dest.c_str(), FALSE)) {
            icon = dest;
            P(L"[ok] 图标已安装到本机固定位置: " + dest);
        } else {
            P(L"[warn] 复制失败, 改用原路径");
        }
    }

    std::vector<Target> targets = buildTargets(!o.noFolders, o.exclude);
    P(L"\n扫描到 " + W((int)targets.size()) + L" 个待修改的注册表类型");

    if (o.dryRun) {
        hr(L"预览 (未写入)");
        int ne = 0, no = 0;
        for (auto& t : targets) (t.kind == 0 ? ne : no)++;
        P(L"扩展名 " + W(ne) + L" 个, 关联类型 " + W(no) + L" 个");
        if (!o.noLnk) {
            std::vector<std::wstring> lnks = scanLnks(o.lnkDirs);
            P(L"快捷方式 " + W((int)lnks.size()) + L" 个 (将逐个改写文件内的图标位置)");
        }
        P(L"\n图标将引用: " + icon);
        if (!o.keepThumbs) P(L"[计划] 同时关闭资源管理器缩略图 (否则图片按自身内容显示)");
        P(L"\n这是预览模式, 没有做任何修改。去掉 --dry-run 即可实际应用。");
        return 0;
    }

    // 真正要写之前, 才清理上一轮的记录 (预览模式绝不能动系统)
    std::wstring prevIcon;
    std::vector<Entry> prev;
    if (loadState(prevIcon, prev)) {
        P(L"\n[提示] 本机已有应用记录, 将先自动还原再应用新图标。");
        cmdRestore(false, false);
    }

    if (!o.yes) {
        P(L"\n即将把上述所有类型的图标统一替换为:");
        P(L"    " + icon);
        P(L"\n说明: 只写当前用户的注册表, 不改动文件内容;");
        if (!o.noLnk) P(L"      例外: 快捷方式(.lnk)的图标存在文件内部, 会逐个改写, 原值已留档;");
        if (!o.keepThumbs) P(L"      例外: 会关闭资源管理器缩略图, 否则图片类型按自身内容显示;");
        P(L"      随时可用 restore 完整还原。");
        std::wstring ans = readLine(L"\n确认应用? [y/N] ");
        std::wstring a = lower(ans);
        if (!(a == L"y" || a == L"yes")) { P(L"已取消。"); return 1; }
    }

    hr(L"写入注册表");
    std::wstring value = iconValue(icon, o.index);
    std::vector<Entry> entries;
    int done = 0, failed = 0;
    for (auto& t : targets) {
        Entry e;
        e.cls = t.cls;
        if (writeDefaultIcon(t.cls, value, e.created, e.hadOld, e.oldValue)) {
            entries.push_back(e);
            done++;
        } else {
            failed++;
            if (failed <= 5) P(L"[warn] 跳过: " + t.cls);
        }
        if (done && done % 400 == 0) P(L"  已写入 " + W(done) + L" ...");
    }

    saveState(icon, entries);
    P(L"\n[ok] 写入完成: 成功 " + W(done) + L", 失败 " + W(failed));
    P(L"[ok] 状态记录(用于还原): " + stateFile());

    // ---- .lnk: 图标写在文件内部, 注册表管不到, 只能逐个改写
    std::vector<std::wstring> changedLnks;
    if (!o.noLnk) {
        hr(L"快捷方式 (.lnk)");
        std::vector<std::wstring> lnks = scanLnks(o.lnkDirs);
        P(L"扫描到 " + W((int)lnks.size()) + L" 个快捷方式");
        if (!lnks.empty()) {
            if (comReady()) {
                std::vector<LnkEntry> les;
                std::vector<std::wstring> reasons;
                int ld = 0, lf = 0;
                for (auto& f : lnks) {
                    std::wstring oldIcon; int oldIdx = 0;
                    if (!lnkReadIcon(f, oldIcon, oldIdx)) {
                        lf++;
                        reasons.push_back(f + L"  ->  读不出原图标");
                        continue;
                    }
                    std::wstring why;
                    if (lnkWriteIcon(f, icon, o.index, &why)) {
                        LnkEntry e; e.path = f; e.oldIcon = oldIcon; e.oldIndex = oldIdx;
                        les.push_back(e); ld++;
                        changedLnks.push_back(f);
                    } else {
                        lf++;
                        reasons.push_back(f + L"  ->  " + why);
                    }
                }
                if (!les.empty()) saveLnkState(les);
                P(L"[ok] 已改写 " + W(ld) + L" 个快捷方式图标" +
                  (lf ? std::wstring(L", 失败 ") + W(lf) : std::wstring(L"")));
                if (!reasons.empty()) {
                    P(L"\n[!] 下列快捷方式没能改写 (图标会保持原样):");
                    size_t show = reasons.size() < 12 ? reasons.size() : 12;
                    for (size_t i = 0; i < show; i++) P(L"    " + reasons[i]);
                    if (reasons.size() > show)
                        P(L"    ... 另有 " + W((int)(reasons.size() - show)) + L" 个");
                }
                P(L"[ok] 快捷方式还原记录: " + lnkStateFile());
            } else {
                P(L"[warn] COM 初始化失败, 跳过快捷方式");
            }
        }
    }

    // ---- 缩略图: 不关掉的话 .ico/.jpg/.png 会按图片自身内容显示
    if (!o.keepThumbs) {
        thumbApply();
        P(L"[ok] 已关闭资源管理器缩略图 (图片/视频不再预览, 改用统一图标)");
    }

    // ---- 还原记录在程序目录再留一份, 免得 %LOCALAPPDATA% 那份被清理掉
    std::wstring bk = backupDir();
    if (!bk.empty()) {
        backupState();
        P(L"[ok] 还原记录另存一份: " + bk);
    }

    P(L"\n三类硬骨头的处理:");
    P(L"  .lnk    已逐个改写快捷方式文件内部的图标位置 (--no-lnk 可跳过)");
    P(L"  .ico    已写入 DefaultIcon 并关闭缩略图 (--keep-thumbnails 可保留预览)");
    P(L"  .exe    已写入 exefile 的 DefaultIcon; 个别视图下 shell 仍直接提取原图标");

    if (!o.noRefresh) refreshIcons(!o.noRestart, &changedLnks);
    else P(L"\n[info] 已跳过刷新, 手动重启资源管理器后生效。");

    hr(L"完成");
    P(L"现在所有文件类型的图标都应已统一。");
    return 0;
}

static int cmdRestore(bool doRefresh, bool restart) {
    // 本机那份记录没了就从程序目录的备份里取回
    if (restoreStateFromBackup())
        P(L"[info] 本机还原记录缺失, 已从程序目录的备份取回");

    std::wstring icon;
    std::vector<Entry> es;
    bool hasIcon = loadState(icon, es);

    // 音效可以单独换过, 那时没有图标记录; 不能因为图标记录为空就把整件事跳过
    if (!hasIcon && !fileExists(lnkStateFile()) &&
        !fileExists(thumbStateFile()) && !fileExists(soundStateFile())) {
        P(L"[info] 本机没有应用记录, 当前应该已是系统原始图标。");
        return 0;
    }

    if (hasIcon) {
        P(L"原图标:   " + icon);
        P(L"待还原:   " + W((int)es.size()) + L" 项");

        int ok = 0;
        for (auto& e : es) {
            bool r;
            if (e.created)      r = deleteDefaultIcon(e.cls, NULL, false);        // 新建的, 删掉
            else if (e.hadOld)  r = deleteDefaultIcon(e.cls, &e.oldValue, false); // 恢复原值
            else                r = deleteDefaultIcon(e.cls, NULL, true);         // 只清默认值
            if (r) ok++;
        }

        DeleteFileW(stateFile().c_str());
        DeleteFileW(installedIcon().c_str());
        P(L"\n[ok] 已还原 " + W(ok) + L" 项, 状态记录已清除");
    }

    // 快捷方式: 把改写过的图标位置写回去
    std::vector<std::wstring> changedLnks;
    std::vector<LnkEntry> les;
    if (loadLnkState(les)) {
        int lk = 0;
        if (comReady()) {
            for (auto& e : les) {
                if (lnkWriteIcon(e.path, e.oldIcon, e.oldIndex, nullptr, false)) {
                    lk++;
                    changedLnks.push_back(e.path);
                }
            }
            P(L"[ok] 已还原 " + W(lk) + L"/" + W((int)les.size()) + L" 个快捷方式图标");
        } else {
            P(L"[warn] COM 初始化失败, 快捷方式未能还原");
        }
        DeleteFileW(lnkStateFile().c_str());
    }

    // 缩略图设置
    if (fileExists(thumbStateFile())) {
        thumbRestore();
        P(L"[ok] 缩略图设置已恢复");
    }

    // 系统音效
    std::wstring swav;
    std::vector<SoundEntry> ses;
    if (loadSoundState(swav, ses)) {
        int sn = soundRestoreAll(ses);
        P(L"[ok] 已还原 " + W(sn) + L"/" + W((int)ses.size()) + L" 个系统音效");
        DeleteFileW(soundStateFile().c_str());
    }

    if (doRefresh) refreshIcons(restart, &changedLnks);
    hr(L"完成");
    P(L"系统图标已恢复原状。");
    return 0;
}

static int cmdStatus() {
    hr(L"当前状态");
    std::wstring icon;
    std::vector<Entry> es;
    if (loadState(icon, es)) {
        P(L"状态:     已应用");
        P(L"图标文件: " + icon);
        P(L"条目数:   " + W((int)es.size()));
        P(std::wstring(L"图标存在: ") + (fileExists(icon) ? L"是" : L"否 (可能需要重新应用)"));
    } else {
        P(L"状态:     未应用 (系统原始图标)");
    }

    std::wstring swav;
    std::vector<SoundEntry> ses;
    if (loadSoundState(swav, ses))
        P(L"音效:     已替换为 " + swav + L" (" + W((int)ses.size()) + L" 个事件)");
    else
        P(L"音效:     未改动");

    P(L"状态目录: " + appDir());
    std::wstring bk = backupDir();
    if (!bk.empty()) P(L"备份目录: " + bk);
    return 0;
}

// 自检: 把某个 .lnk 里 shell 顺手塞进去的环境变量图标块清掉, 不碰注册表。
// 用来排查"明明写了图标位置, 却还显示原图标"。
static int cmdLnkStrip(const std::wstring& f) {
    hr(L"快捷方式尾部整理自检");
    std::vector<unsigned char> d;
    if (!fileReadAll(f, d)) { P(L"[错误] 读不出文件: " + f); return 2; }
    if (d.size() < 0x4C) { P(L"[错误] 这不是有效的 .lnk 文件"); return 2; }

    DWORD flags = 0;
    memcpy(&flags, &d[0x14], 4);
    size_t xoff = lnkExtraDataOffset(d);
    size_t boff = 0, blen = 0;
    bool hasBlock = findExpIconBlock(d, boff, blen);

    P(L"文件:       " + f);
    P(L"大小:       " + W((int)d.size()) + L" 字节");
    P(L"HasExpIcon: " + std::wstring((flags & 0x4000) ? L"是 (图标改走环境变量路径)" : L"否"));
    P(L"ExtraData:  " + (xoff ? W((int)xoff) + L" 字节处开始"
                             : std::wstring(L"解析失败, 不做修改")));
    P(L"环境变量图标块: " + (hasBlock ? (L"有, " + W((int)blen) + L" 字节 @ " + W((int)boff))
                                     : std::wstring(L"无")));

    if (!hasBlock && !(flags & 0x4000)) {
        P(L"\n[info] 这个文件本来就没有该问题。");
        return 0;
    }
    if (!xoff) { P(L"\n[错误] ExtraData 结构解析失败, 为安全起见不做修改。"); return 1; }
    if (!lnkStripExpIcon(f)) { P(L"\n[错误] 清理失败 (文件可能只读、被占用或没有写权限)"); return 1; }

    std::vector<unsigned char> n;
    if (!fileReadAll(f, n) || n.size() < 0x4C) { P(L"\n[错误] 清理后回读失败"); return 1; }
    DWORD nf = 0;
    memcpy(&nf, &n[0x14], 4);
    size_t noff = lnkExtraDataOffset(n);

    P(L"\n[ok] 已清理: " + W((int)d.size()) + L" -> " + W((int)n.size()) + L" 字节");
    P(L"[ok] HasExpIcon: " + std::wstring((nf & 0x4000) ? L"仍在" : L"已清除"));
    P(L"[ok] ExtraData 重解析: " + (noff ? W((int)noff) + L" 字节处, 结构完整"
                                        : std::wstring(L"失败")));

    if (comReady()) {
        std::wstring ic; int ix = 0;
        if (lnkReadIcon(f, ic, ix)) P(L"[ok] 图标位置仍可读: " + ic + L" , 索引 " + W(ix));
        else P(L"[warn] 清理后读不出图标位置");
    }
    return 0;
}

// 自检: 让 .lnk 改写能力可被验证。
// 对每个快捷方式做一遍 "读原值 -> 写入 -> 读回 -> 还原 -> 复验",
// 全程不碰注册表, 结束时快捷方式回到原样。
static int cmdLnkTest(const std::wstring& icon, const std::vector<std::wstring>& dirs) {
    hr(L"快捷方式 (.lnk) 改写自检");
    std::vector<std::wstring> lnks;
    for (auto& d : dirs) collectLnk(d, lnks);
    std::sort(lnks.begin(), lnks.end());
    lnks.erase(std::unique(lnks.begin(), lnks.end()), lnks.end());
    if (lnks.empty()) { P(L"[info] 指定目录里没有 .lnk 文件"); return 1; }
    if (!comReady()) { P(L"[错误] COM 初始化失败"); return 2; }

    P(L"图标:   " + icon);
    P(L"样本:   " + W((int)lnks.size()) + L" 个快捷方式");
    P(L"过程:   读原值 -> 写入 -> 读回 -> 还原 -> 复验");
    P(L"");

    int pass = 0, fail = 0;
    for (auto& f : lnks) {
        std::wstring oldIcon; int oldIdx = 0;
        if (!lnkReadIcon(f, oldIcon, oldIdx)) { fail++; if (fail <= 5) P(L"[warn] 读不出原图标: " + f); continue; }

        bool ok = lnkWriteIcon(f, icon, 0);
        std::wstring nowIcon; int nowIdx = 0;
        if (ok) {
            lnkReadIcon(f, nowIcon, nowIdx);
            if (lower(nowIcon) != lower(icon)) ok = false;
        }

        // 无论如何都尝试还原 (还原不抹 exp 块, 保持文件原状)
        lnkWriteIcon(f, oldIcon, oldIdx, nullptr, false);
        std::wstring backIcon; int backIdx = 0;
        lnkReadIcon(f, backIcon, backIdx);
        if (lower(backIcon) != lower(oldIcon) || backIdx != oldIdx) ok = false;

        if (ok) pass++;
        else { fail++; if (fail <= 5) P(L"[warn] 未通过: " + f); }
    }

    hr();
    P(L"通过 " + W(pass) + L" 个, 未通过 " + W(fail) + L" 个 (共 " + W((int)lnks.size()) + L" 个)");
    P(L"所有快捷方式已还原为原图标, 注册表未做任何改动。");
    if (fail == 0) P(L"\n[ok] .lnk 改写能力正常。");
    else P(L"\n[warn] 有快捷方式未能改写, 通常是文件只读或正在被占用。");
    return fail == 0 ? 0 : 1;
}

static int cmdList(const Opts& o) {
    std::vector<Target> t = buildTargets(!o.noFolders, o.exclude);
    hr();
    P(L"扫描结果: " + W((int)t.size()) + L" 项");
    const wchar_t* ks[] = {L"ext    ", L"progid ", L"generic"};
    for (auto& x : t) {
        std::wstring cur;
        std::wstring line = L"  [" + std::wstring(ks[x.kind]) + L"] " + x.cls;
        if (regReadDefault(HKEY_CLASSES_ROOT, x.cls + L"\\DefaultIcon", cur)) {
            if (lower(cur).find(L".ico") != std::wstring::npos) line += L"   <- 当前已是自定义图标";
        }
        P(line);
    }
    return 0;
}

static void usage() {
    hr(L"全局图标替换工具");
    P(L"用法:");
    P(L"  IconReplacer.exe                 打开交互菜单");
    P(L"  IconReplacer.exe <icon.ico>      把 .ico 拖到 exe 上, 直接应用");
    P(L"  IconReplacer.exe apply <ico>     应用图标");
    P(L"  IconReplacer.exe restore         还原为系统原始图标");
    P(L"  IconReplacer.exe status          查看当前状态");
    P(L"  IconReplacer.exe list            列出将被修改的类型");
    P(L"  IconReplacer.exe lnktest <ico>   自检快捷方式能否改写 (改完立即还原)");
    P(L"  IconReplacer.exe lnkstrip <lnk>  自检单个快捷方式的图标位置能否被正常解析");
    P(L"  IconReplacer.exe sound <wav>     把所有系统音效换成这个 WAV");
    P(L"  IconReplacer.exe soundoff        只还原系统音效");
    P(L"");
    P(L"选项:");
    P(L"  --dry-run           只统计不写入");
    P(L"  --no-folders        不改文件夹/驱动器");
    P(L"  --no-refresh        不刷新图标缓存");
    P(L"  --no-restart        刷新但不重启资源管理器");
    P(L"  --install           把图标复制到本机固定目录后再引用");
    P(L"  --index N           图标索引, 默认 0");
    P(L"  --exclude .a .b     排除指定扩展名");
    P(L"  --yes               跳过确认");
    P(L"");
    P(L"快捷方式与缩略图:");
    P(L"  --no-lnk            不改写快捷方式 (.lnk) 文件");
    P(L"  --lnk-dir <目录>    额外扫描该目录下的快捷方式, 可重复写");
    P(L"  --keep-thumbnails   不关闭缩略图 (图片仍按自身内容显示)");
    P(L"");
    P(L"说明: 只写当前用户的注册表 (HKCU), 不需要管理员权限。");
    P(L"      默认会改写快捷方式图标、关闭缩略图, 这样 .lnk/.ico 也能统一;");
    P(L"      两者的原值都会留档, restore 时一并还原。");
}

static int menu() {
    for (;;) {
        hr(L"全局图标替换工具");
        P(L"  1  应用图标 (把电脑上所有文件类型换成同一个 .ico)");
        P(L"  2  还原为系统原始图标");
        P(L"  3  查看当前状态");
        P(L"  4  列出将被修改的类型");
        P(L"  5  替换系统音效 (把所有提示音换成同一个 WAV)");
        P(L"  0  退出");
        P(L"");
        std::wstring c = readLine(L"请选择 [0-5]: ");
        if (c == L"1") {
            std::wstring p = readLine(L"请输入 .ico 路径 (也可把文件拖进窗口): ");
            if (p.empty()) { P(L"路径为空。"); continue; }
            Opts o;
            std::wstring y = readLine(L"同时替换文件夹和驱动器图标? [Y/n] ");
            if (!y.empty() && (y[0] == L'n' || y[0] == L'N')) o.noFolders = true;
            std::wstring l = readLine(L"同时改写桌面/开始菜单的快捷方式图标? [Y/n] ");
            if (!l.empty() && (l[0] == L'n' || l[0] == L'N')) o.noLnk = true;
            cmdApply(p, o);
        } else if (c == L"2") {
            cmdRestore(true, true);
        } else if (c == L"3") {
            cmdStatus();
        } else if (c == L"4") {
            Opts o; cmdList(o);
        } else if (c == L"5") {
            std::wstring p = readLine(L"请输入 WAV 路径 (也可把文件拖进窗口): ");
            if (p.empty()) { P(L"路径为空。"); continue; }
            cmdSound(p);
        } else if (c == L"0" || c.empty()) {
            return 0;
        } else {
            P(L"无效选择。");
        }
        P(L"");
        readLine(L"按回车继续 ...");
    }
}

// ------------------------------------------------------------ 入口

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> av;
    if (!wargv) return 1;
    for (int i = 0; i < argc; ++i) av.push_back(wargv[i]);
    LocalFree(wargv);

    if (argc <= 1) return menu();

    std::wstring cmd = av[1];

    // 拖放 .ico 到 exe 上
    if (cmd.size() > 4 && lower(cmd.substr(cmd.size() - 4)) == L".ico") {
        Opts o;
        return cmdApply(cmd, o);
    }

    // 拖放 .wav 到 exe 上 -> 直接替换系统音效
    if (cmd.size() > 4 && lower(cmd.substr(cmd.size() - 4)) == L".wav") {
        return cmdSound(cmd);
    }

    if (cmd == L"status") return cmdStatus();
    if (cmd == L"restore" || cmd == L"还原") {
        bool nr = false, nrs = false;
        for (int i = 2; i < argc; ++i) {
            if (av[i] == L"--no-refresh") nr = true;
            if (av[i] == L"--no-restart") nrs = true;
        }
        return cmdRestore(!nr, !nrs);
    }
    if (cmd == L"sound") {
        if (argc < 3) { P(L"[错误] 用法: IconReplacer.exe sound <声音.wav>"); return 2; }
        return cmdSound(av[2]);
    }
    if (cmd == L"soundoff") {
        return cmdSoundOff();
    }
    if (cmd == L"lnkstrip") {
        if (argc < 3) { P(L"[错误] 用法: IconReplacer.exe lnkstrip <某个.lnk>"); return 2; }
        return cmdLnkStrip(toAbs(av[2]));
    }
    if (cmd == L"lnktest") {
        std::wstring icon;
        std::vector<std::wstring> dirs;
        for (int i = 2; i < argc; ++i) {
            if (av[i] == L"--lnk-dir" && i + 1 < argc) dirs.push_back(toAbs(av[++i]));
            else if (icon.empty()) icon = toAbs(av[i]);
        }
        if (icon.empty()) { P(L"[错误] 用法: IconReplacer.exe lnktest <icon.ico> [--lnk-dir <目录>]"); return 2; }
        if (dirs.empty()) dirs = defaultLnkDirs();
        return cmdLnkTest(icon, dirs);
    }
    if (cmd == L"list" || cmd == L"list-types") {
        Opts o;
        for (int i = 2; i < argc; ++i) if (av[i] == L"--no-folders") o.noFolders = true;
        return cmdList(o);
    }
    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
        usage(); return 0;
    }
    if (cmd == L"apply") {
        if (argc < 3) { P(L"[错误] 缺少图标路径。示例: IconReplacer.exe apply D:\\a.ico"); return 2; }
        Opts o;
        std::wstring icon;
        for (int i = 2; i < argc; ++i) {
            const std::wstring& a = av[i];
            if (a == L"--dry-run") o.dryRun = true;
            else if (a == L"--no-folders") o.noFolders = true;
            else if (a == L"--no-refresh") o.noRefresh = true;
            else if (a == L"--no-restart") o.noRestart = true;
            else if (a == L"--install") o.install = true;
            else if (a == L"--yes" || a == L"-y") o.yes = true;
            else if (a == L"--index" && i + 1 < argc) o.index = _wtoi(av[++i].c_str());
            else if (a == L"--no-lnk") o.noLnk = true;
            else if (a == L"--keep-thumbnails") o.keepThumbs = true;
            else if (a == L"--lnk-dir" && i + 1 < argc) o.lnkDirs.push_back(toAbs(av[++i]));
            else if (a == L"--exclude") {
                while (i + 1 < argc && av[i + 1][0] != L'-') o.exclude.insert(lower(av[++i]));
            } else if (icon.empty()) icon = a;
        }
        if (icon.empty()) { P(L"[错误] 缺少图标路径。"); return 2; }
        return cmdApply(icon, o);
    }

    // 未知命令: 若看起来像路径则当图标处理
    if (fileExists(cmd)) { Opts o; return cmdApply(cmd, o); }

    usage();
    return 1;
}
