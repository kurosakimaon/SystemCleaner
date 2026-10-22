// Module: utils.cpp - Utility functions, helpers, and data protection

#include "common.h"

std::vector<CleanFailureLog> g_cleanFailures;

/**
 * @brief 将字节数格式化为可读的大小字符串
 */
std::wstring FormatSize(long long bytes) {
    wchar_t buf[64] = { 0 };
    // 负值（异常/溢出输入）不得显示为负大小，按 0 处理
    if (bytes < 0) bytes = 0;
    if (bytes < 1024) swprintf_s(buf, L"%lld B", bytes);
    else if (bytes < 1024LL * 1024) swprintf_s(buf, L"%.2f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) swprintf_s(buf, L"%.2f MB", bytes / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

/**
 * @brief 将字节/秒格式化为可读的速率字符串
 */
std::wstring FormatSpeed(long long bytesPerSec) {
    wchar_t buf[64] = { 0 };
    double mbps = (bytesPerSec * 8.0) / (1024.0 * 1024.0);
    if (mbps < 1) swprintf_s(buf, L"%.2f KB/s", bytesPerSec / 1024.0);
    else swprintf_s(buf, L"%.2f Mbps", mbps);
    return buf;
}

/**
 * @brief 判断字符串是否以指定后缀结尾
 */
bool EndsWith(const std::wstring& str, const std::wstring& suffix) {
    if (str.length() < suffix.length()) return false;
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

/**
 * @brief 执行系统命令并等待其完成（带超时，避免 UI 永久阻塞）
 */
void ExecuteCmd(const std::wstring& cmd) {
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    wchar_t sysDir[MAX_PATH] = { 0 };
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) {
        wcscpy_s(sysDir, L"C:\\Windows\\System32");
    }
    std::wstring shell = std::wstring(sysDir) + L"\\cmd.exe";

    std::wstring fullCmd = L"cmd.exe /c " + cmd;
    std::vector<wchar_t> mutableCmd(fullCmd.begin(), fullCmd.end());
    mutableCmd.push_back(L'\0');

    if (CreateProcessW(shell.c_str(), mutableCmd.data(), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // 最长等待 5 分钟，超时后终止子进程，防止调用线程被永久挂起
        DWORD wait = WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/**
 * @brief 执行命令并返回是否成功（子进程退出码为 0）
 *        ExecuteCmd 丢弃退出码：注册表备份依赖 reg.exe，而 reg.exe 失败（键不存在、
 *        目录不可写、被安全软件拦截）只体现在退出码上。原实现拿不到结果，
 *        于是「备份失败」与「备份成功」走同一条路，用户以为有备份其实没有。
 */
bool ExecuteCmdChecked(const std::wstring& cmd) {
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    wchar_t sysDir[MAX_PATH] = { 0 };
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) {
        wcscpy_s(sysDir, L"C:\\Windows\\System32");
    }
    std::wstring shell = std::wstring(sysDir) + L"\\cmd.exe";

    std::wstring fullCmd = L"cmd.exe /c " + cmd;
    std::vector<wchar_t> mutableCmd(fullCmd.begin(), fullCmd.end());
    mutableCmd.push_back(L'\0');

    if (!CreateProcessW(shell.c_str(), mutableCmd.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
    bool ok = false;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    else {
        DWORD exitCode = 1;
        ok = GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ok;
}

/**
 * @brief 路径归一化：统一分隔符、去除尾部斜杠、展开 8.3 短文件名
 *        用于安全前缀比较，防止用 "C:/..." 或 "PROGRA~1" 绕过黑名单
 */
std::wstring NormalizePathForCompare(const std::wstring& path) {
    std::wstring p;
    p.reserve(path.size());
    for (wchar_t ch : path) p += (ch == L'/') ? L'\\' : ch;

    // 折叠连续分隔符，但必须保留前导的 \\（UNC 主机/共享与 \\?\ 前缀）：
    // 原实现无条件折叠，把 "\\server\share" 变成 "\server\share"，
    // 于是 IsDriveOrShareRoot 认不出 UNC 共享根，整棵网络共享可以被递归删除
    std::wstring folded;
    size_t i = 0;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        folded += L"\\\\";
        i = 2;
    }
    for (; i < p.size(); i++) {
        if (p[i] == L'\\' && i + 1 < p.size() && p[i + 1] == L'\\') continue;
        folded += p[i];
    }
    p.swap(folded);

    while (!p.empty() && p.back() == L'\\') p.pop_back();

    // 展开短名（对存在的路径有效，不存在的路径会尽量展开前缀部分）
    wchar_t longName[MAX_PATH * 4] = { 0 };
    DWORD n = GetLongPathNameW(p.c_str(), longName, (DWORD)(sizeof(longName) / sizeof(longName[0])));
    if (n > 0 && n < (DWORD)(sizeof(longName) / sizeof(longName[0]))) {
        p = longName;
        while (!p.empty() && p.back() == L'\\') p.pop_back();
    }
    return p;
}

/**
 * @brief 读取环境变量目录，失败时返回回退值
 */
static std::wstring GetEnvDir(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    return fallback;
}

/**
 * @brief 检查路径是否为系统关键路径（黑名单保护）
 *        保护前缀基于本机真实系统目录动态生成，与系统所在盘符无关
 */
bool IsSystemCriticalPath(const std::wstring& path) {
    std::wstring target = NormalizePathForCompare(path);
    if (target.empty()) return false;

    wchar_t sysRootBuf[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(sysRootBuf, MAX_PATH) == 0) {
        wcscpy_s(sysRootBuf, L"C:\\Windows");
    }
    std::wstring sysRoot = NormalizePathForCompare(sysRootBuf);

    wchar_t sysDriveBuf[MAX_PATH] = { 0 };
    if (GetSystemWindowsDirectoryW(sysDriveBuf, MAX_PATH) == 0) {
        wcscpy_s(sysDriveBuf, L"C:\\Windows");
    }
    std::wstring sysDrive = GetEnvDir(L"SystemDrive", L"C:");
    if (!sysDrive.empty() && sysDrive.back() != L'\\') sysDrive += L'\\';
    sysDrive = NormalizePathForCompare(sysDrive);

    std::wstring programData = NormalizePathForCompare(GetEnvDir(L"ProgramData", L"C:\\ProgramData"));
    std::wstring publicDir = NormalizePathForCompare(GetEnvDir(L"PUBLIC", L"C:\\Users\\Public"));

    std::vector<std::wstring> critical;
    critical.reserve(32);

    // 系统目录下的关键子目录
    const wchar_t* sysSubs[] = {
        L"\\System32", L"\\SysWOW64", L"\\System", L"\\Boot", L"\\WinSxS",
        L"\\Installer", L"\\Registration", L"\\LiveKernelReports",
        L"\\ServiceProfiles", L"\\assembly", L"\\Microsoft.NET",
    };
    for (const auto* s : sysSubs) critical.push_back(sysRoot + s);

    // 引导/恢复分区目录
    const wchar_t* driveSubs[] = { L"\\Boot", L"\\EFI", L"\\Recovery" };
    for (const auto* s : driveSubs) critical.push_back(sysDrive + s);

    // ProgramData / 公共 / 默认用户配置
    critical.push_back(programData + L"\\Microsoft\\Windows\\Start Menu");
    critical.push_back(programData + L"\\Microsoft\\Windows\\Search");
    critical.push_back(programData + L"\\Package Cache");
    // 默认用户配置与公共目录（不能保护整个 Users，否则用户 Temp 无法清理）
    const wchar_t* usersSubs[] = { L"\\Users\\Default", L"\\Users\\Default User", L"\\Documents and Settings\\Default User" };
    for (const auto* s : usersSubs) critical.push_back(sysDrive + s);
    if (!publicDir.empty()) critical.push_back(publicDir);

    for (const auto& crit : critical) {
        if (crit.empty()) continue;
        size_t critLen = crit.length();
        if (target.length() < critLen) continue;
        if (_wcsnicmp(target.c_str(), crit.c_str(), (int)critLen) != 0) continue;
        if (target.length() == critLen) return true;
        if (target[critLen] == L'\\') return true;
    }
    return false;
}

/**
 * @brief 检查文件扩展名是否在受保护列表中
 */
bool IsProtectedDataFile(const std::wstring& fileName) {
    if (fileName.length() >= 11) {
        std::wstring prefix = fileName.substr(0, 11);
        std::wstring lowerPrefix = prefix;
        for (wchar_t& c : lowerPrefix) c = towlower(c);
        if (lowerPrefix == L"thumbcache_") return false;
    }

    size_t dotPos = fileName.rfind(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = fileName.substr(dotPos);
    for (wchar_t& c : ext) c = towlower(c);

    const wchar_t* protectedExts[] = {
        L".db",
        L".dat",
        L".sqlite",
        L".ldb",
        L".idb",
        L".ini",
        L".cfg",
        L".json",
        L".xml",
        L".config",
        L".log2",
        L".manifest",
    };
    for (const auto& pe : protectedExts) {
        if (ext == pe) return true;
    }
    return false;
}

/**
 * @brief 检查路径是否包含路径遍历序列 ".."
 */
bool IsPathTraversal(const std::wstring& path) {
    size_t pos = 0;
    while ((pos = path.find(L"..", pos)) != std::wstring::npos) {
        bool leftOk = (pos == 0 || path[pos - 1] == L'\\' || path[pos - 1] == L'/');
        bool rightOk = (pos + 2 >= path.size() || path[pos + 2] == L'\\' || path[pos + 2] == L'/');
        if (leftOk && rightOk) return true;
        pos++;
    }
    return false;
}

/**
 * @brief 将子目录通配模式解析为路径段列表
 */
std::vector<std::wstring> ParseSubDirPattern(const std::wstring& pattern) {
    std::vector<std::wstring> segments;
    std::wstring current;
    for (wchar_t ch : pattern) {
        if (ch == L'\\' || ch == L'/') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

/**
 * @brief 检查相对路径是否匹配子目录通配模式
 */
bool MatchSubDirPattern(const std::wstring& relPath, const std::wstring& pattern) {
    std::vector<std::wstring> patternSegs = ParseSubDirPattern(pattern);
    std::vector<std::wstring> pathSegs = ParseSubDirPattern(relPath);

    if (patternSegs.empty() || pathSegs.empty()) return false;
    if (patternSegs.size() > pathSegs.size()) return false;

    size_t offset = pathSegs.size() - patternSegs.size();
    for (size_t i = 0; i < patternSegs.size(); i++) {
        const auto& patSeg = patternSegs[i];
        const auto& dirSeg = pathSegs[offset + i];

        if (patSeg == L"*") {
            if (dirSeg.empty()) return false;
        } else {
            if (_wcsicmp(patSeg.c_str(), dirSeg.c_str()) != 0) return false;
        }
    }
    return true;
}

/**
 * @brief 在基础目录下递归查找所有匹配通配模式的子目录
 */
std::vector<std::wstring> FindMatchingSubDirs(const std::wstring& baseDir,
                                               const std::vector<std::wstring>& patterns) {
    std::vector<std::wstring> result;

    if (patterns.empty() || !PathFileExistsW(baseDir.c_str())) return result;

    std::function<void(const std::wstring&, int)> traverse = [&](const std::wstring& currentDir, int depth) {
        if (depth > 10) return;

        std::wstring relPath = currentDir;
        if (_wcsnicmp(currentDir.c_str(), baseDir.c_str(), (int)baseDir.length()) == 0) {
            relPath = currentDir.substr(baseDir.length());
            while (!relPath.empty() && (relPath[0] == L'\\' || relPath[0] == L'/')) {
                relPath = relPath.substr(1);
            }
        }

        for (const auto& pattern : patterns) {
            if (MatchSubDirPattern(relPath, pattern)) {
                result.push_back(currentDir);
                break;
            }
        }

        WIN32_FIND_DATAW fd;
        std::wstring searchPath = currentDir + L"\\*.*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                // 跳过符号链接/junction：否则 Cache 之类的链接会把扫描/清理
                // 带到用户数据目录，删除目标完全在预期目录之外
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                traverse(currentDir + L"\\" + fd.cFileName, depth + 1);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    };

    traverse(baseDir, 0);
    return result;
}

/**
 * @brief 扫描单个目录下匹配 pattern 的文件，返回匹配文件列表
 */
void ListFilesInDirectory(const std::wstring& dir, const std::wstring& pattern,
                           bool protectData, std::vector<FileInfo>& files) {
    WIN32_FIND_DATAW fd;
    std::wstring searchPath = dir + L"\\" + pattern;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        if (protectData && IsProtectedDataFile(fd.cFileName)) continue;

        ULARGE_INTEGER li;
        li.LowPart = fd.nFileSizeLow;
        li.HighPart = fd.nFileSizeHigh;

        files.push_back({ fd.cFileName, (long long)li.QuadPart });
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

/**
 * @brief 从 WIN32_FIND_DATA 正确还原 64 位文件大小
 *        注意：必须用 high<<32|low，用 high*MAXDWORD 会产生偏差
 */
long long FileSizeFromFindData(const WIN32_FIND_DATAW* fd) {
    ULARGE_INTEGER li;
    li.LowPart = fd->nFileSizeLow;
    li.HighPart = fd->nFileSizeHigh;
    return (long long)li.QuadPart;
}

/**
 * @brief 强制删除文件：先清除只读/隐藏/系统属性再删除
 *        只读属性不代表文件被占用，缓存类只读文件必须能清理
 */
bool ForceDeleteFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;

    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;   // 已不存在视为删除成功

    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        SetFileAttributesW(filePath.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    if (DeleteFileW(filePath.c_str())) return true;

    // 属性清除后仍失败，再尝试一次只读打开后删除（处理句柄已关闭但延迟删除的情况）
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(filePath.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(filePath.c_str())) return true;
    }
    SetLastError(err);
    return false;
}

/**
 * @brief 强制删除目录（要求目录已空）
 */
bool ForceRemoveDirectory(const std::wstring& dirPath) {
    if (dirPath.empty()) return false;
    DWORD attrs = GetFileAttributesW(dirPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        SetFileAttributesW(dirPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    return RemoveDirectoryW(dirPath.c_str()) ? true : false;
}

/**
 * @brief 安全字符串追加：超出缓冲区容量时静默截断
 *        wcscat_s 在参数无效时会调用 invalid parameter handler 直接终止进程，
 *        拼接大量长文件名（如卸载确认框）时必须使用本函数
 */
void SafeAppend(wchar_t* buffer, size_t bufferCount, const wchar_t* text) {
    if (buffer == NULL || bufferCount == 0) return;
    buffer[bufferCount - 1] = L'\0';
    if (text == NULL) return;

    size_t used = 0;
    const wchar_t* existing = buffer;
    while (used + 1 < bufferCount && existing[used] != L'\0') used++;

    size_t remain = bufferCount - 1 - used;   // 可写入的字符数（末尾保留终止符）
    if (remain == 0) return;

    size_t i = 0;
    while (i < remain && text[i] != L'\0') {
        buffer[used + i] = text[i];
        i++;
    }
    buffer[used + i] = L'\0';
}

/**
 * @brief 安全复制字符串：截断而非终止进程
 */
void SafeCopy(wchar_t* buffer, size_t bufferCount, const wchar_t* text) {
    if (buffer == NULL || bufferCount == 0) return;
    if (text == NULL) { buffer[0] = L'\0'; return; }
    size_t i = 0;
    for (; i + 1 < bufferCount && text[i] != L'\0'; i++) buffer[i] = text[i];
    buffer[i] = L'\0';
}

/**
 * @brief 校验残留目录名是否可安全用于拼接删除路径
 *        拒绝空名、"."、".."、含路径分隔符/通配符/盘符的名称
 */
bool IsSafeResidualName(const std::wstring& name) {
    if (name.empty()) return false;
    if (name == L"." || name == L"..") return false;
    if (name.find(L'\\') != std::wstring::npos) return false;
    if (name.find(L'/') != std::wstring::npos) return false;
    if (name.find(L':') != std::wstring::npos) return false;
    if (name.find(L'*') != std::wstring::npos) return false;
    if (name.find(L'?') != std::wstring::npos) return false;
    if (name.find(L"..") != std::wstring::npos) return false;
    // 必须含至少一个可见非空白字符
    bool hasVisible = false;
    for (wchar_t c : name) {
        if (c != L' ' && c != L'\t' && !iswspace(c)) { hasVisible = true; break; }
    }
    if (!hasVisible) return false;
    // 拒绝纯保留设备名
    std::wstring lower = name;
    for (wchar_t& c : lower) c = towlower(c);
    static const wchar_t* reserved[] = {
        L"con", L"prn", L"aux", L"nul",
        L"com1", L"com2", L"com3", L"com4",
        L"lpt1", L"lpt2", L"lpt3", L"lpt4",
    };
    for (const auto* r : reserved) if (lower == r) return false;
    return true;
}

/**
 * @brief 残留目录候选名是否会拼出「被多个软件共用」的目录
 *        残留候选来自注册表 DisplayName / Publisher / 安装目录末段，例如卸载某个
 *        Microsoft 产品时 Publisher="Microsoft"，直接拼接就会得到
 *        %APPDATA%\Microsoft、%LOCALAPPDATA%\Microsoft、%ProgramData%\Microsoft
 *        这类承载大量其它软件数据的公共目录，整棵删掉是灾难级误删。
 *
 *        与 IsSharedProgramRootPath 的区别：那个函数按「完整路径」判断是否等于
 *        共享根（因此不能把 Google/Intel 这类厂商名当黑名单，否则会挡住
 *        C:\Program Files\Google\Chrome 这类真实应用目录）；本函数只判断
 *        「AppData/ProgramData 下的第一层目录名」，作用域窄得多，可以安全地
 *        把厂商名与公共目录名一起拦下。
 */
bool IsSharedVendorDirName(const std::wstring& name) {
    if (name.empty()) return true;    // 空名会拼出 AppData 本身，一律拒绝

    std::wstring n = name;
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
    for (auto& c : n) c = (wchar_t)towlower(c);
    if (n.empty()) return true;

    static const wchar_t* sharedNames[] = {
        // Windows /  shell 自身会写入的目录
        L"microsoft", L"windows", L"microsoft windows", L"microsoft corporation",
        L"microsoft online services", L"microsoft one drive", L"microsoft onedrive",
        L"microsoft sdk", L"microsoft visual studio", L"microsoft devdiv",
        L"temp", L"tmp", L"packages", L"package cache", L"programs", L"common files",
        L"comms", L"connected devices platform", L"crashdumps", L"cre8",
        L"d3dscache", L"iconcache", L"inetcache", L"networklist", L"placeholderdb",
        L"publishers", L"styledll", L"tilestore", L"tokenstore", L"webcache",
        L"history", L"cookies", L"cache", L"saved games", L"start menu",
        L"documents", L"downloads", L"pictures", L"videos", L"music", L"appdata",
        // 常见厂商/平台级公共数据根：其下通常并存多个互不相关的产品
        L"google", L"google llc", L"apple", L"apple inc", L"apple computer",
        L"adobe", L"adobe systems", L"oracle", L"java", L"oracle corporation",
        L"intel", L"intel corporation", L"nvidia", L"nvidia corporation",
        L"amd", L"samsung", L"sony", L"vmware", L"virtualbox", L"mozilla",
        L"mozilla foundation", L"opera software", L"telegram", L"discord",
        L"steam", L"valve", L"ea", L"electronic arts", L"ubisoft", L"epic",
        L"epic games", L"unity", L"unity technologies", L"autodesk",
        L"jetbrains", L"python", L"python software foundation", L"nodejs",
        L"npm", L"nuget", L"pip", L"conda", L"git", L"gitkraken", L"docker",
        L"xming", L"cmake", L"qt project", L"tdi", L"thq", L"team17",
    };

    for (const auto* s : sharedNames) {
        if (n == s) return true;
    }
    return false;
}

// ====================== 强力卸载安全护栏 ======================

/**
 * @brief 判断 child 是否位于 parent 目录之内（含两者相等）
 *        归一化分隔符与 8.3 短名后按「整段」前缀比较：
 *          - "C:\AB"   不算 "C:\A" 的子项（原实现用 find 子串匹配会误判）
 *          - "C:/A/B"  算 "C:\A" 的子项
 *        parent 为空时一律返回 false：空串是任意串的前缀，
 *        否则注册表里 InstallLocation 为空就会匹配上所有路径。
 */
bool IsPathUnderDirectory(const std::wstring& child, const std::wstring& parent) {
    if (child.empty() || parent.empty()) return false;
    std::wstring c = NormalizePathForCompare(child);
    std::wstring p = NormalizePathForCompare(parent);
    if (c.empty() || p.empty() || c.size() < p.size()) return false;
    if (_wcsnicmp(c.c_str(), p.c_str(), (int)p.size()) != 0) return false;
    if (c.size() == p.size()) return true;
    return c[p.size()] == L'\\';
}

/**
 * @brief 展开字符串中的环境变量（%SystemRoot% 等）
 *        服务 ImagePath 多为 REG_EXPAND_SZ，不展开就与真实目录比较必然不相等，
 *        导致「装在某目录下的服务」永远匹配不到。展开失败时保留原值。
 */
bool ExpandEnvVars(const std::wstring& in, std::wstring& out) {
    out = in;
    if (in.empty()) return false;
    wchar_t buf[MAX_PATH * 4] = { 0 };
    DWORD n = ExpandEnvironmentStringsW(in.c_str(), buf, (DWORD)_countof(buf));
    if (n == 0 || n >= (DWORD)_countof(buf)) return false;
    out = buf;
    return true;
}

/**
 * @brief 拆分卸载命令行，得到可执行文件与参数
 *        注册表 UninstallString 常见三种写法：
 *          1. "C:\Program Files\App\unins000.exe" /SILENT   （带引号）
 *          2. C:\Program Files\App\unins000.exe /SILENT     （含空格却没引号，真实存在）
 *          3. msiexec.exe /x {GUID} /qn                     （靠 PATH 解析）
 *        原实现按第一个空格切分，第 2 种会得到 "C:\Program" 而直接失败。
 *        这里取「最长的真实存在的文件前缀」，都不存在时退回第一个 token。
 */
bool SplitUninstallCommand(const std::wstring& cmd, std::wstring& outExe, std::wstring& outParams) {
    outExe.clear();
    outParams.clear();

    size_t start = 0;
    while (start < cmd.size() && (cmd[start] == L' ' || cmd[start] == L'\t')) start++;
    if (start >= cmd.size()) return false;

    if (cmd[start] == L'"') {
        size_t end = cmd.find(L'"', start + 1);
        if (end == std::wstring::npos) {
            outExe = cmd.substr(start + 1);          // 引号未闭合：整段当路径
        }
        else {
            outExe = cmd.substr(start + 1, end - start - 1);
            size_t p = cmd.find_first_not_of(L" \t\"", end + 1);
            if (p != std::wstring::npos) outParams = cmd.substr(p);
        }
        return !outExe.empty();
    }

    size_t firstEnd = cmd.find(L' ', start);
    if (firstEnd == std::wstring::npos) {
        outExe = cmd.substr(start);
        return !outExe.empty();
    }

    std::wstring best;
    size_t bestEnd = std::wstring::npos;
    size_t pos = start;
    while (pos < cmd.size()) {
        size_t sp = cmd.find(L' ', pos);
        size_t stop = (sp == std::wstring::npos) ? cmd.size() : sp;
        std::wstring cand = cmd.substr(start, stop - start);
        if (!cand.empty()) {
            DWORD attr = GetFileAttributesW(cand.c_str());
            // 只认「存在的文件」：目录会让 ShellExecuteEx 打开资源管理器而不是卸载
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                best = cand;          // 继续向后取更长前缀，最终保留最长匹配
                bestEnd = stop;
            }
        }
        if (sp == std::wstring::npos) break;
        pos = sp + 1;
    }

    size_t paramsStart = bestEnd;
    if (best.empty()) {
        outExe = cmd.substr(start, firstEnd - start);
        paramsStart = firstEnd;
    }
    else {
        outExe = best;
    }
    size_t p = cmd.find_first_not_of(L" \t", paramsStart);
    if (p != std::wstring::npos) outParams = cmd.substr(p);
    return !outExe.empty();
}

/**
 * @brief 该路径是否为「共享根 / 公共目录」——即使软件把它写成 InstallLocation
 *        也绝不能整体删除。原实现只挡 IsSystemCriticalPath，
 *        像 C:\Program Files、%LOCALAPPDATA%、%ProgramData%\Microsoft 都能被整棵删掉。
 */
bool IsSharedProgramRootPath(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring target = NormalizePathForCompare(path);
    if (target.empty()) return false;

    std::vector<std::wstring> roots;
    auto addRoot = [&roots](const std::wstring& r) {
        if (r.empty()) return;
        std::wstring n = NormalizePathForCompare(r);
        if (!n.empty()) roots.push_back(n);
    };

    wchar_t buf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) addRoot(buf);
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH)) addRoot(buf);
    if (GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH)) addRoot(buf);
    if (GetEnvironmentVariableW(L"PUBLIC", buf, MAX_PATH)) addRoot(buf);
    if (GetWindowsDirectoryW(buf, MAX_PATH)) addRoot(buf);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, buf))) {
        addRoot(buf);
        // 用户目录根（通常 X:\Users）：只挡 profile 本身会漏掉整个用户配置根，
        // 删掉它等于抹掉所有用户的数据
        std::wstring profile(buf);
        size_t sep = profile.find_last_of(L"\\");
        if (sep != std::wstring::npos && sep > 2) addRoot(profile.substr(0, sep));
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf))) {
        addRoot(buf);
        addRoot(std::wstring(buf) + L"\\Programs");   // 用户级软件的公共安装根
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, buf))) addRoot(buf);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, buf))) addRoot(buf);

    for (const auto& r : roots) {
        if (_wcsicmp(target.c_str(), r.c_str()) == 0) return true;
    }

    // 共享根的「直接子项」且名称是众所周知的公共/厂商目录：
    // 只挡直接子项，C:\Program Files\Google\Chrome 这类真实应用目录仍可正常删除
    // 注意：不要把厂商名（Google/Apple/Intel/NVIDIA 等）放进来，
    // 它们既可能是共享数据目录，也可能是软件自己的安装目录，误拦会导致无法清理
    static const wchar_t* sharedNames[] = {
        L"Common Files", L"Common", L"Microsoft", L"Microsoft Office", L"Microsoft Office 15",
        L"Microsoft OneDrive", L"Microsoft Update", L"Microsoft.NET", L"Microsoft Shared",
        L"Internet Explorer", L"Windows Defender", L"Windows Defender Advanced Threat Protection",
        L"Windows Media Player", L"Windows NT", L"Windows Security", L"Windows Sidebar",
        L"Windows Photo Viewer", L"WindowsAudio", L"WindowsApps", L"ModifiableWindowsApps",
        L"WindowsPowerShell", L"dotnet", L"MSBuild", L"Reference Assemblies", L"IIS Express",
        L"Packages", L"PeerDistRepub", L"Package Cache", L"Temp", L"Programs",
        L"AppData", L"Application Data", L"Local Settings", L"Start Menu", L"SendTo",
        L"Templates", L"Recent", L"Cookies", L"History", L"NetHood", L"PrintHood",
        L"Documents", L"Downloads", L"Desktop", L"Pictures", L"Videos", L"Music",
        L"Favorites", L"Links", L"Saved Games", L"Searches", L"Contacts", L"OneDrive",
        L"Users", L"ProgramData", L"Windows", L"WinSxS", L"System32", L"SysWOW64",
    };

    for (const auto& r : roots) {
        if (r.empty() || target.size() <= r.size() + 1) continue;
        if (_wcsnicmp(target.c_str(), r.c_str(), (int)r.size()) != 0) continue;
        if (target[r.size()] != L'\\') continue;
        std::wstring rest = target.substr(r.size() + 1);
        size_t sep = rest.find(L'\\');
        if (sep != std::wstring::npos) continue;          // 只判断直接子项
        for (const auto* s : sharedNames) {
            if (_wcsicmp(rest.c_str(), s) == 0) return true;
        }
    }
    return false;
}

/**
 * @brief 该 hive 是否为本程序允许操作的注册表根
 *        原实现只区分「== HKCU」与「其它」，句柄为 NULL 或栈垃圾时会被当成 HKLM
 *        直接放行（UninstallItem 默认构造前的随机值即属此类），
 *        必须显式白名单，未知句柄一律拒绝。
 */
static bool IsSupportedRegistryHive(HKEY hive) {
    return hive == HKEY_LOCAL_MACHINE || hive == HKEY_CURRENT_USER ||
        hive == HKEY_USERS || hive == HKEY_CLASSES_ROOT ||
        hive == HKEY_CURRENT_CONFIG;
}

/**
 * @brief 综合判定：该注册表项是否允许被「整棵递归删除」
 *        删除入口的 keyPath 来自注册表枚举结果（卸载残留子键名、服务键名），
 *        也可能被后续功能复用；一旦拿到 SOFTWARE、SYSTEM\CurrentControlSet\Services
 *        这类容器键，RegDeleteTreeW 会把整棵子树抹掉 —— 等价于重装系统。
 *        目录侧有 IsSafeToDeleteDirectory 做总闸，注册表侧此前完全没有对应保护。
 *
 *        判定分三层（缺一层都会出问题）：
 *          1. allowedParents：容器键的「直接子项」是合法清理对象
 *             （Uninstall\AppID 子键、Services 下的第三方服务键、Run 下的启动项），
 *             只放行直接子项，更深层仍按下面两层处理。
 *          2. protectedRoot*：这些键本身绝不能被整棵删除。
 *          3. protectedSubtree*：这些键「内部」任何层级都不能删。
 *             原实现只有第 2 层且按「段数相等」比较，于是
 *             SOFTWARE\Classes\.exe、...\CurrentVersion\Policies\System、
 *             SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\... 全部放行，
 *             删掉即文件关联失效 / 组策略被清 / 用户配置损坏。
 */
bool IsSafeToDeleteRegistryKey(HKEY hive, const std::wstring& keyPath) {
    if (keyPath.empty()) return false;
    if (!IsSupportedRegistryHive(hive)) return false;

    // 归一化：统一分隔符、折叠重复分隔符、去尾部分隔符、去首尾空白
    std::wstring p;
    p.reserve(keyPath.size());
    for (wchar_t ch : keyPath) p += (ch == L'/') ? L'\\' : ch;
    std::wstring folded;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == L'\\' && i + 1 < p.size() && p[i + 1] == L'\\') continue;
        folded += p[i];
    }
    p.swap(folded);
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    size_t b = p.find_first_not_of(L" \t");
    size_t e = p.find_last_not_of(L" \t");
    if (b == std::wstring::npos) return false;
    p = p.substr(b, e - b + 1);
    if (p.empty()) return false;

    // 按段比较，段内忽略大小写；命中「受保护容器键」即拒绝
    auto segments = [](const std::wstring& s) {
        std::vector<std::wstring> out;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t sep = s.find(L'\\', pos);
            size_t stop = (sep == std::wstring::npos) ? s.size() : sep;
            std::wstring seg = s.substr(pos, stop - pos);
            for (auto& c : seg) c = (wchar_t)towlower(c);
            out.push_back(seg);
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }
        return out;
    };

    static const wchar_t* allowedParentsHKLM[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SYSTEM\\CurrentControlSet\\Services",
        L"SYSTEM\\ControlSet001\\Services",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths",
    };
    static const wchar_t* allowedParentsHKCU[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
    };

    static const wchar_t* protectedRootHKLM[] = {
        L"SOFTWARE",
        L"SYSTEM",
        L"SYSTEM\\CurrentControlSet",
        L"SYSTEM\\ControlSet001",
        L"SYSTEM\\CurrentControlSet\\Services",
        L"SYSTEM\\ControlSet001\\Services",
        L"SYSTEM\\CurrentControlSet\\Control",
        L"SYSTEM\\CurrentControlSet\\Enum",
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
        L"SOFTWARE\\Classes",
        L"SOFTWARE\\Microsoft",
        L"SOFTWARE\\Microsoft\\Windows",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies",
        L"SOFTWARE\\Policies",
        L"SOFTWARE\\WOW6432Node",
        L"SOFTWARE\\WOW6432Node\\Microsoft",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Explorer",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Policies",
    };
    static const wchar_t* protectedRootHKCU[] = {
        L"SOFTWARE",
        L"SOFTWARE\\Classes",
        L"SOFTWARE\\Microsoft",
        L"SOFTWARE\\Microsoft\\Windows",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies",
        L"SOFTWARE\\Policies",
        L"CONTROL PANEL",
        L"Environment",
    };

    // 整棵子树保护：这些键下面任何层级都拒绝删除（含其直接子项）
    static const wchar_t* protectedSubtreeHKLM[] = {
        L"SOFTWARE\\Classes",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies",
        L"SOFTWARE\\Policies",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\Profiles",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SVC",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\DriverDownloads",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\PrefetchPaths",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SideBy3",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SideBySide",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Winlogon",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers",
        L"SYSTEM\\CurrentControlSet\\Control",
        L"SYSTEM\\CurrentControlSet\\Enum",
        L"SYSTEM\\CurrentControlSet\\Hardware Profiles",
        L"SYSTEM\\CurrentControlSet\\Services\\EventLog",
    };
    static const wchar_t* protectedSubtreeHKCU[] = {
        L"SOFTWARE\\Classes",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies",
        L"SOFTWARE\\Policies",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts",
    };

    // 系统关键服务：即使位于 Services 白名单之下也不允许删除其内部条目
    static const wchar_t* protectedSubtreeHKLMExtra[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip",
        L"SYSTEM\\CurrentControlSet\\Services\\AFD",
        L"SYSTEM\\CurrentControlSet\\Services\\NSI",
        L"SYSTEM\\CurrentControlSet\\Services\\Dhcp",
        L"SYSTEM\\CurrentControlSet\\Services\\Dnscache",
        L"SYSTEM\\CurrentControlSet\\Services\\RpcSs",
        L"SYSTEM\\CurrentControlSet\\Services\\LSM",
        L"SYSTEM\\CurrentControlSet\\Services\\Schedule",
        L"SYSTEM\\CurrentControlSet\\Services\\Winmgmt",
        L"SYSTEM\\CurrentControlSet\\Services\\BFE",
        L"SYSTEM\\CurrentControlSet\\Services\\mpssvc",
        L"SYSTEM\\CurrentControlSet\\Services\\ntfs",
        L"SYSTEM\\CurrentControlSet\\Services\\fltmgr",
        L"SYSTEM\\CurrentControlSet\\Services\\partmgr",
        L"SYSTEM\\CurrentControlSet\\Services\\volmgr",
        L"SYSTEM\\CurrentControlSet\\Services\\volsnap",
        L"SYSTEM\\CurrentControlSet\\Services\\mountmgr",
        L"SYSTEM\\CurrentControlSet\\Services\\ksecdd",
        L"SYSTEM\\CurrentControlSet\\Services\\ksecpkg",
        L"SYSTEM\\CurrentControlSet\\Services\\tm",
        L"SYSTEM\\CurrentControlSet\\Services\\VSS",
        L"SYSTEM\\CurrentControlSet\\Services\\swprv",
    };

    // 微软自有容器：位于其下的条目一律不删，除非其父级在 allowedParents 白名单里
    // （Uninstall\App Paths\Services 的直接子项就是正常清理对象）。
    // 原实现只比较「整串是否等于容器键」，SOFTWARE\Microsoft\WOW6432Node\Whatever
    // 这类深层键全部放行，等于允许删掉微软组件的任意子键。
    static const wchar_t* denyAncestor[] = {
        L"SOFTWARE\\Microsoft",
        L"SOFTWARE\\Policies",
        L"SOFTWARE\\Classes",
        L"SOFTWARE\\WOW6432Node\\Microsoft",
        L"SYSTEM\\CurrentControlSet\\Control",
        L"SYSTEM\\CurrentControlSet\\Enum",
    };

    // HKEY_USERS 下的路径以用户 SID 开头（S-1-5-21-x\Software\...）。
    // 保护名单都是相对 hive 根写的（SOFTWARE\...），若不剥掉首段 SID，
    // HKU\S-1-5-21-x\Software\Classes\.exe 这类键永远匹配不到保护规则而被放行。
    // 剥掉 SID 后按 HKCU 规则处理，语义与其它用户配置完全一致。
    bool isHKCU = (hive == HKEY_CURRENT_USER);
    if (hive == HKEY_USERS) {
        size_t sep = p.find(L'\\');
        if (sep == std::wstring::npos) return false;   // 只有 SID，视为容器键
        p = p.substr(sep + 1);
        if (p.empty()) return false;
        isHKCU = true;
    }
    const wchar_t** parents = isHKCU ? allowedParentsHKCU : allowedParentsHKLM;
    size_t parentCount = isHKCU ? _countof(allowedParentsHKCU) : _countof(allowedParentsHKLM);
    const wchar_t** roots = isHKCU ? protectedRootHKCU : protectedRootHKLM;
    size_t rootCount = isHKCU ? _countof(protectedRootHKCU) : _countof(protectedRootHKLM);
    const wchar_t** subtrees = isHKCU ? protectedSubtreeHKCU : protectedSubtreeHKLM;
    size_t subtreeCount = isHKCU ? _countof(protectedSubtreeHKCU) : _countof(protectedSubtreeHKLM);

    std::vector<std::wstring> target = segments(p);

    // 前缀（按段）匹配：equal=false 表示「严格位于其下」
    auto under = [&target, &segments](const wchar_t* prefix, bool equal) {
        std::vector<std::wstring> prot = segments(prefix);
        if (target.size() < prot.size()) return false;
        if (!equal && target.size() == prot.size()) return false;
        for (size_t i = 0; i < prot.size(); i++) {
            if (target[i] != prot[i]) return false;
        }
        return true;
    };
    auto equalsSegs = [&under, &segments, &target](const wchar_t* key) {
        return under(key, true) && target.size() == segments(key).size();
    };

    // 1) 整棵子树保护（含关键服务内部）
    for (size_t r = 0; r < subtreeCount; r++) {
        if (under(subtrees[r], true)) return false;
    }
    if (!isHKCU) {
        for (size_t r = 0; r < _countof(protectedSubtreeHKLMExtra); r++) {
            if (under(protectedSubtreeHKLMExtra[r], true)) return false;
        }
    }

    // 2) 白名单容器的子项（Uninstall\MyApp、Services\MySvc、Run\MyApp）放行
    for (size_t a = 0; a < parentCount; a++) {
        if (under(parents[a], false)) return true;
    }

    // 3) 受保护容器键本身
    for (size_t r = 0; r < rootCount; r++) {
        if (equalsSegs(roots[r])) return false;
    }

    // 4) 微软/系统容器内部
    for (size_t r = 0; r < _countof(denyAncestor); r++) {
        if (under(denyAncestor[r], true)) return false;
    }

    return true;
}

/**
 * @brief 归一化后是否为盘符根 / UNC 共享根（"C:"、"\\srv\share"）
 */
static bool IsDriveOrShareRoot(const std::wstring& normalized) {
    if (normalized.size() <= 2) return true;
    if (normalized[0] == L'\\' && normalized[1] == L'\\') {
        size_t p1 = normalized.find(L'\\', 2);
        if (p1 == std::wstring::npos) return true;         // \\server
        return normalized.find(L'\\', p1 + 1) == std::wstring::npos; // \\server\share
    }
    return normalized.find(L'\\', 2) == std::wstring::npos;
}

/**
 * @brief 综合判定：该目录是否允许被「整体递归删除」
 *        强力卸载/残留清理的最后一道闸，任何递归删除入口都必须先过这里。
 */
bool IsSafeToDeleteDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (IsPathTraversal(path)) return false;               // 禁止 .. 绕过黑名单
    std::wstring n = NormalizePathForCompare(path);
    if (n.empty() || IsDriveOrShareRoot(n)) return false;
    if (IsSharedProgramRootPath(path)) return false;
    if (IsSystemCriticalPath(path)) return false;
    return true;
}

/**
 * @brief 服务 ImagePath 是否属于 Windows 自带组件（驱动/系统服务）
 *        强力卸载「驱动」会直接删除 Services 下的服务键，而扫描把 Start=2/3
 *        的服务全部列为驱动（含 ntfs、fltmgr、Wdf01000 等），
 *        删掉这些会直接蓝屏/无法启动，必须按 ImagePath 归属拦下。
 */
bool IsSystemComponentImagePath(const std::wstring& imagePath) {
    if (imagePath.empty()) return true;    // 读不到 ImagePath 时按最保守处理

    std::wstring img;
    if (!ExpandEnvVars(imagePath, img)) img = imagePath;

    // 相对写法：\SystemRoot\System32\drivers\xxx.sys、\System32\...
    std::wstring head = img;
    for (auto& c : head) c = (wchar_t)towlower(c);
    if (head.compare(0, 11, L"\\systemroot\\") == 0) return true;
    if (head.compare(0, 10, L"\\system32\\") == 0) return true;

    // 统一交给 ResolveDriverFilePath 解析：它会处理 \??\ 设备前缀、
    // 包裹引号与尾部参数；原实现只看第一个空格前的内容且不认 \??\，
    // 导致 \??\C:\Program Files\App\x.sys 这类第三方驱动被误判为系统组件
    std::wstring abs = ResolveDriverFilePath(imagePath);
    if (abs.empty()) return true;   // 相对路径（svchost.exe -k ... 等）由系统目录解析
    return IsPathUnderDirectory(abs, GetWindowsDirectorySafe());
}

/**
 * @brief 取 Windows 目录（失败回退 C:\Windows）
 */
std::wstring GetWindowsDirectorySafe() {
    wchar_t buf[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(buf, MAX_PATH) == 0) return L"C:\\Windows";
    return buf;
}

/**
 * @brief 解析服务 ImagePath 为驱动文件绝对路径
 *        常见形式：\SystemRoot\System32\drivers\x.sys、\??\C:\...\x.sys、
 *        "C:\Program Files\App\app.sys"（可能带参数/引号）。
 *        无法得到绝对路径时返回空串，调用方据此跳过文件删除——
 *        宁可漏删，也不能把一个相对片段当成当前目录下的文件删掉。
 */
std::wstring ResolveDriverFilePath(const std::wstring& imagePath) {
    if (imagePath.empty()) return std::wstring();

    std::wstring img;
    if (!ExpandEnvVars(imagePath, img)) img = imagePath;

    auto cutPrefix = [](std::wstring& s, const wchar_t* prefix) -> bool {
        size_t n = wcslen(prefix);
        if (s.size() < n || _wcsnicmp(s.c_str(), prefix, (int)n) != 0) return false;
        s = s.substr(n);
        return true;
    };

    // 设备命名空间前缀（\??\ 与 \\.\，都是 4 个字符）
    cutPrefix(img, L"\\??\\");
    cutPrefix(img, L"\\\\.\\");
    cutPrefix(img, L"\\\\?\\");

    // 相对写法基于真实 Windows 目录展开（不能假定系统装在 C:）
    std::wstring winDir = GetWindowsDirectorySafe();
    if (cutPrefix(img, L"\\SystemRoot\\")) img = winDir + L"\\" + img;
    else if (cutPrefix(img, L"\\System32\\")) img = winDir + L"\\System32\\" + img;
    else if (cutPrefix(img, L"\\SysWOW64\\")) img = winDir + L"\\SysWOW64\\" + img;

    // 去掉包裹引号与尾部参数
    if (!img.empty() && img[0] == L'"') {
        size_t q = img.find(L'"', 1);
        img = (q == std::wstring::npos) ? img.substr(1) : img.substr(1, q - 1);
    }
    else if (img.find(L' ') != std::wstring::npos) {
        // 未加引号却含空格：直接按第一个空格截断会把
        // "C:\Program Files\App\app.sys"（注册表里没写引号，真实存在）截成 "C:\Program"。
        // 先整体试一次，再取「第一个真实存在的文件前缀」，都失败才退回第一个空格。
        std::wstring best;
        size_t pos = 0;
        while (pos < img.size()) {
            size_t sp = img.find(L' ', pos);
            size_t stop = (sp == std::wstring::npos) ? img.size() : sp;
            std::wstring cand = img.substr(0, stop);
            DWORD attr = GetFileAttributesW(cand.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                best = cand;              // 前缀已经是文件，再延长只会把参数卷进来
                break;
            }
            if (sp == std::wstring::npos) break;
            pos = sp + 1;
        }
        img = best.empty() ? img : best;   // 一个前缀都不存在：保留整串，宁可因路径不存在而跳过删除
    }

    // 必须是「盘符:\...」形式的绝对路径，否则宁可不动文件
    if (img.size() < 4 || img[1] != L':' || (img[2] != L'\\' && img[2] != L'/')) return std::wstring();
    return img;
}

/**
 * @brief 安全剥离启动项 .disabled 后缀，返回原始路径
 *        长度不足时安全返回 false，不会因 substr 下溢抛异常
 */
bool TryStripDisabledSuffix(const std::wstring& path, std::wstring& outOriginal) {
    const std::wstring suffix = L".disabled";
    if (path.length() <= suffix.length()) return false;
    if (!EndsWith(path, suffix)) return false;
    outOriginal = path.substr(0, path.length() - suffix.length());
    return !outOriginal.empty();
}

/**
 * @brief 计算文件内容摘要（全量 FNV-1a 128 位，分块读取）
 *        仅采样首尾字节的实现会把中间内容不同的文件误判为重复，导致用户删错文件
 */
std::wstring ComputeFileDigest(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return std::wstring();

    // 两组 64 位 FNV-1a，混合文件大小与总读取字节数
    unsigned long long h1 = 1469598103934665603ULL;
    unsigned long long h2 = 14695981039346656037ULL;
    const unsigned long long prime = 1099511628211ULL;

    const DWORD chunk = 128 * 1024;
    std::vector<BYTE> buf(chunk);
    unsigned long long total = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(hFile, buf.data(), chunk, &read, NULL) || read == 0) break;
        for (DWORD i = 0; i < read; i++) {
            h1 = (h1 ^ buf[i]) * prime;
            h2 = (h2 + buf[i]) * prime;
            h2 ^= (h1 >> 17);
        }
        total += read;
        if (read < chunk) break;
    }
    CloseHandle(hFile);

    wchar_t out[80] = { 0 };
    swprintf_s(out, L"%016llx-%016llx-%llu", h1, h2, total);
    return out;
}

/**
 * @brief 带深度上限的递归文件枚举，防止符号链接环/超深目录导致栈溢出
 */
void ListFilesRecursiveCapped(const std::wstring& dir, const std::wstring& pattern,
                              int maxDepth, std::vector<FileInfo>& files) {
    if (maxDepth < 0 || files.size() >= 200000) return;

    // 先枚举本层匹配文件；即使本层没有匹配文件（FindFirstFileW 失败），
    // 也必须继续递归子目录，否则"中间层无匹配、深层有匹配"的目录会被整体漏扫
    WIN32_FIND_DATAW fd;
    std::wstring search = dir + L"\\" + pattern;
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            files.push_back({ fd.cFileName, FileSizeFromFindData(&fd) });
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    WIN32_FIND_DATAW dd;
    std::wstring searchAll = dir + L"\\*";
    HANDLE hDir = FindFirstFileW(searchAll.c_str(), &dd);
    if (hDir == INVALID_HANDLE_VALUE) return;
    do {
        if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(dd.cFileName, L".") == 0 || wcscmp(dd.cFileName, L"..") == 0) continue;
        if (dd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 跳过链接，避免成环
        ListFilesRecursiveCapped(dir + L"\\" + dd.cFileName, pattern, maxDepth - 1, files);
    } while (FindNextFileW(hDir, &dd));
    FindClose(hDir);
}

/**
 * @brief 检查文件是否被其他进程占用
 *        ERROR_ACCESS_DENIED 通常只是只读属性，不代表被占用，
 *        否则只读缓存文件会被永久跳过而无法清理
 */
bool IsFileLocked(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return false;
    }

    DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) return true;
    if (err == ERROR_ACCESS_DENIED) {
        // 只读属性导致的拒绝访问不算被占用；目录同样不算
        DWORD attrs = GetFileAttributesW(filePath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return false;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return false;
        if (attrs & FILE_ATTRIBUTE_READONLY) return false;
        // 尝试以写权限打开仍失败，说明确实被其他进程以独占方式持有
        HANDLE hWrite = CreateFileW(filePath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hWrite != INVALID_HANDLE_VALUE) { CloseHandle(hWrite); return false; }
        DWORD err2 = GetLastError();
        return err2 == ERROR_SHARING_VIOLATION || err2 == ERROR_LOCK_VIOLATION;
    }
    return false;
}

/**
 * @brief 将清理失败记录写入日志文件
 */
void WriteCleanFailureLog() {
    if (g_cleanFailures.empty()) return;

    wchar_t stampBuf[64] = { 0 };
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(stampBuf, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    wchar_t fileNameBuf[64] = { 0 };
    swprintf_s(fileNameBuf, L"clean_failures_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);

    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = 0;
    std::wstring logPath = std::wstring(exePath) + L"\\" + fileNameBuf;

    HANDLE hFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // BOM 只在文件新建（首次写入）时写一次，避免每次追加都插入 BOM 造成乱码
    if (GetFileSize(hFile, NULL) == 0) {
        WORD bom = 0xFEFF;
        DWORD bomWritten = 0;
        WriteFile(hFile, &bom, 2, &bomWritten, NULL);
    }

    wchar_t line[1024];
    DWORD written = 0;
    for (const auto& fail : g_cleanFailures) {
        swprintf_s(line, L"[%s] %s - %s\r\n", stampBuf, fail.filePath.c_str(), fail.reason.c_str());
        WriteFile(hFile, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, NULL);
    }

    CloseHandle(hFile);
}

/**
 * @brief 开启当前进程的备份/还原特权
 *        RegSaveKeyExW / RegLoadKeyW 需要 SeBackupPrivilege / SeRestorePrivilege。
 *        管理员令牌里这两个权限「存在但默认未启用」，不 AdjustTokenPrivileges
 *        直接调用只会得到 ERROR_PRIVILEGE_NOT_HELD(1314)。
 */
static bool SetTokenPrivilege(const wchar_t* privilegeName, bool enable) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    bool ok = false;
    LUID luid = { 0 };
    if (LookupPrivilegeValueW(NULL, privilegeName, &luid)) {
        TOKEN_PRIVILEGES tp = { 0 };
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
        SetLastError(ERROR_SUCCESS);
        if (AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
            ok = GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    CloseHandle(hToken);
    return ok;
}

void EnableRegistryBackupPrivileges() {
    // SE_BACKUP_NAME/SE_RESTORE_NAME 是 TEXT 宏，未定义 UNICODE 的编译单元
    // （如基线测试）会展开成 char*，这里固定用宽字符字面量
    SetTokenPrivilege(L"SeBackupPrivilege", true);
    SetTokenPrivilege(L"SeRestorePrivilege", true);
}

/**
 * @brief 某视图下该注册表路径的「真实路径」
 *        64 位系统上 32 位视图的 HKLM\SOFTWARE 实际是 HKLM\SOFTWARE\WOW6432Node。
 *        扫描时用 KEY_WOW64_32KEY 枚举得到的 keyPath 仍是未加前缀的写法：
 *          - 展示给用户会指错位置（用户按显示路径根本找不到这个键）
 *          - 用 reg.exe 备份时导出的其实是 64 位视图，备份内容与被删对象无关
 *        因此展示与备份都必须走这个函数。
 */
std::wstring RegRealPath(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag) {
    static const std::wstring prefix = L"SOFTWARE\\WOW6432Node\\";
    if (hive != HKEY_LOCAL_MACHINE || viewFlag != KEY_WOW64_32KEY) return keyPath;

    // 已经带前缀则原样返回，避免二次叠加
    if (keyPath.size() >= prefix.size() &&
        _wcsnicmp(keyPath.c_str(), prefix.c_str(), (int)prefix.size()) == 0)
        return keyPath;

    static const std::wstring software = L"SOFTWARE\\";
    if (keyPath.size() >= software.size() &&
        _wcsnicmp(keyPath.c_str(), software.c_str(), (int)software.size()) == 0) {
        return software + L"WOW6432Node\\" + keyPath.substr(software.size());
    }
    return keyPath;
}

/**
 * @brief 该视图下 keyPath 是否真实存在
 *        删除前用来区分「删除失败」与「本来就不存在（已清理）」，
 *        否则用户会看到一堆假失败，而真正没删掉的项混在里面看不出来。
 */
bool RegKeyExistsInView(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag) {
    if (hive == NULL || keyPath.empty()) return false;
    HKEY hKey = NULL;
    LONG ret = RegOpenKeyExW(hive, keyPath.c_str(), 0, KEY_READ | viewFlag, &hKey);
    if (ret != ERROR_SUCCESS) return false;
    RegCloseKey(hKey);
    return true;
}

/**
 * @brief 卸载条目是否为系统组件 / 补丁条目（这类条目绝不能当残留删除）
 *        SystemComponent=1 是软件声明「系统组件」的标准标记，Windows 更新、
 *        VC++ 运行库、.NET 等都会带；ReleaseType/ParentKeyName 是 MSI 补丁
 *        （安全更新、累积更新）的标记。原实现完全没有排除，
 *        只要这类条目 InstallLocation 指向一个不存在的目录就会被列进清理列表，
 *        删掉之后组件无法再被卸载/修复，Windows Update 记录也会错乱。
 */
bool IsSystemOrPatchUninstallEntry(DWORD systemComponent, const std::wstring& releaseType,
    const std::wstring& parentKeyName) {
    if (systemComponent == 1) return true;
    if (!releaseType.empty()) return true;
    if (!parentKeyName.empty()) return true;
    return false;
}

/**
 * @brief 判断一条卸载记录是否属于「软件已卸载、注册表残留」
 *        原实现只看 InstallLocation 非空且 PathFileExistsW 为假，问题有三：
 *          1. 大量软件不写 InstallLocation（MSI 尤其常见），残留完全扫不到；
 *          2. InstallLocation 写成文件、或写成 "C:" 这类无意义值时也会命中；
 *          3. 卸载器（UninstallString）已经消失是更强的残留信号，原实现不看。
 *        本函数只做判定不做删除，命中时 reason 给出依据，便于列表展示与单测。
 *
 *        保守原则：任何「无法确定」的情况都返回 false（不列为残留）。
 */
bool IsUninstallEntryLeftover(const std::wstring& installLocation,
    const std::wstring& uninstallString, std::wstring& reason) {
    reason.clear();

    // 1) 安装目录明确写了却不存在 —— 最可靠的残留信号
    std::wstring loc = installLocation;
    while (!loc.empty() && (loc.back() == L'\\' || loc.back() == L'/')) loc.pop_back();
    size_t head = loc.find_first_not_of(L" \t");
    if (head == std::wstring::npos) loc.clear();
    else loc = loc.substr(head);

    if (!loc.empty()) {
        // 通配符 / 非法字符说明这个值本身不可信，无法据此判定
        if (loc.find_first_of(L"*?\"<>|") != std::wstring::npos) return false;

        DWORD attr = GetFileAttributesW(loc.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            reason = L"安装目录已不存在: " + loc;
            return true;
        }
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            reason = L"安装位置不是目录: " + loc;
            return true;
        }
        // 目录仍在：软件大概率还装着，不再看别的信号
        return false;
    }

    // 2) 没有 InstallLocation 时看卸载器是否还在
    std::wstring cmd = uninstallString;
    size_t chead = cmd.find_first_not_of(L" \t");
    if (chead == std::wstring::npos) return false;      // 两者都没有：无法判定
    cmd = cmd.substr(chead);

    std::wstring exe, params;
    if (!SplitUninstallCommand(cmd, exe, params) || exe.empty()) return false;

    // msiexec.exe /x {GUID}：卸载器是系统程序，永远存在，不构成信号
    std::wstring exeLower = exe;
    for (auto& c : exeLower) c = (wchar_t)towlower(c);
    if (exeLower == L"msiexec.exe" || exeLower == L"msiexec") return false;

    // 相对路径（靠 PATH 解析）无法可靠判定，放过
    bool absolute = (exe.size() >= 3 && exe[1] == L':' && (exe[2] == L'\\' || exe[2] == L'/')) ||
        (exe.size() >= 2 && exe[0] == L'\\' && exe[1] == L'\\');
    if (!absolute) return false;

    std::wstring expanded;
    if (ExpandEnvVars(exe, expanded)) exe = expanded;
    if (PathFileExistsW(exe.c_str())) return false;

    // MSI 缓存被清理属正常现象（Windows\Installer），不能据此判定软件残留
    wchar_t winDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring installerDir = std::wstring(winDir) + L"\\Installer";
    if (IsPathUnderDirectory(exe, installerDir)) return false;

    reason = L"卸载程序已不存在: " + exe;
    return true;
}

/**
 * @brief 判断 Run / RunOnce 启动值是否指向不存在的程序（无效启动项）
 *        只处理「绝对路径且确实不存在」这一种最明确的情况：
 *        相对名、regsvr32 之类系统程序、以及注册表里写了半截命令的情况一律放过。
 */
bool IsInvalidStartupCommand(const std::wstring& command, std::wstring& reason) {
    reason.clear();
    std::wstring cmd = command;
    size_t head = cmd.find_first_not_of(L" \t");
    if (head == std::wstring::npos) {
        reason = L"启动命令为空";
        return true;
    }
    cmd = cmd.substr(head);

    std::wstring exe, params;
    if (!SplitUninstallCommand(cmd, exe, params) || exe.empty()) return false;

    std::wstring expanded;
    if (ExpandEnvVars(exe, expanded)) exe = expanded;

    bool absolute = (exe.size() >= 3 && exe[1] == L':' && (exe[2] == L'\\' || exe[2] == L'/')) ||
        (exe.size() >= 2 && exe[0] == L'\\' && exe[1] == L'\\');
    if (!absolute) return false;

    if (exe.find_first_of(L"*?\"<>|") != std::wstring::npos) return false;
    if (PathFileExistsW(exe.c_str())) return false;

    reason = L"启动目标已不存在: " + exe;
    return true;
}

/**
 * @brief 备份单个注册表项到文件（reg.exe 优先，失败回退 RegSaveKeyExW）
 *        返回 false 表示「这个项没有备份成功」，调用方必须据此中止删除。
 */
bool BackupRegistryKey(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag,
    const std::wstring& outFile) {
    if (hive == NULL || keyPath.empty() || outFile.empty()) return false;

    // 项本身不存在时无需备份，也不算失败（删除同样不会造成任何损失）
    if (!RegKeyExistsInView(hive, keyPath, viewFlag)) return true;

    std::wstring realPath = RegRealPath(hive, keyPath, viewFlag);
    const wchar_t* hiveName = (hive == HKEY_CURRENT_USER) ? L"HKCU"
        : (hive == HKEY_CLASSES_ROOT) ? L"HKCR"
        : (hive == HKEY_USERS) ? L"HKU"
        : (hive == HKEY_CURRENT_CONFIG) ? L"HKCC" : L"HKLM";

    DeleteFileW(outFile.c_str());

    // reg.exe 导出的是可直接双击恢复的 .reg 文本，优先使用
    std::wstring cmd = L"reg export \"";
    cmd += hiveName;
    cmd += L"\\";
    cmd += realPath;
    cmd += L"\" \"";
    cmd += outFile;
    cmd += L"\" /y";
    if (ExecuteCmdChecked(cmd) && PathFileExistsW(outFile.c_str())) return true;

    DeleteFileW(outFile.c_str());

    // 回退：RegSaveKeyExW 导出配置单元文件（regedit「加载配置单元」可打开）
    EnableRegistryBackupPrivileges();
    HKEY hKey = NULL;
    if (RegOpenKeyExW(hive, keyPath.c_str(), 0, KEY_READ | viewFlag, &hKey) != ERROR_SUCCESS)
        return false;
    LONG ret = RegSaveKeyExW(hKey, outFile.c_str(), NULL, REG_STANDARD_FORMAT);
    RegCloseKey(hKey);

    return ret == ERROR_SUCCESS && PathFileExistsW(outFile.c_str());
}

/**
 * @brief 注册表备份目录（exe 所在目录下的 reg_backup），不存在时创建
 */
std::wstring GetRegBackupDir() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *slash = 0;
    std::wstring backupDir = std::wstring(exePath) + L"\\reg_backup";
    CreateDirectoryW(backupDir.c_str(), NULL);
    return backupDir;
}

/**
 * @brief 清理前备份注册表关键分支（备份目录基于 exe 所在目录，避免依赖当前工作目录）
 *        原实现用 ExecuteCmd 调 reg.exe 且完全丢弃结果：reg.exe 失败（项不存在、
 *        目录不可写、被拦截）时照样提示「备份已保存」，用户以为可恢复其实没有。
 *        现在两个视图（64 位与 WOW6432Node）都备份，并把失败项写进 failed 供调用方提示。
 */
bool BackupRegistry(std::vector<std::wstring>* failed) {
    wchar_t timeBuf[64] = { 0 };
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(timeBuf, L"reg_backup_%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring backupDir = GetRegBackupDir();

    struct Target {
        HKEY hive;
        const wchar_t* path;
        const wchar_t* tag;
    };
    // HKCU\Software 整体导出常有几十 MB 且耗时数十秒（UI 线程会假死），
    // 逐项备份已经覆盖真正要删的键，这里只备份两个 Uninstall 根作为兜底。
    static const Target targets[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"HKLM_Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"HKLM_Uninstall_32" },
    };

    bool allOk = true;
    for (const auto& t : targets) {
        REGSAM view = (wcsstr(t.tag, L"_32") != NULL) ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
        std::wstring file = backupDir + L"\\" + timeBuf + L"_" + t.tag + L".reg";
        if (BackupRegistryKey(t.hive, t.path, view, file)) continue;
        allOk = false;
        if (failed) failed->push_back(std::wstring(L"注册表根备份失败: ") + t.path);
    }
    return allOk;
}

/**
 * @brief 检查当前进程是否以管理员权限运行
 */
bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID pAdminGroup = NULL;
    if (AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &pAdminGroup)) {
        CheckTokenMembership(NULL, pAdminGroup, &isAdmin);
        FreeSid(pAdminGroup);
    }
    return isAdmin == TRUE;
}

bool Is64BitProcess() {
#if defined(_WIN64)
    return true;
#else
    BOOL isWow64 = FALSE;
    typedef BOOL(WINAPI* IsWow64Process_t)(HANDLE, PBOOL);
    IsWow64Process_t pIsWow64Process = (IsWow64Process_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (pIsWow64Process && pIsWow64Process(GetCurrentProcess(), &isWow64)) {
        return isWow64 == TRUE;
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
#endif
}

REGSAM GetRegistryViewAccess() {
#if defined(_WIN64)
    return KEY_WOW64_64KEY;
#else
    if (Is64BitProcess()) {
        return KEY_WOW64_64KEY;
    }
    return KEY_WOW64_32KEY;
#endif
}