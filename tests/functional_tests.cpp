// SystemCleaner 功能测试（真实文件系统 / 真实 HKCU 注册表 / 真实进程与快捷方式）
//
// 目的：验证各模块的实际行为，而不仅是 UI 显隐：
//   - 清理：递归扫描/清理、保护数据、只读与占用文件、深度上限、重解析点
//   - 启动项：超长值、路径解析、RunOnce 备份恢复、StartupApproved 隔离、旧版兼容
//   - 右键菜单：扫描、禁用/恢复、嵌套子键保留
//   - 注册表：长值读取、递归删除、容器键护栏、.reg 备份
//   - 网络：Hosts 换行归一化、备份、保存（重定向到临时文件，不碰真实 hosts）
//   - 粉碎：普通/只读/占用/目录/关键数据文件
//   - 实用工具：系统信息、重复文件、进程结束、快捷方式修复、磁盘清理
//
// 所有测试数据都在 %TEMP%\SCFuncTest_<pid>_<tick> 与 HKCU\Software\SystemCleanerFuncTest_<pid>
// 下，结束时会完整清理；不写入真实 Run/RunOnce，不调用网络修复（会重置 winsock）。
//
// 编译：tests\build_functional.cmd（控制台子系统、无 requireAdministrator manifest）。

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

// ====================== MessageBox 桩 ======================
// 关键：可配置返回值。删除类确认框测试需要 IDYES，默认 -1 走安全取消分支。
static int g_mbResult = -1;
static int g_messageBoxCalls = 0;
static std::wstring g_lastMessageBoxText;

static int TestMessageBoxStub(LPCWSTR text) {
    ++g_messageBoxCalls;
    if (text) g_lastMessageBoxText = text;
    return g_mbResult;
}
#define MessageBoxW(hwnd, text, caption, type) TestMessageBoxStub(text)

#define wWinMain func_unused_wWinMain
#include "../SystemCleaner/SystemCleaner.cpp"
#undef wWinMain

#include "../SystemCleaner/cleaner.cpp"
#include "../SystemCleaner/utils.cpp"
#include "../SystemCleaner/startup.cpp"
#include "../SystemCleaner/contextmenu.cpp"
#include "../SystemCleaner/registry.cpp"
#include "../SystemCleaner/network.cpp"
#include "../SystemCleaner/uninstaller.cpp"
#include "../SystemCleaner/tools.cpp"
#include "../SystemCleaner/ui_theme.cpp"

// ====================== 断言框架 ======================

static int g_total = 0;
static int g_failed = 0;
static const char* g_group = "";

#define CHECK(expr) \
    do { ++g_total; if (!(expr)) { ++g_failed; \
        printf("  [FAIL] %s (line %d): %s\n", g_group, __LINE__, #expr); } } while (0)

#define CHECK_MSG(expr, msg) \
    do { ++g_total; if (!(expr)) { ++g_failed; \
        printf("  [FAIL] %s (line %d): %s -> %s\n", g_group, __LINE__, #expr, msg); } } while (0)

#define GROUP(name) g_group = name; printf("\n== %s ==\n", name)

// ====================== 临时文件工具 ======================

static std::wstring g_tempRoot;

static std::wstring TempPath(const std::wstring& name) {
    return g_tempRoot + L"\\" + name;
}

static void InitTempRoot() {
    wchar_t base[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, base);
    wchar_t name[128];
    swprintf_s(name, L"SCFuncTest_%lu_%llu",
        (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    g_tempRoot = std::wstring(base) + name;
    CreateDirectoryW(g_tempRoot.c_str(), NULL);
}

static void RemoveTree(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // junction / 符号链接：只删除链接本身，绝不深入目标目录
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            RemoveDirectoryW(path.c_str());
            return;
        }
        std::wstring search = path + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring child = path + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    RemoveTree(child);
                } else {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryW(path.c_str());
        return;
    }

    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path.c_str());
}

static bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
    return ok && written == size;
}

static bool WriteText(const std::wstring& path, const std::wstring& text, bool append = false) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    int bytes = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, NULL, 0, NULL, NULL);
    std::string ansi((size_t)bytes, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, ansi.data(), bytes, NULL, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(h, ansi.data(), bytes - 1, &written, NULL);
    CloseHandle(h);
    return ok;
}

static std::vector<BYTE> ReadAllBytes(const std::wstring& path) {
    std::vector<BYTE> data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return data;
    DWORD size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE) {
        data.resize(size);
        DWORD read = 0;
        if (!ReadFile(h, data.data(), size, &read, NULL)) data.clear();
        else data.resize(read);
    }
    CloseHandle(h);
    return data;
}

static std::string ReadAllAnsi(const std::wstring& path) {
    auto bytes = ReadAllBytes(path);
    return std::string(bytes.begin(), bytes.end());
}

static bool FileContainsWideText(const std::wstring& file, const std::wstring& needle) {
    auto bytes = ReadAllBytes(file);
    if (bytes.size() < needle.size() * sizeof(wchar_t)) return false;
    for (size_t i = 0; i + needle.size() * sizeof(wchar_t) <= bytes.size(); i += 2) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); j++) {
            const wchar_t* p = (const wchar_t*)(bytes.data() + i + j * sizeof(wchar_t));
            if (*p != needle[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static void MakeDir(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), NULL);
}

static void MakeDirs(const std::wstring& path) {
    std::wstring cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == L'\\' && cur.size() > 3) CreateDirectoryW(cur.c_str(), NULL);
    }
    CreateDirectoryW(path.c_str(), NULL);
}

static bool SetFileAgeDays(const std::wstring& path, int days) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    li.QuadPart -= (ULONGLONG)days * 24ULL * 60ULL * 60ULL * 10000000ULL;
    ft.dwLowDateTime = li.LowPart;
    ft.dwHighDateTime = li.HighPart;
    BOOL ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok != FALSE;
}

// ====================== 注册表测试工具 ======================

static std::wstring g_regRoot; // HKCU\Software\SystemCleanerFuncTest_<pid>
static const wchar_t* kApprovedRun =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t* kApprovedFolder =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";

static bool RegSetString(HKEY hive, const std::wstring& subKey, const wchar_t* name,
                         const std::wstring& value, DWORD type = REG_SZ) {
    HKEY h = NULL;
    if (RegCreateKeyExW(hive, subKey.c_str(), 0, NULL, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL) != ERROR_SUCCESS) return false;
    LONG ret = RegSetValueExW(h, name, 0, type,
        (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return ret == ERROR_SUCCESS;
}

static bool RegSetDword(HKEY hive, const std::wstring& subKey, const wchar_t* name, DWORD value) {
    HKEY h = NULL;
    if (RegCreateKeyExW(hive, subKey.c_str(), 0, NULL, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL) != ERROR_SUCCESS) return false;
    LONG ret = RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(h);
    return ret == ERROR_SUCCESS;
}

static bool RegValueExists(HKEY hive, const std::wstring& subKey, const wchar_t* name) {
    HKEY h = NULL;
    if (RegOpenKeyExW(hive, subKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return false;
    LONG ret = RegQueryValueExW(h, name, NULL, NULL, NULL, NULL);
    RegCloseKey(h);
    return ret == ERROR_SUCCESS;
}

static bool RegKeyExists(HKEY hive, const std::wstring& subKey) {
    return RegKeyExistsInView(hive, subKey, KEY_WOW64_64KEY);
}

static void RegDeleteValueIfExists(HKEY hive, const wchar_t* subKey, const std::wstring& name) {
    HKEY h = NULL;
    if (RegOpenKeyExW(hive, subKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(h, name.c_str());
    RegCloseKey(h);
}

static bool IsApprovedDisabled(const wchar_t* approvedKey, const std::wstring& name) {
    HKEY h = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, approvedKey, 0,
            KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return false;
    BYTE data[32] = { 0 };
    DWORD size = sizeof(data);
    DWORD type = 0;
    LONG ret = RegQueryValueExW(h, name.c_str(), NULL, &type, data, &size);
    RegCloseKey(h);
    return ret == ERROR_SUCCESS && size >= 1 && (data[0] & 0x01) != 0;
}

static const StartupItem* FindStartup(const std::wstring& realName) {
    for (const auto& item : g_startupItems)
        if (_wcsicmp(item.realName.c_str(), realName.c_str()) == 0) return &item;
    return nullptr;
}

// ====================== 假 UI 宿主（供 tools.cpp 使用） ======================

static HWND g_host = NULL;
static std::vector<std::wstring> g_approvedTestValues;

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static void SetToolColumns(const std::vector<std::pair<const wchar_t*, int>>& cols) {
    while (ListView_DeleteColumn(g_hToolListView, 0)) {}
    for (size_t i = 0; i < cols.size(); i++) {
        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
        lvc.fmt = LVCFMT_LEFT;
        lvc.pszText = (LPWSTR)cols[i].first;
        lvc.cx = cols[i].second;
        ListView_InsertColumn(g_hToolListView, (int)i, &lvc);
    }
}

static std::wstring ListText(int row, int col) {
    wchar_t buf[2048] = { 0 };
    ListView_GetItemText(g_hToolListView, row, col, buf, 2048);
    return buf;
}

// 控制台默认 locale 无法转换中文，%ls 会直接丢内容；诊断输出统一转 UTF-8
static std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
        NULL, 0, NULL, NULL);
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
        out.data(), need, NULL, NULL);
    return out;
}

static void SelectToolRow(int row) {
    ListView_SetItemState(g_hToolListView, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(g_hToolListView, row, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
}

// 工作线程会同步向主线程拥有的 ListView 发送消息；等待时必须泵消息，
// 否则 SendMessage 永远等不到窗口线程处理，扫描状态也就永远不会结束
static void PumpFor(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    } while ((LONG)(end - GetTickCount()) > 0);
}

static void CreateFakeUi() {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SCFuncTestHost";
    RegisterClassExW(&wc);

    g_host = CreateWindowExW(0, L"SCFuncTestHost", L"func", WS_OVERLAPPED,
        0, 0, 900, 600, NULL, NULL, hInst, NULL);
    g_hMainWnd = g_host;

    g_hToolListView = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 700, 400, g_host, NULL, hInst, NULL);
    ListView_SetExtendedListViewStyle(g_hToolListView,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    g_hToolPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL, 0, 0, 300, 24, g_host, NULL, hInst, NULL);
    g_hToolAgeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 120, 200, g_host, NULL, hInst, NULL);
    SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"7天前");
    SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"30天前");
    SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"90天前");
    SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"180天前");
    SendMessageW(g_hToolAgeCombo, CB_SETCURSEL, 0, 0);

    g_hToolInfoLabel = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT, 0, 0, 400, 20, g_host, NULL, hInst, NULL);
}

// ====================== 1. 清理模块 ======================

static void Test_CleanerScanAndClean() {
    GROUP("FUNC-CLEANER 递归扫描/清理");
    std::wstring dir = TempPath(L"clean");
    MakeDir(dir);
    MakeDir(dir + L"\\sub");

    WriteBytes(dir + L"\\a.tmp", "0123456789", 10);
    WriteBytes(dir + L"\\sub\\b.tmp", "01234567890123456789", 20);
    WriteBytes(dir + L"\\sub\\keep.db", "012345678901234567890123456789", 30);
    WriteBytes(dir + L"\\thumbcache_1.db", "0123456789012345678901234567890123456789", 40);
    WriteBytes(dir + L"\\noext", "01234567890123456789012345678901234567890123456789", 50);
    WriteBytes(dir + L"\\c.tmp", "012345678901234567890123456789012345678901234567890123456789", 60);
    SetFileAttributesW((dir + L"\\c.tmp").c_str(), FILE_ATTRIBUTE_READONLY);

    long long size = 0;
    int count = 0;
    g_stopScan = false;
    ScanDirectory(dir, L"*.tmp", size, count, false);
    CHECK_MSG(count == 3, "*.tmp 应匹配 3 个文件");
    CHECK_MSG(size == 90, "*.tmp 总大小应为 90 字节");

    size = 0;
    count = 0;
    ScanDirectory(dir, L"*.*", size, count, true);
    CHECK_MSG(count == 5, "protectData 应保留 .db 但 thumbcache 例外（5 个文件）");
    CHECK_MSG(size == 180, "protectData 扫描大小应为 180 字节");

    // 只读文件不应被误判为「被占用」
    CHECK_MSG(!IsFileLocked(dir + L"\\c.tmp"), "只读文件不是被占用");

    // 占用文件：独占打开 a.tmp
    HANDLE hLock = CreateFileW((dir + L"\\a.tmp").c_str(), GENERIC_READ, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK_MSG(hLock != INVALID_HANDLE_VALUE, "应能独占打开测试文件");
    if (hLock != INVALID_HANDLE_VALUE) {
        CHECK_MSG(IsFileLocked(dir + L"\\a.tmp"), "独占打开的文件应判定为被占用");
    }

    g_cleanFailures.clear();
    g_stopClean = false;
    long long freed = 0;
    CleanDirectory(dir, L"*.tmp", freed, false);
    CHECK_MSG(freed == 80, "只应释放 b.tmp+c.tmp 共 80 字节");
    CHECK_MSG(GetFileAttributesW((dir + L"\\sub\\b.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
        "未被占用且非只读的 b.tmp 应被删除");
    CHECK_MSG(GetFileAttributesW((dir + L"\\c.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
        "只读 c.tmp 应先清属性再删除");
    CHECK_MSG(GetFileAttributesW((dir + L"\\a.tmp").c_str()) != INVALID_FILE_ATTRIBUTES,
        "被占用文件必须保留");
    CHECK_MSG(g_cleanFailures.size() == 1, "被占用文件应记录一条失败日志");
    if (!g_cleanFailures.empty()) {
        CHECK_MSG(g_cleanFailures[0].filePath.find(L"a.tmp") != std::wstring::npos,
            "失败日志应指向 a.tmp");
    }

    if (hLock != INVALID_HANDLE_VALUE) CloseHandle(hLock);
}

static void Test_CleanerDepthLimit() {
    GROUP("FUNC-DEPTH 深度上限");
    std::wstring root = TempPath(L"depth");
    MakeDir(root);
    WriteBytes(root + L"\\f.tmp", "x", 1);

    std::wstring cur = root;
    for (int i = 1; i <= 41; i++) {
        cur += L"\\d";
        MakeDir(cur);
        WriteBytes(cur + L"\\f.tmp", "x", 1);
    }

    long long size = 0;
    int count = 0;
    g_stopScan = false;
    ScanDirectory(root, L"*.tmp", size, count, false);
    CHECK_MSG(count == 41, "深度 0..40 的 41 个文件应被扫描，深度 41 的应被截断");

    g_stopClean = false;
    long long freed = 0;
    g_cleanFailures.clear();
    CleanDirectory(root, L"*.tmp", freed, false);
    CHECK_MSG(freed == 41, "清理深度上限应同样为 40 层");
}

static void Test_FindMatchingSubDirs() {
    GROUP("FUNC-SUBDIR 子目录通配与 junction");
    std::wstring base = TempPath(L"subdir");
    MakeDir(base);
    MakeDirs(base + L"\\app1\\Cache\\data");
    MakeDirs(base + L"\\app2\\Cache");
    MakeDirs(base + L"\\plain");
    WriteBytes(base + L"\\app1\\Cache\\data\\f.bin", "x", 1);
    WriteBytes(base + L"\\app2\\Cache\\f.bin", "x", 1);

    auto r1 = FindMatchingSubDirs(base, { L"*\\Cache\\*" });
    CHECK_MSG(r1.size() == 1, "*\\Cache\\* 应只匹配 app1\\Cache\\data");
    if (r1.size() == 1) {
        CHECK(r1[0].find(L"app1") != std::wstring::npos);
    }

    auto r2 = FindMatchingSubDirs(base, { L"*\\Cache" });
    CHECK_MSG(r2.size() == 2, "*\\Cache 应匹配 app1 与 app2 的 Cache");

    auto r3 = FindMatchingSubDirs(base, {});
    CHECK_MSG(r3.empty(), "空模式应返回空");
    auto r4 = FindMatchingSubDirs(base + L"\\missing", { L"*" });
    CHECK_MSG(r4.empty(), "不存在的根目录应返回空");

    // 指向外部的 junction：遍历绝不能进入，否则会清理扫描根之外的数据
    std::wstring outside = TempPath(L"outside");
    MakeDirs(outside + L"\\Cache");
    WriteBytes(outside + L"\\Cache\\secret.bin", "secret", 6);
    std::wstring link = base + L"\\junction";
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + outside + L"\"";
    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };
    bool made = false;
    if (CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        made = (code == 0) && PathFileExistsW(link.c_str());
    }
    if (made) {
        auto r5 = FindMatchingSubDirs(base, { L"*\\Cache" });
        bool throughJunction = false;
        for (const auto& p : r5)
            if (p.find(L"junction") != std::wstring::npos) throughJunction = true;
        CHECK_MSG(!throughJunction, "FindMatchingSubDirs 不得进入 junction 目标");
        CHECK_MSG(r5.size() == 2, "junction 不应引入额外匹配目录");

        long long size = 0;
        int count = 0;
        g_stopScan = false;
        ScanDirectory(base, L"secret.bin", size, count, false);
        CHECK_MSG(count == 0, "清理扫描不得穿过 junction");
    } else {
        printf("  (skip) 无法创建 junction，跳过重解析点断言\n");
    }
}

// ====================== 2. 注册表模块 ======================

static void Test_RegistryReadLong() {
    GROUP("FUNC-REGREAD 长值与类型过滤");
    std::wstring sub = g_regRoot + L"\\Read";
    std::wstring longValue(4000, L'A');
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, L"Long", longValue));
    CHECK(RegSetDword(HKEY_CURRENT_USER, sub, L"Dword", 123));

    HKEY h = NULL;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0,
        KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS);
    if (h) {
        std::wstring got = RegReadStrW(h, L"Long");
        CHECK_MSG(got.size() == 4000, "4000 字符的 REG_SZ 必须完整读取");
        CHECK_MSG(got == longValue, "长值内容必须一致");
        CHECK_MSG(RegReadStrW(h, L"Dword").empty(), "REG_DWORD 不应被当作字符串返回");
        CHECK_MSG(RegReadStrW(h, L"Missing").empty(), "不存在的值应返回空");
        RegCloseKey(h);
    }
}

static void Test_RegistryDeleteGuard() {
    GROUP("FUNC-REGDEL 递归删除与容器护栏");
    // 纯函数护栏：绝不能尝试真的删除 Software / Services
    CHECK_MSG(!IsSafeToDeleteRegistryKey(HKEY_CURRENT_USER, L"Software"),
        "HKCU\\Software 不允许整棵删除");
    CHECK_MSG(!IsSafeToDeleteRegistryKey(HKEY_CURRENT_USER, L"Software\\Microsoft"),
        "HKCU\\Software\\Microsoft 不允许整棵删除");
    CHECK_MSG(!IsSafeToDeleteRegistryKey(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services"),
        "HKLM\\...\\Services 不允许整棵删除");
    CHECK_MSG(!IsSafeToDeleteRegistryKey(HKEY_CURRENT_USER, L""),
        "空路径不允许删除");

    std::wstring sub = g_regRoot + L"\\Del";
    std::wstring leaf = sub + L"\\B\\C";
    CHECK(RegSetString(HKEY_CURRENT_USER, leaf, L"x", L"value"));
    HKEY h = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, leaf.c_str(), 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
        RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE*)L"default", 16);
        RegCloseKey(h);
    }
    CHECK(RegKeyExists(HKEY_CURRENT_USER, sub));
    CHECK(RecursiveDeleteKeyEx(HKEY_CURRENT_USER, sub, KEY_WOW64_64KEY));
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, sub), "递归删除后目标键应不存在");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, g_regRoot), "父键不应被误删");

    CHECK_MSG(RecursiveDeleteKeyEx(HKEY_CURRENT_USER,
        g_regRoot + L"\\NotExist\\X", KEY_WOW64_64KEY),
        "删除不存在的键应视为成功（幂等）");
}

static void Test_RegistryBackupFile() {
    GROUP("FUNC-REGBACKUP 单键备份");
    std::wstring sub = g_regRoot + L"\\BackupKey";
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, L"Name", L"BackupValue"));

    std::wstring outFile = TempPath(L"backup.reg");
    bool ok = BackupRegistryKey(HKEY_CURRENT_USER, sub, KEY_WOW64_64KEY, outFile);
    CHECK_MSG(ok, "HKCU 测试键应能导出为 .reg");
    CHECK_MSG(PathFileExistsW(outFile.c_str()), "备份文件必须存在");
    if (PathFileExistsW(outFile.c_str())) {
        auto bytes = ReadAllBytes(outFile);
        CHECK_MSG(bytes.size() > 100, "备份文件不应为空");
        CHECK_MSG(FileContainsWideText(outFile, L"SystemCleanerFuncTest_"),
            "备份内容应包含目标键路径");
        CHECK_MSG(FileContainsWideText(outFile, L"BackupValue"),
            "备份内容应包含键内的值");
    }

    std::wstring missing = TempPath(L"missing.reg");
    CHECK_MSG(BackupRegistryKey(HKEY_CURRENT_USER, g_regRoot + L"\\NoSuchKey",
        KEY_WOW64_64KEY, missing), "项不存在时备份应返回成功（无内容可备份）");
}

// ====================== 3. 粉碎 ======================

static void Test_ShredFile() {
    GROUP("FUNC-SHRED 文件粉碎");
    std::wstring dir = TempPath(L"shred");
    MakeDir(dir);

    std::wstring normal = dir + L"\\normal.tmp";
    WriteBytes(normal, "0123456789", 10);
    CHECK_MSG(ShredFile(normal), "普通文件应粉碎成功");
    CHECK_MSG(!PathFileExistsW(normal.c_str()), "粉碎后文件应不存在");

    std::wstring ro = dir + L"\\readonly.tmp";
    WriteBytes(ro, "0123456789", 10);
    SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK_MSG(ShredFile(ro), "只读文件应粉碎成功");
    CHECK_MSG(!PathFileExistsW(ro.c_str()), "只读文件粉碎后应不存在");

    std::wstring locked = dir + L"\\locked.txt";
    WriteBytes(locked, "0123456789", 10);
    HANDLE hLock = CreateFileW(locked.c_str(), GENERIC_READ, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(hLock != INVALID_HANDLE_VALUE);
    if (hLock != INVALID_HANDLE_VALUE) {
        CHECK_MSG(!ShredFile(locked), "被占用文件必须拒绝粉碎");
        CHECK_MSG(PathFileExistsW(locked.c_str()), "被占用文件不得被删除");
        CloseHandle(hLock);
    }

    std::wstring protectedFile = dir + L"\\keep.db";
    WriteBytes(protectedFile, "data", 4);
    CHECK_MSG(!ShredFile(protectedFile), "数据库文件应受保护，拒绝粉碎");
    CHECK_MSG(PathFileExistsW(protectedFile.c_str()), "受保护文件不得被删除");

    CHECK_MSG(!ShredFile(dir), "目录不能被粉碎");
    CHECK_MSG(PathFileExistsW(dir.c_str()), "目录必须保留");
    CHECK_MSG(!ShredFile(dir + L"\\not_exist.tmp"), "不存在的文件应返回失败");
    CHECK_MSG(!ShredFile(dir + L"\\..\\escape.tmp"), "包含 .. 的路径必须拒绝");
}

// ====================== 4. 启动项 ======================

static void Test_StartupLongValues() {
    GROUP("FUNC-STARTUP 长名称/长命令");
    std::wstring sub = g_regRoot + L"\\Run";
    std::wstring name(300, L'N');
    std::wstring data(4000, L'x');
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, name.c_str(), data));
    CHECK(RegSetDword(HKEY_CURRENT_USER, sub, L"IgnoredDword", 7));

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadRegistryStartup(HKEY_CURRENT_USER, sub, L"test", 0);
    CHECK_MSG(g_startupItems.size() == 1, "只应加载一个字符串启动项，DWORD 应被忽略");
    if (!g_startupItems.empty()) {
        CHECK_MSG(g_startupItems[0].realName.size() == 300, "300 字符的值名必须完整保留");
        CHECK_MSG(g_startupItems[0].path.size() == 4000, "4000 字符的命令必须完整保留");
        CHECK_MSG(g_startupItems[0].isEnabled, "普通 Run 值应视为启用");
    }

    // 同一物理键重复扫描时应去重
    LoadRegistryStartup(HKEY_CURRENT_USER, sub, L"test", 0);
    CHECK_MSG(g_startupItems.size() == 1, "重复扫描同一位置不得产生重复条目");
}

static void Test_StartupTargetExists() {
    GROUP("FUNC-STARTUPPATH 启动目标解析");
    std::wstring dir = TempPath(L"startup");
    MakeDir(dir);
    std::wstring exe = dir + L"\\my app.exe";
    WriteBytes(exe, "MZ", 2);

    StartupItem item;
    item.isRegistry = true;

    item.path = L"\"" + exe + L"\" /min";
    CHECK_MSG(StartupTargetExists(item), "带引号的路径应解析成功");
    item.path = exe + L" /min";
    CHECK_MSG(StartupTargetExists(item), "未加引号且含空格的路径应按最长真实前缀解析");
    item.path = dir + L"\\missing.exe";
    CHECK_MSG(!StartupTargetExists(item), "不存在的绝对路径应判为无效");
    item.path = L"%SystemRoot%\\explorer.exe";
    CHECK_MSG(StartupTargetExists(item), "环境变量路径应展开后判断");
    item.path = L"rundll32.exe shell32.dll,Control_RunDLL";
    CHECK_MSG(StartupTargetExists(item), "相对命令由 PATH 解析，不能判为无效");
    item.path = dir;
    CHECK_MSG(!StartupTargetExists(item), "目录不是可启动文件");
    item.path.clear();
    CHECK_MSG(StartupTargetExists(item), "空命令按无法判定处理（不误报无效）");

    StartupItem folderItem;
    folderItem.isRegistry = false;
    folderItem.path = exe;
    CHECK_MSG(StartupTargetExists(folderItem), "启动文件夹中存在的文件应有效");
    folderItem.path = exe + L".disabled";
    CHECK_MSG(StartupTargetExists(folderItem), ".disabled 文件应回退检查原名");
    folderItem.path = dir + L"\\gone.lnk.disabled";
    CHECK_MSG(!StartupTargetExists(folderItem), "原名与禁用名都不存在时应无效");
}

static void Test_StartupLegacy() {
    GROUP("FUNC-STARTUPLEGACY 旧版 _disabled 值");
    std::wstring sub = g_regRoot + L"\\LegacyRun";
    std::wstring cmd = L"cmd.exe /c exit 0";
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, L"LegacyApp_disabled", cmd));

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadRegistryStartup(HKEY_CURRENT_USER, sub, L"test", 0);
    const StartupItem* item = FindStartup(L"LegacyApp_disabled");
    CHECK_MSG(item != nullptr, "应加载 _disabled 值");
    if (item) {
        CHECK_MSG(!item->isEnabled, "_disabled 值应显示为已禁用");
        CHECK_MSG(item->displayName == L"LegacyApp", "显示名应去掉 _disabled 后缀");
        CHECK_MSG(DisableStartupItem(*item), "对已禁用项再次禁用应为幂等成功");
        CHECK_MSG(EnableStartupItem(*item), "恢复旧版项应改名回原名");
        CHECK_MSG(RegValueExists(HKEY_CURRENT_USER, sub, L"LegacyApp"),
            "恢复后应存在原值名");
        CHECK_MSG(!RegValueExists(HKEY_CURRENT_USER, sub, L"LegacyApp_disabled"),
            "恢复后不应残留 _disabled 值名");
    }

    // 原名已被占用时不得覆盖
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, L"Collision_disabled", L"cmd.exe /c exit 1"));
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, L"Collision", L"cmd.exe /c exit 2"));
    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadRegistryStartup(HKEY_CURRENT_USER, sub, L"test", 0);
    const StartupItem* collision = FindStartup(L"Collision_disabled");
    if (collision) {
        CHECK_MSG(!EnableStartupItem(*collision), "原名被占用时恢复必须失败");
        CHECK_MSG(RegValueExists(HKEY_CURRENT_USER, sub, L"Collision"),
            "冲突时不得覆盖原有值");
        CHECK_MSG(RegValueExists(HKEY_CURRENT_USER, sub, L"Collision_disabled"),
            "冲突时 _disabled 值应保留，便于用户处理");
    } else {
        CHECK_MSG(false, "应加载到 Collision_disabled");
    }
}

static void Test_StartupRunOnceBackup() {
    GROUP("FUNC-STARTUPBACKUP RunOnce 备份/恢复");
    std::wstring sub = g_regRoot + L"\\RunOnce";
    std::wstring name = L"Once_" + std::to_wstring(GetCurrentProcessId());
    std::wstring data = L"%TEMP%\\once.exe --flag";
    CHECK(RegSetString(HKEY_CURRENT_USER, sub, name.c_str(), data, REG_EXPAND_SZ));

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadRegistryStartup(HKEY_CURRENT_USER, sub, L"test", 0);
    const StartupItem* item = FindStartup(name);
    CHECK_MSG(item != nullptr, "应加载 RunOnce 项");
    if (!item) return;

    StartupItem toDisable = *item;
    CHECK_MSG(DisableStartupItem(toDisable), "RunOnce 禁用应成功");
    CHECK_MSG(!RegValueExists(HKEY_CURRENT_USER, sub, name.c_str()),
        "禁用 RunOnce 必须把值从原位置移走");

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadBackedUpStartupItems(HKEY_CURRENT_USER, sub, 0, L"test");
    const StartupItem* backed = FindStartup(name);
    CHECK_MSG(backed != nullptr, "禁用后应能从备份区加载");
    if (backed) {
        CHECK_MSG(backed->isBackedUp, "备份项应带 isBackedUp 标记");
        CHECK_MSG(!backed->isEnabled, "备份项应显示为已禁用");
        CHECK_MSG(backed->path == data, "备份项命令内容应完整");
        CHECK_MSG(EnableStartupItem(*backed), "从备份恢复应成功");
        CHECK_MSG(RegValueExists(HKEY_CURRENT_USER, sub, name.c_str()),
            "恢复后值应回到原位置");

        HKEY h = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0,
                KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
            DWORD type = 0;
            wchar_t buf[512] = { 0 };
            DWORD size = sizeof(buf);
            LONG ret = RegQueryValueExW(h, name.c_str(), NULL, &type, (BYTE*)buf, &size);
            CHECK_MSG(ret == ERROR_SUCCESS && type == REG_EXPAND_SZ,
                "恢复后必须保留 REG_EXPAND_SZ 类型");
            CHECK_MSG(data == buf, "恢复后命令内容必须一致");
            RegCloseKey(h);
        } else {
            CHECK_MSG(false, "恢复后应能打开原键");
        }
    }
}

static void Test_StartupApprovedPlainRun() {
    GROUP("FUNC-STARTUPAPPROVED 普通 Run 禁用机制");
    // 使用真实 Run 路径名匹配规则，但只写 StartupApproved 记录，绝不创建真实 Run 值
    std::wstring name = L"SCFuncTest_" + std::to_wstring(GetCurrentProcessId()) + L"_PlainRun";
    g_approvedTestValues.push_back(name);
    RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedRun, name);

    StartupItem item;
    item.realName = name;
    item.displayName = name;
    item.path = L"C:\\Windows\\explorer.exe";
    item.hive = HKEY_CURRENT_USER;
    item.subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    item.isRegistry = true;
    item.view = GetRegistryViewAccess();
    item.isEnabled = true;

    CHECK_MSG(DisableStartupItem(item), "普通 Run 禁用应写入 StartupApproved");
    CHECK_MSG(IsApprovedDisabled(kApprovedRun, name),
        "StartupApproved\\Run 中应有首字节为 0x03 的禁用记录");
    CHECK_MSG(!RegValueExists(HKEY_CURRENT_USER, item.subKey, name.c_str()),
        "禁用动作不得在真实 Run 中创建值");

    StartupItem disabled = item;
    disabled.isEnabled = false;
    CHECK_MSG(EnableStartupItem(disabled), "清除 StartupApproved 应成功");
    CHECK_MSG(!RegValueExists(HKEY_CURRENT_USER, kApprovedRun, name.c_str()),
        "恢复后 StartupApproved 记录应删除");
}

static void Test_StartupApprovedIsolation() {
    GROUP("FUNC-STARTUPAPPROVED 多来源隔离");
    std::wstring name = L"SCFuncTest_" + std::to_wstring(GetCurrentProcessId()) + L"_Iso";
    g_approvedTestValues.push_back(name);
    RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedRun, name);

    // 写一条 12 字节的禁用记录（与任务管理器格式一致）
    HKEY h = NULL;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, NULL, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL) == ERROR_SUCCESS);
    if (h) {
        BYTE data[12] = { 0x03, 0 };
        CHECK(RegSetValueExW(h, name.c_str(), 0, REG_BINARY, data, 12) == ERROR_SUCCESS);
        RegCloseKey(h);
    }

    std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    StartupItem hkcuItem;
    hkcuItem.realName = name;
    hkcuItem.displayName = name;
    hkcuItem.hive = HKEY_CURRENT_USER;
    hkcuItem.subKey = runKey;
    hkcuItem.isRegistry = true;
    hkcuItem.view = KEY_WOW64_64KEY;
    hkcuItem.isEnabled = true;

    StartupItem hklmItem = hkcuItem;
    hklmItem.hive = HKEY_LOCAL_MACHINE;

    StartupItem hkcu32Item = hkcuItem;
    hkcu32Item.view = KEY_WOW64_32KEY;

    StartupItem otherName = hkcuItem;
    otherName.realName = name + L"_Other";
    otherName.displayName = name + L"_Other";

    g_startupItems.clear();
    g_startupItems.push_back(hkcuItem);
    g_startupItems.push_back(hklmItem);
    g_startupItems.push_back(hkcu32Item);
    g_startupItems.push_back(otherName);
    ApplyStartupApproved();

    CHECK_MSG(!g_startupItems[0].isEnabled, "HKCU 64 位普通 Run 项应被标记禁用");
    CHECK_MSG(g_startupItems[1].isEnabled, "HKLM 同名项不得被 HKCU 记录影响");
    CHECK_MSG(g_startupItems[2].isEnabled, "32 位视图项不得被 64 位 Run 记录影响");
    CHECK_MSG(g_startupItems[3].isEnabled, "不同值名不得被误标记");
}

static void Test_StartupLegacyCrossEffect() {
    GROUP("FUNC-STARTUPAPPROVED 旧版兼容隔离");
    // 回归：恢复 RunOnce 的 X_disabled 不得清掉真正 Run 项 X 的禁用记录
    std::wstring base = L"SCFuncTest_" + std::to_wstring(GetCurrentProcessId()) + L"_Cross";
    g_approvedTestValues.push_back(base);
    RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedRun, base);

    HKEY h = NULL;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, NULL, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL) == ERROR_SUCCESS);
    if (h) {
        BYTE data[12] = { 0x03, 0 };
        CHECK(RegSetValueExW(h, base.c_str(), 0, REG_BINARY, data, 12) == ERROR_SUCCESS);
        RegCloseKey(h);
    }

    std::wstring runOnce = g_regRoot + L"\\RunOnce";
    CHECK(RegSetString(HKEY_CURRENT_USER, runOnce, (base + L"_disabled").c_str(),
        L"cmd.exe /c exit 0"));

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadRegistryStartup(HKEY_CURRENT_USER, runOnce, L"test", 0);
    const StartupItem* legacy = FindStartup(base + L"_disabled");
    CHECK_MSG(legacy != nullptr, "应加载到 RunOnce 的旧版禁用值");
    if (legacy) {
        CHECK_MSG(EnableStartupItem(*legacy), "恢复旧版 RunOnce 值应成功");
        CHECK_MSG(RegValueExists(HKEY_CURRENT_USER, runOnce, base.c_str()),
            "原值名应恢复");
        CHECK_MSG(IsApprovedDisabled(kApprovedRun, base),
            "RunOnce 的恢复不得清除另一个 Run 项的 StartupApproved 禁用记录");
    }
}

static void Test_StartupFolder() {
    GROUP("FUNC-STARTUPFOLDER 启动文件夹");
    std::wstring dir = TempPath(L"startupfolder");
    MakeDir(dir);
    WriteText(dir + L"\\desktop.ini", L"[.ShellClassInfo]\r\n");
    WriteBytes(dir + L"\\app.lnk", "lnk", 3);
    WriteBytes(dir + L"\\gone.lnk.disabled", "lnk", 3);

    g_startupItems.clear();
    g_seenStartupKeys.clear();
    LoadFolderStartup(dir, L"test", HKEY_CURRENT_USER);
    CHECK_MSG(g_startupItems.size() == 2, "desktop.ini 不应算作启动项");
    const StartupItem* app = FindStartup(L"app.lnk");
    const StartupItem* gone = FindStartup(L"gone.lnk.disabled");
    CHECK_MSG(app != nullptr && app->isEnabled, "app.lnk 应为启用状态");
    CHECK_MSG(gone != nullptr && !gone->isEnabled, "gone.lnk.disabled 应为禁用状态");
    if (gone) {
        CHECK_MSG(gone->displayName == L"gone.lnk", "禁用项显示名应去掉 .disabled");
        CHECK_MSG(!StartupTargetExists(*gone), "原名不存在时禁用项应显示路径无效");
    }

    if (app) {
        CHECK_MSG(DisableStartupItem(*app), "禁用文件启动项应成功");
        CHECK_MSG(PathFileExistsW((dir + L"\\app.lnk.disabled").c_str()),
            "禁用后应改名为 .disabled");
        CHECK_MSG(!PathFileExistsW((dir + L"\\app.lnk").c_str()), "原文件名应不存在");

        g_startupItems.clear();
        g_seenStartupKeys.clear();
        LoadFolderStartup(dir, L"test", HKEY_CURRENT_USER);
        const StartupItem* disabledApp = FindStartup(L"app.lnk.disabled");
        CHECK_MSG(disabledApp != nullptr, "重新扫描应加载禁用后的文件项");
        if (disabledApp) {
            CHECK_MSG(EnableStartupItem(*disabledApp), "恢复文件启动项应成功");
            CHECK_MSG(PathFileExistsW((dir + L"\\app.lnk").c_str()), "恢复后原文件名应存在");
        }
    }

    // 任务管理器禁用的文件项：文件名没有后缀，要靠清除 StartupApproved 恢复
    std::wstring fname = L"SCFuncTest_" + std::to_wstring(GetCurrentProcessId()) + L"_Folder.lnk";
    g_approvedTestValues.push_back(fname);
    RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedFolder, fname);
    WriteBytes(dir + L"\\" + fname, "lnk", 3);
    HKEY h = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedFolder, 0, NULL, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &h, NULL) == ERROR_SUCCESS) {
        BYTE data[12] = { 0x03, 0 };
        RegSetValueExW(h, fname.c_str(), 0, REG_BINARY, data, 12);
        RegCloseKey(h);
    }
    CHECK(IsApprovedDisabled(kApprovedFolder, fname));
    StartupItem approvedItem;
    approvedItem.realName = fname;
    approvedItem.displayName = fname;
    approvedItem.path = dir + L"\\" + fname;
    approvedItem.hive = HKEY_CURRENT_USER;
    approvedItem.isRegistry = false;
    approvedItem.isEnabled = false;
    CHECK_MSG(EnableStartupItem(approvedItem), "启用文件夹项应成功");
    CHECK_MSG(!RegValueExists(HKEY_CURRENT_USER, kApprovedFolder, fname.c_str()),
        "启用后 StartupApproved 记录应删除");
}

// ====================== 5. 右键菜单 ======================

static void Test_ContextMenu() {
    GROUP("FUNC-CTXMENU 右键菜单禁用/恢复");
    // HKCR 合并的是 HKCU\Software\Classes，测试菜单必须建在该路径下才能被扫描到
    std::wstring className = L"SCFuncTestCtx_" + std::to_wstring(GetCurrentProcessId());
    std::wstring classesRoot = L"Software\\Classes\\" + className;
    std::wstring menuKey = classesRoot + L"\\shell\\TestMenu";
    CHECK(RegSetString(HKEY_CURRENT_USER, menuKey + L"\\command", NULL,
        L"\"C:\\Windows\\notepad.exe\" \"%1\""));

    g_contextMenuItems.clear();
    g_seenContextKeys.clear();
    ScanContextMenuReg(HKEY_CLASSES_ROOT, className + L"\\shell", L"测试", 0);
    CHECK_MSG(g_contextMenuItems.size() == 1, "应扫描到一个测试菜单项");
    if (g_contextMenuItems.empty()) return;

    RightMenuItem item = g_contextMenuItems[0];
    CHECK_MSG(item.realKeyName == L"TestMenu", "键名应为 TestMenu");
    CHECK_MSG(item.hive == HKEY_CLASSES_ROOT, "扫描 HKCR 时 hive 应为 HKCR");
    CHECK_MSG(item.isEnabled, "新菜单项应为启用状态");

    CHECK_MSG(DisableContextMenuItem(item), "禁用右键菜单应成功");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, menuKey + L"_disabled"),
        "禁用后应存在 _disabled 键");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, menuKey), "禁用后原键应不存在");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, menuKey + L"_disabled\\command"),
        "禁用改名必须保留嵌套子键");

    g_contextMenuItems.clear();
    g_seenContextKeys.clear();
    // HKCR 自身就是合并后的 Software\Classes 视图，路径不能再带该前缀
    ScanContextMenuReg(HKEY_CLASSES_ROOT, className + L"\\shell", L"测试", 0);
    CHECK_MSG(g_contextMenuItems.size() == 1, "重新扫描应看到禁用项");
    if (!g_contextMenuItems.empty()) {
        RightMenuItem disabled = g_contextMenuItems[0];
        CHECK_MSG(!disabled.isEnabled, "重新扫描应识别 _disabled 状态");
        CHECK(disabled.displayName == L"TestMenu");
        CHECK_MSG(EnableContextMenuItem(disabled), "恢复右键菜单应成功");
        CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, menuKey), "恢复后原键应存在");
        CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, menuKey + L"\\command"),
            "恢复后嵌套子键应保留");
        CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, menuKey + L"_disabled"),
            "恢复后不应残留 _disabled 键");
    }

    // 父键不存在时必须失败而不是崩溃
    RightMenuItem missing;
    missing.realKeyName = L"X";
    missing.displayName = L"X";
    missing.hive = HKEY_CLASSES_ROOT;
    missing.parentPath = className + L"\\NoSuchParent";
    missing.isEnabled = true;
    CHECK_MSG(!DisableContextMenuItem(missing), "父键不存在时禁用应返回失败");
    missing.isEnabled = false;
    CHECK_MSG(!EnableContextMenuItem(missing), "父键不存在时恢复应返回失败");

    // 全量扫描不得产生同一物理项的重复记录
    g_contextMenuItems.clear();
    g_seenContextKeys.clear();
    ScanAllContextMenus();
    std::set<std::wstring> seen;
    bool dup = false;
    for (const auto& it : g_contextMenuItems) {
        std::wstring key = std::to_wstring((unsigned long)it.view) + L"|" +
            it.parentPath + L"|" + it.realKeyName;
        for (auto& c : key) c = towlower(c);
        if (!seen.insert(key).second) dup = true;
    }
    CHECK_MSG(!dup, "全量扫描结果不得有重复物理项");
    printf("  (info) 系统右键菜单项: %zu\n", g_contextMenuItems.size());
}

// ====================== 6. 网络 / Hosts ======================

static void Test_HostsFile() {
    GROUP("FUNC-HOSTS 读写/备份/保存（临时文件）");
    std::wstring dir = TempPath(L"hosts");
    MakeDir(dir);
    std::wstring hosts = dir + L"\\hosts";
    std::string raw = "# test hosts\n127.0.0.1 localhost\n::1 localhost\n";
    WriteBytes(hosts, raw.data(), (DWORD)raw.size());

    const wchar_t* savedPath = HOSTS_PATH;
    HOSTS_PATH = hosts.c_str();

    std::wstring text = ReadHostsFile();
    CHECK_MSG(!text.empty(), "应读到 hosts 内容");
    CHECK_MSG(text.find(L"localhost") != std::wstring::npos, "hosts 内容应完整");
    bool loneLf = false;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) loneLf = true;
    }
    CHECK_MSG(!loneLf, "LF 换行必须归一化为 CRLF，否则多行 EDIT 会挤成一行");

    std::wstring backup = BackupHostsFile();
    CHECK_MSG(!backup.empty(), "Hosts 备份应成功");
    CHECK_MSG(PathFileExistsW(backup.c_str()), "备份文件应存在");
    if (!backup.empty()) {
        CHECK_MSG(ReadAllAnsi(backup) == raw, "备份内容必须与原文逐字节一致");
        CHECK_MSG(backup.find(dir) == 0, "备份必须与 hosts 同目录");
    }

    HWND edit = CreateWindowExW(0, L"EDIT", L"",
        WS_POPUP | ES_MULTILINE, 0, 0, 400, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(edit != NULL);
    g_hHostsEdit = edit;
    SetWindowTextW(edit, L"# saved by test\r\n127.0.0.1 test.local\r\n");
    SaveHostsFile();
    std::string saved = ReadAllAnsi(hosts);
    CHECK_MSG(saved.find("test.local") != std::string::npos, "保存后 hosts 应包含新内容");
    CHECK_MSG(saved.find("saved by test") != std::string::npos, "保存内容应完整写入");
    g_hHostsEdit = NULL;
    DestroyWindow(edit);

    std::wstring missing = dir + L"\\missing_hosts";
    HOSTS_PATH = missing.c_str();
    CHECK_MSG(ReadHostsFile().empty(), "不存在的 hosts 应返回空");
    CHECK_MSG(BackupHostsFile().empty(), "不存在的 hosts 备份应返回空");

    HOSTS_PATH = savedPath;
}

// ====================== 7. 卸载安全谓词 ======================

static void Test_UninstallPredicates() {
    GROUP("FUNC-UNINSTALL 残留与安全谓词");
    std::wstring reason;

    CHECK(IsSystemOrPatchUninstallEntry(1, L"", L""));
    CHECK(IsSystemOrPatchUninstallEntry(0, L"Update", L""));
    CHECK(IsSystemOrPatchUninstallEntry(0, L"", L"KB123456"));
    CHECK(!IsSystemOrPatchUninstallEntry(0, L"", L""));

    std::wstring dir = TempPath(L"uninst");
    MakeDir(dir);
    CHECK_MSG(!IsUninstallEntryLeftover(dir, L"", reason), "存在的安装目录不算残留");
    CHECK_MSG(IsUninstallEntryLeftover(dir + L"\\missing", L"", reason),
        "InstallLocation 不存在应判为残留");
    std::wstring filePath = dir + L"\\file.exe";
    WriteBytes(filePath, "x", 1);
    CHECK_MSG(IsUninstallEntryLeftover(filePath, L"", reason),
        "InstallLocation 指向文件应判为残留");
    CHECK_MSG(!IsUninstallEntryLeftover(L"C:\\bad*path", L"", reason),
        "含通配符的 InstallLocation 不可信，不判残留");
    CHECK_MSG(IsUninstallEntryLeftover(L"", dir + L"\\no_uninstaller.exe", reason),
        "卸载器不存在应判为残留");
    CHECK_MSG(!IsUninstallEntryLeftover(L"", L"msiexec.exe /x {0000}", reason),
        "msiexec 永远存在，不构成残留信号");
    CHECK_MSG(!IsUninstallEntryLeftover(L"", L"", reason), "两者都没有时无法判定");

    CHECK_MSG(IsInvalidStartupCommand(L"", reason), "空启动命令应判无效");
    std::wstring exe = dir + L"\\app.exe";
    WriteBytes(exe, "MZ", 2);
    CHECK_MSG(!IsInvalidStartupCommand(L"\"" + exe + L"\" /silent", reason),
        "存在的绝对路径不应判无效");
    CHECK_MSG(IsInvalidStartupCommand(dir + L"\\missing.exe", reason),
        "不存在的绝对路径应判无效");
    CHECK_MSG(!IsInvalidStartupCommand(L"rundll32.exe shell32.dll,Test", reason),
        "相对命令不应判无效");
    CHECK_MSG(!IsInvalidStartupCommand(L"%SystemRoot%\\explorer.exe", reason),
        "环境变量命令应展开后判断");

    std::wstring outExe, outParams;
    CHECK(SplitUninstallCommand(L"\"C:\\Program Files\\App\\unins.exe\" /S", outExe, outParams));
    CHECK_MSG(outExe == L"C:\\Program Files\\App\\unins.exe", "引号包裹的路径应完整提取");
    CHECK_MSG(outParams == L"/S", "参数应正确提取");

    std::wstring exeWithSpace = dir + L"\\my tool.exe";
    WriteBytes(exeWithSpace, "MZ", 2);
    CHECK(SplitUninstallCommand(exeWithSpace + L" /SILENT", outExe, outParams));
    CHECK_MSG(outExe == exeWithSpace, "无引号含空格路径应按最长真实前缀提取");
    CHECK_MSG(outParams == L"/SILENT", "参数应正确提取");

    CHECK(SplitUninstallCommand(L"C:\\NoSuchApp\\unins.exe /x", outExe, outParams));
    CHECK_MSG(outExe == L"C:\\NoSuchApp\\unins.exe", "都不存在时退回第一个 token");

    CHECK_MSG(IsPathUnderDirectory(dir + L"\\sub", dir), "子目录应判定在父目录下");
    CHECK_MSG(!IsPathUnderDirectory(dir + L"2", dir), "同名前缀的兄弟目录不算子目录");
    CHECK_MSG(IsPathUnderDirectory(dir, dir), "同一路径算包含");

    CHECK_MSG(IsSafeToDeleteDirectory(dir), "普通临时目录应允许删除");
    CHECK_MSG(!IsSafeToDeleteDirectory(L"C:\\"), "盘符根不允许删除");
    CHECK_MSG(!IsSafeToDeleteDirectory(dir + L"\\..\\other"), "含 .. 的路径不允许删除");
    wchar_t pf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH))
        CHECK_MSG(!IsSafeToDeleteDirectory(pf), "Program Files 根不允许删除");

    CHECK_MSG(IsSystemComponentImagePath(L""), "读不到 ImagePath 时按系统组件处理");
    CHECK_MSG(IsSystemComponentImagePath(L"\\System32\\drivers\\ntfs.sys"),
        "System32 驱动应判为系统组件");
    CHECK_MSG(!IsSystemComponentImagePath(L"\\??\\" + dir + L"\\third.sys"),
        "第三方目录下的驱动不应判为系统组件");
}

// ====================== 7.1 Shell 扩展 / App Paths 残留清理 ======================

static void Test_UninstallShellExtCleanup() {
    GROUP("FUNC-UNINSTALLEXT 安装目录关联的 Shell 扩展/App Paths");

    std::wstring installDir = TempPath(L"ext\\7-Zip");
    MakeDirs(installDir);
    std::wstring dll = installDir + L"\\7-zip.dll";
    WriteBytes(dll, "MZ", 2);

    std::wstring classesRoot = g_regRoot + L"\\Classes";
    const wchar_t* fakeGuid = L"{23170F69-40C1-278A-1000-000100020000}";
    const wchar_t* localGuid = L"{22222222-3333-4444-5555-666666666666}";
    const wchar_t* otherGuid = L"{11111111-2222-3333-4444-555555555555}";

    std::wstring clsIn = classesRoot + L"\\CLSID\\" + fakeGuid;
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, clsIn + L"\\InprocServer32", NULL, dll),
        "应能写入测试 InprocServer32");
    std::wstring clsLocal = classesRoot + L"\\CLSID\\" + localGuid;
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, clsLocal + L"\\LocalServer32", NULL,
        L"\"" + installDir + L"\\7zFM.exe\" /Automation"),
        "应能写入带引号与参数的 LocalServer32");

    std::wstring otherDir = TempPath(L"ext\\Other");
    MakeDirs(otherDir);
    std::wstring otherDll = otherDir + L"\\other.dll";
    WriteBytes(otherDll, "MZ", 2);
    std::wstring clsOut = classesRoot + L"\\CLSID\\" + otherGuid;
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, clsOut + L"\\InprocServer32", NULL, otherDll),
        "应能写入无关 CLSID");

    std::wstring handlerParent = classesRoot + L"\\*\\shellex\\ContextMenuHandlers";
    std::wstring handlerKey = handlerParent + L"\\7-Zip";
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, handlerKey, NULL, fakeGuid),
        "应能写入右键 handler 引用");
    std::wstring otherHandler = handlerParent + L"\\Other";
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, otherHandler, NULL, otherGuid),
        "应能写入无关 handler");

    // 容器键/根键绝不能成为递归删除目标
    CHECK_MSG(!DeleteClassesSubkeyExact(HKEY_CURRENT_USER, classesRoot, KEY_WOW64_64KEY),
        "Classes 根必须拒绝删除");
    CHECK_MSG(!DeleteClassesSubkeyExact(HKEY_CURRENT_USER, classesRoot + L"\\CLSID", KEY_WOW64_64KEY),
        "CLSID 容器必须拒绝删除");
    CHECK_MSG(!DeleteClassesSubkeyExact(HKEY_CURRENT_USER, handlerParent, KEY_WOW64_64KEY),
        "ContextMenuHandlers 容器必须拒绝删除");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, classesRoot), "拒绝后 Classes 根必须仍在");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, handlerParent), "拒绝后 handler 容器必须仍在");

    std::set<std::wstring> removed;
    CleanShellExtensionsAtRoot(HKEY_CURRENT_USER, classesRoot, installDir, &removed);
    CHECK_MSG(removed.size() == 2 && removed.count(fakeGuid) && removed.count(localGuid),
        "只应识别指向安装目录的 CLSID");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, clsIn), "孤儿 InprocServer32 CLSID 必须删除");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, clsLocal), "孤儿 LocalServer32 CLSID 必须删除");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, handlerKey), "引用孤儿 CLSID 的右键 handler 必须删除");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, clsOut), "其它目录的 CLSID 不得误删");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, otherHandler), "无关 handler 不得误删");

    // App Paths：默认值匹配 + Path 值匹配 + 无关项保留
    std::wstring appPathsRoot = g_regRoot + L"\\App Paths";
    std::wstring appByName = appPathsRoot + L"\\7zFM.exe";
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, appByName, NULL, installDir + L"\\7zFM.exe"),
        "应能写入 App Paths 默认值");
    std::wstring appByPath = appPathsRoot + L"\\7zG.exe";
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, appByPath, NULL, L"C:\\Windows\\notepad.exe"),
        "应能写入无关默认值");
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, appByPath, L"Path", installDir),
        "应能写入指向安装目录的 Path 值");
    std::wstring appOther = appPathsRoot + L"\\notepad.exe";
    CHECK_MSG(RegSetString(HKEY_CURRENT_USER, appOther, NULL, L"C:\\Windows\\notepad.exe"),
        "应能写入无关 App Paths 项");

    CleanAppPathsForInstallDir(HKEY_CURRENT_USER, appPathsRoot, installDir);
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, appByName), "默认值指向安装目录的 App Paths 必须删除");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, appByPath), "Path 值指向安装目录的 App Paths 必须删除");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, appOther), "无关 App Paths 不得误删");

    RemoveTree(TempPath(L"ext"));
}

// ====================== 7.2 被占用文件的重启删除 ======================

static void Test_UninstallLockedFile() {
    GROUP("FUNC-UNINSTALLOCK 占用文件延迟删除");

    std::wstring dir = TempPath(L"locked");
    MakeDir(dir);
    std::wstring file = dir + L"\\locked.dll";
    WriteBytes(file, "MZ", 2);

    // share=0：模拟 explorer.exe 映射中的 shell 扩展 DLL，运行期不可删除
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK_MSG(h != INVALID_HANDLE_VALUE, "应能独占打开测试文件");
    if (h == INVALID_HANDLE_VALUE) return;

    ResetUninstallPendingReboot();
    CHECK_MSG(GetUninstallPendingRebootCount() == 0, "重置后等待重启计数应为 0");

    bool deleted = DeleteDirectoryRecursive(dir);
    int pending = GetUninstallPendingRebootCount();
    if (IsRunAsAdmin()) {
        // 管理员才能写 PendingFileRenameOperations：占用文件应转为「等待重启」
        CHECK_MSG(deleted, "管理员下被占用文件应登记重启删除并返回成功");
        CHECK_MSG(pending > 0, "必须记录等待重启的文件/目录");
    }
    else {
        CHECK_MSG(!deleted, "无法登记重启删除时应如实返回失败，不得虚报成功");
        CHECK_MSG(pending == 0, "登记失败时等待重启计数必须为 0");
    }

    CloseHandle(h);
    RemoveTree(dir);
    ResetUninstallPendingReboot();
}

// ====================== 7.5 卸载模块真实扫描（只读） ======================

static void Test_UninstallScan() {
    GROUP("FUNC-UNINSTALLSCAN 已安装软件/驱动扫描");
    g_uninstallScanCancelled = false;
    ScanInstalledApps();
    size_t appCount = g_uninstallItems.size();
    printf("  (info) 软件条目: %zu\n", appCount);
    CHECK_MSG(appCount > 0, "应扫描到已安装软件");

    bool nameOk = true;
    bool viewOk = true;
    for (size_t i = 0; i < appCount; i++) {
        if (g_uninstallItems[i].displayName.empty()) nameOk = false;
        // 卸载项的注册表视图必须已确定，否则删除时可能操作错误的视图
        if (g_uninstallItems[i].regHive == NULL) viewOk = false;
    }
    CHECK_MSG(nameOk, "软件条目不得有空显示名");
    CHECK_MSG(viewOk, "软件条目必须带注册表根键");

    ScanInstalledDrivers();
    size_t total = g_uninstallItems.size();
    size_t driverCount = total - appCount;
    printf("  (info) 驱动条目: %zu\n", driverCount);
    CHECK_MSG(driverCount > 0, "应扫描到自启动/按需启动的内核驱动");

    bool driverOk = true;
    bool onlyDrivers = true;
    size_t systemDrivers = 0;
    for (size_t i = appCount; i < total; i++) {
        const UninstallItem& it = g_uninstallItems[i];
        if (it.type != L"驱动") driverOk = false;
        if (it.imagePath.empty()) driverOk = false;
        // ImagePath 是文件路径，不能塞进安装目录，否则强删会递归删除错误的“目录”
        if (!it.installLocation.empty()) driverOk = false;
        if (it.isSystemComponent) systemDrivers++;

        HKEY hSvc = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, it.regKeyPath.c_str(), 0, KEY_READ, &hSvc) == ERROR_SUCCESS) {
            DWORD svcType = 0;
            DWORD svcSize = sizeof(svcType);
            if (RegQueryValueExW(hSvc, L"Type", NULL, NULL, (LPBYTE)&svcType, &svcSize) == ERROR_SUCCESS &&
                (svcType & SERVICE_DRIVER) == 0) {
                onlyDrivers = false;
            }
            RegCloseKey(hSvc);
        }
    }
    CHECK_MSG(onlyDrivers, "驱动列表不得混入用户态服务（必须校验 SERVICE_DRIVER 类型）");
    if (!driverOk) {
        int shown = 0;
        for (size_t i = appCount; i < total && shown < 12; i++) {
            const UninstallItem& it = g_uninstallItems[i];
            if (it.type == L"驱动" && !it.imagePath.empty() && it.installLocation.empty())
                continue;
            printf("  [debug] 异常驱动项 key=%s image=[%s] install=[%s] type=%s\n",
                ToUtf8(it.regKeyPath).c_str(), ToUtf8(it.imagePath).c_str(),
                ToUtf8(it.installLocation).c_str(), ToUtf8(it.type).c_str());
            shown++;
        }
    }
    CHECK_MSG(driverOk, "驱动条目必须带 ImagePath 且 installLocation 为空");
    CHECK_MSG(systemDrivers > 0, "Windows 目录内的驱动应被标记为系统组件");

    // 清空，避免影响后续用例对卸载列表的假设
    g_uninstallItems.clear();
}

// ====================== 8. 实用工具 ======================

static void Test_ToolsSystemInfo() {
    GROUP("FUNC-TOOL 系统信息");
    SetToolColumns({ { L"项目", 200 }, { L"数值", 500 } });
    SwitchToolFunction(1);

    int rows = ListView_GetItemCount(g_hToolListView);
    printf("  (info) 系统信息条目: %d\n", rows);
    CHECK_MSG(rows >= 10, "系统信息应至少有 10 项");

    auto findRow = [&](const wchar_t* name) -> int {
        for (int i = 0; i < rows; i++)
            if (ListText(i, 0) == name) return i;
        return -1;
    };
    int cpu = findRow(L"CPU型号");
    int mem = findRow(L"总内存");
    int os = findRow(L"操作系统");
    if (cpu < 0) {
        wchar_t probe[256] = { 0 };
        DWORD probeSize = (DWORD)sizeof(probe);
        LONG probeRet = RegGetValueW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString", RRF_RT_REG_SZ, NULL, probe, &probeSize);
        printf("  [debug] CPU RegGetValue ret=%ld size=%lu val=%s\n",
            probeRet, probeSize, ToUtf8(probe).c_str());
        printf("  [debug] 当前条目：\n");
        for (int i = 0; i < rows; i++) {
            printf("    row %d: [%s] = [%s]\n", i,
                ToUtf8(ListText(i, 0)).c_str(), ToUtf8(ListText(i, 1)).c_str());
        }
    }
    CHECK_MSG(cpu >= 0, "应包含 CPU型号");
    CHECK_MSG(mem >= 0, "应包含 总内存");
    CHECK_MSG(os >= 0, "应包含 操作系统");
    if (cpu >= 0) CHECK_MSG(!ListText(cpu, 1).empty(), "CPU型号不能为空");

    // RegGetValue 的类型过滤参数曾误传 REG_SZ，导致这些行全部静默缺失
    int vendor = findRow(L"CPU厂商");
    int freq = findRow(L"CPU频率");
    int cores = findRow(L"核心数");
    int threads = findRow(L"线程数");
    CHECK_MSG(vendor >= 0 && !ListText(vendor, 1).empty(), "应包含非空的 CPU厂商");
    CHECK_MSG(freq >= 0 && ListText(freq, 1).find(L"GHz") != std::wstring::npos,
        "CPU频率应带 GHz 单位");
    CHECK_MSG(cores >= 0 && threads >= 0, "应包含核心数/线程数");
    if (cores >= 0 && threads >= 0) {
        int coreVal = _wtoi(ListText(cores, 1).c_str());
        int threadVal = _wtoi(ListText(threads, 1).c_str());
        CHECK_MSG(coreVal > 0 && coreVal <= threadVal, "物理核心数应在 1..线程数 之间");
    }
    if (mem >= 0) CHECK_MSG(ListText(mem, 1).find(L"MB") != std::wstring::npos,
        "总内存应带单位");
    if (os >= 0) CHECK_MSG(!ListText(os, 1).empty(), "操作系统不能为空");

    // 刷新不应重复累加条目
    int before = ListView_GetItemCount(g_hToolListView);
    // 通过 SwitchToolFunction(1) 再次进入（不经过 UI 的完整流程）
    SwitchToolFunction(1);
    CHECK_MSG(ListView_GetItemCount(g_hToolListView) == before,
        "重复刷新系统信息不得让条目成倍增加");
}

static void Test_ToolsDuplicate() {
    GROUP("FUNC-TOOL 重复文件扫描/删除");
    std::wstring dir = TempPath(L"dup");
    MakeDir(dir);
    MakeDir(dir + L"\\sub");

    std::vector<BYTE> same(4096);
    for (size_t i = 0; i < same.size(); i++) same[i] = (BYTE)(i * 31 + 7);
    std::vector<BYTE> different = same;
    different[2048] ^= 0xFF; // 同大小、首尾相同、中间不同

    WriteBytes(dir + L"\\a.bin", same.data(), (DWORD)same.size());
    WriteBytes(dir + L"\\sub\\b.bin", same.data(), (DWORD)same.size());
    WriteBytes(dir + L"\\c.bin", different.data(), (DWORD)different.size());

    CHECK_MSG(ComputeFileDigest(dir + L"\\a.bin") == ComputeFileDigest(dir + L"\\sub\\b.bin"),
        "相同内容应得到相同摘要");
    CHECK_MSG(ComputeFileDigest(dir + L"\\a.bin") != ComputeFileDigest(dir + L"\\c.bin"),
        "中间字节不同必须得到不同摘要（全量摘要，不能只采样首尾）");

    SetToolColumns({ { L"文件名", 200 }, { L"大小", 100 }, { L"路径", 400 } });
    SetWindowTextW(g_hToolPathEdit, dir.c_str());
    g_isDupScanning = false;
    StartDuplicateScan();
    DWORD deadline = GetTickCount() + 15000;
    while (g_isDupScanning && GetTickCount() < deadline) PumpFor(20);
    CHECK_MSG(!g_isDupScanning, "重复文件扫描应在超时前完成");

    int rows = ListView_GetItemCount(g_hToolListView);
    CHECK_MSG(rows == 2, "只有 a.bin 与 b.bin 应被列为重复文件（c.bin 内容不同）");
    if (rows == 2) {
        CHECK(ListText(0, 0).find(L".bin") != std::wstring::npos);
        CHECK(ListText(1, 0).find(L".bin") != std::wstring::npos);
        CHECK(ListText(0, 2).find(dir) == 0);
    }

    if (rows >= 1) {
        SelectToolRow(0);
        g_mbResult = IDYES;
        CleanSelectedDuplicates();
        g_mbResult = -1;
        CHECK_MSG(ListView_GetItemCount(g_hToolListView) == 1, "删除成功后列表应只余一项");
        int remain = 0;
        if (PathFileExistsW((dir + L"\\a.bin").c_str())) remain++;
        if (PathFileExistsW((dir + L"\\sub\\b.bin").c_str())) remain++;
        CHECK_MSG(remain == 1, "两个重复副本应恰好保留一个");
        CHECK_MSG(PathFileExistsW((dir + L"\\c.bin").c_str()), "非重复文件不得被删除");
    }
}

static void Test_ToolsProcess() {
    GROUP("FUNC-TOOL 进程刷新/结束");
    std::wstring cmd = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };
    BOOL created = CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
    CHECK_MSG(created != FALSE, "应能启动测试进程");
    if (!created) return;
    CloseHandle(pi.hThread);

    Sleep(300);
    SetToolColumns({ { L"进程名", 180 }, { L"PID", 80 }, { L"内存(MB)", 100 }, { L"路径", 320 } });
    RefreshProcessList();
    int rows = ListView_GetItemCount(g_hToolListView);
    CHECK_MSG(rows > 0, "进程列表不应为空");

    std::wstring pidText = std::to_wstring(pi.dwProcessId);
    int targetRow = -1;
    for (int i = 0; i < rows; i++) {
        if (ListText(i, 1) == pidText) { targetRow = i; break; }
    }
    CHECK_MSG(targetRow >= 0, "进程列表应包含刚启动的测试进程 PID");

    if (targetRow >= 0) {
        SelectToolRow(targetRow);
        g_mbResult = IDYES;
        KillSelectedProcess();
        g_mbResult = -1;
        DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
        CHECK_MSG(wait == WAIT_OBJECT_0, "被结束的测试进程应在 5 秒内退出");
        DWORD code = 0;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        CHECK_MSG(code != STILL_ACTIVE, "进程退出码不应仍是 STILL_ACTIVE");
    }

    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hProcess);
}

static bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target) {
    IShellLinkW* link = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) return false;
    link->SetPath(target.c_str());
    IPersistFile* file = NULL;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    bool ok = false;
    if (SUCCEEDED(hr) && file) {
        ok = SUCCEEDED(file->Save(lnkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

static void Test_ToolsShortcut() {
    GROUP("FUNC-TOOL 快捷方式扫描/修复");
    std::wstring dir = TempPath(L"lnk");
    MakeDir(dir);

    std::wstring validLnk = dir + L"\\valid.lnk";
    std::wstring brokenLnk = dir + L"\\broken.lnk";
    std::wstring winDir = GetWindowsDirectorySafe();
    bool createdValid = CreateShortcut(validLnk, winDir + L"\\explorer.exe");
    bool createdBroken = CreateShortcut(brokenLnk, dir + L"\\missing_target.exe");
    CHECK_MSG(createdValid, "应能创建有效快捷方式");
    CHECK_MSG(createdBroken, "应能创建失效快捷方式");
    if (!createdValid || !createdBroken) return;

    SetToolColumns({ { L"快捷方式", 200 }, { L"指向目标", 400 }, { L"状态", 100 } });
    SetWindowTextW(g_hToolPathEdit, dir.c_str());
    StartShortcutScan();

    int rows = ListView_GetItemCount(g_hToolListView);
    CHECK_MSG(rows == 1, "只应扫描到 1 个失效快捷方式");
    if (rows >= 1) {
        CHECK_MSG(ListText(0, 0) == L"broken", "失效项应有正确的显示名");
        CHECK_MSG(ListText(0, 1).find(L"missing_target.exe") != std::wstring::npos,
            "应展示失效目标路径");
        CHECK_MSG(ListText(0, 2) == L"失效", "状态列应为「失效」");

        SelectToolRow(0);
        g_mbResult = IDYES;
        FixSelectedShortcuts();
        g_mbResult = -1;
        CHECK_MSG(!PathFileExistsW(brokenLnk.c_str()), "选中的失效快捷方式应被删除");
        CHECK_MSG(PathFileExistsW(validLnk.c_str()), "有效快捷方式不得被删除");
    }
}

static void Test_ToolsDiskCleanup() {
    GROUP("FUNC-TOOL 磁盘清理（老文件）");
    std::wstring dir = TempPath(L"disk");
    MakeDir(dir);
    std::wstring oldFile = dir + L"\\old.log";
    std::wstring newFile = dir + L"\\new.log";
    WriteBytes(oldFile, "old", 3);
    WriteBytes(newFile, "new", 3);
    CHECK_MSG(SetFileAgeDays(oldFile, 10), "应能把测试文件改为 10 天前");

    SetToolColumns({ { L"文件名", 250 }, { L"大小", 100 }, { L"修改时间", 150 }, { L"路径", 200 } });
    SetWindowTextW(g_hToolPathEdit, dir.c_str());
    SendMessageW(g_hToolAgeCombo, CB_SETCURSEL, 0, 0); // 7 天前
    g_mbResult = -1;
    StartDiskCleanupScan();

    int rows = ListView_GetItemCount(g_hToolListView);
    CHECK_MSG(rows == 1, "只应扫描到 10 天前的 old.log");
    if (rows >= 1) {
        CHECK_MSG(ListText(0, 0) == L"old.log", "结果应指向 old.log");

        SelectToolRow(0);
        g_mbResult = IDYES;
        CleanSelectedDiskFiles();
        g_mbResult = -1;
        CHECK_MSG(!PathFileExistsW(oldFile.c_str()), "选中的旧文件应被删除");
        CHECK_MSG(PathFileExistsW(newFile.c_str()), "新文件不得被删除");
    }
}

// ====================== 清理与收尾 ======================

static void CleanupAll() {
    for (const auto& name : g_approvedTestValues) {
        RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedRun, name);
        RegDeleteValueIfExists(HKEY_CURRENT_USER, kApprovedFolder, name);
    }
    if (!g_regRoot.empty()) {
        RegDeleteTreeW(HKEY_CURRENT_USER, g_regRoot.c_str());
        std::wstring backupPath = std::wstring(L"Software\\SystemCleaner\\DisabledStartup\\HKCU\\") + g_regRoot;
        RegDeleteTreeW(HKEY_CURRENT_USER, backupPath.c_str());
        std::wstring ctxClass = L"Software\\Classes\\SCFuncTestCtx_" +
            std::to_wstring(GetCurrentProcessId());
        RegDeleteTreeW(HKEY_CURRENT_USER, ctxClass.c_str());
    }
    // 测试宿主窗口控件随窗口销毁
    if (g_host) {
        g_hToolListView = NULL;
        g_hToolPathEdit = NULL;
        g_hToolAgeCombo = NULL;
        g_hToolInfoLabel = NULL;
        g_hMainWnd = NULL;
        DestroyWindow(g_host);
        g_host = NULL;
    }
    if (!g_tempRoot.empty()) {
        RemoveTree(g_tempRoot);
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("SystemCleaner functional tests\n");

    InitializeCriticalSection(&g_uninstallItemsLock);
    INITCOMMONCONTROLSEX icex = { sizeof(icex) };
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    InitTempRoot();
    g_regRoot = L"Software\\SystemCleanerFuncTest_" + std::to_wstring(GetCurrentProcessId());
    RegDeleteTreeW(HKEY_CURRENT_USER, g_regRoot.c_str());
    CreateFakeUi();

    Test_CleanerScanAndClean();
    Test_CleanerDepthLimit();
    Test_FindMatchingSubDirs();
    Test_RegistryReadLong();
    Test_RegistryDeleteGuard();
    Test_RegistryBackupFile();
    Test_ShredFile();
    Test_StartupLongValues();
    Test_StartupTargetExists();
    Test_StartupLegacy();
    Test_StartupRunOnceBackup();
    Test_StartupApprovedPlainRun();
    Test_StartupApprovedIsolation();
    Test_StartupLegacyCrossEffect();
    Test_StartupFolder();
    Test_ContextMenu();
    Test_HostsFile();
    Test_UninstallPredicates();
    Test_UninstallShellExtCleanup();
    Test_UninstallLockedFile();
    Test_UninstallScan();
    Test_ToolsSystemInfo();
    Test_ToolsDuplicate();
    Test_ToolsProcess();
    Test_ToolsShortcut();
    Test_ToolsDiskCleanup();

    CleanupAll();
    if (SUCCEEDED(hrCo)) CoUninitialize();

    printf("\n=====================================\n");
    printf("Total: %d   Failed: %d\n", g_total, g_failed);
    printf("=====================================\n");
    return g_failed == 0 ? 0 : 1;
}
