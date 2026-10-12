// Module: common.h - Shared declarations, structures, and function prototypes

#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wininet.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winsvc.h>
#include <functional>
#include <new>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

// Fallback macros for ListView item data access (may not be defined in older SDK headers)
#ifndef LVM_GETITEMDATA
#define LVM_GETITEMDATA 0x1033
#endif
#ifndef ListView_GetItemData
#define ListView_GetItemData(hwnd, i) (LPARAM)SendMessageW((hwnd), LVM_GETITEMDATA, (WPARAM)(i), 0)
#endif

// WinINet chunked upload flag (not defined in older SDK versions)
#ifndef INTERNET_FLAG_CHUNKED_WRITE
#define INTERNET_FLAG_CHUNKED_WRITE 0x20000000
#endif

// WinINet status code (not defined in older SDK versions)
#ifndef INTERNET_STATUS_REQUEST_SENDING
#define INTERNET_STATUS_REQUEST_SENDING 12
#endif

// Custom messages
#define WM_SCAN_PROGRESS      (WM_USER + 101)
#define WM_SCAN_DONE          (WM_USER + 102)
#define WM_CLEAN_PROGRESS     (WM_USER + 103)
#define WM_CLEAN_DONE         (WM_USER + 104)
#define WM_SPEED_UPDATE       (WM_USER + 105)
#define WM_SPEED_DONE         (WM_USER + 106)
#define WM_REG_SCAN_PROGRESS  (WM_USER + 107)
#define WM_REG_SCAN_DONE      (WM_USER + 108)
#define WM_UNINSTALL_PROGRESS (WM_USER + 109)
#define WM_UNINSTALL_DONE     (WM_USER + 110)
#define WM_NET_REPAIR_DONE    (WM_USER + 111)
#define WM_UNINSTALL_FINISHED (WM_USER + 112)

// Timer IDs for debounce
#define TIMER_UNINSTALL_DEBOUNCE 1001
#define UNINSTALL_DEBOUNCE_MS 300

// Special clean type enumeration
enum class SpecialCleanType {
    None = 0,
    RecycleBin = 1,
    MemoryDump = 2
};

// Progress update info structure for network speed test
struct SpeedUpdateInfo {
    int progress;       // Progress percentage (0-100)
    long long bytes;    // Bytes downloaded/uploaded
    long long speed;    // Current speed (bytes/sec)
    int remainSec;      // Remaining seconds (download phase)
    int phase;          // 0=download phase, 1=upload phase
};

// Clean item structure
struct CleanItem {
    std::wstring name;
    std::wstring desc;
    std::vector<std::wstring> paths;
    std::wstring pattern;
    long long totalSize;
    int fileCount;
    bool defaultChecked;
    bool isSensitive;
    bool protectData;
    std::vector<std::wstring> subDirPatterns;
    SpecialCleanType specialType;
    bool isScanned;
};

// Startup item structure
struct StartupItem {
    std::wstring displayName;
    std::wstring realName;
    std::wstring path;
    std::wstring location;
    HKEY hive;
    std::wstring subKey;
    bool isRegistry;
    bool isEnabled;
};

// Right-click menu item structure
struct RightMenuItem {
    std::wstring displayName;
    std::wstring realKeyName;
    std::wstring type;
    std::wstring location;
    HKEY hive;
    std::wstring parentPath;
    bool isEnabled;
};

// Registry clean item structure
struct RegCleanItem {
    std::wstring keyPath;
    std::wstring valueName;
    std::wstring desc;
    std::wstring category;
    HKEY hive = NULL;
    bool isInvalid = false;
    // 扫描时使用的注册表视图（KEY_WOW64_64KEY / KEY_WOW64_32KEY）。
    // 原实现靠 category 里有没有「[32位]」这个中文字串来判断视图，
    // 文案一改（翻译、加空格、改标签）删除就会用错视图：32 位残留删不掉，
    // 而 64 位视图里同名键可能被误删。
    REGSAM view = 0;
};

// File info structure for ListFilesInDirectory
struct FileInfo {
    std::wstring name;
    long long size;
};

// Clean failure log structure
struct CleanFailureLog {
    std::wstring filePath;
    std::wstring reason;
};

// Uninstall item structure
// 所有标量成员必须带默认初始化器：卸载线程在「列表已被刷新」时会先默认构造一个
// item 再判断索引是否有效，未初始化的 regHive/regViewAccess 会被当成真句柄传给
// RegDeleteTreeW（原实现即如此，属随机崩溃/随机误删）。
struct UninstallItem {
    std::wstring displayName;
    std::wstring publisher;
    std::wstring version;
    // 安装目录（普通软件）。驱动条目一律留空：驱动没有安装目录，
    // 原实现把服务 ImagePath（.sys 文件）写进这里，导致卸载流程拿一个文件
    // 路径去「递归删除目录」并按目录前缀匹配进程/服务。
    std::wstring installLocation;
    std::wstring uninstallString;
    std::wstring regKeyPath;
    std::wstring type;
    // 驱动条目专用：服务 ImagePath 原文（REG_EXPAND_SZ，未展开），仅用于展示与删除驱动文件
    std::wstring imagePath;
    long long estimatedSize = 0;
    HKEY regHive = NULL;
    REGSAM regViewAccess = 0;
    bool isChecked = false;
    bool isScanned = false;
    // 系统组件（系统自带驱动 / ImagePath 落在 Windows 目录内的服务）：
    // 删除其 Services 键会直接蓝屏或无法启动，普通卸载与强制卸载都必须拒绝
    bool isSystemComponent = false;
};

// Uninstall failure log structure
struct UninstallFailureLog {
    std::wstring appName;
    std::wstring reason;
};

// ====================== Global variable declarations ======================

// Global control handles
extern HWND g_hTab;
extern HWND g_hCleanList;
extern HWND g_hStartupList;
extern HWND g_hContextMenuList;
extern HWND g_hRegCleanList;
extern HWND g_hUninstallList;
extern HWND g_hUninstallFilter;
extern HWND g_hUninstallSearch;
extern HWND g_hCleanProgress;
extern HWND g_hRegProgress;
extern HWND g_hStatusText;
extern HWND g_hSpeedResult;
extern HWND g_hSpeedProgress;
extern HWND g_hHostsEdit;
extern HWND g_hShredPath;
extern HWND g_hMainWnd;

// Global data
extern std::vector<CleanItem> g_cleanItems;
extern std::vector<StartupItem> g_startupItems;
extern std::vector<RightMenuItem> g_contextMenuItems;
extern std::vector<RegCleanItem> g_regCleanItems;
extern std::vector<CleanFailureLog> g_cleanFailures;
extern std::vector<UninstallItem> g_uninstallItems;
extern std::vector<UninstallFailureLog> g_uninstallFailures;
extern HINSTANCE g_hInst;
extern bool g_isScanning;
extern bool g_isCleaning;
extern bool g_stopScan;
extern bool g_stopClean;
extern bool g_isSpeedTesting;
extern bool g_stopSpeedTest;
extern bool g_isNetworkRepairing;
extern bool g_isRegScanning;
extern bool g_isUninstalling;
extern bool g_isUninstallScanning;
extern bool g_uninstallScanCancelled;
extern bool g_keepUserData;
extern int g_uninstallFilterType;
extern std::wstring g_uninstallSearchText;
extern int g_startupFilterType;
extern int g_contextMenuFilterType;

// Tools page state
extern int g_toolFunction; // 0=粉碎 1=系统信息 2=重复文件 3=进程管理 4=快捷方式修复 5=磁盘清理
extern HWND g_hToolFuncCombo;
extern HWND g_hToolListView;
extern HWND g_hToolPathEdit;
extern HWND g_hToolAgeCombo;
extern HWND g_hToolInfoLabel;
extern std::wstring g_toolCurrentPath;

// Debounce state for uninstall filter/search
extern bool g_uninstallDebouncePending;
extern bool g_uninstallDebounceForceRescan;

// Scan generation counter: incremented each time a new uninstall scan starts.
// Old scan threads whose captured generation no longer matches are stale and
// must exit silently (no UI update), which eliminates laggy duplicate refreshes.
extern DWORD g_uninstallScanGeneration;
// Guards g_uninstallItems against concurrent access by old/new scan threads
extern CRITICAL_SECTION g_uninstallItemsLock;

// Hosts file path constant
extern const wchar_t* HOSTS_PATH;

// 测速单阶段时长（毫秒），默认 10000；测试可临时调小以快速跑完完整流程
extern DWORD g_speedPhaseMs;
// 测速连接/收发超时（毫秒），默认 15000
extern DWORD g_speedIoTimeoutMs;

// ====================== Function declarations ======================

// utils.cpp
std::wstring FormatSize(long long bytes);
std::wstring FormatSpeed(long long bytesPerSec);
bool EndsWith(const std::wstring& str, const std::wstring& suffix);
void ExecuteCmd(const std::wstring& cmd);
bool IsSystemCriticalPath(const std::wstring& path);
bool IsProtectedDataFile(const std::wstring& fileName);
bool IsPathTraversal(const std::wstring& path);
std::vector<std::wstring> ParseSubDirPattern(const std::wstring& pattern);
bool MatchSubDirPattern(const std::wstring& relPath, const std::wstring& pattern);
std::vector<std::wstring> FindMatchingSubDirs(const std::wstring& baseDir, const std::vector<std::wstring>& patterns);
void ListFilesInDirectory(const std::wstring& dir, const std::wstring& pattern, bool protectData, std::vector<FileInfo>& files);
bool IsFileLocked(const std::wstring& filePath);
void WriteCleanFailureLog();
// 注册表备份：返回是否全部成功，failed 回收失败原因供 UI 提示
bool BackupRegistry(std::vector<std::wstring>* failed);
bool ExecuteCmdChecked(const std::wstring& cmd);
void EnableRegistryBackupPrivileges();
// 32 位视图下 HKLM\SOFTWARE 的真实路径（WOW6432Node），用于展示与备份
std::wstring RegRealPath(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag);
bool RegKeyExistsInView(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag);
bool BackupRegistryKey(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag,
    const std::wstring& outFile);
// 备份输出目录（exe 目录下 reg_backup），不存在时自动创建
std::wstring GetRegBackupDir();
// 残留判定（纯逻辑 + 少量文件系统探测，便于单测）
bool IsSystemOrPatchUninstallEntry(DWORD systemComponent, const std::wstring& releaseType,
    const std::wstring& parentKeyName);
bool IsUninstallEntryLeftover(const std::wstring& installLocation,
    const std::wstring& uninstallString, std::wstring& reason);
bool IsInvalidStartupCommand(const std::wstring& command, std::wstring& reason);
bool IsRunAsAdmin();
bool Is64BitProcess();
REGSAM GetRegistryViewAccess();

// utils.cpp - 安全与数值辅助
long long FileSizeFromFindData(const WIN32_FIND_DATAW* fd);
bool ForceDeleteFile(const std::wstring& filePath);
bool ForceRemoveDirectory(const std::wstring& dirPath);
void SafeAppend(wchar_t* buffer, size_t bufferCount, const wchar_t* text);
void SafeCopy(wchar_t* buffer, size_t bufferCount, const wchar_t* text);
bool IsSafeResidualName(const std::wstring& name);
bool IsSharedVendorDirName(const std::wstring& name);
bool TryStripDisabledSuffix(const std::wstring& path, std::wstring& outOriginal);
// 强力卸载安全护栏（纯函数，可单测）
std::wstring NormalizePathForCompare(const std::wstring& path);
bool IsPathUnderDirectory(const std::wstring& child, const std::wstring& parent);
bool ExpandEnvVars(const std::wstring& in, std::wstring& out);
bool SplitUninstallCommand(const std::wstring& cmd, std::wstring& outExe, std::wstring& outParams);
bool IsSharedProgramRootPath(const std::wstring& path);
bool IsSafeToDeleteDirectory(const std::wstring& path);
// 注册表递归删除总闸：拒绝 SOFTWARE / Services 等容器键被整棵删除
bool IsSafeToDeleteRegistryKey(HKEY hive, const std::wstring& keyPath);
bool IsSystemComponentImagePath(const std::wstring& imagePath);
// 服务 ImagePath -> 驱动文件绝对路径（无法解析时返回空串）
std::wstring ResolveDriverFilePath(const std::wstring& imagePath);
std::wstring GetWindowsDirectorySafe();
std::wstring ComputeFileDigest(const std::wstring& filePath);
void ListFilesRecursiveCapped(const std::wstring& dir, const std::wstring& pattern, int maxDepth, std::vector<FileInfo>& files);

// cleaner.cpp
void ScanRecycleBin(long long& totalSize, int& fileCount);
bool CleanRecycleBin(long long& freedSize);
void ScanDirectory(const std::wstring& dir, const std::wstring& pattern, long long& totalSize, int& fileCount, bool protectData);
void CleanDirectory(const std::wstring& dir, const std::wstring& pattern, long long& freedSize, bool protectData);
void InitCleanItems();
void AdjustListViewHeight(HWND hListView, const RECT& tabRect, int topOffset, int bottomReserve);
void RefreshCleanList();
// 采集 UI 线程上的勾选快照，返回勾选数量（0 表示无需启动扫描线程）
int CollectScanSnapshot();
void CollectCleanSnapshot();
DWORD WINAPI ScanThreadProc(LPVOID lpParam);
DWORD WINAPI CleanThreadProc(LPVOID lpParam);

// startup.cpp
void LoadRegistryStartup(HKEY hive, const std::wstring& subKey, const std::wstring& locationName, REGSAM viewFlag);
void LoadFolderStartup(const std::wstring& folderPath, const std::wstring& locationName);
void ScanAllStartupItems();
void RefreshStartupList();
bool DisableStartupItem(const StartupItem& item);
bool EnableStartupItem(const StartupItem& item);

// contextmenu.cpp
void ScanContextMenuReg(HKEY hive, const std::wstring& basePath, const std::wstring& typeName);
void ScanAllContextMenus();
void RefreshContextMenuList();
bool DisableContextMenuItem(const RightMenuItem& item);
bool EnableContextMenuItem(const RightMenuItem& item);

// network.cpp
DWORD WINAPI SpeedTestThreadProc(LPVOID lpParam);
DWORD WINAPI NetworkRepairThreadProc(LPVOID lpParam);
// 由测速总耗时推算进度/阶段/剩余秒数（纯函数，便于单测）
int SpeedElapsedToProgress(DWORD elapsedTotal, int& phase, int& remainSec);
std::wstring BackupHostsFile();
std::wstring ReadHostsFile();
void SaveHostsFile();
void NetworkRepairAll();

// registry.cpp
bool ShredFile(const std::wstring& filePath);
void ScanUninstallView(REGSAM viewFlag, const wchar_t* viewName);
void ScanStartupValueView(HKEY hive, const wchar_t* subKey, const wchar_t* viewName);
void ScanUninstallLeftovers();
DWORD WINAPI RegScanThreadProc(LPVOID lpParam);
void RefreshRegCleanList();
bool RecursiveDeleteKeyEx(HKEY hive, const std::wstring& keyPath, REGSAM viewFlag);
void CleanSelectedRegistry();

// uninstaller.cpp
void ScanInstalledApps();
void ScanInstalledDrivers();
void StartUninstallScan();
void InitUninstallList();
void RefreshUninstallList();
DWORD WINAPI UninstallThreadProc(LPVOID lpParam);
DWORD WINAPI ForceUninstallThreadProc(LPVOID lpParam);
DWORD WINAPI ScanUninstallThreadProc(LPVOID lpParam);
bool UninstallSingleApp(const UninstallItem& item, bool keepUserData);
bool CleanAppResiduals(const UninstallItem& item, bool keepUserData);
int ExecuteUninstallCommand(const std::wstring& uninstallCmd);
void WriteUninstallFailureLog();
// 线程安全地追加一条卸载失败记录（多个工作线程会并发写 g_uninstallFailures）
void AddUninstallFailure(const std::wstring& appName, const std::wstring& reason);

// tools.cpp
void InitToolsPage();
void SwitchToolFunction(int funcId);
void LayoutToolControls(const RECT& tabRect);
void ShowToolControls(bool show);

// SystemCleaner.cpp - Tab 页切换（控制各页控件显隐矩阵，GUI 测试直接调用）
void SwitchTab(int index);

// 系统信息
void RefreshSystemInfo();

// 重复文件查找
void StartDuplicateScan();
void CleanSelectedDuplicates();
void StopDuplicateScan();

// 进程管理
void RefreshProcessList();
void KillSelectedProcess();

// 快捷方式修复
void StartShortcutScan();
void FixSelectedShortcuts();

// 磁盘清理助手
void StartDiskCleanupScan();
void CleanSelectedDiskFiles();
