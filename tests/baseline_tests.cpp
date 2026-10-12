// SystemCleaner 基线测试 —— 只调用现有函数，用于暴露当前缺陷
#include "../SystemCleaner/utils.cpp"

#include <cstdio>
#include <shellapi.h>

// SafeCopy / SafeAppend 在 utils.cpp 中定义但未在 common.h 声明，测试侧显式声明
void SafeAppend(wchar_t* buffer, size_t bufferCount, const wchar_t* text);
void SafeCopy(wchar_t* buffer, size_t bufferCount, const wchar_t* text);

// 确定性 PRNG（xorshift64*）：跨进程/跨平台可复现，避免 rand() 实现差异
// 导致"内容不同则摘要不同"这类断言随机失败
static unsigned long long g_rngState = 0x9E3779B97F4A7C15ULL;
static unsigned int NextRand() {
    g_rngState ^= g_rngState >> 12;
    g_rngState ^= g_rngState << 25;
    g_rngState ^= g_rngState >> 27;
    return (unsigned int)((g_rngState * 0x2545F4914F6CDD1DULL) >> 40);
}
static void FillRandom(void* dst, size_t len) {
    unsigned char* p = (unsigned char*)dst;
    for (size_t i = 0; i < len; i++) p[i] = (unsigned char)(NextRand() & 0xFF);
}

static int g_total = 0;
static int g_failed = 0;
static const char* g_group = "";

#define CHECK(expr) \
    do { ++g_total; if (!(expr)) { ++g_failed; \
        printf("  [FAIL] %s (line %d): %s\n", g_group, __LINE__, #expr); } } while (0)

#define CHECK_EQ_STR(actual, expected) \
    do { ++g_total; std::wstring _a = (actual); std::wstring _e = (expected); \
        if (_a != _e) { ++g_failed; \
            int _n = WideCharToMultiByte(CP_UTF8,0,_a.c_str(),-1,NULL,0,NULL,NULL); \
            std::vector<char> _b(_n>0?_n:1,0); \
            if(_n>0) WideCharToMultiByte(CP_UTF8,0,_a.c_str(),-1,_b.data(),_n,NULL,NULL); \
            printf("  [FAIL] %s (line %d): got \"%s\" expected \"%s\"\n", g_group, __LINE__, _b.data(), #expected); \
        } } while (0)

#define GROUP(name) g_group = name; printf("\n== %s ==\n", name)

static std::wstring MakeTempDir(const wchar_t* name) {
    wchar_t base[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"sc_tests_" + name;
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}
static void RemoveTree(const std::wstring& dir) {
    std::wstring cmd = L"/c rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring shell = std::wstring(sysDir) + L"\\cmd.exe";
    if (CreateProcessW(shell.c_str(), cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}
static void WriteBlob(const std::wstring& path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(h, data, size, &w, NULL); CloseHandle(h);
}

// 宽字符 -> UTF-8，便于把摘要等 wstring 打印/比较
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::vector<char> b(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b.data(), n, NULL, NULL);
    return std::string(b.data());
}

static void Test_SystemCriticalPath() {
    GROUP("SEC 系统关键路径保护");
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32"));                 // SEC-02
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32\\drivers\\etc"));
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32\\drivers\\etc\\hosts")); // SEC-01
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\Temp"));                     // SEC-03
    CHECK(!IsSystemCriticalPath(L"C:\\Users\\foo\\AppData\\Local\\Temp"));
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\System323\\x"));             // SEC-05
    CHECK(!IsSystemCriticalPath(L"C:\\Windows\\TempFiles"));

    // SEC-04 必须与系统实际盘符无关
    wchar_t sysRoot[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(sysRoot, MAX_PATH);
    std::wstring sys32 = std::wstring(sysRoot) + L"\\System32";
    CHECK(IsSystemCriticalPath(sys32));
    CHECK(IsSystemCriticalPath(sys32 + L"\\kernel32.dll"));

    // SEC-06 8.3 短文件名同样必须被保护
    wchar_t shortDir[MAX_PATH] = { 0 };
    DWORD rc = GetShortPathNameW(sys32.c_str(), shortDir, MAX_PATH);
    if (rc > 0 && rc < MAX_PATH && wcscmp(shortDir, sys32.c_str()) != 0) {
        printf("  (info) short path = %ls\n", shortDir);
        CHECK(IsSystemCriticalPath(shortDir));
        CHECK(IsSystemCriticalPath(std::wstring(shortDir) + L"\\kernel32.dll"));
    }

    // 统一分隔符 / 尾部斜杠 / 大小写
    CHECK(IsSystemCriticalPath(L"c:\\windows\\system32"));
    CHECK(IsSystemCriticalPath(L"C:/Windows/System32"));
    CHECK(IsSystemCriticalPath(L"C:\\Windows\\System32\\"));
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
    CHECK(IsProtectedDataFile(L"Book1.IDB"));
    CHECK(!IsProtectedDataFile(L"thumbcache_abc.db"));
    CHECK(!IsProtectedDataFile(L"a.log"));
    CHECK(!IsProtectedDataFile(L"noext"));
}

static void Test_Format() {
    GROUP("NUM 格式化");
    CHECK_EQ_STR(FormatSize(0), L"0 B");
    CHECK_EQ_STR(FormatSize(512), L"512 B");
    CHECK_EQ_STR(FormatSize(1024), L"1.00 KB");
    CHECK_EQ_STR(FormatSize(1024LL * 1024), L"1.00 MB");
    CHECK_EQ_STR(FormatSize(1024LL * 1024 * 1024), L"1.00 GB");
    std::wstring neg = FormatSize(-1);   // NUM-07
    CHECK(neg.find(L'-') == std::wstring::npos);
    CHECK(FormatSpeed(1024).find(L"KB/s") != std::wstring::npos);
}

static void Test_EndsWith() {
    GROUP("UTIL EndsWith");
    CHECK(EndsWith(L"foo_disabled", L"_disabled"));
    CHECK(!EndsWith(L"disabled", L"_disabled"));
    CHECK(!EndsWith(L"dis", L"_disabled"));
}

static void Test_SubDirPattern() {
    GROUP("GLOB 子目录通配");
    CHECK(MatchSubDirPattern(L"Default\\Cache", L"*\\Cache"));
    CHECK(MatchSubDirPattern(L"Profile 1\\Code Cache", L"*\\Code Cache"));
    CHECK(MatchSubDirPattern(L"A\\B\\Cache", L"*\\Cache"));
    CHECK(!MatchSubDirPattern(L"Cache", L"*\\Cache"));
    CHECK(!MatchSubDirPattern(L"Default\\CacheX", L"*\\Cache"));
    CHECK(ParseSubDirPattern(L"a\\b/c").size() == 3);
}

static void Test_ReadOnlyFile() {
    GROUP("RO 只读文件处理");
    std::wstring dir = MakeTempDir(L"ro");
    std::wstring f = dir + L"\\readonly_cache.tmp";
    WriteBlob(f, "hello", 5);
    SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_READONLY);

    CHECK(!IsFileLocked(f));                 // RO-01 只读 != 被占用

    std::vector<FileInfo> files;
    ListFilesInDirectory(dir, L"*.tmp", false, files);
    CHECK(files.size() == 1);

    // RO-02 清理流程必须能删除只读文件（CleanDirectory 走 ForceDeleteFile）
    BOOL del = ForceDeleteFile(f);
    CHECK(del);
    CHECK(GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES);

    // 真被占用的文件必须判定为锁定
    std::wstring g = dir + L"\\locked.tmp";
    WriteBlob(g, "hello", 5);
    HANDLE h = CreateFileW(g.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) { CHECK(IsFileLocked(g)); CloseHandle(h); }

    RemoveTree(dir);
}

static void Test_ListFiles() {
    GROUP("LIST 目录枚举");
    std::wstring dir = MakeTempDir(L"list");
    WriteBlob(dir + L"\\a.log", "x", 1);
    WriteBlob(dir + L"\\b.db", "yy", 2);
    WriteBlob(dir + L"\\c.tmp", "zzz", 3);

    std::vector<FileInfo> files;
    ListFilesInDirectory(dir, L"*.*", false, files);
    CHECK(files.size() == 3);
    files.clear();
    ListFilesInDirectory(dir, L"*.*", true, files);
    CHECK(files.size() == 2);
    files.clear();
    ListFilesInDirectory(dir + L"\\__nope__", L"*.*", false, files);
    CHECK(files.empty());
    RemoveTree(dir);
}

// DIG —— 文件内容摘要：必须覆盖全文件，且对同内容稳定
static void Test_FileDigest() {
    GROUP("DIG 文件内容摘要");
    std::wstring dir = MakeTempDir(L"digest");

    // 构造两个大小相同、首尾相同、仅中间 1 字节不同的文件（越过 128KB 分块边界）
    const size_t N = 200 * 1024;
    std::vector<unsigned char> a(N), b(N);
    FillRandom(a.data(), N);
    b = a;
    b[150 * 1024] = (unsigned char)(b[150 * 1024] ^ 0xA5);
    WriteBlob(dir + L"\\a.bin", a.data(), (DWORD)N);
    WriteBlob(dir + L"\\b.bin", b.data(), (DWORD)N);

    std::string ha = WideToUtf8(ComputeFileDigest(dir + L"\\a.bin"));
    std::string hb = WideToUtf8(ComputeFileDigest(dir + L"\\b.bin"));
    CHECK(!ha.empty());
    CHECK(!hb.empty());
    CHECK(ha != hb);                                          // DIG-01 中间不同必须可区分

    // DIG-02 同内容两次计算必须一致（稳定）
    CHECK(ComputeFileDigest(dir + L"\\a.bin") == ComputeFileDigest(dir + L"\\a.bin"));

    // DIG-03 完全相同内容必须摘要相同
    WriteBlob(dir + L"\\c.bin", a.data(), (DWORD)N);
    CHECK(ComputeFileDigest(dir + L"\\c.bin") == ComputeFileDigest(dir + L"\\a.bin"));

    // DIG-04 仅首字节不同也必须可区分
    std::vector<unsigned char> d = a;
    d[0] = (unsigned char)(d[0] ^ 0xFF);
    WriteBlob(dir + L"\\d.bin", d.data(), (DWORD)N);
    CHECK(ComputeFileDigest(dir + L"\\d.bin") != ComputeFileDigest(dir + L"\\a.bin"));

    // DIG-05 不存在的文件返回空串，而不是崩溃
    CHECK(ComputeFileDigest(dir + L"\\__nope__.bin").empty());

    // DIG-06 空文件可计算且不崩溃
    WriteBlob(dir + L"\\empty.bin", "", 0);
    CHECK(!ComputeFileDigest(dir + L"\\empty.bin").empty());

    RemoveTree(dir);
}

// RDEPTH —— 递归枚举必须受深度上限与 reparse 保护
static void Test_RecursiveCapped() {
    GROUP("RDEPTH 递归枚举深度上限");
    std::wstring dir = MakeTempDir(L"depth");

    std::wstring cur = dir;
    for (int i = 0; i < 6; i++) {
        cur += L"\\d" + std::to_wstring(i);
        CreateDirectoryW(cur.c_str(), NULL);
        WriteBlob(cur + L"\\f.txt", "x", 1);
    }

    std::vector<FileInfo> files;
    ListFilesRecursiveCapped(dir, L"*.txt", 0, files);
    CHECK(files.empty());                                     // RD-01 maxDepth=0 只扫根目录

    files.clear();
    ListFilesRecursiveCapped(dir, L"*.txt", 2, files);
    CHECK(files.size() == 2);                                 // RD-02 深度 2 只到第 2 层

    files.clear();
    ListFilesRecursiveCapped(dir, L"*.txt", 40, files);
    CHECK(files.size() == 6);                                 // RD-03 足够深度可全部枚举

    files.clear();                                            // RD-04 负深度直接返回
    ListFilesRecursiveCapped(dir, L"*.txt", -1, files);
    CHECK(files.empty());

    // RD-05 符号链接目录不得被跟随（无权限创建时自动跳过）
    // 目标目录必须位于被扫描目录之外，否则 secret.txt 会被当作普通子文件命中
    std::wstring linkDir = MakeTempDir(L"depth_link");
    std::wstring outside = MakeTempDir(L"depth_out");
    std::wstring target = outside + L"\\target";
    CreateDirectoryW(target.c_str(), NULL);
    WriteBlob(target + L"\\secret.txt", "x", 1);
    std::wstring link = linkDir + L"\\link";
    CreateSymbolicLinkW(link.c_str(), target.c_str(), 0);   // 非管理员可能失败，失败则跳过该断言语义
    files.clear();
    ListFilesRecursiveCapped(linkDir, L"*.txt", 40, files);
    // 无论符号链接是否创建成功都不得崩溃；创建成功时不得跟随链接枚举到 secret.txt
    CHECK(std::none_of(files.begin(), files.end(),
        [](const FileInfo& f) { return f.name == L"secret.txt"; }));
    RemoveTree(linkDir);
    RemoveTree(outside);
    RemoveTree(dir);
}

// SAFE —— SafeCopy / SafeAppend 在缓冲区不足时必须截断而非终止进程
static void Test_SafeCopyAppend() {
    GROUP("SAFE 安全字符串拷贝");
    wchar_t buf[8] = { 0 };
    SafeCopy(buf, 8, L"0123456789ABCDEF");                    // SC-01 截断
    CHECK(wcslen(buf) <= 7);
    CHECK_EQ_STR(buf, L"0123456");

    wchar_t b2[4] = { 0 };
    SafeCopy(b2, 4, L"abcdefgh");                             // SC-02 极小缓冲
    CHECK_EQ_STR(b2, L"abc");

    wchar_t b3[1] = { 0 };
    SafeCopy(b3, 1, L"xyz");                                  // SC-03 只有终止符空间
    CHECK(b3[0] == L'\0');

    wchar_t b4[6] = { 0 };
    SafeCopy(b4, 6, L"abc");
    SafeAppend(b4, 6, L"defghij");                            // SA-01 追加截断
    CHECK(wcslen(b4) <= 5);
    CHECK_EQ_STR(b4, L"abcde");

    wchar_t b5[4] = { 0 };
    SafeCopy(b5, 4, L"zz");
    SafeAppend(b5, 4, L"");                                   // SA-02 空串追加
    CHECK_EQ_STR(b5, L"zz");

    wchar_t b6[1] = { 0 };
    SafeAppend(b6, 1, L"toolong");                            // SA-03 无空间时静默
    CHECK(b6[0] == L'\0');
}

// RESID —— 残留目录名安全校验
static void Test_ResidualName() {
    GROUP("RESID 残留名安全校验");
    CHECK(IsSafeResidualName(L"FooApp"));
    CHECK(IsSafeResidualName(L"My App 2"));
    CHECK(!IsSafeResidualName(L""));                          // RS-01 空名
    CHECK(!IsSafeResidualName(L"."));
    CHECK(!IsSafeResidualName(L".."));
    CHECK(!IsSafeResidualName(L"a..b"));                      // RS-02 含 ..
    CHECK(!IsSafeResidualName(L"..\\..\\Windows"));
    CHECK(!IsSafeResidualName(L"sub\\dir"));                  // RS-03 含分隔符
    CHECK(!IsSafeResidualName(L"C:/windows"));
    CHECK(!IsSafeResidualName(L"C:foo"));                     // RS-04 含盘符
    CHECK(!IsSafeResidualName(L"a*b"));                       // RS-05 通配符
    CHECK(!IsSafeResidualName(L"a?b"));
    CHECK(!IsSafeResidualName(L"   "));                       // RS-06 纯空白
    CHECK(!IsSafeResidualName(L"\t"));
    CHECK(!IsSafeResidualName(L"con"));                       // RS-07 保留设备名
    CHECK(!IsSafeResidualName(L"NUL"));
    CHECK(!IsSafeResidualName(L"com1"));
    CHECK(IsSafeResidualName(L"LPT9"));                       // 列表只到 lpt4
    CHECK(IsSafeResidualName(L"console"));                    // 前缀相同但不是保留名
}

// SUFFIX —— .disabled 后缀剥离
static void Test_DisabledSuffix() {
    GROUP("SUFFIX 禁用后缀剥离");
    std::wstring out;
    CHECK(TryStripDisabledSuffix(L"C:\\a\\app.exe.disabled", out));
    CHECK_EQ_STR(out, L"C:\\a\\app.exe");
    out.clear();
    CHECK(!TryStripDisabledSuffix(L"C:\\a\\app.exe", out));   // 无后缀
    CHECK(!TryStripDisabledSuffix(L".disabled", out));        // DS-01 长度等于后缀
    CHECK(!TryStripDisabledSuffix(L".disabl", out));          // DS-02 长度不足
    CHECK(!TryStripDisabledSuffix(L"", out));                 // DS-03 空串
    out.clear();
    CHECK(TryStripDisabledSuffix(L"x.disabled", out));        // DS-04 最短合法输入
    CHECK_EQ_STR(out, L"x");
}

// FSIZE —— 64 位文件大小还原
static void Test_FileSize64() {
    GROUP("FSIZE 64位大小还原");
    WIN32_FIND_DATAW fd = {};
    fd.nFileSizeHigh = 0; fd.nFileSizeLow = 1000;
    CHECK(FileSizeFromFindData(&fd) == 1000LL);
    fd.nFileSizeHigh = 1; fd.nFileSizeLow = 0;
    CHECK(FileSizeFromFindData(&fd) == 4294967296LL);            // FS-01 4 GiB
    fd.nFileSizeHigh = 3; fd.nFileSizeLow = 7;
    CHECK(FileSizeFromFindData(&fd) == 3LL * 4294967296LL + 7LL);
    fd.nFileSizeHigh = 0xFFFFFFFF; fd.nFileSizeLow = 0xFFFFFFFF;
    CHECK(FileSizeFromFindData(&fd) == 18446744073709551615ULL); // FS-02 上限不溢出为负
}

// SPEED —— 速率格式化边界
static void Test_FormatSpeed() {
    GROUP("SPEED 速率格式化");
    CHECK(FormatSpeed(0).find(L"KB/s") != std::wstring::npos);
    CHECK(FormatSpeed(512).find(L"KB/s") != std::wstring::npos);   // 512 B/s < 1Mbps
    CHECK(FormatSpeed(1024LL * 1024 * 1024).find(L"Mbps") != std::wstring::npos);
    // 边界：恰好 1 Mbps（131072 B/s）必须切到 Mbps
    CHECK(FormatSpeed(131072).find(L"Mbps") != std::wstring::npos);
    CHECK(FormatSpeed(131071).find(L"KB/s") != std::wstring::npos);
}

// SUBDIR —— 子目录通配查找（真实目录）
static void Test_FindMatchingSubDirs() {
    GROUP("SUBDIR 子目录通配查找");
    std::wstring dir = MakeTempDir(L"subdir");
    CreateDirectoryW((dir + L"\\Default").c_str(), NULL);
    CreateDirectoryW((dir + L"\\Default\\Cache").c_str(), NULL);
    CreateDirectoryW((dir + L"\\Profile 1").c_str(), NULL);
    CreateDirectoryW((dir + L"\\Profile 1\\Code Cache").c_str(), NULL);
    CreateDirectoryW((dir + L"\\Cache").c_str(), NULL);      // 顶层同名不应匹配 *\Cache

    std::vector<std::wstring> pats = { L"*\\Cache" };   // 与 CleanItem.subDirPatterns 相同的形式
    auto hits = FindMatchingSubDirs(dir, pats);
    bool hasDefault = false, hasCodeCache = false, hasTopCache = false;
    for (const auto& h : hits) {
        if (h == dir + L"\\Default\\Cache") hasDefault = true;
        if (h == dir + L"\\Profile 1\\Code Cache") hasCodeCache = true;
        if (h == dir + L"\\Cache") hasTopCache = true;
    }
    CHECK(hasDefault);                                        // SD-01 匹配带父目录的 Cache
    CHECK(!hasCodeCache);                                     // SD-02 Code Cache != Cache
    CHECK(!hasTopCache);                                      // SD-03 顶层 Cache 不匹配 *\Cache

    CHECK(FindMatchingSubDirs(dir, {}).empty());              // SD-04 空模式列表
    CHECK(FindMatchingSubDirs(dir + L"\\__nope__", pats).empty()); // SD-05 基目录不存在
    std::vector<std::wstring> exact = { L"Default\\Cache" };  // SD-06 精确模式
    CHECK(FindMatchingSubDirs(dir, exact).size() == 1);
    RemoveTree(dir);
}

// FORCE —— 强制删除只读文件/嵌套目录
static void Test_ForceDelete() {
    GROUP("FORCE 强制删除");
    std::wstring dir = MakeTempDir(L"force");
    CreateDirectoryW((dir + L"\\sub").c_str(), NULL);
    CreateDirectoryW((dir + L"\\sub\\deep").c_str(), NULL);
    std::wstring ro = dir + L"\\sub\\deep\\ro.txt";
    WriteBlob(ro, "data", 4);
    SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);

    CHECK(ForceDeleteFile(ro));                               // FD-01 只读文件可强删
    CHECK(GetFileAttributesW(ro.c_str()) == INVALID_FILE_ATTRIBUTES);

    // FD-02 非空目录必须删除失败（ForceRemoveDirectory 只删空目录，避免误删数据）
    WriteBlob(dir + L"\\sub\\deep\\keep.txt", "x", 1);
    CHECK(!ForceRemoveDirectory(dir + L"\\sub"));
    CHECK(GetFileAttributesW((dir + L"\\sub").c_str()) != INVALID_FILE_ATTRIBUTES);

    // FD-04 只读空目录可删除
    DeleteFileW((dir + L"\\sub\\deep\\keep.txt").c_str());
    SetFileAttributesW((dir + L"\\sub\\deep").c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(ForceRemoveDirectory(dir + L"\\sub\\deep"));
    CHECK(GetFileAttributesW((dir + L"\\sub\\deep").c_str()) == INVALID_FILE_ATTRIBUTES);

    // FD-05 不存在的路径按实现语义视为"删除成功"（返回 true，且不崩溃）
    CHECK(ForceDeleteFile(dir + L"\\__nope__.txt"));
    CHECK(ForceRemoveDirectory(dir + L"\\__nope__"));
    RemoveTree(dir);
}

// VENDOR —— AppData/ProgramData 第一层公共/厂商目录名拦截
// 卸载 Publisher="Microsoft" 的软件时，残留候选名会拼出 %APPDATA%\Microsoft，
// 整棵删掉会把其它软件的数据一起清掉，必须在名字层面拦下
static void Test_SharedVendorDirName() {
    GROUP("VENDOR 公共/厂商目录名");
    CHECK(IsSharedVendorDirName(L""));                        // VN-01 空名
    CHECK(IsSharedVendorDirName(L"."));                       // VN-02 只有点
    CHECK(IsSharedVendorDirName(L"..."));
    CHECK(IsSharedVendorDirName(L" "));                       // VN-03 只有空白
    CHECK(IsSharedVendorDirName(L"Microsoft"));               // VN-04 大小写无关
    CHECK(IsSharedVendorDirName(L"MICROSOFT"));
    CHECK(IsSharedVendorDirName(L"microsoft "));              // VN-05 尾随空格
    CHECK(IsSharedVendorDirName(L"microsoft."));              // VN-06 尾随点
    CHECK(IsSharedVendorDirName(L"Google"));
    CHECK(IsSharedVendorDirName(L"Temp"));
    CHECK(IsSharedVendorDirName(L"Packages"));
    CHECK(IsSharedVendorDirName(L"Package Cache"));
    CHECK(IsSharedVendorDirName(L"Common Files"));
    CHECK(IsSharedVendorDirName(L"NVIDIA"));
    CHECK(IsSharedVendorDirName(L"Adobe"));
    CHECK(IsSharedVendorDirName(L"Python"));
    CHECK(IsSharedVendorDirName(L"comms"));
    CHECK(IsSharedVendorDirName(L"Publishers"));
    // 真实应用自己的目录不能被拦，否则残留清不掉
    CHECK(!IsSharedVendorDirName(L"Google Chrome"));          // VN-07 不是 "google"
    CHECK(!IsSharedVendorDirName(L"Microsoft Edge"));
    CHECK(!IsSharedVendorDirName(L"Microsoft.VisualStudio"));
    CHECK(!IsSharedVendorDirName(L"Code"));
    CHECK(!IsSharedVendorDirName(L"Discord App"));
    CHECK(!IsSharedVendorDirName(L"GUITestA"));
    CHECK(!IsSharedVendorDirName(L"My App 2"));
}

// UNDER —— 路径前缀包含判定
static void Test_PathUnderDirectory() {
    GROUP("UNDER 路径包含判定");
    CHECK(IsPathUnderDirectory(L"C:\\A\\B", L"C:\\A"));
    CHECK(IsPathUnderDirectory(L"C:\\A", L"C:\\A"));          // PU-01 相等算包含
    CHECK(IsPathUnderDirectory(L"C:/A/B", L"C:\\A"));         // PU-02 分隔符归一
    CHECK(IsPathUnderDirectory(L"c:\\a\\b", L"C:\\A"));       // PU-03 大小写无关
    CHECK(!IsPathUnderDirectory(L"C:\\AB", L"C:\\A"));        // PU-04 整段比较，非子串
    CHECK(!IsPathUnderDirectory(L"C:\\A", L"C:\\A\\B"));      // PU-05 反向
    CHECK(!IsPathUnderDirectory(L"C:\\A\\B", L""));           // PU-06 空 parent
    CHECK(!IsPathUnderDirectory(L"", L"C:\\A"));              // PU-07 空 child
    CHECK(!IsPathUnderDirectory(L"D:\\A\\B", L"C:\\A"));      // PU-08 不同盘符
    // 8.3 短名归一后仍应判定为同一目录之下
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(sysDir, MAX_PATH);
    std::wstring s32 = std::wstring(sysDir) + L"\\System32";
    wchar_t shrt[MAX_PATH] = { 0 };
    DWORD rc = GetShortPathNameW(s32.c_str(), shrt, MAX_PATH);
    if (rc > 0 && rc < MAX_PATH && wcscmp(shrt, s32.c_str()) != 0) {
        CHECK(IsPathUnderDirectory(s32 + L"\\kernel32.dll", shrt)); // PU-09 短名
    }
}

// ENVX —— 环境变量展开
static void Test_ExpandEnvVars() {
    GROUP("ENVX 环境变量展开");
    std::wstring out;
    CHECK(ExpandEnvVars(L"%SystemRoot%\\System32\\drivers\\x.sys", out));
    CHECK(out.find(L"%") == std::wstring::npos);              // EV-01 已展开
    CHECK(out.find(L"\\System32\\drivers\\x.sys") != std::wstring::npos);
    CHECK(!ExpandEnvVars(L"", out));                          // EV-02 空串
    out = L"sentinel";
    // EV-03 超过内部缓冲区（MAX_PATH*4）时必须返回 false 而不是越界；
    //     失败时 out 被置为原值（调用方据此回退），绝不能是半截内容
    CHECK(!ExpandEnvVars(std::wstring(200000, L'a'), out));
    CHECK(out.size() == 200000);                              // 失败时 out 等于原值，不是半截
    out.clear();
    CHECK(ExpandEnvVars(L"C:\\plain\\path", out));            // EV-04 无变量原样返回
    CHECK_EQ_STR(out, L"C:\\plain\\path");
}

// CMD —— 卸载命令行拆分（注册表 UninstallString 三种写法）
static void Test_SplitUninstallCommand() {
    GROUP("CMD 卸载命令拆分");
    std::wstring exe, params;

    // CD-01 带引号 + 参数
    CHECK(SplitUninstallCommand(L"\"C:\\Program Files\\App\\unins000.exe\" /SILENT", exe, params));
    CHECK_EQ_STR(exe, L"C:\\Program Files\\App\\unins000.exe");
    CHECK_EQ_STR(params, L"/SILENT");

    // CD-02 含空格却没引号：原实现按第一个空格切分会得到 "C:\Program"
    //     这里用真实存在的文件做最长前缀匹配
    std::wstring dir = MakeTempDir(L"cmd");
    std::wstring fake = dir + L"\\unins000.exe";
    WriteBlob(fake, "MZ", 2);
    std::wstring raw = fake + L" /SILENT /LOG";
    CHECK(SplitUninstallCommand(raw, exe, params));
    CHECK_EQ_STR(exe, fake);                                  // CD-03 路径完整
    CHECK_EQ_STR(params, L"/SILENT /LOG");

    // CD-04 只有目录存在时：参数绝不能被并进 exe（目录交给 ExecuteUninstallCommand 拦下）
    std::wstring sub = dir + L"\\sub";
    CreateDirectoryW(sub.c_str(), NULL);
    CHECK(SplitUninstallCommand(sub + L" /x", exe, params));
    CHECK_EQ_STR(exe, sub);
    CHECK_EQ_STR(params, L"/x");

    // CD-05 靠 PATH 解析的形式
    CHECK(SplitUninstallCommand(L"msiexec.exe /x {GUID} /qn", exe, params));
    CHECK_EQ_STR(exe, L"msiexec.exe");
    CHECK_EQ_STR(params, L"/x {GUID} /qn");

    // CD-06 无参数
    CHECK(SplitUninstallCommand(L"notepad.exe", exe, params));
    CHECK_EQ_STR(exe, L"notepad.exe");
    CHECK_EQ_STR(params, L"");

    // CD-07 引号未闭合：整段当路径，不能崩溃
    CHECK(SplitUninstallCommand(L"\"C:\\a b\\unins.exe", exe, params));
    CHECK_EQ_STR(exe, L"C:\\a b\\unins.exe");

    CHECK(!SplitUninstallCommand(L"", exe, params));          // CD-08 空串
    CHECK(!SplitUninstallCommand(L"   \t ", exe, params));    // CD-09 纯空白
    CHECK_EQ_STR(exe, L"");
    CHECK_EQ_STR(params, L"");

    // CD-10 前导空白
    CHECK(SplitUninstallCommand(L"   notepad.exe /x", exe, params));
    CHECK_EQ_STR(exe, L"notepad.exe");
    CHECK_EQ_STR(params, L"/x");
    RemoveTree(dir);
}

// SHROOT —— 共享根路径判定（即使被写成 InstallLocation 也不能整删）
static void Test_SharedProgramRoot() {
    GROUP("SHROOT 共享根路径");
    wchar_t buf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) {
        CHECK(IsSharedProgramRootPath(buf));                  // SR-01 C:\Program Files
        CHECK(IsSharedProgramRootPath(std::wstring(buf) + L"\\Common Files"));
        CHECK(!IsSharedProgramRootPath(std::wstring(buf) + L"\\Google\\Chrome")); // SR-02 真实应用
        CHECK(!IsSharedProgramRootPath(std::wstring(buf) + L"\\MyApp"));
    }
    if (GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH)) {
        CHECK(IsSharedProgramRootPath(buf));
        CHECK(IsSharedProgramRootPath(std::wstring(buf) + L"\\Microsoft")); // SR-03 公共子项
        CHECK(!IsSharedProgramRootPath(std::wstring(buf) + L"\\MyApp\\Data"));
    }
    wchar_t win[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
        CHECK(IsSharedProgramRootPath(win));
        CHECK(IsSharedProgramRootPath(std::wstring(win) + L"\\System32"));
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf))) {
        CHECK(IsSharedProgramRootPath(buf));                  // SR-04 %LOCALAPPDATA%
        CHECK(IsSharedProgramRootPath(std::wstring(buf) + L"\\Programs"));
    }
    CHECK(!IsSharedProgramRootPath(L""));                     // SR-05 空串
    CHECK(!IsSharedProgramRootPath(L"C:\\Users\\foo\\MyApp"));
}

// SAFEDEL —— 递归删除总闸
static void Test_SafeToDeleteDirectory() {
    GROUP("SAFEDEL 递归删除总闸");
    CHECK(!IsSafeToDeleteDirectory(L""));                     // SD-01 空
    CHECK(!IsSafeToDeleteDirectory(L"C:\\"));                 // SD-02 盘符根
    CHECK(!IsSafeToDeleteDirectory(L"D:\\"));
    CHECK(!IsSafeToDeleteDirectory(L"C:"));                   // SD-03 无斜杠盘符
    CHECK(!IsSafeToDeleteDirectory(L"\\\\server\\share"));    // SD-04 UNC 共享根
    CHECK(!IsSafeToDeleteDirectory(L"\\\\server"));           // SD-05 UNC 主机
    CHECK(!IsSafeToDeleteDirectory(L"C:\\Windows\\System32"));
    CHECK(!IsSafeToDeleteDirectory(L"C:\\Windows"));
    CHECK(!IsSafeToDeleteDirectory(L"C:\\Program Files"));
    CHECK(!IsSafeToDeleteDirectory(L"C:\\a\\..\\..\\Windows")); // SD-06 遍历绕过
    CHECK(!IsSafeToDeleteDirectory(L"C:\\Windows\\..\\Windows\\System32"));
    CHECK(IsSafeToDeleteDirectory(L"C:\\Users\\foo\\AppData\\Local\\MyApp"));
    CHECK(IsSafeToDeleteDirectory(L"C:\\Program Files\\MyApp"));
    // 实际盘符根同样被拦（不能只硬编码 C:）
    wchar_t sysRootBuf[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(sysRootBuf, MAX_PATH);
    std::wstring sysRoot = std::wstring(1, sysRootBuf[0]) + L":\\";
    CHECK(!IsSafeToDeleteDirectory(sysRoot));                 // SD-07 系统盘根
    CHECK(!IsSafeToDeleteDirectory(sysRoot + L"Users"));      // SD-08 用户目录根
}

// SYSCMP —— 服务 ImagePath 是否属于 Windows 自带组件
static void Test_SystemComponentImagePath() {
    GROUP("SYSCMP 系统组件 ImagePath");
    CHECK(IsSystemComponentImagePath(L""));                   // SC-01 读不到时最保守
    CHECK(IsSystemComponentImagePath(L"\\SystemRoot\\System32\\drivers\\ntfs.sys"));
    CHECK(IsSystemComponentImagePath(L"\\system32\\drivers\\fltmgr.sys"));
    CHECK(IsSystemComponentImagePath(L"%SystemRoot%\\System32\\drivers\\Wdf01000.sys"));
    CHECK(IsSystemComponentImagePath(L"C:\\Windows\\System32\\drivers\\acpi.sys"));
    CHECK(IsSystemComponentImagePath(L"c:\\windows\\syswow64\\drivers\\x.sys")); // SC-02 大小写
    CHECK(IsSystemComponentImagePath(L"svchost.exe -k netsvcs"));  // SC-03 非绝对路径
    CHECK(IsSystemComponentImagePath(L"\\??\\C:\\Windows\\System32\\drivers\\x.sys")); // SC-04 设备前缀
    CHECK(!IsSystemComponentImagePath(L"C:\\Program Files\\App\\app.sys"));
    CHECK(!IsSystemComponentImagePath(L"\"C:\\Program Files\\App\\app.sys\"")); // SC-05 带引号
    CHECK(!IsSystemComponentImagePath(L"C:\\Program Files\\App\\app.sys -param")); // SC-06 带参数
    CHECK(!IsSystemComponentImagePath(L"D:\\Games\\AntiCheat\\sys.sys"));
    // 与实际系统盘符无关
    wchar_t win[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(win, MAX_PATH);
    CHECK(IsSystemComponentImagePath(std::wstring(win) + L"\\System32\\drivers\\x.sys"));
    CHECK(IsSystemComponentImagePath(std::wstring(win) + L"\\System32\\drivers\\x.sys -p")); // SC-07 带参数
}

// DRVPATH —— 服务 ImagePath -> 驱动文件绝对路径
static void Test_ResolveDriverFilePath() {
    GROUP("DRVPATH 驱动路径解析");
    wchar_t win[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring winDir(win);

    CHECK_EQ_STR(ResolveDriverFilePath(L""), L"");            // DP-01 空
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\SystemRoot\\System32\\drivers\\ntfs.sys"),
                 winDir + L"\\System32\\drivers\\ntfs.sys");  // DP-02 相对写法
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\System32\\drivers\\x.sys"),
                 winDir + L"\\System32\\drivers\\x.sys");
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\SysWOW64\\drivers\\x.sys"),
                 winDir + L"\\SysWOW64\\drivers\\x.sys");
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\??\\C:\\App\\a.sys"), L"C:\\App\\a.sys"); // DP-03 设备前缀
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\\\.\\C:\\App\\a.sys"), L"C:\\App\\a.sys"); // DP-03b
    CHECK_EQ_STR(ResolveDriverFilePath(L"\"C:\\App\\a.sys\""), L"C:\\App\\a.sys");   // DP-05 去引号
    CHECK_EQ_STR(ResolveDriverFilePath(L"%SystemRoot%\\System32\\drivers\\y.sys"),
                 winDir + L"\\System32\\drivers\\y.sys");     // DP-07 环境变量
    // DP-08 非绝对路径必须返回空：否则会被当成当前目录下的文件误删
    CHECK_EQ_STR(ResolveDriverFilePath(L"svchost.exe -k netsvcs"), L"");
    CHECK_EQ_STR(ResolveDriverFilePath(L"drivers\\x.sys"), L"");
    CHECK_EQ_STR(ResolveDriverFilePath(L"C:x"), L"");         // DP-09 盘符相对
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\\\"), L"");        // DP-10 过短
    // DP-11 UNC 路径不返回（调用方只处理盘符绝对路径）
    CHECK_EQ_STR(ResolveDriverFilePath(L"\\\\server\\share\\x.sys"), L"");

    // 未加引号且含空格：必须用「真实存在的文件」解析，绝不能截成 "C:\Program"
    std::wstring dir = MakeTempDir(L"drv");
    std::wstring pf = dir + L"\\Program Files";
    CreateDirectoryW(pf.c_str(), NULL);
    std::wstring real = pf + L"\\app.sys";
    WriteBlob(real, "MZ", 2);
    CHECK_EQ_STR(ResolveDriverFilePath(real), real);          // DP-12 含空格完整保留
    CHECK_EQ_STR(ResolveDriverFilePath(real + L" -x"), real); // DP-13 去参数
    CHECK_EQ_STR(ResolveDriverFilePath(L"\"" + real + L"\" /q"), real); // DP-14 引号+参数
    // DP-15 文件不存在时保留整串，宁可因路径不存在而跳过删除，也不能误删前缀目录
    std::wstring ghost = pf + L"\\nope.sys";
    CHECK_EQ_STR(ResolveDriverFilePath(ghost), ghost);
    RemoveTree(dir);
}

// REGDEL —— 注册表递归删除总闸（容器键保护）
static void Test_SafeToDeleteRegistryKey() {
    GROUP("REGDEL 注册表删除总闸");
    // 必须用真实的预定义句柄常量：手工拼 0x80000001 在 x64 上高位不同，
    // 会被函数当成 HKLM 分支
    const HKEY HKLM = HKEY_LOCAL_MACHINE;
    const HKEY HKCU = HKEY_CURRENT_USER;

    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L""));                       // RD-01 空
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"   "));                    // RD-02 纯空白
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE"));               // RD-03 顶层容器
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet\\Services")); // RD-04 服务根
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet\\Control"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet\\Enum"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Classes"));      // RD-05 文件关联根
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Microsoft"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"));   // RD-06 卸载注册根
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"));
    CHECK(!IsSafeToDeleteRegistryKey(HKCU, L"SOFTWARE"));
    CHECK(!IsSafeToDeleteRegistryKey(HKCU, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run")); // RD-07 启动项根
    CHECK(!IsSafeToDeleteRegistryKey(HKCU, L"Environment"));            // RD-08 环境变量

    // RD-09 归一化：分隔符/大小写/尾部斜杠/重复斜杠/首尾空白都不能绕过
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"system\\currentcontrolset\\services"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM//CurrentControlSet//Services"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet\\Services\\"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"  SYSTEM\\CurrentControlSet\\Services  "));

    // RD-10 前缀相似但更深的真实条目必须放行，否则无法清理任何残留
    CHECK(IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MyApp"));
    CHECK(IsSafeToDeleteRegistryKey(HKLM,
        L"SYSTEM\\CurrentControlSet\\Services\\SCTestDrv"));
    CHECK(IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\myapp.exe"));
    CHECK(IsSafeToDeleteRegistryKey(HKCU, L"SOFTWARE\\MyCompany\\MyApp"));
    CHECK(IsSafeToDeleteRegistryKey(HKCU,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run\\MyApp"));
    // RD-11 「Services 的同名兄弟键」不应被误拦（按段比较而非子串比较）
    CHECK(IsSafeToDeleteRegistryKey(HKLM, L"SYSTEM\\CurrentControlSet\\ServicesMyApp"));
    // RD-12 HKCU 的放行项不应受 HKLM 名单影响
    CHECK(IsSafeToDeleteRegistryKey(HKCU, L"SOFTWARE\\MyVendor\\Tool"));

    // RD-13 深层微软容器子键：原实现只比较整串是否等于容器键，
    // 这类深层键全部被放行，等于允许删微软组件任意子键，必须拦截
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Classes\\.exe"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Classes\\AppX\\Sub\\Key"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\S-1-5-21-1"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\x"));
    CHECK(!IsSafeToDeleteRegistryKey(HKCU,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.txt"));
    // RD-14 关键服务内部条目即使在 Services 白名单下也拦截
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM,
        L"SYSTEM\\CurrentControlSet\\Services\\ntfs\\Security"));
    // RD-15 WOW6432Node 下的 Uninstall/App Paths 直接子项放行，Microsoft 深层键拦截
    CHECK(IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OldApp"));
    CHECK(IsSafeToDeleteRegistryKey(HKLM,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\tool.exe"));
    CHECK(!IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion"));
    // WOW6432Node\Whatever 是第三方 32 位键（等价于 SOFTWARE\Whatever），应可清理
    CHECK(IsSafeToDeleteRegistryKey(HKLM, L"SOFTWARE\\WOW6432Node\\Whatever\\Deep\\Key"));
    // RD-16 非法 hive（NULL / 未知句柄）必须拒绝，绝不能当 HKLM 处理
    CHECK(!IsSafeToDeleteRegistryKey(NULL, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\X"));
    CHECK(!IsSafeToDeleteRegistryKey((HKEY)(ULONG_PTR)0xDEADBEEF,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\X"));
    // RD-17 HKU（HKEY_USERS）是合法 hive，非保护路径放行
    CHECK(IsSafeToDeleteRegistryKey(HKEY_USERS,
        L"S-1-5-21-1\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App"));
    CHECK(!IsSafeToDeleteRegistryKey(HKEY_USERS,
        L"S-1-5-21-1\\Software\\Classes\\.exe"));
}

// REGPATH —— WOW6432Node 真实路径映射
static void Test_RegRealPath() {
    GROUP("REGPATH 注册表真实路径映射");
    const HKEY HKLM = HKEY_LOCAL_MACHINE;
    const HKEY HKCU = HKEY_CURRENT_USER;

    // RP-01 32 位视图下的 SOFTWARE 键映射到 WOW6432Node
    CHECK_EQ_STR(RegRealPath(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App", KEY_WOW64_32KEY),
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App");
    // RP-02 64 位视图原样返回
    CHECK_EQ_STR(RegRealPath(HKLM,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App", KEY_WOW64_64KEY),
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App");
    CHECK_EQ_STR(RegRealPath(HKLM, L"SOFTWARE\\Foo", 0), L"SOFTWARE\\Foo");
    // RP-03 幂等：已带前缀不能再叠加
    CHECK_EQ_STR(RegRealPath(HKLM,
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App", KEY_WOW64_32KEY),
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\App");
    // RP-04 非 SOFTWARE 开头的路径不映射（SYSTEM 等不受 WOW64 重定向影响）
    CHECK_EQ_STR(RegRealPath(HKLM, L"SYSTEM\\CurrentControlSet\\Services\\X", KEY_WOW64_32KEY),
        L"SYSTEM\\CurrentControlSet\\Services\\X");
    // RP-05 非 HKLM 不映射（HKCU 无 WOW64 重定向）
    CHECK_EQ_STR(RegRealPath(HKCU, L"SOFTWARE\\Foo", KEY_WOW64_32KEY), L"SOFTWARE\\Foo");
    // RP-06 大小写不敏感识别前缀
    CHECK_EQ_STR(RegRealPath(HKLM, L"software\\foo", KEY_WOW64_32KEY),
        L"SOFTWARE\\WOW6432Node\\foo");
    // RP-07 "SOFTWARE" 本身（无子路径）不映射
    CHECK_EQ_STR(RegRealPath(HKLM, L"SOFTWARE", KEY_WOW64_32KEY), L"SOFTWARE");
}

// REGEXIST —— 视图下键存在性探测
static void Test_RegKeyExistsInView() {
    GROUP("REGEXIST 键存在性探测");
    // RE-01 非法参数
    CHECK(!RegKeyExistsInView(NULL, L"SOFTWARE", 0));
    CHECK(!RegKeyExistsInView(HKEY_CURRENT_USER, L"", 0));
    // RE-02 一定存在的键
    CHECK(RegKeyExistsInView(HKEY_CURRENT_USER, L"Software", 0));
    CHECK(RegKeyExistsInView(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", 0));
    // RE-03 一定不存在的键
    CHECK(!RegKeyExistsInView(HKEY_CURRENT_USER,
        L"Software\\SC_Tests_Definitely_Not_Present_Key", 0));
    // RE-04 视图标志不影响 HKCU（无重定向）
    CHECK(RegKeyExistsInView(HKEY_CURRENT_USER, L"Software", KEY_WOW64_32KEY));
}

// LEFTOVER —— 卸载残留判定
static void Test_UninstallEntryLeftover() {
    GROUP("LEFTOVER 卸载残留判定");
    std::wstring reason;

    // LF-01 安装目录明确写了却不存在 —— 残留
    CHECK(IsUninstallEntryLeftover(L"C:\\SC_NoSuch_Dir_12345", L"", reason));
    CHECK(!reason.empty());

    // LF-02 安装目录存在 —— 不是残留（即便卸载器缺失）
    std::wstring dir = MakeTempDir(L"left");
    CHECK(!IsUninstallEntryLeftover(dir, L"", reason));

    // LF-03 安装位置写成文件 —— 残留
    std::wstring f = dir + L"\\a.txt";
    WriteBlob(f, "x", 1);
    CHECK(IsUninstallEntryLeftover(f, L"", reason));

    // LF-04 尾部斜杠归一化后目录仍存在 —— 不是残留
    CHECK(!IsUninstallEntryLeftover(dir + L"\\", L"", reason));

    // LF-05 含通配符的安装位置不可信 —— 不判定
    CHECK(!IsUninstallEntryLeftover(L"C:\\Program Files\\*\\app", L"", reason));

    // LF-06 无 InstallLocation，卸载器绝对路径不存在 —— 残留
    CHECK(IsUninstallEntryLeftover(L"", L"C:\\SC_NoSuch_Dir_12345\\unins000.exe", reason));

    // LF-07 无 InstallLocation，卸载器存在 —— 不是残留
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring notepad = std::wstring(sysDir) + L"\\notepad.exe";
    CHECK(!IsUninstallEntryLeftover(L"", notepad, reason));

    // LF-08 msiexec 卸载器永远存在，不构成残留信号
    CHECK(!IsUninstallEntryLeftover(L"",
        L"msiexec.exe /x {12345678-1234-1234-1234-123456789012}", reason));
    CHECK(!IsUninstallEntryLeftover(L"",
        L"C:\\Windows\\System32\\msiexec.exe /x {ABCD}", reason));

    // LF-09 相对路径卸载器（靠 PATH 解析）—— 放过
    CHECK(!IsUninstallEntryLeftover(L"", L"unins000.exe", reason));

    // LF-10 两者都为空 —— 无法判定，保守返回 false
    CHECK(!IsUninstallEntryLeftover(L"", L"", reason));
    CHECK(!IsUninstallEntryLeftover(L"   ", L"   ", reason));

    // LF-11 Windows\Installer 缓存缺失属正常，不判定为残留
    wchar_t winDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(winDir, MAX_PATH);
    CHECK(!IsUninstallEntryLeftover(L"",
        std::wstring(winDir) + L"\\Installer\\{AABBCCDD}.Msi", reason));

    // LF-12 带引号的卸载命令，路径不存在 —— 残留
    CHECK(IsUninstallEntryLeftover(L"",
        L"\"C:\\SC_NoSuch_Dir_12345\\unins000.exe\" /SILENT", reason));

    // LF-13 未定义环境变量：展开后仍是 %VAR% 字面量，非绝对路径，保守放过
    CHECK(!IsUninstallEntryLeftover(L"",
        L"%SC_NOEXIST_VAR_12345%\\unins.exe", reason));

    RemoveTree(dir);
}

// SYSCOMP —— 系统组件/补丁条目识别
static void Test_SystemOrPatchEntry() {
    GROUP("SYSCOMP 系统组件/补丁识别");
    // SC-01 普通软件条目
    CHECK(!IsSystemOrPatchUninstallEntry(0, L"", L""));
    // SC-02 SystemComponent=1
    CHECK(IsSystemOrPatchUninstallEntry(1, L"", L""));
    // SC-03 MSI 补丁：ReleaseType 非空
    CHECK(IsSystemOrPatchUninstallEntry(0, L"Security Update", L""));
    // SC-04 MSI 补丁：ParentKeyName 非空
    CHECK(IsSystemOrPatchUninstallEntry(0, L"", L"{GUID}_KEY"));
    // SC-05 SystemComponent 非 1（如 0/2）不算组件
    CHECK(!IsSystemOrPatchUninstallEntry(2, L"", L""));
}

// STARTUPCMD —— 无效启动项判定
static void Test_InvalidStartupCommand() {
    GROUP("STARTUPCMD 无效启动项判定");
    std::wstring reason;

    // SU-01 空命令 —— 无效
    CHECK(IsInvalidStartupCommand(L"", reason));
    CHECK(IsInvalidStartupCommand(L"   ", reason));

    // SU-02 绝对路径且不存在 —— 无效
    CHECK(IsInvalidStartupCommand(L"C:\\SC_NoSuch_Dir_12345\\app.exe", reason));

    // SU-03 绝对路径且存在 —— 有效
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring notepad = std::wstring(sysDir) + L"\\notepad.exe";
    CHECK(!IsInvalidStartupCommand(notepad, reason));

    // SU-04 带参数的不存在路径 —— 无效
    CHECK(IsInvalidStartupCommand(L"C:\\SC_NoSuch_Dir_12345\\app.exe -run", reason));

    // SU-05 带引号的不存在路径 —— 无效
    CHECK(IsInvalidStartupCommand(L"\"C:\\SC_NoSuch_Dir_12345\\app.exe\"", reason));

    // SU-06 相对命令名（靠 PATH）—— 放过
    CHECK(!IsInvalidStartupCommand(L"notepad.exe", reason));
    CHECK(!IsInvalidStartupCommand(L"regsvr32.exe /s dll.dll", reason));

    // SU-07 含通配符 —— 放过
    CHECK(!IsInvalidStartupCommand(L"C:\\SC_NoSuch*\\app.exe", reason));

    // SU-08 环境变量展开后为绝对路径且不存在 —— 无效
    CHECK(IsInvalidStartupCommand(L"%SystemRoot%\\SC_NoSuch_Dir_12345\\app.exe", reason));
    // SU-09 未定义变量展开后仍是 %VAR% 字面量，非绝对路径 —— 放过
    CHECK(!IsInvalidStartupCommand(L"%SC_NOEXIST_VAR_12345%\\app.exe", reason));
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("SystemCleaner BASELINE tests\n");
    Test_SystemCriticalPath();
    Test_PathTraversal();
    Test_ProtectedDataFile();
    Test_Format();
    Test_EndsWith();
    Test_SubDirPattern();
    Test_ReadOnlyFile();
    Test_ListFiles();
    Test_FileDigest();
    Test_RecursiveCapped();
    Test_SafeCopyAppend();
    Test_ResidualName();
    Test_DisabledSuffix();
    Test_FileSize64();
    Test_FormatSpeed();
    Test_FindMatchingSubDirs();
    Test_ForceDelete();
    Test_SharedVendorDirName();
    Test_PathUnderDirectory();
    Test_ExpandEnvVars();
    Test_SplitUninstallCommand();
    Test_SharedProgramRoot();
    Test_SafeToDeleteDirectory();
    Test_SystemComponentImagePath();
    Test_ResolveDriverFilePath();
    Test_SafeToDeleteRegistryKey();
    Test_RegRealPath();
    Test_RegKeyExistsInView();
    Test_UninstallEntryLeftover();
    Test_SystemOrPatchEntry();
    Test_InvalidStartupCommand();
    printf("\n=====================================\n");
    printf("Total: %d   Failed: %d\n", g_total, g_failed);
    printf("=====================================\n");
    return g_failed == 0 ? 0 : 1;
}
