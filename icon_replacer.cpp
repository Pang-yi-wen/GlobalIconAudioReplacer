// 全局图标替换工具 (Global Icon Replacer) — C++ 单文件版
//
// 自带的图标示例为蒋结实（即仓库内附带的 OIP-C_256x256.ico，可直接拿来试用）。
//
// 编译: g++ -O2 -std=c++17 -s -static -static-libgcc -static-libstdc++ ^
//         -o IconReplacer.exe icon_replacer.cpp -ladvapi32 -lshell32 -lole32
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
#include <stdio.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

static const wchar_t* HKCU_CLASSES = L"Software\\Classes";
static const wchar_t* APP_NAME     = L"GlobalIconReplacer";

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
    std::wstring sub = std::wstring(HKCU_CLASSES) + L"\\" + cls + L"\\DefaultIcon";
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
    std::wstring sub = std::wstring(HKCU_CLASSES) + L"\\" + cls + L"\\DefaultIcon";

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

static const wchar_t* STATE_NAMES[] = {L"applied.txt", L"applied_lnk.txt", L"applied_thumb.txt"};

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
static bool lnkOpen(const std::wstring& lnk, IShellLinkW*& sl, IPersistFile*& pf) {
    CLSID clsid; IID iidSL, iidPF;
    if (FAILED(CLSIDFromString(L"{00021401-0000-0000-C000-000000000046}", &clsid))) return false;
    if (FAILED(IIDFromString(L"{000214F9-0000-0000-C000-000000000046}", &iidSL))) return false;
    if (FAILED(IIDFromString(L"{0000010B-0000-0000-C000-000000000046}", &iidPF))) return false;
    sl = NULL; pf = NULL;
    if (FAILED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, iidSL, (void**)&sl)) || !sl) return false;
    if (FAILED(sl->QueryInterface(iidPF, (void**)&pf)) || !pf) { sl->Release(); sl = NULL; return false; }
    if (FAILED(pf->Load(lnk.c_str(), STGM_READWRITE)) && FAILED(pf->Load(lnk.c_str(), STGM_READ))) {
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

static bool lnkWriteIcon(const std::wstring& lnk, const std::wstring& icon, int index) {
    IShellLinkW* sl; IPersistFile* pf;
    if (!lnkOpen(lnk, sl, pf)) return false;
    bool ok = SUCCEEDED(sl->SetIconLocation(icon.c_str(), index));
    if (ok) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    pf->Release(); sl->Release();
    return ok;
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

static void refreshIcons(bool restartExplorer) {
    hr(L"刷新图标");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    P(L"[ok] 已通知系统关联发生变化");

    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring local = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L"";

    if (restartExplorer) {
        P(L"正在重启资源管理器以重建图标缓存 ...");
        system("taskkill /F /IM explorer.exe >nul 2>&1");
        Sleep(1200);
    }

    int removed = 0;
    std::wstring main = local + L"\\IconCache.db";
    if (fileExists(main)) { DeleteFileW(main.c_str()); removed++; }

    std::wstring dir = local + L"\\Microsoft\\Windows\\Explorer";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\iconcache_*.db").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) removed++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (removed) P(L"[ok] 已清除 " + W(removed) + L" 个图标缓存文件");
    else P(L"[info] 图标缓存文件被占用或未找到 (下次登录时会自动重建)");

    if (restartExplorer) {
        system("start \"\" explorer.exe >nul 2>&1");
        P(L"[ok] 资源管理器已重启");
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
    if (!o.noLnk) {
        hr(L"快捷方式 (.lnk)");
        std::vector<std::wstring> lnks = scanLnks(o.lnkDirs);
        P(L"扫描到 " + W((int)lnks.size()) + L" 个快捷方式");
        if (!lnks.empty()) {
            if (comReady()) {
                std::vector<LnkEntry> les;
                int ld = 0, lf = 0;
                for (auto& f : lnks) {
                    std::wstring oldIcon; int oldIdx = 0;
                    if (!lnkReadIcon(f, oldIcon, oldIdx)) { lf++; continue; }
                    if (lnkWriteIcon(f, icon, o.index)) {
                        LnkEntry e; e.path = f; e.oldIcon = oldIcon; e.oldIndex = oldIdx;
                        les.push_back(e); ld++;
                    } else lf++;
                }
                if (!les.empty()) saveLnkState(les);
                P(L"[ok] 已改写 " + W(ld) + L" 个快捷方式图标" +
                  (lf ? std::wstring(L", 失败 ") + W(lf) : std::wstring(L"")));
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

    if (!o.noRefresh) refreshIcons(!o.noRestart);
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
    if (!loadState(icon, es)) {
        P(L"[info] 本机没有应用记录, 当前应该已是系统原始图标。");
        return 0;
    }
    P(L"原图标:   " + icon);
    P(L"待还原:   " + W((int)es.size()) + L" 项");

    int ok = 0;
    for (auto& e : es) {
        bool r;
        if (e.created)      r = deleteDefaultIcon(e.cls, NULL, false);            // 新建的, 删掉
        else if (e.hadOld)  r = deleteDefaultIcon(e.cls, &e.oldValue, false);     // 恢复原值
        else                r = deleteDefaultIcon(e.cls, NULL, true);             // 只清默认值
        if (r) ok++;
    }

    DeleteFileW(stateFile().c_str());
    DeleteFileW(installedIcon().c_str());
    P(L"\n[ok] 已还原 " + W(ok) + L" 项, 状态记录已清除");

    // 快捷方式: 把改写过的图标位置写回去
    std::vector<LnkEntry> les;
    if (loadLnkState(les)) {
        int lk = 0;
        if (comReady()) {
            for (auto& e : les) if (lnkWriteIcon(e.path, e.oldIcon, e.oldIndex)) lk++;
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

    if (doRefresh) refreshIcons(restart);
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
    P(L"状态目录: " + appDir());
    std::wstring bk = backupDir();
    if (!bk.empty()) P(L"备份目录: " + bk);
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

        // 无论如何都尝试还原
        lnkWriteIcon(f, oldIcon, oldIdx);
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
        P(L"  0  退出");
        P(L"");
        std::wstring c = readLine(L"请选择 [0-4]: ");
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

    if (cmd == L"status") return cmdStatus();
    if (cmd == L"restore" || cmd == L"还原") {
        bool nr = false, nrs = false;
        for (int i = 2; i < argc; ++i) {
            if (av[i] == L"--no-refresh") nr = true;
            if (av[i] == L"--no-restart") nrs = true;
        }
        return cmdRestore(!nr, !nrs);
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
