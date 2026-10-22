// Module: tools.cpp - Utility tools page (system info, duplicates, processes, shortcuts, disk cleanup)

#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <shobjidl.h>
#include <shlwapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")

// ====================== Tools page control IDs ======================
#define ID_TOOL_FUNC_COMBO    6010
#define ID_TOOL_PATH_EDIT     6011
#define ID_TOOL_BTN_BROWSE    6012
#define ID_TOOL_BTN_SCAN      6013
#define ID_TOOL_BTN_ACTION    6014
#define ID_TOOL_AGE_COMBO     6015
#define ID_TOOL_INFO_LABEL    6016

// Duplicate scan state
static bool g_isDupScanning = false;
static bool g_stopDupScan = false;

// Forward declarations for internal helpers
static void CreateToolListColumns();
static void PopulateSystemInfo();
static DWORD WINAPI DupScanThreadProc(LPVOID lpParam);
static std::wstring GetFileHash(const std::wstring& path);
static bool IsShortcutBroken(const std::wstring& lnkVPath, std::wstring& targetPath);

// ====================== Initialization ======================

void InitToolsPage() {
    // Controls created dynamically in WM_CREATE; this initializes default state
    g_toolFunction = 0;
}

// ====================== Panel Switching ======================

void SwitchToolFunction(int funcId) {
    g_toolFunction = funcId;
    if (!g_hToolListView) return;

    // First, ALWAYS hide all dynamic controls to clean slate
    ShowWindow(g_hToolListView, SW_HIDE);
    ShowWindow(g_hToolPathEdit, SW_HIDE);
    ShowWindow(g_hShredPath, SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_HIDE);
    ShowWindow(g_hToolAgeCombo, SW_HIDE);
    ShowWindow(g_hToolInfoLabel, SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 6001), SW_HIDE); // 选择文件
    ShowWindow(GetDlgItem(g_hTab, 6002), SW_HIDE); // 开始粉碎

    ListView_DeleteAllItems(g_hToolListView);

    wchar_t scanLabel[64] = { 0 };
    wchar_t actionLabel[64] = { 0 };
    const wchar_t* infoHint = nullptr;

    switch (funcId) {
    case 0: // 文件粉碎
        ShowWindow(g_hShredPath, SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, 6001), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, 6002), SW_SHOW);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
        infoHint = L"粉碎会覆写文件内容，操作不可撤销";
        break;

    case 1: // 系统信息
        ShowWindow(g_hToolListView, SW_SHOW);
        CreateToolListColumns();
        PopulateSystemInfo();
        break;

    case 2: // 重复文件查找
        wcscpy_s(scanLabel, L"开始扫描");
        wcscpy_s(actionLabel, L"删除选中");
        ShowWindow(g_hToolPathEdit, SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_SHOW);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
        ShowWindow(g_hToolListView, SW_SHOW);
        CreateToolListColumns();
        infoHint = L"选择文件夹后点击「开始扫描」查找重复文件";
        break;

    case 3: // 进程管理
        wcscpy_s(scanLabel, L"刷新列表");
        wcscpy_s(actionLabel, L"结束进程");
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_SHOW);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
        ShowWindow(g_hToolListView, SW_SHOW);
        CreateToolListColumns();
        RefreshProcessList();
        break;

    case 4: // 快捷方式修复
        wcscpy_s(scanLabel, L"扫描失效");
        wcscpy_s(actionLabel, L"修复选中");
        ShowWindow(g_hToolPathEdit, SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_SHOW);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
        ShowWindow(g_hToolListView, SW_SHOW);
        CreateToolListColumns();
        infoHint = L"选择文件夹后点击「扫描失效」检查快捷方式";
        break;

    case 5: // 磁盘清理助手
        wcscpy_s(scanLabel, L"开始扫描");
        wcscpy_s(actionLabel, L"删除选中");
        ShowWindow(g_hToolPathEdit, SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_SHOW);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_SHOW);
        ShowWindow(g_hToolAgeCombo, SW_SHOW);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
        ShowWindow(g_hToolListView, SW_SHOW);
        CreateToolListColumns();
        infoHint = L"选择文件夹与文件年龄后点击「开始扫描」";
        break;
    }

    if (scanLabel[0]) SetWindowTextW(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), scanLabel);
    if (actionLabel[0]) SetWindowTextW(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), actionLabel);
    if (infoHint) SetWindowTextW(g_hToolInfoLabel, infoHint);

    if (g_hTab) {
        RECT tabRect;
        GetClientRect(g_hTab, &tabRect);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
        LayoutToolControls(tabRect);
        InvalidateRect(g_hTab, NULL, TRUE);
    }
}

// ====================== Layout Helpers ======================

void LayoutToolControls(const RECT& tabRect) {
    int left = tabRect.left;
    int right = tabRect.right;
    int top = tabRect.top;
    int pad = UiScale(UiMetrics::CardPadding);
    int btnH = UiScale(UiMetrics::ButtonHeight);
    int gap = UiScale(8);
    int btnW = UiScale(94);

    // 第一行：功能选择 + 路径 + 选择/浏览
    SetWindowPos(GetDlgItem(g_hTab, 6030), NULL, left,
        top + (btnH - UiScale(18)) / 2, UiScale(64), UiScale(18), SWP_NOZORDER);
    UiCenterComboInRow(g_hToolFuncCombo, left + UiScale(68), UiScale(168),
        top, btnH, 260);

    int pathX = left + UiScale(248);
    int pathW = right - btnW - gap - pathX;
    if (pathW < UiScale(160)) pathW = UiScale(160);
    if (g_hShredPath)
        SetWindowPos(g_hShredPath, NULL, pathX, top, pathW, btnH, SWP_NOZORDER);
    if (g_hToolPathEdit)
        SetWindowPos(g_hToolPathEdit, NULL, pathX, top, pathW, btnH, SWP_NOZORDER);

    SetWindowPos(GetDlgItem(g_hTab, 6001), NULL, right - btnW, top, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), NULL, right - btnW, top, btnW, btnH, SWP_NOZORDER);

    // 第二行：文件年龄 + 提示信息 + 扫描/操作
    int row2 = top + btnH + gap;
    UiCenterComboInRow(g_hToolAgeCombo, left, UiScale(116), row2, btnH, 240);

    int actionX = right - btnW;
    int scanX = actionX - gap - btnW;
    SetWindowPos(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), NULL, scanX, row2, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), NULL, actionX, row2, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hTab, 6002), NULL, actionX, row2, btnW, btnH, SWP_NOZORDER);

    int infoX = (g_toolFunction == 5) ? left + UiScale(124) : left;
    if (g_hToolInfoLabel)
        SetWindowPos(g_hToolInfoLabel, NULL, infoX,
            row2 + (btnH - UiScale(18)) / 2,
            scanX - gap - infoX, UiScale(18), SWP_NOZORDER);

    // 列表：系统信息页可紧贴功能栏，其余功能为两行工具栏留出空间
    int cardTop = top + UiScale(g_toolFunction == 1 ? UiMetrics::CardTop
                                                   : UiMetrics::CardTop + 40);
    int listTop = cardTop + pad;
    int listH = tabRect.bottom - UiScale(16) - pad - listTop;
    if (listH < UiScale(80)) listH = UiScale(80);
    if (g_hToolListView)
        SetWindowPos(g_hToolListView, NULL, left + pad, listTop,
            right - left - pad * 2, listH, SWP_NOZORDER);

    HWND toolHeader = g_hToolListView ? ListView_GetHeader(g_hToolListView) : NULL;
    if (toolHeader && Header_GetItemCount(toolHeader) > 0) {
        UiStretchLastColumn(g_hToolListView);
    }
}

void ShowToolControls(bool show) {
    if (show) {
        // Show function selector label + combo; other controls handled by SwitchToolFunction
        ShowWindow(GetDlgItem(g_hTab, 6030), SW_SHOW); // 功能选择标签
        ShowWindow(g_hToolFuncCombo, SW_SHOW);
        SwitchToolFunction(g_toolFunction);
    } else {
        // Hide ALL tool-related controls
        ShowWindow(GetDlgItem(g_hTab, 6030), SW_HIDE); // 功能选择标签
        ShowWindow(g_hToolFuncCombo, SW_HIDE);
        ShowWindow(g_hToolListView, SW_HIDE);
        ShowWindow(g_hToolPathEdit, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_BROWSE), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_SCAN), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, ID_TOOL_BTN_ACTION), SW_HIDE);
        ShowWindow(g_hToolAgeCombo, SW_HIDE);
        ShowWindow(g_hToolInfoLabel, SW_HIDE);
        ShowWindow(g_hShredPath, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6002), SW_HIDE);
    }
}

// ====================== ListView Column Setup ======================

// 当前已建立的列数，用于在切换功能时先清理旧列，
// 否则反复切换功能会让列不断累积重复
static int g_toolColumnCount = 0;

static void DeleteToolListColumns() {
    while (g_toolColumnCount > 0) {
        ListView_DeleteColumn(g_hToolListView, 0);
        g_toolColumnCount--;
    }
}

static void AddToolColumn(const wchar_t* title, int width) {
    LVCOLUMNW lvc = { 0 };
    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    lvc.fmt = LVCFMT_LEFT;
    lvc.pszText = (LPWSTR)title;
    lvc.cx = UiScale(width);
    if (ListView_InsertColumn(g_hToolListView, g_toolColumnCount, &lvc) >= 0) {
        g_toolColumnCount++;
    }
}

static void CreateToolListColumns() {
    ListView_DeleteAllItems(g_hToolListView);
    DeleteToolListColumns();

    switch (g_toolFunction) {
    case 1: // 系统信息
        AddToolColumn(L"项目", 200);
        AddToolColumn(L"数值", 500);
        break;
    case 2: // 重复文件
        AddToolColumn(L"文件名", 200);
        AddToolColumn(L"大小", 100);
        AddToolColumn(L"路径", 400);
        break;
    case 3: // 进程管理
        AddToolColumn(L"进程名", 180);
        AddToolColumn(L"PID", 80);
        AddToolColumn(L"内存(MB)", 100);
        AddToolColumn(L"路径", 320);
        break;
    case 4: // 快捷方式修复
        AddToolColumn(L"快捷方式", 200);
        AddToolColumn(L"指向目标", 400);
        AddToolColumn(L"状态", 100);
        break;
    case 5: // 磁盘清理
        AddToolColumn(L"文件名", 250);
        AddToolColumn(L"大小", 100);
        AddToolColumn(L"修改时间", 150);
        AddToolColumn(L"路径", 200);
        break;
    }
}

// ====================== 1. 系统信息 ======================

void RefreshSystemInfo() {
    PopulateSystemInfo();
}

static void PopulateSystemInfo() {
    LVITEMW lvi = { 0 };
    lvi.mask = LVIF_TEXT;

    // 每次刷新前先重建列表与列，否则重复点击「刷新」会让条目成倍增加
    CreateToolListColumns();

    auto addItem = [&](const wchar_t* name, const wchar_t* value) {
        wchar_t nameBuf[128], valBuf[512];
        wcscpy_s(nameBuf, name);
        wcscpy_s(valBuf, value);
        lvi.iItem = ListView_GetItemCount(g_hToolListView);
        lvi.pszText = nameBuf;
        int row = ListView_InsertItem(g_hToolListView, &lvi);
        if (row >= 0) ListView_SetItemText(g_hToolListView, row, 1, valBuf);
    };

    wchar_t buf[512];

    // ===== 操作系统 =====
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) hNtDll = LoadLibraryW(L"ntdll.dll");
    if (hNtDll) {
        typedef NTSTATUS(NTAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        auto RtlGetVersion = (RtlGetVersionPtr) GetProcAddress(hNtDll, "RtlGetVersion");
        if (RtlGetVersion) {
            RTL_OSVERSIONINFOW osvi = { sizeof(osvi) };
            if (RtlGetVersion(&osvi) == 0) {
                wchar_t csName[256] = { 0 };
                DWORD dwSize = sizeof(csName);
                RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"ProductName", RRF_RT_REG_SZ, NULL, csName, &dwSize);
                swprintf_s(buf, L"%ls %u.%u.%u", csName, (UINT)osvi.dwMajorVersion, (UINT)osvi.dwMinorVersion, (UINT)osvi.dwBuildNumber);
                addItem(L"操作系统", buf);

                wchar_t displayVer[64] = { 0 };
                dwSize = sizeof(displayVer);
                RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"DisplayVersion", RRF_RT_REG_SZ, NULL, displayVer, &dwSize);
                if (displayVer[0]) addItem(L"版本号", displayVer);
            }
        }
        if (GetModuleHandleW(L"ntdll.dll") != hNtDll) FreeLibrary(hNtDll);
    }

    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 2] = { 0 };
    DWORD compSize = _countof(compName);
    if (GetComputerNameW(compName, &compSize)) addItem(L"计算机名", compName);

    wchar_t userName[256] = { 0 };
    DWORD nameSize = _countof(userName);
    if (GetUserNameW(userName, &nameSize)) addItem(L"当前用户", userName);
    addItem(L"管理员权限", IsRunAsAdmin() ? L"是" : L"否");

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    addItem(L"系统架构", (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? L"64位 (x64)" : L"32位 (x86)");

    ULONGLONG ticks = GetTickCount64();
    long long uptimeSec = (long long)(ticks / 1000);
    long long days = uptimeSec / 86400;
    long long hours = (uptimeSec % 86400) / 3600;
    long long mins = (uptimeSec % 3600) / 60;
    swprintf_s(buf, L"%lld天 %lld小时 %lld分钟", days, hours, mins);
    addItem(L"运行时长", buf);

    // ===== CPU =====
    wchar_t cpuName[256] = { 0 };
    DWORD cpuSize = (DWORD)sizeof(cpuName);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpuName, &cpuSize) == 0) {
        addItem(L"CPU型号", cpuName);
    }

    wchar_t vendorId[256] = { 0 };
    // 必须重置缓冲区大小，否则沿用上一次读取后的剩余长度会导致读取失败、CPU 厂商为空
    DWORD vendorSize = (DWORD)sizeof(vendorId);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"VendorIdentifier", RRF_RT_REG_SZ, NULL, vendorId, &vendorSize) == 0) {
        addItem(L"CPU厂商", vendorId);
    }

    wchar_t freqBuf[64] = { 0 };
    DWORD freqSize = sizeof(freqBuf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"~MHz", RRF_RT_REG_DWORD, NULL, freqBuf, &freqSize) == 0) {
        DWORD mhz = *(DWORD*)freqBuf;
        swprintf_s(buf, L"%u MHz (%.2f GHz)", mhz, mhz / 1000.0);
        addItem(L"CPU频率", buf);
    }

    // 核心数：物理核心数（从注册表取，比 dwNumberOfProcessors 更准确）
    {
        DWORD coreCount = 0;
        HKEY hCpu = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor", 0, KEY_READ, &hCpu) == 0) {
            wchar_t subName[64] = { 0 };
            DWORD subLen = 0;
            for (DWORD i = 0; ; i++) {
                subLen = _countof(subName);
                if (RegEnumKeyExW(hCpu, i, subName, &subLen, NULL, NULL, NULL, NULL) != 0) break;
                DWORD cores = 0, coreSize = sizeof(cores);
                if (RegGetValueW(HKEY_LOCAL_MACHINE,
                        (std::wstring(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\") + subName).c_str(),
                        L"Core Information", RRF_RT_REG_BINARY, NULL, &cores, &coreSize) == 0) {
                    // Core Information 低 8 位为物理核心数
                    coreCount += (cores & 0xFF);
                } else {
                    coreCount++;   // 无法判定时按 1 核计（超线程机器会退化，但不为 0）
                }
            }
            RegCloseKey(hCpu);
        }
        if (coreCount == 0) coreCount = si.dwNumberOfProcessors;
        addItem(L"核心数", std::to_wstring(coreCount).c_str());
    }
    // 线程数：逻辑处理器数
    addItem(L"线程数", std::to_wstring(si.dwNumberOfProcessors).c_str());

    // ===== 内存 =====
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    swprintf_s(buf, L"%llu MB (%llu GB)", ms.ullTotalPhys / (1024 * 1024), ms.ullTotalPhys / (1024 * 1024 * 1024));
    addItem(L"总内存", buf);
    swprintf_s(buf, L"%llu MB", ms.ullAvailPhys / (1024 * 1024));
    addItem(L"可用内存", buf);
    swprintf_s(buf, L"%u%%", ms.dwMemoryLoad);
    addItem(L"内存使用率", buf);
    swprintf_s(buf, L"%llu MB", ms.ullTotalPageFile / (1024 * 1024));
    addItem(L"虚拟内存", buf);

    // ===== 主板信息 =====
    HKEY hBoard;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &hBoard) == 0) {
        wchar_t infoBuf[256] = { 0 };
        // 每次读取前必须重置缓冲区大小：RegGetValue 成功后会把剩余大小改写为
        // 实际读到的字节数，沿用上一次的小值会让后续更长的值读取失败
        DWORD iSize = sizeof(infoBuf);
        if (RegGetValueW(hBoard, NULL, L"BaseBoardManufacturer", RRF_RT_REG_SZ, NULL, infoBuf, &iSize) == 0)
            addItem(L"主板厂商", infoBuf);
        iSize = sizeof(infoBuf);
        if (RegGetValueW(hBoard, NULL, L"BaseBoardProduct", RRF_RT_REG_SZ, NULL, infoBuf, &iSize) == 0)
            addItem(L"主板型号", infoBuf);
        iSize = sizeof(infoBuf);
        if (RegGetValueW(hBoard, NULL, L"BIOSVendor", RRF_RT_REG_SZ, NULL, infoBuf, &iSize) == 0)
            addItem(L"BIOS厂商", infoBuf);
        iSize = sizeof(infoBuf);
        if (RegGetValueW(hBoard, NULL, L"BIOSVersion", RRF_RT_REG_SZ, NULL, infoBuf, &iSize) == 0)
            addItem(L"BIOS版本", infoBuf);
        RegCloseKey(hBoard);
    }

    HKEY hProduct;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SystemInformation", 0, KEY_READ, &hProduct) == 0) {
        wchar_t prodBuf[256] = { 0 };
        DWORD pSize = sizeof(prodBuf);
        if (RegGetValueW(hProduct, NULL, L"SystemManufacturer", RRF_RT_REG_SZ, NULL, prodBuf, &pSize) == 0)
            addItem(L"系统厂商", prodBuf);
        pSize = sizeof(prodBuf);
        if (RegGetValueW(hProduct, NULL, L"SystemProductName", RRF_RT_REG_SZ, NULL, prodBuf, &pSize) == 0)
            addItem(L"产品型号", prodBuf);
        RegCloseKey(hProduct);
    }

    // ===== 显卡信息 =====
    // 必须遍历 Control\Video 下的所有实例 GUID 并筛选 Class 为显示设备（{4d36e968-...}），
    // 硬编码某一个 GUID 只在特定机器上有效，其他机器永远读不到显卡
    wchar_t gpuFound = 0;
    {
        HKEY hVideo = NULL;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Video", 0, KEY_READ, &hVideo) == 0) {
            wchar_t instanceGuid[256] = { 0 };
            DWORD guidLen = 0;
            for (DWORD gi = 0; ; gi++) {
                guidLen = _countof(instanceGuid);
                if (RegEnumKeyExW(hVideo, gi, instanceGuid, &guidLen, NULL, NULL, NULL, NULL) != 0) break;

                std::wstring instancePath = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Video\\") + instanceGuid;

                // 只处理显示设备类的实例
                wchar_t classGuid[128] = { 0 };
                DWORD classLen = sizeof(classGuid);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, instancePath.c_str(), L"ClassGUID", RRF_RT_REG_SZ, NULL, classGuid, &classLen) != 0) {
                    classGuid[0] = 0;
                }
                if (classGuid[0] && _wcsicmp(classGuid, L"{4d36e968-e325-11ce-bfc1-08002be10318}") != 0) continue;

                HKEY hInstance = NULL;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instancePath.c_str(), 0, KEY_READ, &hInstance) != 0) continue;

                wchar_t subKeyName[256] = { 0 };
                DWORD subKeySize = 0;
                for (DWORD idx = 0; ; idx++) {
                    subKeySize = _countof(subKeyName);
                    // 枚举该实例键下的适配器子键（而不是 Video 下的子键）
                    if (RegEnumKeyExW(hInstance, idx, subKeyName, &subKeySize, NULL, NULL, NULL, NULL) != 0) break;
                    std::wstring adapterPath = instancePath + L"\\" + subKeyName;

                    HKEY hAdapter = NULL;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, adapterPath.c_str(), 0, KEY_READ, &hAdapter) == 0) {
                        wchar_t gpuName[256] = { 0 };
                        DWORD gSize = sizeof(gpuName);
                        if (RegGetValueW(hAdapter, NULL, L"Device Description", RRF_RT_REG_SZ, NULL, gpuName, &gSize) == 0 && gpuName[0]) {
                            addItem(L"显卡型号", gpuName);
                            gpuFound = 1;
                        }
                        wchar_t drvName[256] = { 0 };
                        DWORD dSize = sizeof(drvName);   // 每个值都要独立的缓冲区大小
                        if (RegGetValueW(hAdapter, NULL, L"DriverVersion", RRF_RT_REG_SZ, NULL, drvName, &dSize) == 0)
                            addItem(L"显卡驱动", drvName);
                        ULONG ramSize = 0;
                        DWORD rSize = sizeof(ramSize);
                        if (RegGetValueW(hAdapter, NULL, L"AdapterRAM", RRF_RT_REG_DWORD, NULL, &ramSize, &rSize) == 0 && ramSize > 0) {
                            swprintf_s(buf, L"%u MB", ramSize / (1024 * 1024));
                            addItem(L"显存容量", buf);
                        }
                        RegCloseKey(hAdapter);
                        if (gpuFound) break;
                    }
                }
                RegCloseKey(hInstance);
                if (gpuFound) break;
            }
            RegCloseKey(hVideo);
        }
    }

    if (!gpuFound) {
        HKEY hPCI;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, &hPCI) == 0) {
            wchar_t pciKey[256] = { 0 };
            DWORD pciSize = _countof(pciKey);
            int idx2 = 0;
            while (RegEnumKeyExW(hPCI, idx2, pciKey, &pciSize, NULL, NULL, NULL, NULL) == 0) {
                wchar_t classKey[256] = { 0 };
                DWORD cSize = sizeof(classKey);
                HKEY hPCIInst;
                wchar_t instPath[512] = { 0 };
                swprintf_s(instPath, L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\%s", pciKey);
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instPath, 0, KEY_READ, &hPCIInst) == 0) {
                    wchar_t devClass[64] = { 0 };
                    DWORD dSize = sizeof(devClass);
                    if (RegGetValueW(hPCIInst, NULL, L"Class", RRF_RT_REG_SZ, NULL, devClass, &dSize) == 0) {
                        if (_wcsicmp(devClass, L"Display") == 0) {
                            wchar_t gpuName2[256] = { 0 };
                            DWORD g2Size = sizeof(gpuName2);
                            if (RegGetValueW(hPCIInst, NULL, L"DeviceDesc", RRF_RT_REG_SZ, NULL, gpuName2, &g2Size) == 0) {
                                addItem(L"显卡型号", gpuName2);
                                gpuFound = 1;
                            }
                        }
                    }
                    RegCloseKey(hPCIInst);
                }
                if (gpuFound) break;
                idx2++;
                pciSize = _countof(pciKey);
            }
            RegCloseKey(hPCI);
        }
    }
    if (!gpuFound) addItem(L"显卡型号", L"未检测到");

    // ===== 磁盘驱动器 =====
    wchar_t driveBits[2048] = { 0 };
    GetLogicalDriveStringsW((DWORD)_countof(driveBits), driveBits);
    wchar_t* p = driveBits;
    while (*p) {
        UINT type = GetDriveTypeW(p);
        wchar_t typeName[32] = { 0 };
        switch (type) {
        case DRIVE_FIXED: wcscpy_s(typeName, L"本地磁盘"); break;
        case DRIVE_REMOVABLE: wcscpy_s(typeName, L"可移动磁盘"); break;
        case DRIVE_CDROM: wcscpy_s(typeName, L"光驱"); break;
        case DRIVE_REMOTE: wcscpy_s(typeName, L"网络驱动器"); break;
        default: wcscpy_s(typeName, L"未知"); break;
        }

        ULARGE_INTEGER dfree, dtotal;
        wchar_t droot[8] = { 0 };
        swprintf_s(droot, L"%s", p);

        if (GetDiskFreeSpaceExW(droot, NULL, &dtotal, &dfree) && dtotal.QuadPart > 0) {
            long long totalGB = dtotal.QuadPart / (1024LL * 1024 * 1024);
            long long freeGB = dfree.QuadPart / (1024LL * 1024 * 1024);
            int usedPct = (int)((dtotal.QuadPart - dfree.QuadPart) * 100 / dtotal.QuadPart);
            wchar_t dtext[512] = { 0 };
            swprintf_s(dtext, L"%s (%s) 总%lldGB 剩%lldGB 已用%d%%",
                p, typeName, totalGB, freeGB, usedPct);
            addItem(L"磁盘", dtext);

            wchar_t fsName[32] = { 0 };
            if (GetVolumeInformationW(droot, NULL, 0, NULL, NULL, NULL, fsName, _countof(fsName)))
                addItem(L"文件系统", fsName);
        } else {
            wchar_t dtext[256] = { 0 };
            swprintf_s(dtext, L"%s (%s)", p, typeName);
            addItem(L"磁盘", dtext);
        }
        p += wcslen(p) + 1;
    }

    // ===== 系统目录 =====
    wchar_t winDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(winDir, MAX_PATH);
    addItem(L"Windows目录", winDir);
    wchar_t sysDir[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sysDir, MAX_PATH);
    addItem(L"系统目录", sysDir);
    wchar_t userProfile[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userProfile);
    addItem(L"用户目录", userProfile);
    wchar_t sysDrive[8] = { 0 };
    GetEnvironmentVariableW(L"SystemDrive", sysDrive, _countof(sysDrive));
    addItem(L"系统盘", sysDrive);
}

// ====================== 2. 重复文件查找 ======================

// 读取工具页路径框并去掉首尾空白/成对引号（用户常直接粘贴带引号的路径）
static std::wstring ReadToolPath() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetWindowTextW(g_hToolPathEdit, buf, MAX_PATH);
    std::wstring path = buf;
    size_t first = path.find_first_not_of(L" \t\r\n");
    size_t last = path.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return std::wstring();
    path = path.substr(first, last - first + 1);
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

void StartDuplicateScan() {
    if (g_isDupScanning) return;

    std::wstring path = ReadToolPath();
    if (path.empty()) {
        MessageBoxW(g_hMainWnd, L"请先选择要扫描的目录", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 扫描线程读取的是 g_toolCurrentPath；这里必须先同步，
    // 否则手动输入路径后仍会扫描上一次的目录
    g_toolCurrentPath = path;

    g_isDupScanning = true;
    g_stopDupScan = false;
    ListView_DeleteAllItems(g_hToolListView);
    SetWindowTextW(g_hToolInfoLabel, L"正在扫描重复文件...");
    ShowWindow(g_hToolInfoLabel, SW_SHOW);

    HANDLE hThread = CreateThread(NULL, 0, DupScanThreadProc, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

static DWORD WINAPI DupScanThreadProc(LPVOID) {
    // Collect all files with size > 0
    struct FileEntry {
        std::wstring path;
        long long size;
    };
    std::vector<FileEntry> allFiles;
    std::wstring scanPath = g_toolCurrentPath;

    // 深度上限 + 跳过重解析点，防止符号链接成环导致无限递归栈溢出
    const int kMaxDupDepth = 40;
    std::function<void(const std::wstring&, int)> enumDir = [&](const std::wstring& dir, int depth) {
        if (g_stopDupScan) return;
        if (depth > kMaxDupDepth) return;
        if (allFiles.size() >= 100000) return;
        std::wstring searchPath = dir + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (g_stopDupScan) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring fullPath = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                enumDir(fullPath, depth + 1);
            } else {
                // 必须用 high<<32|low 还原 64 位大小，用 high*MAXDWORD 会算错
                long long fileSize = FileSizeFromFindData(&fd);
                if (fileSize > 0) {
                    allFiles.push_back({ fullPath, fileSize });
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    };

    if (!PathFileExistsW(scanPath.c_str())) {
        g_isDupScanning = false;
        SetWindowTextW(g_hToolInfoLabel, L"路径不存在！");
        return 0;
    }

    enumDir(scanPath, 0);

    if (g_stopDupScan) {
        g_isDupScanning = false;
        SetWindowTextW(g_hToolInfoLabel, L"扫描已取消");
        return 0;
    }

    // Group by size
    std::map<long long, std::vector<FileEntry>> sizeGroups;
    for (const auto& f : allFiles) {
        sizeGroups[f.size].push_back(f);
    }

    // For groups with multiple files, compute hash to find duplicates
    int found = 0;
    int total = 0;
    for (auto& [size, group] : sizeGroups) {
        if (group.size() < 2) continue;
        total += (int)group.size();
    }

    SetWindowTextW(g_hToolInfoLabel, L"正在比对文件内容...");

    int displayed = 0;
    for (auto& [size, group] : sizeGroups) {
        if (g_stopDupScan) break;
        if (group.size() < 2) continue;

        std::map<std::wstring, std::vector<FileEntry>> hashGroups;
        for (const auto& f : group) {
            if (g_stopDupScan) break;
            std::wstring hash = GetFileHash(f.path);
            if (!hash.empty()) {
                hashGroups[hash].push_back(f);
            }
        }

        for (auto& [hash, dupGroup] : hashGroups) {
            if (dupGroup.size() < 2) continue;
            for (const auto& f : dupGroup) {
                if (g_stopDupScan) break;
                LVITEMW lvi = { 0 };
                // 用动态长度的缓冲区，_wsplitpath_s 缓冲区不足会触发 invalid parameter 终止进程
                std::vector<wchar_t> nameBuf(f.path.size() + 4);
                std::vector<wchar_t> extBuf(f.path.size() + 4);
                _wsplitpath_s(f.path.c_str(), NULL, 0, NULL, 0,
                    nameBuf.data(), nameBuf.size(), extBuf.data(), extBuf.size());
                std::wstring display = std::wstring(nameBuf.data()) + extBuf.data();
                lvi.mask = LVIF_TEXT;
                lvi.iItem = ListView_GetItemCount(g_hToolListView);
                lvi.pszText = (LPWSTR)display.c_str();
                int row = ListView_InsertItem(g_hToolListView, &lvi);
                if (row < 0) continue;
                ListView_SetItemText(g_hToolListView, row, 1, (LPWSTR)FormatSize(f.size).c_str());
                ListView_SetItemText(g_hToolListView, row, 2, (LPWSTR)f.path.c_str());
                displayed++;
            }
        }
    }

    wchar_t info[128];
    swprintf_s(info, L"扫描完成，找到 %d 个重复文件", displayed);
    SetWindowTextW(g_hToolInfoLabel, info);
    g_isDupScanning = false;
    return 0;
}

static std::wstring GetFileHash(const std::wstring& path) {
    // 必须基于全量内容计算摘要。只采样首尾若干字节的实现会把「首尾相同、中间不同」
    // 的两个文件判为重复，用户按建议删除后就会删掉正确的文件。
    return ComputeFileDigest(path);
}

void StopDuplicateScan() {
    g_stopDupScan = true;
}

void CleanSelectedDuplicates() {
    int selected = ListView_GetSelectedCount(g_hToolListView);
    if (selected == 0) {
        MessageBoxW(g_hMainWnd, L"请先选择要删除的重复文件", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t msg[128];
    swprintf_s(msg, L"确定删除选中的 %d 个重复文件吗？\n建议保留每组中的一个。", selected);
    if (MessageBoxW(g_hMainWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    int success = 0;
    int idx = -1;
    std::vector<int> toDelete;
    // 路径列可能超过 MAX_PATH；用大缓冲区读取完整路径，
    // 否则截断后的路径要么删不掉，要么指向另一个文件
    std::vector<wchar_t> pathBuf(32768);
    while ((idx = ListView_GetNextItem(g_hToolListView, idx, LVNI_SELECTED)) != -1) {
        pathBuf[0] = 0;
        ListView_GetItemText(g_hToolListView, idx, 2, pathBuf.data(), (int)pathBuf.size());
        if (pathBuf[0] == 0) continue;
        std::wstring fullPath = pathBuf.data();
        // 防御：路径穿越与系统关键目录不允许删除
        if (IsPathTraversal(fullPath) || IsSystemCriticalPath(fullPath)) continue;
        // 只读/隐藏/系统属性的文件 DeleteFileW 会失败，用 ForceDeleteFile
        if (ForceDeleteFile(fullPath)) {
            toDelete.push_back(idx);
            success++;
        }
    }

    // Remove deleted items (reverse order)
    for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
        ListView_DeleteItem(g_hToolListView, *it);
    }

    wchar_t result[128];
    swprintf_s(result, L"成功删除 %d 个文件", success);
    MessageBoxW(g_hMainWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
}

// ====================== 3. 进程管理器 ======================

void RefreshProcessList() {
    ListView_DeleteAllItems(g_hToolListView);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_hMainWnd, L"无法创建进程快照", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    PROCESSENTRY32W pe32 = { sizeof(pe32) };
    if (!Process32FirstW(hSnapshot, &pe32)) {
        CloseHandle(hSnapshot);
        return;
    }

    do {
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(g_hToolListView);

        wchar_t name[MAX_PATH] = { 0 };
        wcscpy_s(name, pe32.szExeFile);
        lvi.pszText = name;
        ListView_InsertItem(g_hToolListView, &lvi);

        wchar_t pid[32] = { 0 };
        swprintf_s(pid, L"%u", pe32.th32ProcessID);
        ListView_SetItemText(g_hToolListView, lvi.iItem, 1, pid);

        // Get memory usage
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
        if (hProc) {
            PROCESS_MEMORY_COUNTERS pmc = { 0 };
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                wchar_t mem[64] = { 0 };
                // SIZE_T is 32-bit on Win32, so cast explicitly to keep %llu valid on all platforms
                const unsigned long long workingSetMB =
                    static_cast<unsigned long long>(pmc.WorkingSetSize) / (1024ull * 1024ull);
                swprintf_s(mem, L"%llu", workingSetMB);
                ListView_SetItemText(g_hToolListView, lvi.iItem, 2, mem);
            }

            wchar_t path[MAX_PATH] = { 0 };
            DWORD pathSize = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &pathSize)) {
                ListView_SetItemText(g_hToolListView, lvi.iItem, 3, path);
            }
            CloseHandle(hProc);
        }
    } while (Process32NextW(hSnapshot, &pe32));

    CloseHandle(hSnapshot);

    wchar_t info[128];
    swprintf_s(info, L"共检测到 %d 个进程", ListView_GetItemCount(g_hToolListView));
    SetWindowTextW(g_hToolInfoLabel, info);
    ShowWindow(g_hToolInfoLabel, SW_SHOW);
}

void KillSelectedProcess() {
    int selected = ListView_GetSelectedCount(g_hToolListView);
    if (selected == 0) {
        MessageBoxW(g_hMainWnd, L"请先选择要结束的进程", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t msg[128];
    swprintf_s(msg, L"确定结束选中的 %d 个进程吗？", selected);
    if (MessageBoxW(g_hMainWnd, msg, L"确认", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    int success = 0;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_hToolListView, idx, LVNI_SELECTED)) != -1) {
        wchar_t pidStr[32] = { 0 };
        ListView_GetItemText(g_hToolListView, idx, 1, pidStr, 32);
        DWORD pid = _wtoi(pidStr);
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            if (TerminateProcess(hProc, 0)) success++;
            CloseHandle(hProc);
        }
    }

    wchar_t result[128];
    swprintf_s(result, L"成功结束 %d 个进程", success);
    MessageBoxW(g_hMainWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
    RefreshProcessList();
}

// ====================== 4. 快捷方式修复 ======================

// 扫描到的失效快捷方式完整路径表。
// 删除时必须按扫描得到的真实完整路径删除；仅靠文件名到 4 个默认目录里重新查找，
// 会让自定义路径扫描出的快捷方式永远删不掉，且可能误删同名文件。
static std::vector<std::wstring> g_brokenShortcutPaths;

void StartShortcutScan() {
    g_brokenShortcutPaths.clear();
    wchar_t path[MAX_PATH] = { 0 };
    GetWindowTextW(g_hToolPathEdit, path, MAX_PATH);
    if (wcslen(path) == 0) {
        // Default: scan common shortcut locations
        wchar_t desktop[MAX_PATH] = { 0 };
        wchar_t startMenu[MAX_PATH] = { 0 };
        SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop);
        SHGetFolderPathW(NULL, CSIDL_STARTMENU, NULL, 0, startMenu);

        std::vector<std::wstring> scanPaths;
        if (wcslen(desktop) > 0) scanPaths.push_back(desktop);
        if (wcslen(startMenu) > 0) scanPaths.push_back(startMenu);
        // Also public desktop and common start menu
        wchar_t commonDesktop[MAX_PATH] = { 0 };
        wchar_t commonStart[MAX_PATH] = { 0 };
        SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, commonDesktop);
        SHGetFolderPathW(NULL, CSIDL_COMMON_STARTMENU, NULL, 0, commonStart);
        if (wcslen(commonDesktop) > 0) scanPaths.push_back(commonDesktop);
        if (wcslen(commonStart) > 0) scanPaths.push_back(commonStart);

        ListView_DeleteAllItems(g_hToolListView);
        int broken = 0;
        for (const auto& dir : scanPaths) {
            std::wstring searchPath = dir + L"\\*.lnk";
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring lnkPath = dir + L"\\" + fd.cFileName;
                std::wstring target;
                if (IsShortcutBroken(lnkPath, target)) {
                    LVITEMW lvi = { 0 };
                    std::wstring name = fd.cFileName;
                    size_t dot = name.rfind(L'.');
                    if (dot != std::wstring::npos) name = name.substr(0, dot);
                    lvi.mask = LVIF_TEXT | LVIF_PARAM;
                    lvi.iItem = ListView_GetItemCount(g_hToolListView);
                    lvi.lParam = (LPARAM)g_brokenShortcutPaths.size();
                    lvi.pszText = (LPWSTR)name.c_str();
                    int row = ListView_InsertItem(g_hToolListView, &lvi);
                    if (row < 0) continue;
                    ListView_SetItemText(g_hToolListView, row, 1, (LPWSTR)target.c_str());
                    wchar_t brokenStr[] = L"失效";
                    ListView_SetItemText(g_hToolListView, row, 2, brokenStr);
                    g_brokenShortcutPaths.push_back(lnkPath);
                    broken++;
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        wchar_t info[128];
        swprintf_s(info, L"扫描完成，发现 %d 个失效快捷方式", broken);
        SetWindowTextW(g_hToolInfoLabel, info);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
    } else {
        // Scan custom path recursively for .lnk files
        ListView_DeleteAllItems(g_hToolListView);
        int broken = 0;
        const int kMaxLnkDepth = 40;
        std::function<void(const std::wstring&, int)> scanDir = [&](const std::wstring& dir, int depth) {
            if (depth > kMaxLnkDepth) return;
            std::wstring searchPath = dir + L"\\*.lnk";
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    std::wstring lnkPath = dir + L"\\" + fd.cFileName;
                    std::wstring target;
                    if (IsShortcutBroken(lnkPath, target)) {
                        LVITEMW lvi = { 0 };
                        std::wstring name = fd.cFileName;
                        size_t dot = name.rfind(L'.');
                        if (dot != std::wstring::npos) name = name.substr(0, dot);
                        lvi.mask = LVIF_TEXT | LVIF_PARAM;
                        lvi.iItem = ListView_GetItemCount(g_hToolListView);
                        lvi.lParam = (LPARAM)g_brokenShortcutPaths.size();
                        lvi.pszText = (LPWSTR)name.c_str();
                        int row = ListView_InsertItem(g_hToolListView, &lvi);
                        if (row < 0) continue;
                        ListView_SetItemText(g_hToolListView, row, 1, (LPWSTR)target.c_str());
                        wchar_t brokenStr3[] = L"失效";
                        ListView_SetItemText(g_hToolListView, row, 2, brokenStr3);
                        g_brokenShortcutPaths.push_back(lnkPath);
                        broken++;
                    }
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }

            // Also recurse into subdirectories
            std::wstring subSearch = dir + L"\\*";
            HANDLE hFind2 = FindFirstFileW(subSearch.c_str(), &fd);
            if (hFind2 != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0 &&
                        !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        scanDir(dir + L"\\" + fd.cFileName, depth + 1);
                    }
                } while (FindNextFileW(hFind2, &fd));
                FindClose(hFind2);
            }
        };

        scanDir(path, 0);

        wchar_t info[128];
        swprintf_s(info, L"扫描完成，发现 %d 个失效快捷方式", broken);
        SetWindowTextW(g_hToolInfoLabel, info);
        ShowWindow(g_hToolInfoLabel, SW_SHOW);
    }
}

static bool IsShortcutBroken(const std::wstring& lnkPath, std::wstring& targetPath) {
    HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool comInitialized = (hrCoInit == S_OK);

    IShellLinkW* pShellLink = NULL;
    IPersistFile* pPersistFile = NULL;

    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pShellLink));
    if (FAILED(hr)) {
        if (comInitialized) CoUninitialize();
        return false;
    }

    hr = pShellLink->QueryInterface(IID_PPV_ARGS(&pPersistFile));
    if (FAILED(hr)) {
        pShellLink->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    hr = pPersistFile->Load(lnkPath.c_str(), STGM_READ);
    if (FAILED(hr)) {
        pPersistFile->Release();
        pShellLink->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    wchar_t target[MAX_PATH] = { 0 };
    hr = pShellLink->GetPath(target, MAX_PATH, NULL, SLGP_RAWPATH);
    if (FAILED(hr) || wcslen(target) == 0) {
        pPersistFile->Release();
        pShellLink->Release();
        if (comInitialized) CoUninitialize();
        return false;
    }

    targetPath = target;

    DWORD attrs = GetFileAttributesW(target);
    pPersistFile->Release();
    pShellLink->Release();
    if (comInitialized) CoUninitialize();

    return (attrs == INVALID_FILE_ATTRIBUTES);
}

void FixSelectedShortcuts() {
    int selected = ListView_GetSelectedCount(g_hToolListView);
    if (selected == 0) {
        MessageBoxW(g_hMainWnd, L"请先选择要处理的快捷方式", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t msg[128];
    swprintf_s(msg, L"确定删除选中的 %d 个失效快捷方式吗？", selected);
    if (MessageBoxW(g_hMainWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    int success = 0;
    int idx = -1;
    std::vector<int> toDelete;
    while ((idx = ListView_GetNextItem(g_hToolListView, idx, LVNI_SELECTED)) != -1) {
        // 直接使用扫描阶段记录的完整 .lnk 路径（存在 lParam 的索引中），
        // 之前按文件名到 4 个默认目录重新查找会导致自定义路径下的快捷方式无法删除
        std::wstring lnkPath;
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_PARAM;
        lvi.iItem = idx;
        if (ListView_GetItem(g_hToolListView, &lvi) &&
            lvi.lParam >= 0 && lvi.lParam < (LPARAM)g_brokenShortcutPaths.size()) {
            lnkPath = g_brokenShortcutPaths[(size_t)lvi.lParam];
        }
        if (lnkPath.empty()) continue;

        if (ForceDeleteFile(lnkPath)) {
            g_brokenShortcutPaths[(size_t)lvi.lParam].clear();
            toDelete.push_back(idx);
            success++;
        }
    }

    for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
        ListView_DeleteItem(g_hToolListView, *it);
    }

    wchar_t result[128];
    swprintf_s(result, L"成功删除 %d 个失效快捷方式", success);
    MessageBoxW(g_hMainWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
}

// ====================== 5. 磁盘清理助手 ======================

// 扫描结果对应的完整路径表：删除时必须按扫描时的真实路径删除。
// 之前删除阶段用 g_toolCurrentPath 拼接列表中的相对路径，而扫描用的是局部 path 变量，
// 两者不一致时会删错文件（或删不到）。
static std::vector<std::wstring> g_diskFilePaths;

void StartDiskCleanupScan() {
    std::wstring path = ReadToolPath();
    if (path.empty()) {
        MessageBoxW(g_hMainWnd, L"请先选择要扫描的目录", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 让扫描与后续删除使用同一个基准目录
    g_toolCurrentPath = path;
    g_diskFilePaths.clear();

    ListView_DeleteAllItems(g_hToolListView);

    int ageIndex = (int)SendMessageW(g_hToolAgeCombo, CB_GETCURSEL, 0, 0);
    if (ageIndex < 0 || ageIndex > 3) ageIndex = 0;
    // ageIndex: 0=7天 1=30天 2=90天 3=180天

    wchar_t ageLabel[64] = { 0 };
    if (SendMessageW(g_hToolAgeCombo, CB_GETLBTEXT, ageIndex, (LPARAM)ageLabel) == CB_ERR ||
        ageLabel[0] == 0) {
        wcscpy_s(ageLabel, L"7天前");
    }

    wchar_t info[128];
    swprintf_s(info, L"正在扫描 %s 前修改的文件...", ageLabel);
    SetWindowTextW(g_hToolInfoLabel, info);
    ShowWindow(g_hToolInfoLabel, SW_SHOW);

    DWORD daysThreshold = 7;
    if (ageIndex == 1) daysThreshold = 30;
    else if (ageIndex == 2) daysThreshold = 90;
    else if (ageIndex == 3) daysThreshold = 180;

    SYSTEMTIME now;
    GetLocalTime(&now);
    FILETIME nowFt;
    SystemTimeToFileTime(&now, &nowFt);
    ULONGLONG nowTime = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    ULONGLONG threshold = nowTime - (ULONGLONG)daysThreshold * 24 * 60 * 60 * 10000000ULL;

    // depth 递归深度上限：目录符号链接 / junction 可能构成环，
    // 不设上限会无限递归直至栈溢出；重解析点本身也一律不进入（其目标在扫描根之外）
    std::function<void(const std::wstring&, int)> scanDir = [&](const std::wstring& dir, int depth) {
        if (depth > 40) return;
        // 用 wstring 拼接：固定 wchar_t[MAX_PATH] 在深层目录下会被 swprintf_s 判定溢出，
        // 进而触发 invalid parameter handler 直接终止整个进程
        std::wstring search = dir + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            std::wstring fullPath = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                scanDir(fullPath, depth + 1);
            } else {
                ULONGLONG modTime = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                if (modTime < threshold) {
                    long long fileSize = FileSizeFromFindData(&fd);

                    LVITEMW lvi = { 0 };
                    lvi.mask = LVIF_TEXT | LVIF_PARAM;
                    lvi.iItem = ListView_GetItemCount(g_hToolListView);
                    lvi.lParam = (LPARAM)g_diskFilePaths.size();
                    lvi.pszText = (LPWSTR)fd.cFileName;
                    int row = ListView_InsertItem(g_hToolListView, &lvi);
                    if (row < 0) continue;

                    ListView_SetItemText(g_hToolListView, row, 1, (LPWSTR)FormatSize(fileSize).c_str());

                    // Format time
                    SYSTEMTIME st;
                    FILETIME ft = fd.ftLastWriteTime;
                    FileTimeToLocalFileTime(&ft, &ft);
                    FileTimeToSystemTime(&ft, &st);
                    wchar_t timeBuf[64] = { 0 };
                    swprintf_s(timeBuf, L"%04d-%02d-%02d %02d:%02d",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                    ListView_SetItemText(g_hToolListView, row, 2, timeBuf);

                    // Show relative path（仅用于展示，删除以 lParam 记录的完整路径为准）
                    std::wstring relPath = fullPath;
                    if (fullPath.length() > g_toolCurrentPath.length() &&
                        _wcsnicmp(fullPath.c_str(), g_toolCurrentPath.c_str(), (int)g_toolCurrentPath.length()) == 0) {
                        relPath = fullPath.substr(g_toolCurrentPath.length());
                        if (!relPath.empty() && relPath[0] == L'\\') relPath = relPath.substr(1);
                    }
                    ListView_SetItemText(g_hToolListView, row, 3, (LPWSTR)relPath.c_str());

                    g_diskFilePaths.push_back(fullPath);
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    };

    scanDir(path, 0);

    wchar_t done[128];
    swprintf_s(done, L"扫描完成，共找到 %d 个符合条件的文件", ListView_GetItemCount(g_hToolListView));
    SetWindowTextW(g_hToolInfoLabel, done);
}

void CleanSelectedDiskFiles() {
    int selected = ListView_GetSelectedCount(g_hToolListView);
    if (selected == 0) {
        MessageBoxW(g_hMainWnd, L"请先选择要删除的文件", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t msg[128];
    swprintf_s(msg, L"确定删除选中的 %d 个旧文件吗？", selected);
    if (MessageBoxW(g_hMainWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    int success = 0;
    int idx = -1;
    std::vector<int> toDelete;
    while ((idx = ListView_GetNextItem(g_hToolListView, idx, LVNI_SELECTED)) != -1) {
        std::wstring fullPath;
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_PARAM;
        lvi.iItem = idx;
        if (ListView_GetItem(g_hToolListView, &lvi) &&
            lvi.lParam >= 0 && lvi.lParam < (LPARAM)g_diskFilePaths.size()) {
            fullPath = g_diskFilePaths[(size_t)lvi.lParam];
        }
        if (fullPath.empty()) continue;
        if (IsSystemCriticalPath(fullPath) || IsPathTraversal(fullPath)) continue;

        if (ForceDeleteFile(fullPath)) {
            g_diskFilePaths[(size_t)lvi.lParam].clear();
            toDelete.push_back(idx);
            success++;
        }
    }

    for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
        ListView_DeleteItem(g_hToolListView, *it);
    }

    wchar_t result[128];
    swprintf_s(result, L"成功删除 %d 个文件", success);
    MessageBoxW(g_hMainWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
}