// Module: cleaner.cpp - Junk file scanning, cleaning, and list management

#include "common.h"

/**
 * @brief 扫描回收站（仅当前用户）
 */
void ScanRecycleBin(long long& totalSize, int& fileCount) {
    totalSize = 0;
    fileCount = 0;

    DWORD drives = GetLogicalDrives();
    for (char letter = 'C'; letter <= 'Z'; letter++) {
        if (!(drives & (1 << (letter - 'A')))) continue;

        wchar_t root[4];
        root[0] = letter;
        root[1] = L':';
        root[2] = L'\\';
        root[3] = 0;

        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOTE) continue;

        SHQUERYRBINFO rbInfo = { 0 };
        rbInfo.cbSize = sizeof(SHQUERYRBINFO);

        if (SUCCEEDED(SHQueryRecycleBinW(root, &rbInfo))) {
            totalSize += (long long)rbInfo.i64Size;
            fileCount += (int)rbInfo.i64NumItems;
        }
    }
}

/**
 * @brief 清空回收站（仅当前用户）
 *        释放大小必须在清空成功后重新统计，失败时不得虚报已释放
 */
bool CleanRecycleBin(long long& freedSize) {
    long long rbSize = 0;
    int rbCount = 0;
    ScanRecycleBin(rbSize, rbCount);
    freedSize = 0;

    DWORD drives = GetLogicalDrives();
    bool success = true;

    for (char letter = 'C'; letter <= 'Z'; letter++) {
        if (!(drives & (1 << (letter - 'A')))) continue;

        wchar_t root[4] = { 0 };
        root[0] = letter;
        root[1] = L':';
        root[2] = L'\\';

        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOTE) continue;

        HRESULT hr = SHEmptyRecycleBinW(NULL, root,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        // 回收站本来为空时返回 RB_NO_ITEMS，视为成功而非失败
        if (FAILED(hr) && hr != (HRESULT)0x8000FFFF) success = false;
    }

    if (!success) return false;

    // 清空后重新统计，以实际释放量为准
    long long remainSize = 0;
    int remainCount = 0;
    ScanRecycleBin(remainSize, remainCount);
    freedSize = rbSize - remainSize;
    if (freedSize < 0) freedSize = 0;
    return true;
}

// 递归扫描/清理的最大目录深度，防止符号链接环或超深目录导致栈溢出
static const int kMaxScanDepth = 40;

/**
 * @brief 递归扫描目录中匹配模式的文件
 */
static void ScanDirectoryDepth(const std::wstring& dir, const std::wstring& pattern,
                    long long& totalSize, int& fileCount, bool protectData, int depth) {
    if (g_stopScan) return;
    if (depth > kMaxScanDepth) return;
    if (IsSystemCriticalPath(dir)) return;
    if (IsPathTraversal(dir)) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = dir + L"\\" + pattern;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (g_stopScan) break;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (protectData && IsProtectedDataFile(fd.cFileName)) continue;
            totalSize += FileSizeFromFindData(&fd);
            fileCount++;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    searchPath = dir + L"\\*.*";
    hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (g_stopScan) break;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                // 跳过重解析点（符号链接/junction），避免目录成环造成无限递归
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                ScanDirectoryDepth(dir + L"\\" + fd.cFileName, pattern, totalSize, fileCount, protectData, depth + 1);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

void ScanDirectory(const std::wstring& dir, const std::wstring& pattern,
                    long long& totalSize, int& fileCount, bool protectData) {
    ScanDirectoryDepth(dir, pattern, totalSize, fileCount, protectData, 0);
}

/**
 * @brief 递归清理目录中匹配模式的文件
 */
static void CleanDirectoryDepth(const std::wstring& dir, const std::wstring& pattern,
                     long long& freedSize, bool protectData, int depth) {
    if (g_stopClean) return;
    if (depth > kMaxScanDepth) return;
    if (IsSystemCriticalPath(dir)) return;
    if (IsPathTraversal(dir)) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = dir + L"\\" + pattern;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (g_stopClean) break;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (protectData && IsProtectedDataFile(fd.cFileName)) continue;

            std::wstring filePath = dir + L"\\" + fd.cFileName;

            if (IsFileLocked(filePath)) {
                g_cleanFailures.push_back({ filePath, L"文件被占用，跳过" });
                continue;
            }

            long long fileSize = FileSizeFromFindData(&fd);

            // 只读缓存文件需先清除只读属性再删除，否则永久无法清理
            if (ForceDeleteFile(filePath.c_str())) {
                freedSize += fileSize;
            }
            else {
                DWORD err = GetLastError();
                wchar_t reason[256];
                swprintf_s(reason, L"删除失败 (错误码: %lu)", err);
                g_cleanFailures.push_back({ filePath, reason });
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    searchPath = dir + L"\\*.*";
    hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (g_stopClean) break;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                CleanDirectoryDepth(dir + L"\\" + fd.cFileName, pattern, freedSize, protectData, depth + 1);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

void CleanDirectory(const std::wstring& dir, const std::wstring& pattern,
                     long long& freedSize, bool protectData) {
    CleanDirectoryDepth(dir, pattern, freedSize, protectData, 0);
}

/**
 * @brief 扫描单个转储文件（如 C:\Windows\MEMORY.DMP），不存在则跳过
 */
static void ScanDumpFile(const std::wstring& filePath, long long& totalSize, int& fileCount) {
    if (g_stopScan) return;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(filePath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    bool isFile = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    ULARGE_INTEGER li;
    li.LowPart = fd.nFileSizeLow;
    li.HighPart = fd.nFileSizeHigh;
    FindClose(hFind);

    if (isFile) {
        totalSize += li.QuadPart;
        fileCount++;
    }
}

/**
 * @brief 清理单个转储文件（如 C:\Windows\MEMORY.DMP），不存在则跳过
 */
static void CleanDumpFile(const std::wstring& filePath, long long& freedSize) {
    if (g_stopClean) return;

    if (IsFileLocked(filePath)) {
        g_cleanFailures.push_back({ filePath, L"文件被占用，跳过" });
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(filePath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    bool isFile = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    long long fileSize = FileSizeFromFindData(&fd);
    FindClose(hFind);

    if (!isFile) return;

    if (ForceDeleteFile(filePath.c_str())) {
        freedSize += fileSize;
    }
    else {
        DWORD err = GetLastError();
        wchar_t reason[256];
        swprintf_s(reason, L"删除失败 (错误码: %lu)", err);
        g_cleanFailures.push_back({ filePath, reason });
    }
}

/**
 * @brief 初始化清理项目列表
 */
void InitCleanItems() {
    wchar_t userPath[MAX_PATH] = { 0 };
    wchar_t tempPath[MAX_PATH] = { 0 };
    wchar_t docPath[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userPath);
    SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docPath);
    GetTempPathW(MAX_PATH, tempPath);

    // 系统目录基于本机真实位置，避免 Windows 不在 C 盘时清理项全部失效
    wchar_t sysRootBuf[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(sysRootBuf, MAX_PATH) == 0) wcscpy_s(sysRootBuf, L"C:\\Windows");
    wchar_t progDataBuf[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, progDataBuf))) {
        wcscpy_s(progDataBuf, L"C:\\ProgramData");
    }
    const std::wstring sysRoot = sysRootBuf;
    const std::wstring progData = progDataBuf;

    std::wstring chromeBase = std::wstring(userPath) + L"\\AppData\\Local\\Google\\Chrome\\User Data";
    std::wstring edgeBase = std::wstring(userPath) + L"\\AppData\\Local\\Microsoft\\Edge\\User Data";
    std::wstring wechatBase = std::wstring(docPath) + L"\\WeChat Files";
    std::wstring qqBase = std::wstring(docPath) + L"\\Tencent Files";
    std::wstring timBase = std::wstring(docPath) + L"\\Tencent Files";
    std::wstring firefoxBase = std::wstring(userPath) + L"\\AppData\\Local\\Mozilla\\Firefox\\Profiles";

    g_cleanItems = {
        // 【安全清理 - 默认勾选】系统/浏览器临时文件、缓存等
        { L"系统临时文件", L"系统和软件产生的临时文件", { tempPath, sysRoot + L"\\Temp" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"Chrome浏览器缓存", L"Chrome网页缓存(Cache/Code Cache/GPU缓存等)", { chromeBase }, L"*.*", 0, 0, true, false, true,
          { L"*\\Cache", L"*\\Code Cache", L"*\\GPUCache", L"*\\Service Worker\\CacheStorage" }, SpecialCleanType::None, false },
        { L"Edge浏览器缓存", L"Edge网页缓存(Cache/Code Cache/GPU缓存等)", { edgeBase }, L"*.*", 0, 0, true, false, true,
          { L"*\\Cache", L"*\\Code Cache", L"*\\GPUCache", L"*\\Service Worker\\CacheStorage" }, SpecialCleanType::None, false },
        { L"Firefox浏览器缓存", L"Firefox网页缓存与页面缩略图", { firefoxBase }, L"*.*", 0, 0, true, false, true,
          { L"*\\cache2", L"*\\startupCache", L"*\\thumbnails" }, SpecialCleanType::None, false },
        { L"缩略图缓存", L"系统图标和缩略图缓存", { std::wstring(userPath) + L"\\AppData\\Local\\Microsoft\\Windows\\Explorer" }, L"thumbcache_*.db", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"系统日志文件", L"Windows系统日志和安装日志", { sysRoot + L"\\Logs", sysRoot + L"\\Panther" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"回收站", L"回收站中的文件", { L"C:\\" }, L"", 0, 0, true, false, true, {}, SpecialCleanType::RecycleBin, false },
        { L"Windows更新缓存", L"Windows更新下载的临时安装包", { sysRoot + L"\\SoftwareDistribution\\Download" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"Windows传递优化缓存", L"系统更新共享下载的缓存文件", { sysRoot + L"\\SoftwareDistribution\\DeliveryOptimization" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"DirectX着色器缓存", L"显卡编译的着色器缓存(删除后游戏会重新编译)", { std::wstring(userPath) + L"\\AppData\\Local\\D3DSCache", std::wstring(userPath) + L"\\AppData\\Local\\NVIDIA\\DXCache", std::wstring(userPath) + L"\\AppData\\Local\\AMD\\DxCache" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"应用崩溃转储", L"应用程序崩溃产生的转储文件", { std::wstring(userPath) + L"\\AppData\\Local\\CrashDumps" }, L"*.dmp", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"程序预取文件", L"程序启动预读取缓存（删除后首次启动可能变慢）", { sysRoot + L"\\Prefetch" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"系统错误报告", L"系统崩溃和程序错误报告文件", { progData + L"\\Microsoft\\Windows\\WER" }, L"*.*", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"内存转储文件", L"系统蓝屏内存转储文件", { sysRoot + L"\\Minidump" }, L"*.dmp", 0, 0, true, false, true, {}, SpecialCleanType::MemoryDump, false },

        // 【谨慎清理 - 默认不勾选】需用户主动选择的项目
        { L"旧版Windows系统文件", L"升级后残留的旧系统文件(删除后无法回退到旧版本)", { sysRoot + L".old" }, L"*.*", 0, 0, false, false, true, {}, SpecialCleanType::None, false },
        { L"最近使用记录", L"开始菜单与资源管理器的最近访问记录", { std::wstring(userPath) + L"\\AppData\\Roaming\\Microsoft\\Windows\\Recent" }, L"*.*", 0, 0, false, true, true, {}, SpecialCleanType::None, false },

        // 【隐私数据 - 默认不勾选】涉及账号、密码、Cookie等敏感信息
        { L"Chrome保存的密码", L"清除所有网站登录密码，删除后不可恢复", { chromeBase }, L"Login Data*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },
        { L"Edge保存的密码", L"清除所有网站登录密码，删除后不可恢复", { edgeBase }, L"Login Data*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },
        { L"Chrome Cookie", L"清除网站登录状态与偏好设置", { chromeBase }, L"Cookies*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },
        { L"Edge Cookie", L"清除网站登录状态与偏好设置", { edgeBase }, L"Cookies*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },
        { L"Chrome浏览历史", L"清除访问记录与下载记录", { chromeBase }, L"History*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },
        { L"Edge浏览历史", L"清除访问记录与下载记录", { edgeBase }, L"History*", 0, 0, false, true, false, {}, SpecialCleanType::None, false },

        // 【聊天软件 - 缓存默认勾选，日志默认勾选，接收文件默认不勾选】
        { L"微信图片视频缓存", L"聊天产生的图片、视频、表情缓存", { wechatBase }, L"*.*", 0, 0, true, false, true, { L"*\\Msg\\Cache", L"*\\Msg\\Emotion", L"*\\Msg\\Attach" }, SpecialCleanType::None, false },
        { L"微信日志文件", L"微信运行日志与崩溃报告", { wechatBase }, L"*.log", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"微信接收文件", L"接收的文档、安装包等文件", { wechatBase }, L"*.*", 0, 0, false, false, true, { L"*\\FileStorage\\FileRecv" }, SpecialCleanType::None, false },

        { L"QQ图片头像缓存", L"聊天图片、头像、表情缓存", { qqBase }, L"*.*", 0, 0, true, false, true, { L"*\\Image", L"*\\Avatar", L"*\\Emotion" }, SpecialCleanType::None, false },
        { L"QQ日志文件", L"QQ运行日志文件", { qqBase }, L"*.log", 0, 0, true, false, true, {}, SpecialCleanType::None, false },
        { L"QQ接收文件", L"接收的文件与离线文件", { qqBase }, L"*.*", 0, 0, false, false, true, { L"*\\FileRecv" }, SpecialCleanType::None, false },

        { L"TIM图片头像缓存", L"TIM聊天图片、头像缓存", { timBase }, L"*.*", 0, 0, true, false, true, { L"*\\Image", L"*\\Avatar" }, SpecialCleanType::None, false },
        { L"TIM接收文件", L"TIM接收的文档文件", { timBase }, L"*.*", 0, 0, false, false, true, { L"*\\FileRecv" }, SpecialCleanType::None, false },
    };
}

/**
 * @brief 根据可用空间调整 ListView 高度，填满可用区域
 */
void AdjustListViewHeight(HWND hListView, const RECT& tabRect, int topOffset, int bottomReserve) {
    if (!hListView) return;

    // 列表统一放进内容卡片；清理页底部额外留出进度条的位置
    int extraBottom = (hListView == g_hCleanList) ? UiScale(20) : 0;
    UiPlaceListViewInCard(hListView, tabRect, extraBottom);
    (void)topOffset;
    (void)bottomReserve;

    RECT listRect;
    GetClientRect(hListView, &listRect);
    int availH = listRect.bottom - listRect.top;
    if (availH <= 0) availH = 100;

    int itemCount = ListView_GetItemCount(hListView);
    if (itemCount <= 1) {
        ShowScrollBar(hListView, SB_VERT, FALSE);
        ShowScrollBar(hListView, SB_HORZ, FALSE);
    }
    else {
        HWND hHeader = ListView_GetHeader(hListView);
        int headerH = 0;
        if (hHeader) {
            RECT headerRect;
            GetWindowRect(hHeader, &headerRect);
            headerH = headerRect.bottom - headerRect.top;
        }

        RECT itemRect;
        itemRect.top = 0;
        ListView_GetItemRect(hListView, 0, &itemRect, LVIR_BOUNDS);
        int itemH = itemRect.bottom - itemRect.top;

        if (itemH <= 0) itemH = 20;

        int maxVisibleRows = (availH - headerH) / itemH;
        if (maxVisibleRows <= 0) maxVisibleRows = 1;

        ShowScrollBar(hListView, SB_VERT, itemCount > maxVisibleRows ? TRUE : FALSE);
        ShowScrollBar(hListView, SB_HORZ, FALSE);
    }

    InvalidateRect(hListView, NULL, TRUE);
    UpdateWindow(hListView);
}

/**
 * @brief 刷新垃圾清理列表控件
 */
void RefreshCleanList() {
    ListView_DeleteAllItems(g_hCleanList);
    wchar_t strPending[] = L"待扫描";
    wchar_t strZero[] = L"0";

    for (size_t i = 0; i < g_cleanItems.size(); i++) {
        g_cleanItems[i].isScanned = false;

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)i;
        lvi.lParam = (LPARAM)&g_cleanItems[i];

        wchar_t buf[256];
        wcscpy_s(buf, g_cleanItems[i].name.c_str());
        lvi.pszText = buf;
        ListView_InsertItem(g_hCleanList, &lvi);

        ListView_SetItemText(g_hCleanList, (int)i, 1, (LPWSTR)g_cleanItems[i].desc.c_str());
        ListView_SetItemText(g_hCleanList, (int)i, 2, strPending);
        ListView_SetItemText(g_hCleanList, (int)i, 3, strZero);
        ListView_SetCheckState(g_hCleanList, (int)i, g_cleanItems[i].defaultChecked ? TRUE : FALSE);
    }

    RECT tabRect;
    GetClientRect(g_hTab, &tabRect);
    TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
    AdjustListViewHeight(g_hCleanList, tabRect, 50, 50);

    InvalidateRect(g_hCleanList, NULL, TRUE);
    UpdateWindow(g_hCleanList);
}

/**
 * @brief 扫描线程过程函数
 */
// 由 UI 线程在启动工作线程前采集的勾选快照。
// ListView 的勾选状态属于控件状态，必须在 UI 线程读取，
// 在工作线程里直接调用 ListView_GetCheckState 属于跨线程访问控件，
// 扫描过程中用户改变勾选会导致结果错乱。
static std::vector<int> g_scanSnapshot;
static std::vector<int> g_cleanSnapshot;

int CollectScanSnapshot() {
    g_scanSnapshot.clear();
    if (!g_hCleanList) return 0;
    int count = ListView_GetItemCount(g_hCleanList);
    for (int i = 0; i < count; i++) {
        if (ListView_GetCheckState(g_hCleanList, i)) g_scanSnapshot.push_back(i);
    }
    return (int)g_scanSnapshot.size();
}

void CollectCleanSnapshot() {
    g_cleanSnapshot.clear();
    if (!g_hCleanList) return;
    int count = ListView_GetItemCount(g_hCleanList);
    for (int i = 0; i < count; i++) {
        if (ListView_GetCheckState(g_hCleanList, i) && g_cleanItems[i].isScanned) {
            g_cleanSnapshot.push_back(i);
        }
    }
}

DWORD WINAPI ScanThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    long long totalSize = 0;

    std::vector<int> selectedIndices = g_scanSnapshot;

    for (auto& item : g_cleanItems) {
        item.totalSize = 0;
        item.fileCount = 0;
    }

    wchar_t sysRootBuf[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(sysRootBuf, MAX_PATH) == 0) wcscpy_s(sysRootBuf, L"C:\\Windows");
    const std::wstring sysRoot = sysRootBuf;

    for (size_t idx = 0; idx < selectedIndices.size(); idx++) {
        if (g_stopScan) break;
        int i = selectedIndices[idx];
        if (i < 0 || i >= (int)g_cleanItems.size()) continue;
        long long size = 0;
        int count = 0;
        bool protectData = g_cleanItems[i].protectData;
        const auto& item = g_cleanItems[i];

        if (item.specialType == SpecialCleanType::RecycleBin) {
            ScanRecycleBin(size, count);
        }
        else if (item.specialType == SpecialCleanType::MemoryDump) {
            ScanDumpFile(sysRoot + L"\\MEMORY.DMP", size, count);
            ScanDirectory(sysRoot + L"\\Minidump", L"*.dmp", size, count, protectData);
        }
        else if (!item.subDirPatterns.empty()) {
            for (const auto& basePath : item.paths) {
                if (IsPathTraversal(basePath)) continue;
                if (!PathFileExistsW(basePath.c_str())) continue;

                auto matchedDirs = FindMatchingSubDirs(basePath, item.subDirPatterns);
                for (const auto& dir : matchedDirs) {
                    if (IsSystemCriticalPath(dir)) continue;
                    ScanDirectory(dir, item.pattern, size, count, protectData);
                }
            }
        }
        else {
            for (const auto& path : item.paths) {
                if (IsPathTraversal(path)) continue;
                if (PathFileExistsW(path.c_str())) {
                    ScanDirectory(path, item.pattern, size, count, protectData);
                }
            }
        }
        g_cleanItems[i].totalSize = size;
        g_cleanItems[i].fileCount = count;
        g_cleanItems[i].isScanned = true;
        totalSize += size;

        PostMessageW(hWnd, WM_SCAN_PROGRESS, (WPARAM)idx, (LPARAM)i);
    }

    // 总大小必须经 LPARAM 的堆指针传递：Win32 构建下 WPARAM 只有 32 位，
    // 直接塞 long long 会把超过 4 GB 的结果截断成错误数值
    long long* pTotal = new (std::nothrow) long long(totalSize);
    if (!PostMessageW(hWnd, WM_SCAN_DONE, 0, (LPARAM)pTotal)) delete pTotal;
    return 0;
}

/**
 * @brief 清理线程过程函数
 */
DWORD WINAPI CleanThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    long long totalFreed = 0;

    g_cleanFailures.clear();

    // 使用 UI 线程启动清理时采集的勾选快照，避免跨线程读取控件状态
    std::vector<int> selectedIndices = g_cleanSnapshot;

    wchar_t sysRootBuf[MAX_PATH] = { 0 };
    if (GetWindowsDirectoryW(sysRootBuf, MAX_PATH) == 0) wcscpy_s(sysRootBuf, L"C:\\Windows");
    const std::wstring sysRoot = sysRootBuf;

    for (size_t idx = 0; idx < selectedIndices.size(); idx++) {
        if (g_stopClean) break;
        int i = selectedIndices[idx];
        if (i < 0 || i >= (int)g_cleanItems.size()) continue;
        long long freed = 0;
        bool protectData = g_cleanItems[i].protectData;
        const auto& item = g_cleanItems[i];

        if (item.specialType == SpecialCleanType::RecycleBin) {
            CleanRecycleBin(freed);
        }
        else if (item.specialType == SpecialCleanType::MemoryDump) {
            CleanDumpFile(sysRoot + L"\\MEMORY.DMP", freed);
            CleanDirectory(sysRoot + L"\\Minidump", L"*.dmp", freed, protectData);
        }
        else if (!item.subDirPatterns.empty()) {
            for (const auto& basePath : item.paths) {
                if (IsPathTraversal(basePath)) continue;
                if (!PathFileExistsW(basePath.c_str())) continue;

                auto matchedDirs = FindMatchingSubDirs(basePath, item.subDirPatterns);
                for (const auto& dir : matchedDirs) {
                    if (IsSystemCriticalPath(dir)) continue;
                    CleanDirectory(dir, item.pattern, freed, protectData);
                }
            }
        }
        else {
            for (const auto& path : item.paths) {
                if (IsPathTraversal(path)) continue;
                if (PathFileExistsW(path.c_str())) {
                    CleanDirectory(path, item.pattern, freed, protectData);
                }
            }
        }
        totalFreed += freed;
        PostMessageW(hWnd, WM_CLEAN_PROGRESS, (WPARAM)idx, (LPARAM)selectedIndices.size());
    }

    // 同上：释放总量经 LPARAM 堆指针传递，避免 Win32 构建下 WPARAM 截断
    long long* pFreed = new (std::nothrow) long long(totalFreed);
    if (!PostMessageW(hWnd, WM_CLEAN_DONE, 0, (LPARAM)pFreed)) delete pFreed;
    return 0;
}