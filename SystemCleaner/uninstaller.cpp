// Module: uninstaller.cpp - Software scanning, force uninstall, and residual cleanup

#include "common.h"
#include <fstream>
#include <string>
#include <cwctype>
#include <cstring>

// Global variables are defined in SystemCleaner.cpp

// ====================== 卸载失败记录（线程安全） ======================
//
// g_uninstallFailures 由卸载/强卸工作线程写入，UI 线程在 WM_UNINSTALL_FINISHED
// 里读取并落盘。common.h 早就声明了 AddUninstallFailure 却没有实现，所有写入点
// 都直接 push_back：vector 扩容与 UI 线程的遍历并发 = 迭代器失效 / 读到半构造元素。
// 临界区用函数内 static 惰性初始化，避免依赖 wWinMain 的初始化顺序（单测宿主里
// 只 include 部分模块也能正常工作）。
static CRITICAL_SECTION& UninstallFailuresLock() {
    static CRITICAL_SECTION cs = [] {
        CRITICAL_SECTION c;
        InitializeCriticalSection(&c);
        return c;
    }();
    return cs;
}

void AddUninstallFailure(const std::wstring& appName, const std::wstring& reason) {
    EnterCriticalSection(&UninstallFailuresLock());
    g_uninstallFailures.push_back({ appName, reason });
    LeaveCriticalSection(&UninstallFailuresLock());
}

static void ClearUninstallFailures() {
    EnterCriticalSection(&UninstallFailuresLock());
    g_uninstallFailures.clear();
    LeaveCriticalSection(&UninstallFailuresLock());
}

// 扫描线程代际：每个扫描线程启动时记录自己的 generation，
// 所有取消判断都同时检查「取消标志」与「代际是否仍是自己」。
// 之前线程启动时直接 g_uninstallScanCancelled=false 会把 UI 线程刚为旧线程设置的
// 取消标志清掉，导致旧线程继续向已清空的列表追加过期数据。
static thread_local DWORD t_uninstallScanGen = 0;
static bool UninstallScanActive() {
    return !g_uninstallScanCancelled && t_uninstallScanGen == g_uninstallScanGeneration;
}

/**
 * @brief Case-insensitive substring search
 */
static bool ContainsTextInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = haystack;
    std::wstring n = needle;
    for (auto& c : h) c = (wchar_t)towlower(c);
    for (auto& c : n) c = (wchar_t)towlower(c);
    return h.find(n) != std::wstring::npos;
}

/**
 * @brief Determine the type of installed software based on install location
 */
static std::wstring DetermineAppType(const std::wstring& installLocation) {
    if (installLocation.empty()) return L"未知";

    wchar_t programFiles[MAX_PATH] = { 0 };
    wchar_t programFilesX86[MAX_PATH] = { 0 };
    wchar_t windowsDir[MAX_PATH] = { 0 };
    wchar_t userProfile[MAX_PATH] = { 0 };

    GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH);
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userProfile);

    // find(空串)==0 会把所有路径误判为系统软件，环境变量读取失败时必须跳过该前缀
    if ((windowsDir[0] != 0 && installLocation.find(windowsDir) == 0) ||
        (programFiles[0] != 0 && installLocation.find(programFiles) == 0) ||
        (programFilesX86[0] != 0 && installLocation.find(programFilesX86) == 0)) {
        return L"系统软件";
    }

    if (userProfile[0] != 0 && installLocation.find(userProfile) == 0) {
        return L"用户软件";
    }

    return L"未知";
}

/**
 * @brief Calculate directory size recursively
 */
static long long CalcDirectorySizeDepth(const std::wstring& path, int depth) {
    long long size = 0;
    // 深度上限 + 跳过重解析点，防止符号链接成环导致无限递归栈溢出
    if (path.empty() || depth > 40 || !PathFileExistsW(path.c_str())) return 0;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = path + L"\\*.*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    do {
        if (!UninstallScanActive()) {
            FindClose(hFind);
            return size;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0 &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                size += CalcDirectorySizeDepth(path + L"\\" + fd.cFileName, depth + 1);
            }
        }
        else {
            size += FileSizeFromFindData(&fd);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return size;
}

static long long CalcDirectorySize(const std::wstring& path) {
    return CalcDirectorySizeDepth(path, 0);
}

/**
 * @brief Read a string value from registry
 */
static std::wstring RegReadString(HKEY hKey, const wchar_t* valueName) {
    wchar_t data[1024] = { 0 };
    DWORD dataSize = sizeof(data);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, valueName, NULL, &type, (LPBYTE)data, &dataSize) == ERROR_SUCCESS) {
        return std::wstring(data);
    }
    return L"";
}

/**
 * @brief 判断条目记录的「安装位置」是否可安全用于批量终止/删除
 *        注册表 InstallLocation 由软件自行写入，可能是空、盘符根、
 *        C:\Windows、C:\Program Files 等；直接拿来做前缀匹配会
 *        误杀/误删整盘或整个系统目录下的所有进程与文件。
 */
static bool IsUsableInstallLocation(const std::wstring& loc) {
    if (loc.empty()) return false;
    // 服务条目的 ImagePath 是文件而非目录，不能按目录前缀匹配进程/服务
    DWORD attr = GetFileAttributesW(loc.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return IsSafeToDeleteDirectory(loc);
}

/**
 * @brief Kill all running processes whose executable path is inside the given directory
 */
static void KillProcessesInDirectory(const std::wstring& dirPath) {
    if (dirPath.empty()) return;
    // 危险目录（盘符根 / 共享根 / 系统目录）绝不据此终止进程：
    // 否则 InstallLocation 被写成 C:\Windows 时会把系统进程全部杀掉
    if (!IsUsableInstallLocation(dirPath)) return;

    std::wstring targetDir = dirPath;
    while (!targetDir.empty() && (targetDir.back() == L'\\' || targetDir.back() == L'/'))
        targetDir.pop_back();

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = { sizeof(pe32) };
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
            if (!hProcess) continue;

            // 可执行路径可能超过 MAX_PATH：固定缓冲区会让 QueryFullProcessImageNameW
            // 直接失败，于是被占用的进程杀不掉、目录也删不掉
            wchar_t exePath[MAX_PATH * 8] = { 0 };
            DWORD pathSize = (DWORD)_countof(exePath);
            if (QueryFullProcessImageNameW(hProcess, 0, exePath, &pathSize) && pathSize > 0) {
                if (IsPathUnderDirectory(exePath, targetDir)) {
                    // Skip our own process
                    if (pe32.th32ProcessID != GetCurrentProcessId()) {
                        TerminateProcess(hProcess, 0);
                    }
                }
            }
            CloseHandle(hProcess);
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

/**
 * @brief Stop and remove a Windows service by name
 *        等待服务停止用 200ms 细粒度轮询：原实现固定 Sleep(2000) 最多阻塞 60 秒，
 *        卸载多个服务时界面长时间无进度反馈，像卡死。
 */
static bool RemoveServiceByName(const std::wstring& serviceName) {
    if (serviceName.empty()) return false;

    SC_HANDLE hSCManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) return false;

    bool success = false;
    SC_HANDLE hService = OpenServiceW(hSCManager, serviceName.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);

    if (!hService) {
        CloseServiceHandle(hSCManager);
        return false;
    }

    SERVICE_STATUS_PROCESS status = { 0 };
    DWORD bytesNeeded = 0;
    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        if (status.dwCurrentState == SERVICE_STOPPED) {
            success = DeleteService(hService);
        }
        else {
            SERVICE_STATUS stopStatus;
            if (ControlService(hService, SERVICE_CONTROL_STOP, &stopStatus)) {
                for (int i = 0; i < 100; i++) {   // 最长 20 秒
                    Sleep(200);
                    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
                        if (status.dwCurrentState == SERVICE_STOPPED) break;
                    }
                }
            }
            // 即使仍在「停止中」也尝试标记删除：SCM 会在服务真正停止后完成删除。
            // 原实现只在 ControlService 成功的分支里 DeleteService，服务容易残留。
            if (DeleteService(hService)) success = true;
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    return success;
}

/**
 * @brief Find and remove services whose ImagePath points into the given directory
 */
static void RemoveServicesInDirectory(const std::wstring& dirPath) {
    // 危险目录（盘符根 / 共享根 / 系统目录）绝不据此删除服务：
    // 否则 InstallLocation 被写成 C:\Windows 会把系统服务全部标记删除
    if (!IsUsableInstallLocation(dirPath)) return;

    std::wstring dirNorm = NormalizePathForCompare(dirPath);
    if (dirNorm.size() < 8) return; // Minimum safety: paths like C:\X are too generic

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;

    wchar_t serviceName[256] = { 0 };
    DWORD nameSize;
    int index = 0;
    while (true) {
        nameSize = _countof(serviceName);
        LONG ret = RegEnumKeyExW(hKey, index, serviceName, &nameSize, NULL, NULL, NULL, NULL);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) { index++; continue; }

        std::wstring svcName(serviceName);
        HKEY hSubKey;
        if (RegOpenKeyExW(hKey, svcName.c_str(), 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
            std::wstring imagePath = RegReadString(hSubKey, L"ImagePath");
            RegCloseKey(hSubKey);

            if (!imagePath.empty()) {
                // ImagePath 多为 REG_EXPAND_SZ（%SystemRoot%\...），不展开就永远匹配不上
                std::wstring img;
                if (!ExpandEnvVars(imagePath, img)) img = imagePath;
                // 只取可执行文件部分：可能带参数，也可能整体被引号包住
                if (!img.empty() && img[0] == L'"') {
                    size_t q = img.find(L'"', 1);
                    img = (q == std::wstring::npos) ? img.substr(1) : img.substr(1, q - 1);
                }
                else {
                    size_t sp = img.find(L' ');
                    if (sp != std::wstring::npos) img = img.substr(0, sp);
                }

                // 整段目录前缀比较：原实现用 wcsstr 子串匹配，
                // "C:\App" 会命中 "D:\Other\C:\AppFake\svc.exe" 这类无关服务
                if (IsPathUnderDirectory(img, dirNorm)) {
                    RemoveServiceByName(svcName);
                }
            }
        }
        index++;
    }
    RegCloseKey(hKey);
}

/**
 * @brief Delete a registry key recursively using RegDeleteTreeW
 *
 * Opens the parent key, then calls RegDeleteTreeW to recursively delete
 * the target key and all its subkeys/values, then removes the now-empty key.
 */
static bool DeleteRegistryKeyRecursive(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag) {
    if (keyPath.empty()) return true;

    // 递归删除的总闸：keyPath 来自注册表枚举结果，一旦被写成 SOFTWARE、
    // SYSTEM\CurrentControlSet\Services 这类容器键，RegDeleteTreeW 会抹掉整棵子树
    if (!IsSafeToDeleteRegistryKey(hive, keyPath)) return false;

    // Find the parent key path and the subkey name
    size_t lastSep = keyPath.find_last_of(L"\\");
    if (lastSep == std::wstring::npos) return false;

    std::wstring parentPath = keyPath.substr(0, lastSep);
    std::wstring subKeyName = keyPath.substr(lastSep + 1);

    REGSAM access = KEY_READ | KEY_WRITE;
    if (viewFlag != 0) access |= viewFlag;

    HKEY hParent = NULL;
    LONG openRet = RegOpenKeyExW(hive, parentPath.c_str(), 0, access, &hParent);

    if (openRet == ERROR_ACCESS_DENIED) {
        // Try without view flag
        openRet = RegOpenKeyExW(hive, parentPath.c_str(), 0, KEY_READ | KEY_WRITE, &hParent);
    }

    if (openRet != ERROR_SUCCESS) {
        // Key doesn't exist - that's fine
        if (openRet == ERROR_FILE_NOT_FOUND || openRet == ERROR_PATH_NOT_FOUND)
            return true;

        // Try opposite view
        REGSAM altView = 0;
        if (viewFlag == KEY_WOW64_64KEY) altView = KEY_WOW64_32KEY;
        else if (viewFlag == KEY_WOW64_32KEY) altView = KEY_WOW64_64KEY;
        if (altView != 0) {
            openRet = RegOpenKeyExW(hive, parentPath.c_str(), 0, KEY_READ | KEY_WRITE | altView, &hParent);
        }

        if (openRet != ERROR_SUCCESS) {
            return (openRet == ERROR_FILE_NOT_FOUND || openRet == ERROR_PATH_NOT_FOUND);
        }
    }

    // Recursively delete the target key and everything under it
    LONG delRet = RegDeleteTreeW(hParent, subKeyName.c_str());
    RegCloseKey(hParent);

    if (delRet == ERROR_SUCCESS || delRet == ERROR_FILE_NOT_FOUND || delRet == ERROR_PATH_NOT_FOUND) {
        return true;
    }

    // Fallback: try to delete without view flag
    HKEY hParent2 = NULL;
    if (viewFlag != 0 && RegOpenKeyExW(hive, parentPath.c_str(), 0, KEY_READ | KEY_WRITE, &hParent2) == ERROR_SUCCESS) {
        delRet = RegDeleteTreeW(hParent2, subKeyName.c_str());
        RegCloseKey(hParent2);
        return (delRet == ERROR_SUCCESS || delRet == ERROR_FILE_NOT_FOUND || delRet == ERROR_PATH_NOT_FOUND);
    }

    return false;
}

/**
 * @brief Delete an App Paths registry entry for the given app name
 */
static void CleanAppPathsEntry(const std::wstring& exeName) {
    if (exeName.empty()) return;

    std::wstring nameLower = exeName;
    for (auto& c : nameLower) c = (wchar_t)towlower(c);

    static const wchar_t* appPathsPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";

    for (REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, appPathsPath, 0, KEY_READ | view, &hKey) == ERROR_SUCCESS) {
            wchar_t subName[256] = { 0 };
            DWORD nameSize;
            int idx = 0;
            while (true) {
                nameSize = _countof(subName);
                LONG ret = RegEnumKeyExW(hKey, idx, subName, &nameSize, NULL, NULL, NULL, NULL);
                if (ret == ERROR_NO_MORE_ITEMS) break;
                if (ret != ERROR_SUCCESS) { idx++; continue; }

                std::wstring subLower = subName;
                for (auto& c : subLower) c = (wchar_t)towlower(c);

                if (wcsstr(subLower.c_str(), nameLower.c_str())) {
                    std::wstring fullPath = std::wstring(appPathsPath) + L"\\" + subName;
                    DeleteRegistryKeyRecursive(HKEY_LOCAL_MACHINE, fullPath, view);
                }
                idx++;
            }
            RegCloseKey(hKey);
        }
    }
}

/**
 * @brief Recursively delete a directory with retry for locked files
 */
static bool DeleteDirectoryRecursiveDepth(const std::wstring& path, int depth) {
    if (path.empty() || depth > 40 || !PathFileExistsW(path.c_str())) return true;
    // 递归的每一层都重新判定：只挡顶层时，深层子目录若恰好是 junction 目标 /
    // 公共目录（如 AppData\Local\Microsoft）会被连带删掉
    if (!IsSafeToDeleteDirectory(path)) return false;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = path + L"\\*.*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool success = true;

    do {
        std::wstring fullPath = path + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                // 重解析点（符号链接/junction）只断开链接，绝不递归进入其目标，
                // 否则会顺着链接删到目录之外的真实数据
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (!RemoveDirectoryW(fullPath.c_str())) success = false;
                    continue;
                }
                if (!DeleteDirectoryRecursiveDepth(fullPath, depth + 1)) {
                    success = false;
                }
            }
        }
        else {
            bool deleted = false;
            for (int r = 0; r < 3; r++) {
                if (r > 0) Sleep(200 * r); // Progressive backoff: 200ms, 400ms, 600ms
                // 只读/隐藏/系统属性文件 DeleteFileW 会直接失败（ACCESS_DENIED），
                // 原实现把它当作「被占用」跳过，导致卸载后目录残留删不掉
                if (ForceDeleteFile(fullPath)) {
                    deleted = true;
                    break;
                }
                DWORD err = GetLastError();
                if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
                    break; // 真正被占用，跳过
                }
            }
            if (!deleted) {
                success = false;
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (success) {
        for (int r = 0; r < 3; r++) {
            if (r > 0) Sleep(200 * r);
            if (RemoveDirectoryW(path.c_str())) {
                return true;
            }
        }
        success = false;
    }
    return success;
}

static bool DeleteDirectoryRecursive(const std::wstring& path) {
    return DeleteDirectoryRecursiveDepth(path, 0);
}

/**
 * @brief Delete shortcuts (.lnk, .url) matching the app name from a directory tree
 */
static void DeleteMatchingShortcuts(const std::wstring& dir, const std::wstring& appName, int depth, int maxDepth) {
    if (depth > maxDepth) return;
    if (!PathFileExistsW(dir.c_str())) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = dir + L"\\*.*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::wstring lowerName = appName;
    for (auto& c : lowerName) c = (wchar_t)towlower(c);
    // 名称过短时（如驱动条目、单字母 DisplayName）子串匹配会命中大量无关快捷方式，
    // 直接跳过快捷方式清理，宁可留几个死链也不要误删用户其它软件的入口
    if (lowerName.size() < 3) { FindClose(hFind); return; }

    do {
        std::wstring fullPath = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                // 重解析点（junction/符号链接）绝不递归进入：
                // 开始菜单里存在指向用户目录的链接，跟进去会删到链接目标里的快捷方式
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                DeleteMatchingShortcuts(fullPath, appName, depth + 1, maxDepth);
            }
        }
        else {
            // Check if it's a shortcut file (.lnk or .url)
            const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
            // 扩展名可能长于缓冲区：wcscpy_s 溢出会触发 invalid parameter 终止进程，改用比较
            if (dot && (_wcsicmp(dot, L".lnk") == 0 || _wcsicmp(dot, L".url") == 0)) {
                std::wstring lowerFile = fd.cFileName;
                for (auto& c : lowerFile) c = (wchar_t)towlower(c);
                if (lowerFile.find(lowerName) != std::wstring::npos) {
                    ForceDeleteFile(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

/**
 * @brief Clean desktop shortcuts (current user + all users)
 */
static void CleanDesktopShortcuts(const std::wstring& appName) {
    wchar_t path[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path);
    DeleteMatchingShortcuts(path, appName, 0, 1);

    wchar_t publicPath[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, publicPath);
    DeleteMatchingShortcuts(publicPath, appName, 0, 1);
}

/**
 * @brief Clean start menu shortcuts (current user + all users)
 */
static void CleanStartMenuShortcuts(const std::wstring& appName) {
    wchar_t path[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_STARTMENU, NULL, 0, path);
    DeleteMatchingShortcuts(path, appName, 0, 3);

    wchar_t publicPath[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_COMMON_STARTMENU, NULL, 0, publicPath);
    DeleteMatchingShortcuts(publicPath, appName, 0, 3);
}

/**
 * @brief Clean Quick Launch shortcuts
 */
static void CleanQuickLaunchShortcuts(const std::wstring& appName) {
    wchar_t path[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path);
    std::wstring quickLaunch = std::wstring(path) + L"\\Microsoft\\Internet Explorer\\Quick Launch";
    DeleteMatchingShortcuts(quickLaunch, appName, 0, 1);
}

/**
 * @brief Enumerate uninstall items from a registry path
 */
static void EnumerateUninstallKeys(HKEY hive, const wchar_t* subPath, REGSAM viewAccess) {
    HKEY hKey;
    REGSAM access = KEY_READ;
    if (viewAccess) access |= viewAccess;
    if (RegOpenKeyExW(hive, subPath, 0, access, &hKey) != ERROR_SUCCESS) return;

    wchar_t keyName[256] = { 0 };
    DWORD keyNameSize = sizeof(keyName) / sizeof(wchar_t);
    int index = 0;

    while (RegEnumKeyExW(hKey, index, keyName, &keyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (!UninstallScanActive()) {
            RegCloseKey(hKey);
            return;
        }

        HKEY hSubKey;
        REGSAM subAccess = KEY_READ;
        if (viewAccess) subAccess |= viewAccess;
        if (RegOpenKeyExW(hKey, keyName, 0, subAccess, &hSubKey) == ERROR_SUCCESS) {
            std::wstring displayName = RegReadString(hSubKey, L"DisplayName");
            if (!displayName.empty()) {
                UninstallItem item;
                item.displayName = displayName;
                item.publisher = RegReadString(hSubKey, L"Publisher");
                item.version = RegReadString(hSubKey, L"DisplayVersion");
                item.installLocation = RegReadString(hSubKey, L"InstallLocation");
                item.uninstallString = RegReadString(hSubKey, L"UninstallString");
                item.regKeyPath = std::wstring(subPath) + L"\\" + keyName;
                item.regHive = hive;
                item.regViewAccess = viewAccess;
                item.isChecked = false;
                item.isScanned = true;
                item.estimatedSize = 0;

                if (item.installLocation.empty()) {
                    item.installLocation = RegReadString(hSubKey, L"InstallSource");
                }

                item.type = DetermineAppType(item.installLocation);

                if (!item.installLocation.empty() && PathFileExistsW(item.installLocation.c_str())) {
                    item.estimatedSize = CalcDirectorySize(item.installLocation);
                }

                if (item.type == L"系统软件") {
                    item.estimatedSize = 0;
                }

                EnterCriticalSection(&g_uninstallItemsLock);
                g_uninstallItems.push_back(item);
                LeaveCriticalSection(&g_uninstallItemsLock);
            }
            RegCloseKey(hSubKey);
        }
        index++;
        keyNameSize = sizeof(keyName) / sizeof(wchar_t);
    }
    RegCloseKey(hKey);
}

/**
 * @brief Scan installed applications from registry
 */
void ScanInstalledApps() {
    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems.clear();
    LeaveCriticalSection(&g_uninstallItemsLock);

    REGSAM nativeView = GetRegistryViewAccess();
    REGSAM otherView = (nativeView == KEY_WOW64_64KEY) ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;

    EnumerateUninstallKeys(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", nativeView);
    if (!UninstallScanActive()) return;
    EnumerateUninstallKeys(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", otherView);
    if (!UninstallScanActive()) return;
    EnumerateUninstallKeys(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", nativeView);
}

/**
 * @brief Scan installed drivers
 */
void ScanInstalledDrivers() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

    wchar_t keyName[256] = { 0 };
    DWORD keyNameSize = sizeof(keyName) / sizeof(wchar_t);
    int index = 0;

    while (RegEnumKeyExW(hKey, index, keyName, &keyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (!UninstallScanActive()) {
            RegCloseKey(hKey);
            return;
        }

        HKEY hSubKey;
        if (RegOpenKeyExW(hKey, keyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
            std::wstring displayName = RegReadString(hSubKey, L"DisplayName");
            DWORD startType = 0;
            DWORD startSize = sizeof(startType);
            DWORD serviceType = 0;
            DWORD typeSize = sizeof(serviceType);
            BOOL startOk = RegQueryValueExW(hSubKey, L"Start", NULL, NULL,
                (LPBYTE)&startType, &startSize) == ERROR_SUCCESS;
            BOOL typeOk = RegQueryValueExW(hSubKey, L"Type", NULL, NULL,
                (LPBYTE)&serviceType, &typeSize) == ERROR_SUCCESS;

            // 驱动页只应列内核/文件系统驱动。仅看 Start=2/3 会把 Winmgmt、
            // Spooler 等用户态自动服务也当成驱动，允许“强删驱动”就可能删掉系统服务
            if (startOk && typeOk && (startType == 2 || startType == 3) &&
                (serviceType & SERVICE_DRIVER) != 0) {
                std::wstring imagePath = RegReadString(hSubKey, L"ImagePath");
                // 无 ImagePath 的是内核内置驱动（Ntfs/Fastfat 等），删其服务键必然
                // 破坏系统且无法清理文件，不应出现在可卸载列表里
                if (!imagePath.empty()) {
                    UninstallItem item;
                    item.displayName = displayName.empty() ? std::wstring(keyName) : displayName;
                    item.publisher = L"系统驱动";
                    item.version = L"";
                    // ImagePath 是 .sys 文件而不是目录：绝不能写进 installLocation，
                    // 否则 CleanAppResiduals 会把它当目录递归删除、并按目录前缀杀进程/删服务
                    item.imagePath = imagePath;
                    item.uninstallString = L"";
                    item.regKeyPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + keyName;
                    item.regHive = HKEY_LOCAL_MACHINE;
                    item.regViewAccess = 0; // Services 键不受 WOW64 重定向，必须显式初始化，否则删除阶段用到随机值
                    item.type = L"驱动";
                    item.isScanned = true;
                    // ntfs / fltmgr / Wdf01000 等 Windows 自带组件同样满足 Start=2/3，
                    // 强删其服务键会直接蓝屏，按 ImagePath 归属提前标记并在卸载阶段拒绝
                    item.isSystemComponent = IsSystemComponentImagePath(item.imagePath);

                    EnterCriticalSection(&g_uninstallItemsLock);
                    g_uninstallItems.push_back(item);
                    LeaveCriticalSection(&g_uninstallItemsLock);
                }
            }
            RegCloseKey(hSubKey);
        }
        index++;
        keyNameSize = sizeof(keyName) / sizeof(wchar_t);
    }
    RegCloseKey(hKey);
}

/**
 * @brief Thread procedure for scanning installed software
 */
DWORD WINAPI ScanUninstallThreadProc(LPVOID lpParam) {
    // 不再重置全局取消标志（那会覆盖 UI 线程为旧线程设置的取消请求），
    // 改为绑定自己的代际；取消标志由 StartUninstallScan 在 UI 线程统一复位
    t_uninstallScanGen = g_uninstallScanGeneration;
    g_isUninstallScanning = true;

    SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在扫描已安装软件...");

    if (UninstallScanActive()) {
        ScanInstalledApps();
    }
    if (UninstallScanActive()) {
        ScanInstalledDrivers();
    }

    if (t_uninstallScanGen != g_uninstallScanGeneration) {
        return 0;
    }

    g_isUninstallScanning = false;

    if (g_uninstallScanCancelled) {
        g_uninstallScanCancelled = false;
        PostMessageW(g_hMainWnd, WM_UNINSTALL_DONE, 0, 0);
    }
    else {
        PostMessageW(g_hMainWnd, WM_UNINSTALL_DONE, 1, 0);
    }

    return 0;
}

/**
 * @brief Start a fresh uninstall scan, immediately invalidating any running one
 */
void StartUninstallScan() {
    g_uninstallScanGeneration++;
    g_uninstallScanCancelled = false;

    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems.clear();
    LeaveCriticalSection(&g_uninstallItemsLock);

    ListView_DeleteAllItems(g_hUninstallList);

    HANDLE hThread = CreateThread(NULL, 0, ScanUninstallThreadProc, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

/**
 * @brief Initialize uninstall list by scanning all installed software
 */
void InitUninstallList() {
    if (g_uninstallDebouncePending) {
        KillTimer(g_hMainWnd, TIMER_UNINSTALL_DEBOUNCE);
        g_uninstallDebouncePending = false;
    }

    StartUninstallScan();
}

/**
 * @brief Refresh the uninstall list view
 */
void RefreshUninstallList() {
    if (!g_hUninstallList) return;

    ListView_DeleteAllItems(g_hUninstallList);

    EnterCriticalSection(&g_uninstallItemsLock);
    if (g_uninstallItems.empty()) {
        LeaveCriticalSection(&g_uninstallItemsLock);
        RECT tabRect;
        GetClientRect(g_hTab, &tabRect);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
        AdjustListViewHeight(g_hUninstallList, tabRect, 50, 50);
        return;
    }

    int displayIndex = 0;
    for (size_t i = 0; i < g_uninstallItems.size(); i++) {
        const auto& item = g_uninstallItems[i];

        if (g_uninstallFilterType > 0) {
            bool match = false;
            if (g_uninstallFilterType == 1 && item.type == L"系统软件") match = true;
            else if (g_uninstallFilterType == 2 && item.type == L"用户软件") match = true;
            else if (g_uninstallFilterType == 3 && item.type == L"驱动") match = true;
            else if (g_uninstallFilterType == 4 && item.type == L"未知") match = true;
            if (!match) continue;
        }

        if (!g_uninstallSearchText.empty() && !ContainsTextInsensitive(item.displayName, g_uninstallSearchText)) {
            continue;
        }

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
        lvi.iItem = displayIndex;
        lvi.lParam = (LPARAM)i;
        lvi.state = INDEXTOSTATEIMAGEMASK(1);
        lvi.stateMask = LVIS_STATEIMAGEMASK;

        // displayName 来自注册表，可能超过固定缓冲区；wcscpy_s 溢出会终止进程
        std::wstring displayNameBuf = item.displayName;
        lvi.pszText = (LPWSTR)displayNameBuf.c_str();
        ListView_InsertItem(g_hUninstallList, &lvi);

        ListView_SetItemText(g_hUninstallList, displayIndex, 1, (LPWSTR)item.version.c_str());
        ListView_SetItemText(g_hUninstallList, displayIndex, 2, (LPWSTR)item.type.c_str());

        std::wstring sizeStr;
        if (item.estimatedSize > 0) {
            sizeStr = FormatSize(item.estimatedSize);
        }
        else {
            sizeStr = L"-";
        }
        ListView_SetItemText(g_hUninstallList, displayIndex, 3, (LPWSTR)sizeStr.c_str());
        ListView_SetItemText(g_hUninstallList, displayIndex, 4, (LPWSTR)item.installLocation.c_str());

        displayIndex++;
    }
    LeaveCriticalSection(&g_uninstallItemsLock);

    RECT tabRect;
    GetClientRect(g_hTab, &tabRect);
    TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
    AdjustListViewHeight(g_hUninstallList, tabRect, 50, 50);
}

/**
 * @brief Execute an uninstall command and return exit code
 *        解析交给 SplitUninstallCommand：原实现按第一个空格切分，
 *        `C:\Program Files\App\unins000.exe /S`（含空格却没引号，真实存在）
 *        会得到 "C:\Program" 而直接失败。
 *        返回 -1 表示无法启动，-2 表示等待超时（10 分钟）。
 */
int ExecuteUninstallCommand(const std::wstring& uninstallCmd) {
    if (uninstallCmd.empty()) return -1;

    std::wstring exePath, params;
    if (!SplitUninstallCommand(uninstallCmd, exePath, params) || exePath.empty()) return -1;

    // 工作目录取 exe 所在目录；目录不存在时不能传给 ShellExecuteEx，
    // 否则整个调用直接失败（注册表里 InstallLocation 缺失/过期的条目很常见）
    std::wstring workingDir;
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0) {
        workingDir = exePath.substr(0, slash);
    }
    if (workingDir.empty() || GetFileAttributesW(workingDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        workingDir.clear();
    }

    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    // 注册表 UninstallString 长度不可信，SafeCopy 截断而不是终止进程
    wchar_t file[MAX_PATH * 4] = { 0 };
    wchar_t paramsBuf[4096] = { 0 };
    wchar_t workingDirBuf[MAX_PATH * 4] = { 0 };
    SafeCopy(file, (int)_countof(file), exePath.c_str());
    SafeCopy(paramsBuf, (int)_countof(paramsBuf), params.c_str());
    SafeCopy(workingDirBuf, (int)_countof(workingDirBuf), workingDir.c_str());

    sei.lpFile = file;
    sei.lpParameters = paramsBuf;
    sei.lpDirectory = workingDir.empty() ? NULL : workingDirBuf;

    if (!ShellExecuteExW(&sei)) {
        return -1;
    }

    if (sei.hProcess) {
        DWORD wait = WaitForSingleObject(sei.hProcess, 600000);
        if (wait == WAIT_TIMEOUT) {
            // 超时后进程仍在运行：STILL_ACTIVE(259) 会被当成「卸载程序返回 259」，
            // 既误导用户也可能被误判为成功，这里显式区分并终止等待
            CloseHandle(sei.hProcess);
            return -2;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return (int)exitCode;
    }

    return 0;
}

/**
 * @brief 驱动/系统服务条目的专用清理
 *        驱动没有「安装目录」，原实现把 ImagePath（.sys 文件）写进 installLocation，
 *        于是走「递归删除目录」分支，并把一个文件路径当目录前缀去匹配进程/服务。
 *        这里只停服务、删驱动文件、删 Services 键；系统自带组件直接拒绝。
 */
static bool CleanDriverResiduals(const UninstallItem& item) {
    if (item.isSystemComponent) {
        AddUninstallFailure(item.displayName,
            L"Windows 自带组件（ImagePath 位于系统目录内），禁止删除");
        return false;
    }

    bool success = true;

    // 1. 先从 SCM 停止并删除服务：驱动仍在加载时删文件/删键会失败或残留
    std::wstring svcName = item.regKeyPath;
    size_t slash = svcName.find_last_of(L'\\');
    if (slash != std::wstring::npos) svcName = svcName.substr(slash + 1);
    if (!svcName.empty()) {
        // 服务可能已不存在，删除失败不直接判失败，继续清理键与文件
        RemoveServiceByName(svcName);
    }

    // 2. 删除驱动文件
    std::wstring driverFile = ResolveDriverFilePath(item.imagePath);
    if (!driverFile.empty() && PathFileExistsW(driverFile.c_str())) {
        if (!ForceDeleteFile(driverFile)) {
            AddUninstallFailure(item.displayName, L"无法删除驱动文件: " + driverFile);
            success = false;
        }
    }

    // 3. 删除 Services 下的服务键
    if (!item.regKeyPath.empty()) {
        if (!DeleteRegistryKeyRecursive(item.regHive, item.regKeyPath, item.regViewAccess)) {
            AddUninstallFailure(item.displayName, L"无法删除服务注册表项: " + item.regKeyPath);
            success = false;
        }
    }

    return success;
}

/**
 * @brief Comprehensive cleanup of all app residuals after uninstall
 */
bool CleanAppResiduals(const UninstallItem& item, bool keepUserData) {
    // 驱动条目走独立分支：不能按目录/快捷方式/AppData 残留的方式处理
    if (item.type == L"驱动") return CleanDriverResiduals(item);

    bool success = true;

    wchar_t userPath[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userPath);
    std::wstring userProfile(userPath);

    wchar_t programDataPath[MAX_PATH] = { 0 };
    GetEnvironmentVariableW(L"ProgramData", programDataPath, MAX_PATH);
    std::wstring programData(programDataPath);

    // 安装目录是否可用：注册表 InstallLocation 由软件自写，可能是空、盘符根、
    // C:\Windows 或一个文件路径；不可用时下面所有「按目录」的操作全部跳过
    const bool locUsable = IsUsableInstallLocation(item.installLocation);

    // ===== 1. Kill running processes before deleting files =====
    if (locUsable) {
        KillProcessesInDirectory(item.installLocation);
    }
    Sleep(500);

    // ===== 2. Remove Windows services related to the software =====
    if (locUsable) {
        RemoveServicesInDirectory(item.installLocation);
    }

    // ===== 3. Delete App Paths registry entries =====
    if (locUsable) {
        std::vector<wchar_t> drive(512), dir(512), name(512), ext(512);
        if (_wsplitpath_s(item.installLocation.c_str(), drive.data(), (int)drive.size(),
            dir.data(), (int)dir.size(), name.data(), (int)name.size(),
            ext.data(), (int)ext.size()) == 0 && name[0] != 0) {
            CleanAppPathsEntry(std::wstring(name.data()) + L".exe");
        }
    }

    // ===== 4. Delete install directory =====
    if (!item.installLocation.empty() && PathFileExistsW(item.installLocation.c_str())) {
        // 保留用户数据时，位于用户配置目录内的安装目录不删
        bool underProfile = userProfile.empty() ? true
            : IsPathUnderDirectory(item.installLocation, userProfile);
        bool doDelete = keepUserData ? !underProfile : true;

        if (doDelete && !locUsable) {
            // 不能删又必须让用户知道：静默跳过会被当成「卸载成功但目录还在」
            AddUninstallFailure(item.displayName,
                L"安装位置不是可安全删除的目录，已跳过: " + item.installLocation);
            success = false;
        }
        else if (doDelete) {
            if (!DeleteDirectoryRecursive(item.installLocation)) {
                AddUninstallFailure(item.displayName, L"无法删除安装目录: " + item.installLocation);
                success = false;
            }
        }
    }

    // ===== 5. Delete registry keys (both 32-bit and 64-bit views) =====
    if (!item.regKeyPath.empty()) {
        REGSAM viewAccess = item.regViewAccess ? item.regViewAccess : 0;
        bool regDeleted = DeleteRegistryKeyRecursive(item.regHive, item.regKeyPath, viewAccess);

        // Also clean the opposite view if on 64-bit system
        REGSAM otherView = 0;
        if (viewAccess == KEY_WOW64_64KEY) otherView = KEY_WOW64_32KEY;
        else if (viewAccess == KEY_WOW64_32KEY) otherView = KEY_WOW64_64KEY;
        if (otherView != 0) {
            DeleteRegistryKeyRecursive(item.regHive, item.regKeyPath, otherView);
        }

        if (!regDeleted) {
            AddUninstallFailure(item.displayName,
                L"无法删除注册表项: " + item.regKeyPath);
            success = false;
        }
    }

    // ===== 6. Clean shortcuts (desktop + start menu + quick launch) =====
    CleanDesktopShortcuts(item.displayName);
    CleanStartMenuShortcuts(item.displayName);
    CleanQuickLaunchShortcuts(item.displayName);

    // Also clean shortcuts matching the install directory's last folder name
    if (!item.installLocation.empty()) {
        std::wstring loc = item.installLocation;
        while (!loc.empty() && (loc.back() == L'\\' || loc.back() == L'/')) loc.pop_back();
        size_t slash = loc.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < loc.length()) {
            std::wstring shortName = loc.substr(slash + 1);
            if (shortName != item.displayName) {
                CleanDesktopShortcuts(shortName);
                CleanStartMenuShortcuts(shortName);
            }
        }
    }

    // ===== 7. Clean residual directories =====
    if (!keepUserData) {
        std::vector<std::wstring> candidates;
        if (!item.publisher.empty()) candidates.push_back(item.publisher);
        if (!item.displayName.empty()) candidates.push_back(item.displayName);
        if (!item.installLocation.empty()) {
            std::wstring loc = item.installLocation;
            while (!loc.empty() && (loc.back() == L'\\' || loc.back() == L'/')) loc.pop_back();
            size_t slash = loc.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash + 1 < loc.length()) {
                candidates.push_back(loc.substr(slash + 1));
            }
        }

        for (const auto& cand : candidates) {
            // 残留目录名来自注册表（DisplayName/Publisher 可被任意软件写入），
            // 含路径分隔符/.. 等可直接拼出 AppData\..\... 删除用户其他数据，必须先校验
            if (!IsSafeResidualName(cand)) continue;
            // 命中厂商/公共目录名（Microsoft、Temp、Packages 等）时会拼出被多个软件
            // 共用的目录，整棵删掉会破坏其它软件的数据，必须跳过
            if (IsSharedVendorDirName(cand)) continue;

            std::wstring appDataLocal = userProfile + L"\\AppData\\Local\\" + cand;
            if (PathFileExistsW(appDataLocal.c_str())) {
                if (!DeleteDirectoryRecursive(appDataLocal)) {
                    AddUninstallFailure(item.displayName, L"无法删除残留目录: " + appDataLocal);
                    success = false;
                }
            }
            std::wstring appDataRoaming = userProfile + L"\\AppData\\Roaming\\" + cand;
            if (PathFileExistsW(appDataRoaming.c_str())) {
                if (!DeleteDirectoryRecursive(appDataRoaming)) {
                    AddUninstallFailure(item.displayName, L"无法删除残留目录: " + appDataRoaming);
                    success = false;
                }
            }
            if (!programData.empty()) {
                std::wstring progDataDir = programData + L"\\" + cand;
                if (PathFileExistsW(progDataDir.c_str())) {
                    if (!DeleteDirectoryRecursive(progDataDir)) {
                        AddUninstallFailure(item.displayName, L"无法删除公共残留目录: " + progDataDir);
                        success = false;
                    }
                }
            }
        }
    }

    return success;
}

/**
 * @brief Uninstall a single app (uses normal uninstaller if available, then cleans residuals)
 */
bool UninstallSingleApp(const UninstallItem& item, bool keepUserData) {
    // 驱动条目不参与常规卸载：它们没有 UninstallString，常规卸载过去是静默失败。
    // 这里保持「不做任何破坏性操作」，删除服务/驱动文件只能通过「强制卸载」触发，
    // 避免用户在卸载软件时误删系统驱动。
    if (item.type == L"驱动") {
        AddUninstallFailure(item.displayName, L"驱动条目不支持常规卸载，如需删除请使用「强制卸载」");
        return false;
    }

    if (item.uninstallString.empty()) {
        // 没有卸载程序但确实有可删的安装目录时，等价于强制卸载
        if (IsUsableInstallLocation(item.installLocation)) {
            return CleanAppResiduals(item, keepUserData);
        }
        AddUninstallFailure(item.displayName, L"没有卸载命令且没有可用的安装目录，无法卸载");
        return false;
    }

    int exitCode = ExecuteUninstallCommand(item.uninstallString);

    if (exitCode == -2) {
        // 卸载程序 10 分钟没退出（弹了对话框等人点）：此时绝不能去删安装目录，
        // 否则会把正在执行卸载的程序的目录删掉
        AddUninstallFailure(item.displayName, L"卸载程序超时未退出，已跳过残留清理");
        return false;
    }

    if (exitCode == 0 || exitCode == 3010 || exitCode == 3011) {
        Sleep(2000);
        return CleanAppResiduals(item, keepUserData);
    }
    else {
        AddUninstallFailure(item.displayName,
            L"卸载程序返回错误代码: " + std::to_wstring(exitCode));
        return false;
    }
}

/**
 * @brief Thread procedure for standard uninstalling selected apps
 */
DWORD WINAPI UninstallThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    ClearUninstallFailures();

    std::vector<int> selectedIndices;
    EnterCriticalSection(&g_uninstallItemsLock);
    for (int i = 0; i < (int)g_uninstallItems.size(); i++) {
        if (g_uninstallItems[i].isChecked) {
            selectedIndices.push_back(i);
        }
    }
    LeaveCriticalSection(&g_uninstallItemsLock);

    int successCount = 0;
    int failCount = 0;

    for (size_t idx = 0; idx < selectedIndices.size(); idx++) {
        int i = selectedIndices[idx];
        UninstallItem item;
        bool valid = false;
        EnterCriticalSection(&g_uninstallItemsLock);
        if (i >= 0 && i < (int)g_uninstallItems.size()) {
            item = g_uninstallItems[i];
            valid = true;
        }
        LeaveCriticalSection(&g_uninstallItemsLock);

        // 列表在卸载期间被刷新（筛选/搜索/重扫）时索引会失效：
        // 绝不能拿一个空条目继续跑删除流程
        if (!valid) {
            AddUninstallFailure(L"(未知项)", L"列表已刷新，该项索引失效，已跳过");
            failCount++;
        }
        else if (UninstallSingleApp(item, g_keepUserData)) {
            successCount++;
        }
        else {
            failCount++;
        }

        PostMessageW(hWnd, WM_UNINSTALL_PROGRESS, (WPARAM)(idx + 1), (LPARAM)selectedIndices.size());
    }

    PostMessageW(hWnd, WM_UNINSTALL_FINISHED, (WPARAM)successCount, (LPARAM)failCount);
    return 0;
}

/**
 * @brief Thread procedure for force uninstalling selected apps (bypasses normal uninstaller)
 */
DWORD WINAPI ForceUninstallThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    ClearUninstallFailures();

    std::vector<int> selectedIndices;
    EnterCriticalSection(&g_uninstallItemsLock);
    for (int i = 0; i < (int)g_uninstallItems.size(); i++) {
        if (g_uninstallItems[i].isChecked) {
            selectedIndices.push_back(i);
        }
    }
    LeaveCriticalSection(&g_uninstallItemsLock);

    int successCount = 0;
    int failCount = 0;

    for (size_t idx = 0; idx < selectedIndices.size(); idx++) {
        int i = selectedIndices[idx];
        UninstallItem item;
        bool valid = false;
        EnterCriticalSection(&g_uninstallItemsLock);
        if (i >= 0 && i < (int)g_uninstallItems.size()) {
            item = g_uninstallItems[i];
            valid = true;
        }
        LeaveCriticalSection(&g_uninstallItemsLock);

        // 同 UninstallThreadProc：索引失效时不得拿空条目执行删除
        if (!valid) {
            AddUninstallFailure(L"(未知项)", L"列表已刷新，该项索引失效，已跳过");
            failCount++;
        }
        else if (CleanAppResiduals(item, false)) {
            successCount++;
        }
        else {
            failCount++;
        }

        PostMessageW(hWnd, WM_UNINSTALL_PROGRESS, (WPARAM)(idx + 1), (LPARAM)selectedIndices.size());
    }

    PostMessageW(hWnd, WM_UNINSTALL_FINISHED, (WPARAM)successCount, (LPARAM)failCount);
    return 0;
}

/**
 * @brief Write uninstall failure log
 *        读取 g_uninstallFailures 必须持锁：写入方是卸载工作线程，
 *        遍历未加保护的 vector 与并发扩容 = 迭代器失效。
 */
void WriteUninstallFailureLog() {
    std::vector<UninstallFailureLog> snapshot;
    EnterCriticalSection(&UninstallFailuresLock());
    snapshot = g_uninstallFailures;
    LeaveCriticalSection(&UninstallFailuresLock());

    if (snapshot.empty()) return;

    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *(slash + 1) = 0;
    // 用 std::wstring 拼接，避免 wcscat_s 在路径接近 MAX_PATH 时溢出终止进程
    std::wstring logPath = std::wstring(exePath) + L"uninstall_failures.log";

    std::wofstream logFile(logPath, std::ios::app);
    if (logFile.is_open()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        logFile << L"\n=== 卸载失败日志 "
            << st.wYear << L"-" << st.wMonth << L"-" << st.wDay
            << L" " << st.wHour << L":" << st.wMinute << L":" << st.wSecond
            << L" ===\n";
        for (const auto& failure : snapshot) {
            logFile << failure.appName << L" - " << failure.reason << L"\n";
        }
        logFile.close();
    }
}
