// SystemCleaner 单元测试
// 直接包含被测源码，便于测试内部逻辑。
// 编译：见 build_tests.cmd

#include "../SystemCleaner/utils.cpp"

#include <cstdio>
#include <shellapi.h>

// ============================ 测试框架 ============================
static int g_total = 0;
static int g_failed = 0;
static const char* g_group = "";

#define CHECK(expr) \
    do { \
        ++g_total; \
        bool _ok = (expr); \
        if (!_ok) { \
            ++g_failed; \
            printf("  [FAIL] %s  (line %d): %s\n", g_group, __LINE__, #expr); \
        } \
    } while (0)

#define CHECK_EQ_STR(actual, expected) \
    do { \
        ++g_total; \
        std::wstring _a = (actual); \
        std::wstring _e = (expected); \
        if (_a != _e) { \
            ++g_failed; \
            char* _ac = WideCharToMultiByteBuf(_a).data(); \
            char* _ec = WideCharToMultiByteBuf(_e).data(); \
            printf("  [FAIL] %s  (line %d): got \"%s\" expected \"%s\"\n", g_group, __LINE__, _ac, _ec); \
        } \
    } while (0)

#define CHECK_EQ_LL(actual, expected) \
    do { \
        ++g_total; \
        long long _a = (long long)(actual); \
        long long _e = (long long)(expected); \
        if (_a != _e) { \
            ++g_failed; \
            printf("  [FAIL] %s  (line %d): got %lld expected %lld\n", g_group, __LINE__, _a, _e); \
        } \
    } while (0)

#define GROUP(name) g_group = name; printf("\n== %s ==\n", name)

static std::vector<char> WideCharToMultiByteBuf(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::vector<char> buf(n > 0 ? n : 1, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf.data(), n, NULL, NULL);
    return buf;
}

// 创建临时目录
static std::wstring MakeTempDir(const wchar_t* name) {
    wchar_t base[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"sc_tests_" + name;
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static void RemoveTree(const std::wstring& dir) {
    std::wstring cmd = L"cmd /c rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring mutableCmd = cmd;
    if (CreateProcessW(NULL, mutableCmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void WriteFileWithContent(const std::wstring& path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
}

// ============================ 用例 ============================

static void Test_SystemCriticalPath() {
    GROUP("SEC 系统关键路径保护");
    // SEC-02 精确匹配
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32"));
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32\\drivers\\etc"));
    // SEC-01 关键：hosts 文件位于 System32 下，必须被保护
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32\\drivers\\etc\\hosts"));
    // SEC-03 可清理目录不应被保护
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\Temp"));
    CHECK(!IsSystemCriticalPath(L"C:\\Users\\foo\\AppData\\Local\\Temp"));
    // SEC-05 前缀相似但不同的目录不应被误伤
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\System323\\x"));
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\TempFiles"));

    // SEC-04 关键：必须与系统实际所在盘符无关（用真实 SystemRoot 验证）
    wchar_t sysRoot[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(sysRoot, MAX_PATH);
    std::wstring sys32 = std::wstring(sysRoot) + L"\\System32";
    CHECK(IsSystemCriticalPath(sys32));
    CHECK(IsSystemCriticalPath(sys32 + L"\\kernel32.dll"));

    // SEC-06 短文件名（8.3）形式同样必须被识别
    wchar_t shortDir[MAX_PATH] = { 0 };
    DWORD rc = GetShortPathNameW(sys32.c_str(), shortDir, MAX_PATH);
    if (rc > 0 && rc < MAX_PATH) {
        std::wstring sp(shortDir);
        if (sp != sys32) {
            printf("  (short path = %s)\n", WideCharToMultiByteBuf(sp).data());
            CHECK(IsSystemCriticalPath(sp));
            CHECK(IsSystemCriticalPath(sp + L"\\kernel32.dll"));
        }
    }
}

static void Test_PathTraversal() {
    GROUP("SEC 路径遍历");
    CHECK(IsPathTraversal(L"C:\\a\\..\\..\\Windows"));
    CHECK(IsPathTraversal(L"..\\foo"));
    CHECK(IsPathTraversal(L"C:\\a\\.."));
    CHECK(!IsPathTraversal(L"C:\\a\\b..c"));
    CHECK(!IsPathTraversal(L"C:\\a\\b.txt"));
    CHECK(!IsPathTraversal(L"C:\\..foo\\bar"));
}

static void Test_ProtectedDataFile() {
    GROUP("PROT 受保护数据文件");
    CHECK(IsProtectedDataFile(L"login.db"));
    CHECK(IsProtectedDataFile(L"config.json"));
    CHECK(IsProtectedDataFile(L"site.dat"));
    CHECK(IsProtectedDataFile(L"Book1.IDB"));   // 大小写不敏感
    CHECK(!IsProtectedDataFile(L"thumbcache_abc.db")); // 缩略图缓存必须可清理
    CHECK(!IsProtectedDataFile(L"a.log"));
    CHECK(!IsProtectedDataFile(L"noext"));
}

static void Test_FormatSize() {
    GROUP("NUM 大小格式化");
    CHECK_EQ_STR(FormatSize(0), L"0 B");
    CHECK_EQ_STR(FormatSize(512), L"512 B");
    CHECK_EQ_STR(FormatSize(1024), L"1.00 KB");
    CHECK_EQ_STR(FormatSize(1024LL * 1024), L"1.00 MB");
    CHECK_EQ_STR(FormatSize(1024LL * 1024 * 1024), L"1.00 GB");
    // NUM-07 负数不应产生误导性的负值显示
    std::wstring neg = FormatSize(-1);
    CHECK(neg.find(L'-') == std::wstring::npos);
}

static void Test_FormatSpeed() {
    GROUP("NUM 速率格式化");
    CHECK_EQ_STR(FormatSpeed(0), L"0.00 KB/s");
    CHECK(FormatSpeed(1024).find(L"KB/s") != std::wstring::npos);
}

static void Test_EndsWith() {
    GROUP("UTIL EndsWith");
    CHECK(EndsWith(L"foo_disabled", L"_disabled"));
    CHECK(!EndsWith(L"disabled", L"_disabled"));
    CHECK(!EndsWith(L"dis", L"_disabled"));
    CHECK(EndsWith(L"a.txt", L".txt"));
}

static void Test_SubDirPattern() {
    GROUP("GLOB 子目录通配");
    CHECK(MatchSubDirPattern(L"Default\\Cache", L"*\\Cache"));
    CHECK(MatchSubDirPattern(L"Profile 1\\Code Cache", L"*\\Code Cache"));
    CHECK(MatchSubDirPattern(L"A\\B\\Cache", L"*\\Cache"));
    CHECK(!MatchSubDirPattern(L"Cache", L"*\\Cache"));
    CHECK(!MatchSubDirPattern(L"Default\\CacheX", L"*\\Cache"));
    CHECK(ParseSubDirPattern(L"a\\b/c").size() == 3);
    CHECK(ParseSubDirPattern(L"a\\\\b").size() == 2);
}

// NUM-01/03 关键：从 WIN32_FIND_DATA 还原 64 位文件大小
static void Test_FindDataSize() {
    GROUP("NUM 大文件大小还原");
    WIN32_FIND_DATAW fd = {};
    fd.nFileSizeHigh = 0; fd.nFileSizeLow = 1000;
    CHECK_EQ_LL(FileSizeFromFindData(&fd), 1000LL);

    fd.nFileSizeHigh = 1; fd.nFileSizeLow = 0;
    CHECK_EQ_LL(FileSizeFromFindData(&fd), 4294967296LL); // 4 GiB

    fd.nFileSizeHigh = 3; fd.nFileSizeLow = 7;
    CHECK_EQ_LL(FileSizeFromFindData(&fd), 3LL * 4294967296LL + 7LL);

    fd.nFileSizeHigh = 0xFFFFFFFF; fd.nFileSizeLow = 0xFFFFFFFF;
    CHECK_EQ_LL(FileSizeFromFindData(&fd), 18446744073709551615ULL);
}

// RO-01/RO-02 关键：只读文件不等于被占用，必须能清理
static void Test_ReadOnlyFile() {
    GROUP("RO 只读文件处理");
    std::wstring dir = MakeTempDir(L"ro");
    std::wstring f = dir + L"\\readonly_cache.tmp";
    WriteFileWithContent(f, "hello", 5);
    SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_READONLY);

    // 只读文件没有被任何进程持有，不应判定为"被占用"
    CHECK(!IsFileLocked(f));

    // 清理流程必须能删除只读文件
    CHECK(ForceDeleteFile(f));
    CHECK(GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES);

    // 真正被占用的文件必须判定为锁定
    std::wstring g = dir + L"\\locked.tmp";
    WriteFileWithContent(g, "hello", 5);
    HANDLE h = CreateFileW(g.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(IsFileLocked(g));
        CloseHandle(h);
    }

    RemoveTree(dir);
}

// LIST 文件枚举
static void Test_ListFiles() {
    GROUP("LIST 目录枚举与保护");
    std::wstring dir = MakeTempDir(L"list");
    WriteFileWithContent(dir + L"\\a.log", "x", 1);
    WriteFileWithContent(dir + L"\\b.db", "yy", 2);
    WriteFileWithContent(dir + L"\\c.tmp", "zzz", 3);

    std::vector<FileInfo> files;
    ListFilesInDirectory(dir, L"*.*", false, files);
    CHECK(files.size() == 3);

    files.clear();
    ListFilesInDirectory(dir, L"*.*", true, files);   // protectData=true 时 .db 被保护
    CHECK(files.size() == 2);

    files.clear();
    ListFilesInDirectory(dir, L"*.tmp", false, files);
    CHECK(files.size() == 1);
    if (files.size() == 1) CHECK_EQ_LL(files[0].size, 3);

    // 不存在的目录不应崩溃
    files.clear();
    ListFilesInDirectory(dir + L"\\__nope__", L"*.*", false, files);
    CHECK(files.empty());

    RemoveTree(dir);
}

// BUF-01 关键：长名称拼接不得触发 invalid parameter handler（进程终止）
static void Test_SafeAppend() {
    GROUP("BUF 安全字符串拼接");
    wchar_t buf[64] = L"";
    SafeAppend(buf, 64, L"abc");
    CHECK_EQ_STR(buf, L"abc");
    // 超长输入必须静默截断而不是终止进程
    std::wstring huge(500, L'X');
    SafeAppend(buf, 64, huge.c_str());
    CHECK(wcslen(buf) < 64);
    SafeAppend(buf, 64, L"");
    CHECK(wcslen(buf) < 64);
}

// RES 残留目录名合法性校验
static void Test_ResidualNameValidation() {
    GROUP("SEC 残留目录名校验");
    CHECK(IsSafeResidualName(L"Google"));
    CHECK(IsSafeResidualName(L"Microsoft Edge"));
    CHECK(!IsSafeResidualName(L""));
    CHECK(!IsSafeResidualName(L"."));
    CHECK(!IsSafeResidualName(L".."));
    CHECK(!IsSafeResidualName(L"..\\..\\Windows"));
    CHECK(!IsSafeResidualName(L"a\\b"));
    CHECK(!IsSafeResidualName(L"a/b"));
    CHECK(!IsSafeResidualName(L"*"));
    CHECK(!IsSafeResidualName(L"C:"));
    CHECK(!IsSafeResidualName(L" "));
}

// MEM-03 关键：短路径不得导致 substr 下溢抛异常
static void Test_ShortPathSafety() {
    GROUP("MEM 短路径安全");
    // ".disabled" 长度 9，短于该长度的路径必须安全返回而不是抛 std::out_of_range
    StartupItem item;
    item.isRegistry = false;
    item.isEnabled = false;
    item.realName = L"x.disabled";
    item.path = L"a.disabled";  // 长度 10，去掉 9 个字符后剩 "a"
    bool threw = false;
    try {
        EnableStartupItem_PathCheck(item);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);

    item.path = L".disabled"; // 长度正好 9
    threw = false;
    try {
        EnableStartupItem_PathCheck(item);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
}

// HASH 重复文件判定强度
static void Test_DuplicateHashStrength() {
    GROUP("HASH 重复文件判定");
    std::wstring dir = MakeTempDir(L"hash");
    const DWORD n = 256 * 1024;
    std::vector<BYTE> a(n), b(n);
    for (DWORD i = 0; i < n; i++) { a[i] = (BYTE)(i & 0xFF); b[i] = (BYTE)(i & 0xFF); }
    // 首尾 4 字节完全相同，中间内容完全不同 —— 绝不能判为重复
    b[n / 2] ^= 0xFF;
    b[n / 2 + 1] ^= 0xFF;

    std::wstring fa = dir + L"\\a.bin", fb = dir + L"\\b.bin";
    WriteFileWithContent(fa, a.data(), n);
    WriteFileWithContent(fb, b.data(), n);

    std::wstring ha = ComputeFileDigest(fa);
    std::wstring hb = ComputeFileDigest(fb);
    CHECK(!ha.empty());
    CHECK(!hb.empty());
    CHECK(ha != hb);

    // 内容完全相同的两个文件必须判为重复
    std::wstring fc = dir + L"\\c.bin";
    WriteFileWithContent(fc, a.data(), n);
    CHECK(ComputeFileDigest(fc) == ha);

    // 同大小但首字节不同
    std::vector<BYTE> d = a; d[0] ^= 0xFF;
    std::wstring fd2 = dir + L"\\d.bin";
    WriteFileWithContent(fd2, d.data(), n);
    CHECK(ComputeFileDigest(fd2) != ha);

    RemoveTree(dir);
}

// RECURSE 深度限制
static void Test_RecursionDepth() {
    GROUP("RECURSE 递归深度上限");
    // 构造 60 层深目录，带深度上限的实现不应崩溃且不应无限下钻
    std::wstring dir = MakeTempDir(L"deep");
    std::wstring cur = dir;
    for (int i = 0; i < 60; i++) {
        cur += L"\\d";
        CreateDirectoryW(cur.c_str(), NULL);
    }
    std::vector<FileInfo> out;
    ListFilesRecursiveCapped(cur, L"*.*", 32, out);
    CHECK(true); // 未崩溃即通过
    RemoveTree(dir);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("SystemCleaner unit tests\n");

    Test_SystemCriticalPath();
    Test_PathTraversal();
    Test_ProtectedDataFile();
    Test_FormatSize();
    Test_FormatSpeed();
    Test_EndsWith();
    Test_SubDirPattern();
    Test_FindDataSize();
    Test_ReadOnlyFile();
    Test_ListFiles();
    Test_SafeAppend();
    Test_ResidualNameValidation();
    Test_ShortPathSafety();
    Test_DuplicateHashStrength();
    Test_RecursionDepth();

    printf("\n=====================================\n");
    printf("Total: %d   Failed: %d\n", g_total, g_failed);
    printf("=====================================\n");
    return g_failed == 0 ? 0 : 1;
}
