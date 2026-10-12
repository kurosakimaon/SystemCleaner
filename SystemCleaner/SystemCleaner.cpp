// Module: SystemCleaner.cpp - Main entry point, window procedure, and global variables

#include "common.h"

// ====================== 全局控件句柄 ======================

HWND g_hTab = NULL;
HWND g_hCleanList = NULL;
HWND g_hStartupList = NULL;
HWND g_hContextMenuList = NULL;
HWND g_hRegCleanList = NULL;
HWND g_hUninstallList = NULL;
HWND g_hUninstallFilter = NULL;
HWND g_hUninstallSearch = NULL;
HWND g_hCleanProgress = NULL;
HWND g_hRegProgress = NULL;
HWND g_hStatusText = NULL;
HWND g_hSpeedResult = NULL;
HWND g_hSpeedProgress = NULL;
HWND g_hHostsEdit = NULL;
HWND g_hShredPath = NULL;
HWND g_hMainWnd = NULL;

// ====================== 全局数据 ======================

std::vector<CleanItem> g_cleanItems;
std::vector<StartupItem> g_startupItems;
std::vector<RightMenuItem> g_contextMenuItems;
std::vector<RegCleanItem> g_regCleanItems;
std::vector<UninstallItem> g_uninstallItems;
std::vector<UninstallFailureLog> g_uninstallFailures;
HINSTANCE g_hInst = NULL;
bool g_isScanning = false;
bool g_isCleaning = false;
bool g_stopScan = false;
bool g_stopClean = false;
bool g_isSpeedTesting = false;
bool g_stopSpeedTest = false;
bool g_isNetworkRepairing = false;
bool g_isRegScanning = false;
bool g_isUninstalling = false;
bool g_isUninstallScanning = false;
bool g_uninstallScanCancelled = false;
bool g_keepUserData = true;
int g_uninstallFilterType = 0;
std::wstring g_uninstallSearchText;
int g_startupFilterType = 0;
int g_contextMenuFilterType = 0;

// Helper function to reliably get item lParam from ListView
static LPARAM GetListViewItemParam(HWND hListView, int itemIndex) {
    LVITEMW lvi = { 0 };
    lvi.mask = LVIF_PARAM;
    lvi.iItem = itemIndex;
    if (ListView_GetItem(hListView, &lvi)) {
        return lvi.lParam;
    }
    return 0;
}

// Tools page
int g_toolFunction = 0;
HWND g_hToolFuncCombo = NULL;
HWND g_hToolListView = NULL;
HWND g_hToolPathEdit = NULL;
HWND g_hToolAgeCombo = NULL;
HWND g_hToolInfoLabel = NULL;
std::wstring g_toolCurrentPath;
bool g_toolsReady = false;

// Debounce state for uninstall filter/search
bool g_uninstallDebouncePending = false;
bool g_uninstallDebounceForceRescan = false;

// Scan generation counter (see common.h)
DWORD g_uninstallScanGeneration = 0;
CRITICAL_SECTION g_uninstallItemsLock;

// ====================== Tab控件子类化 ======================

/**
 * @brief Tab 控件的子类化窗口过程
 */
LRESULT CALLBACK TabSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    if (uMsg == WM_COMMAND)
    {
        SendMessage(g_hMainWnd, uMsg, wParam, lParam);
        return 0;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ====================== 标签页切换 ======================

/**
 * @brief 根据选中的标签页索引显示对应的控件，隐藏其他页面的控件
 */
void SwitchTab(int index) {
    bool showClean = (index == 0);
    ShowWindow(g_hCleanList, showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hCleanProgress, showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1001), showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1002), showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1003), showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1004), showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1005), showClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 1006), showClean ? SW_SHOW : SW_HIDE);

    bool showStartup = (index == 1);
    ShowWindow(g_hStartupList, showStartup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 2001), showStartup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 2002), showStartup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 2003), showStartup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 2005), showStartup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 2006), showStartup ? SW_SHOW : SW_HIDE);

    bool showContext = (index == 2);
    ShowWindow(g_hContextMenuList, showContext ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 3001), showContext ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 3002), showContext ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 3003), showContext ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 3005), showContext ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 3006), showContext ? SW_SHOW : SW_HIDE);

    bool showReg = (index == 3);
    ShowWindow(g_hRegCleanList, showReg ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hRegProgress, showReg ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 5001), showReg ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 5002), showReg ? SW_SHOW : SW_HIDE);

    bool showNet = (index == 4);
    ShowWindow(g_hSpeedResult, showNet ? SW_SHOW : SW_HIDE);
    // 进度条只在测速进行中显示：否则一进入网络页就出现一条空进度条
    ShowWindow(g_hSpeedProgress, (showNet && g_isSpeedTesting) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hHostsEdit, showNet ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 4001), showNet ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 4002), showNet ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 4003), showNet ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 4004), showNet ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 4005), showNet ? SW_SHOW : SW_HIDE);
    // 4006 =「Hosts文件编辑：」标签：此前遗漏，导致该标签在网络页从不显示、
    // 离开网络页也不会被隐藏（会残留在其它页面上）
    ShowWindow(GetDlgItem(g_hTab, 4006), showNet ? SW_SHOW : SW_HIDE);

    bool showUninstall = (index == 5);
    ShowWindow(g_hUninstallList, showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7001), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7002), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7003), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7004), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7010), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hUninstallFilter, showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hTab, 7007), showUninstall ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hUninstallSearch, showUninstall ? SW_SHOW : SW_HIDE);

    bool showTool = (index == 6);
    ShowToolControls(showTool);

    // Initialize tools page on first activation
    if (showTool && !g_toolsReady) {
        g_toolsReady = true;
        SendMessageW(g_hToolFuncCombo, CB_SETCURSEL, g_toolFunction, 0);
        SwitchToolFunction(g_toolFunction);
        RECT tabRect;
        GetClientRect(g_hTab, &tabRect);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
        LayoutToolControls(tabRect);
    } else if (showTool) {
        // Re-apply current function's visibility when re-entering tools page
        SwitchToolFunction(g_toolFunction);
    }

    // 功能选择下拉框始终可见
    ShowWindow(g_hToolFuncCombo, showTool ? SW_SHOW : SW_HIDE);
}

// ====================== 窗口过程 ======================

/**
 * @brief 主窗口过程函数，处理所有窗口消息
 */
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hMainWnd = hWnd;
        g_hInst = ((LPCREATESTRUCTW)lParam)->hInstance;

        InitCleanItems();

        srand((unsigned)time(NULL));

        INITCOMMONCONTROLSEX icex = { 0 };
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icex);

        g_hTab = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH,
            10, 10, 780, 500, hWnd, NULL, g_hInst, NULL);

        SetWindowSubclass(g_hTab, TabSubclassProc, 1, 0);

        TCITEMW tie = { 0 };
        tie.mask = TCIF_TEXT;
        wchar_t tab1[32] = L"垃圾清理"; tie.pszText = tab1; TabCtrl_InsertItem(g_hTab, 0, &tie);
        wchar_t tab2[32] = L"开机启动项"; tie.pszText = tab2; TabCtrl_InsertItem(g_hTab, 1, &tie);
        wchar_t tab3[32] = L"右键菜单"; tie.pszText = tab3; TabCtrl_InsertItem(g_hTab, 2, &tie);
        wchar_t tab5[32] = L"注册表清理"; tie.pszText = tab5; TabCtrl_InsertItem(g_hTab, 3, &tie);
        wchar_t tab4[32] = L"网络工具"; tie.pszText = tab4; TabCtrl_InsertItem(g_hTab, 4, &tie);
        wchar_t tab7[32] = L"强力卸载"; tie.pszText = tab7; TabCtrl_InsertItem(g_hTab, 5, &tie);
        wchar_t tab6[32] = L"实用工具"; tie.pszText = tab6; TabCtrl_InsertItem(g_hTab, 6, &tie);

        RECT tabRect;
        GetClientRect(g_hTab, &tabRect);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
        int listW = tabRect.right - tabRect.left - 20;
        int listH = tabRect.bottom - tabRect.top - 50;

        // ========== 垃圾清理页 ==========
        g_hCleanList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 50, listW, listH, g_hTab, NULL, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hCleanList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH;
        wchar_t colText[64];
        wcscpy_s(colText, L"清理项目"); lvc.pszText = colText; lvc.cx = 180; ListView_InsertColumn(g_hCleanList, 0, &lvc);
        wcscpy_s(colText, L"说明"); lvc.pszText = colText; lvc.cx = 280; ListView_InsertColumn(g_hCleanList, 1, &lvc);
        wcscpy_s(colText, L"可清理大小"); lvc.pszText = colText; lvc.cx = 120; ListView_InsertColumn(g_hCleanList, 2, &lvc);
        wcscpy_s(colText, L"文件数"); lvc.pszText = colText; lvc.cx = 80; ListView_InsertColumn(g_hCleanList, 3, &lvc);

        CreateWindowW(L"BUTTON", L"全选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 80, 30, g_hTab, (HMENU)1001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"反选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.left + 100, tabRect.top + 10, 80, 30, g_hTab, (HMENU)1002, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"全不选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.left + 190, tabRect.top + 10, 80, 30, g_hTab, (HMENU)1006, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.right - 330, tabRect.top + 10, 100, 30, g_hTab, (HMENU)1005, g_hInst, NULL);
        EnableWindow(GetDlgItem(g_hTab, 1005), FALSE);
        CreateWindowW(L"BUTTON", L"开始扫描", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.right - 220, tabRect.top + 10, 100, 30, g_hTab, (HMENU)1003, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"立即清理", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 10, 100, 30, g_hTab, (HMENU)1004, g_hInst, NULL);

        g_hCleanProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH, tabRect.left + 10, tabRect.bottom - 30, listW, 20, g_hTab, NULL, g_hInst, NULL);

        // ========== 启动项页 ==========
        g_hStartupList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 50, listW, listH + 20, g_hTab, NULL, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hStartupList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        wcscpy_s(colText, L"名称"); lvc.pszText = colText; lvc.cx = 180; ListView_InsertColumn(g_hStartupList, 0, &lvc);
        wcscpy_s(colText, L"命令/路径"); lvc.pszText = colText; lvc.cx = 320; ListView_InsertColumn(g_hStartupList, 1, &lvc);
        wcscpy_s(colText, L"位置"); lvc.pszText = colText; lvc.cx = 180; ListView_InsertColumn(g_hStartupList, 2, &lvc);
        wcscpy_s(colText, L"状态"); lvc.pszText = colText; lvc.cx = 80; ListView_InsertColumn(g_hStartupList, 3, &lvc);

        CreateWindowW(L"BUTTON", L"刷新扫描", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 100, 30, g_hTab, (HMENU)2001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"禁用选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 120, tabRect.top + 10, 100, 30, g_hTab, (HMENU)2002, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"恢复选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 230, tabRect.top + 10, 100, 30, g_hTab, (HMENU)2003, g_hInst, NULL);

        // 启动项状态筛选下拉框（全部/已启用/已禁用）
        CreateWindowW(L"STATIC", L"状态筛选:", WS_CHILD | SS_LEFT,
            tabRect.right - 300, tabRect.top + 17, 75, 20, g_hTab, (HMENU)2006, g_hInst, NULL);
        HWND hStartupFilter = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | CBS_DROPDOWNLIST,
            tabRect.right - 225, tabRect.top + 12, 130, 200, g_hTab, (HMENU)2005, g_hInst, NULL);
        SendMessageW(hStartupFilter, CB_ADDSTRING, 0, (LPARAM)L"全部");
        SendMessageW(hStartupFilter, CB_ADDSTRING, 0, (LPARAM)L"已启用");
        SendMessageW(hStartupFilter, CB_ADDSTRING, 0, (LPARAM)L"已禁用");
        SendMessageW(hStartupFilter, CB_SETCURSEL, 0, 0);

        // ========== 右键菜单页 ==========
        g_hContextMenuList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 50, listW, listH + 20, g_hTab, NULL, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hContextMenuList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        wcscpy_s(colText, L"菜单名称"); lvc.pszText = colText; lvc.cx = 200; ListView_InsertColumn(g_hContextMenuList, 0, &lvc);
        wcscpy_s(colText, L"类型"); lvc.pszText = colText; lvc.cx = 120; ListView_InsertColumn(g_hContextMenuList, 1, &lvc);
        wcscpy_s(colText, L"注册表位置"); lvc.pszText = colText; lvc.cx = 320; ListView_InsertColumn(g_hContextMenuList, 2, &lvc);
        wcscpy_s(colText, L"状态"); lvc.pszText = colText; lvc.cx = 80; ListView_InsertColumn(g_hContextMenuList, 3, &lvc);

        CreateWindowW(L"BUTTON", L"刷新扫描", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 100, 30, g_hTab, (HMENU)3001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"禁用选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 120, tabRect.top + 10, 100, 30, g_hTab, (HMENU)3002, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"恢复选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 230, tabRect.top + 10, 100, 30, g_hTab, (HMENU)3003, g_hInst, NULL);

        // 右键菜单状态筛选下拉框（全部/已启用/已禁用）
        CreateWindowW(L"STATIC", L"状态筛选:", WS_CHILD | SS_LEFT,
            tabRect.right - 300, tabRect.top + 17, 75, 20, g_hTab, (HMENU)3006, g_hInst, NULL);
        HWND hContextFilter = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | CBS_DROPDOWNLIST,
            tabRect.right - 225, tabRect.top + 12, 130, 200, g_hTab, (HMENU)3005, g_hInst, NULL);
        SendMessageW(hContextFilter, CB_ADDSTRING, 0, (LPARAM)L"全部");
        SendMessageW(hContextFilter, CB_ADDSTRING, 0, (LPARAM)L"已启用");
        SendMessageW(hContextFilter, CB_ADDSTRING, 0, (LPARAM)L"已禁用");
        SendMessageW(hContextFilter, CB_SETCURSEL, 0, 0);

        // ========== 注册表清理页 ==========
        g_hRegCleanList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 50, listW, listH + 20, g_hTab, NULL, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hRegCleanList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        wcscpy_s(colText, L"注册表路径"); lvc.pszText = colText; lvc.cx = 320; ListView_InsertColumn(g_hRegCleanList, 0, &lvc);
        wcscpy_s(colText, L"类别"); lvc.pszText = colText; lvc.cx = 120; ListView_InsertColumn(g_hRegCleanList, 1, &lvc);
        wcscpy_s(colText, L"说明"); lvc.pszText = colText; lvc.cx = 200; ListView_InsertColumn(g_hRegCleanList, 2, &lvc);

        CreateWindowW(L"BUTTON", L"扫描垃圾", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 100, 30, g_hTab, (HMENU)5001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"清理选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 120, tabRect.top + 10, 100, 30, g_hTab, (HMENU)5002, g_hInst, NULL);

        // ========== 网络工具页 ==========
        g_hSpeedResult = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"点击「开始测速」检测上下行速度",
            WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
            tabRect.left + 10, tabRect.top + 60, listW, 95, g_hTab, NULL, g_hInst, NULL);

        g_hSpeedProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | PBS_SMOOTH,
            tabRect.left + 10, tabRect.top + 160, listW, 20, g_hTab, NULL, g_hInst, NULL);
        SendMessageW(g_hSpeedProgress, PBM_SETRANGE, 0, MAKELONG(0, 100));
        ShowWindow(g_hSpeedProgress, SW_HIDE);

        CreateWindowW(L"BUTTON", L"开始测速", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 120, 30, g_hTab, (HMENU)4001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"停止测速", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 140, tabRect.top + 10, 120, 30, g_hTab, (HMENU)4002, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"一键修复网络", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 270, tabRect.top + 10, 120, 30, g_hTab, (HMENU)4003, g_hInst, NULL);

        CreateWindowW(L"STATIC", L"Hosts文件编辑：", WS_CHILD | SS_LEFT,
            tabRect.left + 10, tabRect.top + 195, 200, 20, g_hTab, (HMENU)4006, g_hInst, NULL);

        CreateWindowW(L"BUTTON", L"保存Hosts", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 220, tabRect.top + 190, 100, 30, g_hTab, (HMENU)4004, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"恢复默认", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 190, 100, 30, g_hTab, (HMENU)4005, g_hInst, NULL);

        g_hHostsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            tabRect.left + 10, tabRect.top + 225, listW, listH - 180, g_hTab, NULL, g_hInst, NULL);
        SetWindowTextW(g_hHostsEdit, ReadHostsFile().c_str());

        // ========== 强力卸载页 ==========
        g_hUninstallList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 50, listW, listH, g_hTab, NULL, g_hInst, NULL);

        ListView_SetExtendedListViewStyle(g_hUninstallList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        wcscpy_s(colText, L"软件名称"); lvc.pszText = colText; lvc.cx = 180; ListView_InsertColumn(g_hUninstallList, 0, &lvc);
        wcscpy_s(colText, L"版本"); lvc.pszText = colText; lvc.cx = 100; ListView_InsertColumn(g_hUninstallList, 1, &lvc);
        wcscpy_s(colText, L"类型"); lvc.pszText = colText; lvc.cx = 80; ListView_InsertColumn(g_hUninstallList, 2, &lvc);
        wcscpy_s(colText, L"大小"); lvc.pszText = colText; lvc.cx = 100; ListView_InsertColumn(g_hUninstallList, 3, &lvc);
        wcscpy_s(colText, L"安装位置"); lvc.pszText = colText; lvc.cx = 280; ListView_InsertColumn(g_hUninstallList, 4, &lvc);

        CreateWindowW(L"BUTTON", L"刷新扫描", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 10, tabRect.top + 10, 80, 30, g_hTab, (HMENU)7001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"卸载选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.left + 95, tabRect.top + 10, 80, 30, g_hTab, (HMENU)7002, g_hInst, NULL);

        g_keepUserData = false;
        CreateWindowW(L"BUTTON", L"保留用户数据", WS_CHILD | BS_AUTOCHECKBOX,
            tabRect.left + 180, tabRect.top + 17, 110, 20, g_hTab, (HMENU)7003, g_hInst, NULL);

        CreateWindowW(L"STATIC", L"类型:", WS_CHILD | SS_LEFT,
            tabRect.left + 300, tabRect.top + 17, 35, 20, g_hTab, (HMENU)7010, g_hInst, NULL);

        g_hUninstallFilter = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | CBS_DROPDOWNLIST,
            tabRect.left + 340, tabRect.top + 12, 90, 200, g_hTab, (HMENU)7005, g_hInst, NULL);
        SendMessageW(g_hUninstallFilter, CB_ADDSTRING, 0, (LPARAM)L"全部");
        SendMessageW(g_hUninstallFilter, CB_ADDSTRING, 0, (LPARAM)L"系统软件");
        SendMessageW(g_hUninstallFilter, CB_ADDSTRING, 0, (LPARAM)L"用户软件");
        SendMessageW(g_hUninstallFilter, CB_ADDSTRING, 0, (LPARAM)L"驱动");
        SendMessageW(g_hUninstallFilter, CB_ADDSTRING, 0, (LPARAM)L"未知");
        SendMessageW(g_hUninstallFilter, CB_SETCURSEL, 0, 0);

        CreateWindowW(L"STATIC", L"搜索:", WS_CHILD | SS_LEFT,
            tabRect.left + 440, tabRect.top + 17, 35, 20, g_hTab, (HMENU)7007, g_hInst, NULL);

        g_hUninstallSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL,
            tabRect.left + 475, tabRect.top + 12, 200, 25, g_hTab, (HMENU)7008, g_hInst, NULL);

        CreateWindowW(L"BUTTON", L"强制卸载", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 10, 100, 30, g_hTab, (HMENU)7004, g_hInst, NULL);

        // ========== 实用工具页 ==========
        // 功能选择标签 + 下拉框（统一在顶部）
        CreateWindowW(L"STATIC", L"功能选择:", WS_CHILD | SS_LEFT,
            tabRect.left + 10, tabRect.top + 12, 80, 20, g_hTab, (HMENU)6030, g_hInst, NULL);

        g_hToolFuncCombo = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | CBS_DROPDOWNLIST,
            tabRect.left + 90, tabRect.top + 10, 180, 300, g_hTab, (HMENU)6010, g_hInst, NULL);
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"文件粉碎");
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"系统信息");
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"重复文件查找");
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"进程管理");
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"快捷方式修复");
        SendMessageW(g_hToolFuncCombo, CB_ADDSTRING, 0, (LPARAM)L"磁盘清理助手");

        // 路径编辑框（文件粉碎和其他工具共用）
        g_hShredPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL,
            tabRect.left + 10, tabRect.top + 50, listW - 230, 25, g_hTab, NULL, g_hInst, NULL);

        g_hToolPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL,
            tabRect.left + 10, tabRect.top + 50, listW - 230, 25, g_hTab, (HMENU)6011, g_hInst, NULL);

        // 按钮组（统一位置）
        CreateWindowW(L"BUTTON", L"选择文件", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 50, 100, 25, g_hTab, (HMENU)6001, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"浏览", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 50, 100, 25, g_hTab, (HMENU)6012, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"开始粉碎", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 82, 100, 25, g_hTab, (HMENU)6002, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"开始扫描", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 82, 100, 25, g_hTab, (HMENU)6013, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"删除选中", WS_CHILD | BS_PUSHBUTTON,
            tabRect.right - 110, tabRect.top + 114, 100, 25, g_hTab, (HMENU)6014, g_hInst, NULL);

        // 文件年龄下拉框（用于磁盘清理）
        g_hToolAgeCombo = CreateWindowW(WC_COMBOBOXW, L"",
            WS_CHILD | CBS_DROPDOWNLIST,
            tabRect.left + 10, tabRect.top + 82, 150, 250, g_hTab, (HMENU)6015, g_hInst, NULL);
        SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"7天前");
        SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"30天前");
        SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"90天前");
        SendMessageW(g_hToolAgeCombo, CB_ADDSTRING, 0, (LPARAM)L"180天前");
        SendMessageW(g_hToolAgeCombo, CB_SETCURSEL, 1, 0);

        // 信息标签
        g_hToolInfoLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT,
            tabRect.left + 10, tabRect.top + 114, listW - 130, 20, g_hTab, (HMENU)6016, g_hInst, NULL);

        // ListView
        DWORD toolListH = tabRect.bottom - tabRect.top - 160;
        // 不能用 LVS_SINGLESEL：重复文件/磁盘清理/进程管理都支持多选批量操作,
        // 单选样式会让「删除选中/结束进程/修复快捷方式」一次只能处理一项
        g_hToolListView = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            tabRect.left + 10, tabRect.top + 140, listW - 20, toolListH, g_hTab, NULL, g_hInst, NULL);

        // 设置列表视图扩展样式（禁用滚动条直到需要时）
        ListView_SetExtendedListViewStyle(g_hToolListView,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        g_hStatusText = CreateWindowW(L"STATIC", L"就绪 | 管理员模式", WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 520, 780, 20, hWnd, NULL, g_hInst, NULL);

        RefreshCleanList();
        RefreshStartupList();
        RefreshContextMenuList();

        ShowWindow(g_hStartupList, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 2001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 2002), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 2003), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 2005), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 2006), SW_HIDE);
        ShowWindow(g_hContextMenuList, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 3001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 3002), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 3003), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 3005), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 3006), SW_HIDE);
        ShowWindow(g_hRegCleanList, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 5001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 5002), SW_HIDE);
        ShowWindow(g_hSpeedResult, SW_HIDE);
        ShowWindow(g_hHostsEdit, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 4001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 4002), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 4003), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 4004), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 4005), SW_HIDE);
        ShowWindow(g_hUninstallList, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 7001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 7002), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 7003), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 7004), SW_HIDE);
        ShowWindow(g_hUninstallFilter, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 7007), SW_HIDE);
        ShowWindow(g_hUninstallSearch, SW_HIDE);
        ShowWindow(g_hShredPath, SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6001), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6002), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6010), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6011), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6012), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6013), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6014), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6015), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6016), SW_HIDE);
        ShowWindow(GetDlgItem(g_hTab, 6020), SW_HIDE);
        ShowWindow(g_hToolListView, SW_HIDE);
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);

        SetWindowPos(g_hTab, NULL, 10, 10, w - 20, h - 50, SWP_NOZORDER);

        RECT tabRect;
        GetClientRect(g_hTab, &tabRect);
        TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);

        int listW = tabRect.right - tabRect.left - 20;
        int listH = tabRect.bottom - tabRect.top - 50;

        AdjustListViewHeight(g_hCleanList, tabRect, 50, 50);
        SetWindowPos(g_hCleanProgress, NULL, tabRect.left + 10, tabRect.bottom - 30, listW, 20, SWP_NOZORDER);

        AdjustListViewHeight(g_hStartupList, tabRect, 50, 50);

        AdjustListViewHeight(g_hContextMenuList, tabRect, 50, 50);

        AdjustListViewHeight(g_hRegCleanList, tabRect, 50, 50);

        AdjustListViewHeight(g_hUninstallList, tabRect, 50, 50);

        SetWindowPos(g_hSpeedResult, NULL, tabRect.left + 10, tabRect.top + 60, listW, 95, SWP_NOZORDER);
        SetWindowPos(g_hSpeedProgress, NULL, tabRect.left + 10, tabRect.top + 160, listW, 20, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4006), NULL, tabRect.left + 10, tabRect.top + 195, 200, 20, SWP_NOZORDER);
        SetWindowPos(g_hHostsEdit, NULL, tabRect.left + 10, tabRect.top + 225, listW, listH - 180, SWP_NOZORDER);

        int btnRight = tabRect.right - 10;
        SetWindowPos(GetDlgItem(g_hTab, 1004), NULL, btnRight - 100, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 1003), NULL, btnRight - 210, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 1005), NULL, btnRight - 330, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 1006), NULL, tabRect.left + 190, tabRect.top + 10, 80, 30, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(g_hTab, 4001), NULL, tabRect.left + 10, tabRect.top + 10, 120, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4002), NULL, tabRect.left + 140, tabRect.top + 10, 120, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4003), NULL, tabRect.left + 270, tabRect.top + 10, 120, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4004), NULL, btnRight - 220, tabRect.top + 190, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4005), NULL, btnRight - 110, tabRect.top + 190, 100, 30, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(g_hTab, 2001), NULL, tabRect.left + 10, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 2002), NULL, tabRect.left + 120, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 2003), NULL, tabRect.left + 230, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 2005), NULL, tabRect.right - 225, tabRect.top + 12, 130, 200, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 2006), NULL, tabRect.right - 300, tabRect.top + 17, 75, 20, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(g_hTab, 3001), NULL, tabRect.left + 10, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 3002), NULL, tabRect.left + 120, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 3003), NULL, tabRect.left + 230, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 3005), NULL, tabRect.right - 225, tabRect.top + 12, 130, 200, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 3006), NULL, tabRect.right - 300, tabRect.top + 17, 75, 20, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(g_hTab, 5001), NULL, tabRect.left + 10, tabRect.top + 10, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 5002), NULL, tabRect.left + 120, tabRect.top + 10, 100, 30, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(g_hTab, 7001), NULL, tabRect.left + 10, tabRect.top + 10, 80, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7002), NULL, tabRect.left + 95, tabRect.top + 10, 80, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7003), NULL, tabRect.left + 180, tabRect.top + 17, 110, 20, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7010), NULL, tabRect.left + 300, tabRect.top + 17, 35, 20, SWP_NOZORDER);
        SetWindowPos(g_hUninstallFilter, NULL, tabRect.left + 340, tabRect.top + 12, 90, 200, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7007), NULL, tabRect.left + 440, tabRect.top + 17, 35, 20, SWP_NOZORDER);
        SetWindowPos(g_hUninstallSearch, NULL, tabRect.left + 475, tabRect.top + 12, 200, 25, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7004), NULL, btnRight - 110, tabRect.top + 10, 100, 30, SWP_NOZORDER);

        // Tools page layout
        int toolTop = tabRect.top + 10;
        int btnRight2 = tabRect.right - 110;

        // Function selector label + combo (top)
        SetWindowPos(GetDlgItem(g_hTab, 6030), NULL, tabRect.left + 10, toolTop + 2, 80, 20, SWP_NOZORDER);
        SetWindowPos(g_hToolFuncCombo, NULL, tabRect.left + 90, toolTop, 180, 300, SWP_NOZORDER);

        // Path edit (shared between shred and other tools)
        SetWindowPos(g_hShredPath, NULL, tabRect.left + 10, toolTop + 40, tabRect.right - tabRect.left - 230, 25, SWP_NOZORDER);
        SetWindowPos(g_hToolPathEdit, NULL, tabRect.left + 10, toolTop + 40, tabRect.right - tabRect.left - 230, 25, SWP_NOZORDER);

        // Buttons (unified positions)
        SetWindowPos(GetDlgItem(g_hTab, 6001), NULL, btnRight2, toolTop + 40, 100, 25, SWP_NOZORDER); // select file
        SetWindowPos(GetDlgItem(g_hTab, 6012), NULL, btnRight2, toolTop + 40, 100, 25, SWP_NOZORDER); // browse
        SetWindowPos(GetDlgItem(g_hTab, 6002), NULL, btnRight2, toolTop + 72, 100, 25, SWP_NOZORDER); // shred
        SetWindowPos(GetDlgItem(g_hTab, 6013), NULL, btnRight2, toolTop + 72, 100, 25, SWP_NOZORDER); // scan
        SetWindowPos(GetDlgItem(g_hTab, 6014), NULL, btnRight2, toolTop + 104, 100, 25, SWP_NOZORDER); // action

        // Age combo for disk cleanup
        SetWindowPos(g_hToolAgeCombo, NULL, tabRect.left + 10, toolTop + 72, 150, 250, SWP_NOZORDER);

        // Info label
        SetWindowPos(g_hToolInfoLabel, NULL, tabRect.left + 10, toolTop + 104, tabRect.right - tabRect.left - 130, 20, SWP_NOZORDER);

        // ListView fills remaining space
        int listTop = toolTop + 130;
        int toolListH = tabRect.bottom - listTop - 5;
        SetWindowPos(g_hToolListView, NULL, tabRect.left + 10, listTop, tabRect.right - tabRect.left - 20, toolListH, SWP_NOZORDER);

        // Apply tool-specific visibility
        {
            bool showTool = (TabCtrl_GetCurSel(g_hTab) == 6);
            if (showTool && g_toolsReady) {
                // Re-apply current function's visibility
                SwitchToolFunction(g_toolFunction);
            } else {
                // Hide all tool controls
                ShowWindow(g_hToolFuncCombo, SW_HIDE);
                ShowWindow(g_hToolListView, SW_HIDE);
                ShowWindow(g_hToolPathEdit, SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6012), SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6013), SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6014), SW_HIDE);
                ShowWindow(g_hToolAgeCombo, SW_HIDE);
                ShowWindow(g_hToolInfoLabel, SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6020), SW_HIDE);
                ShowWindow(g_hShredPath, SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6001), SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6002), SW_HIDE);
                ShowWindow(GetDlgItem(g_hTab, 6030), SW_HIDE);
            }
        }

        SetWindowPos(g_hStatusText, NULL, 10, h - 30, w - 20, 20, SWP_NOZORDER);
        break;
    }

    case WM_NOTIFY: {
        LPNMHDR lpnmhdr = (LPNMHDR)lParam;
        if (lpnmhdr->hwndFrom == g_hTab && lpnmhdr->code == TCN_SELCHANGE) {
            int curSel = TabCtrl_GetCurSel(g_hTab);
            SwitchTab(curSel);
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case 1001:
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) ListView_SetCheckState(g_hCleanList, i, TRUE);
            break;
        case 1002:
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) {
                BOOL checked = ListView_GetCheckState(g_hCleanList, i);
                ListView_SetCheckState(g_hCleanList, i, !checked);
            }
            break;
        case 1006:
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) ListView_SetCheckState(g_hCleanList, i, FALSE);
            break;
        case 1005:
            g_stopScan = true;
            g_stopClean = true;
            break;
        case 1003: {
            if (g_isScanning || g_isCleaning) break;
            int selectedCount = 0;
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) {
                if (ListView_GetCheckState(g_hCleanList, i)) selectedCount++;
            }
            if (selectedCount == 0) { MessageBoxW(hWnd, L"请至少勾选一个清理项目", L"提示", MB_OK | MB_ICONWARNING); break; }
            g_isScanning = true;
            g_stopScan = false;
            EnableWindow(GetDlgItem(g_hTab, 1005), TRUE);
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在扫描...");
            SendMessageW(g_hCleanProgress, PBM_SETRANGE, 0, MAKELPARAM(0, selectedCount));
            CollectScanSnapshot();   // 在 UI 线程采集勾选状态后再启动工作线程
            HANDLE hScanThread = CreateThread(NULL, 0, ScanThreadProc, hWnd, 0, NULL);
            if (hScanThread) CloseHandle(hScanThread);
            break;
        }
        case 1004: {
            if (g_isScanning || g_isCleaning) break;
            int selected = 0;
            int unscannedCount = 0;
            bool hasSensitive = false;
            long long totalSize = 0;
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) {
                if (ListView_GetCheckState(g_hCleanList, i)) {
                    selected++;
                    if (!g_cleanItems[i].isScanned) unscannedCount++;
                    if (g_cleanItems[i].isSensitive) hasSensitive = true;
                    totalSize += g_cleanItems[i].totalSize;
                }
            }
            if (selected == 0) { MessageBoxW(hWnd, L"请至少选择一个清理项目", L"提示", MB_OK | MB_ICONWARNING); break; }
            if (unscannedCount > 0) {
                wchar_t msg[256];
                swprintf_s(msg, L"有 %d 项未扫描，无法清理！\n请先点击「开始扫描」扫描后再清理。", unscannedCount);
                MessageBoxW(hWnd, msg, L"提示", MB_OK | MB_ICONWARNING);
                break;
            }

            std::wstring itemList = L"";
            int shownCount = 0;
            const int maxShow = 5;
            for (int i = 0; i < ListView_GetItemCount(g_hCleanList); i++) {
                if (ListView_GetCheckState(g_hCleanList, i)) {
                    if (shownCount < maxShow) {
                        itemList += L"  • " + g_cleanItems[i].name + L"\n";
                        shownCount++;
                    }
                    else {
                        itemList += L"  • ... 等共 " + std::to_wstring(selected) + L" 项\n";
                        break;
                    }
                }
            }

            if (hasSensitive) {
                wchar_t msg1[512];
                swprintf_s(msg1, L"您勾选了隐私数据项！\n删除后密码、登录状态将永久丢失！\n是否确认继续？");
                if (MessageBoxW(hWnd, msg1, L"风险警告",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) break;
            }

            std::wstring sizeStr = FormatSize(totalSize);
            wchar_t msg[1024];
            swprintf_s(msg,
                L"即将清理以下 %d 项：\n\n%ls\n"
                L"本次预计可释放空间：%ls\n\n"
                L"安全机制：\n"
                L"  ✓ 系统关键路径已锁定\n"
                L"  ✓ 用户数据文件已保护\n"
                L"  ✓ 被占用文件将跳过\n\n"
                L"确定要继续清理吗？",
                selected, itemList.c_str(), sizeStr.c_str());
            if (MessageBoxW(hWnd, msg, L"清理确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            g_isCleaning = true;
            g_stopClean = false;
            EnableWindow(GetDlgItem(g_hTab, 1005), TRUE);
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在清理...");
            SendMessageW(g_hCleanProgress, PBM_SETRANGE, 0, MAKELPARAM(0, selected));
            CollectCleanSnapshot();  // 在 UI 线程采集勾选状态后再启动工作线程
            HANDLE hCleanThread = CreateThread(NULL, 0, CleanThreadProc, hWnd, 0, NULL);
            if (hCleanThread) CloseHandle(hCleanThread);
            break;
        }

        case 2001: RefreshStartupList(); break;
        case 2002: {
            int selected = ListView_GetSelectedCount(g_hStartupList);
            if (selected == 0) { MessageBoxW(hWnd, L"请先选择要禁用的启动项", L"提示", MB_OK | MB_ICONWARNING); break; }
            wchar_t msg[128]; swprintf_s(msg, L"确定禁用选中的 %d 个启动项吗？", selected);
            if (MessageBoxW(hWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            int success = 0;
            int idx = -1;
            while ((idx = ListView_GetNextItem(g_hStartupList, idx, LVNI_SELECTED)) != -1) {
                int itemIdx = (int)GetListViewItemParam(g_hStartupList, idx);
                if (itemIdx >= 0 && itemIdx < (int)g_startupItems.size() && DisableStartupItem(g_startupItems[itemIdx])) success++;
            }
            wchar_t result[128]; swprintf_s(result, L"成功禁用 %d 项", success);
            MessageBoxW(hWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
            RefreshStartupList();
            break;
        }
        case 2003: {
            int selected = ListView_GetSelectedCount(g_hStartupList);
            if (selected == 0) { MessageBoxW(hWnd, L"请先选择要恢复的启动项", L"提示", MB_OK | MB_ICONWARNING); break; }
            wchar_t msg[128]; swprintf_s(msg, L"确定恢复选中的 %d 个启动项吗？", selected);
            if (MessageBoxW(hWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            int success = 0;
            int idx = -1;
            while ((idx = ListView_GetNextItem(g_hStartupList, idx, LVNI_SELECTED)) != -1) {
                int itemIdx = (int)GetListViewItemParam(g_hStartupList, idx);
                if (itemIdx >= 0 && itemIdx < (int)g_startupItems.size() && EnableStartupItem(g_startupItems[itemIdx])) success++;
            }
            wchar_t result[128]; swprintf_s(result, L"成功恢复 %d 项", success);
            MessageBoxW(hWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
            RefreshStartupList();
            break;
        }
        case 2005: { // 启动项状态筛选框
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_startupFilterType = (int)SendMessageW(GetDlgItem(g_hTab, 2005), CB_GETCURSEL, 0, 0);
                RefreshStartupList();
            }
            break;
        }

        case 3001: RefreshContextMenuList(); break;
        case 3002: {
            int selected = ListView_GetSelectedCount(g_hContextMenuList);
            if (selected == 0) { MessageBoxW(hWnd, L"请先选择要禁用的菜单项", L"提示", MB_OK | MB_ICONWARNING); break; }
            wchar_t msg[128]; swprintf_s(msg, L"确定禁用选中的 %d 个右键菜单项吗？", selected);
            if (MessageBoxW(hWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            int success = 0;
            int idx = -1;
            while ((idx = ListView_GetNextItem(g_hContextMenuList, idx, LVNI_SELECTED)) != -1) {
                int itemIdx = (int)GetListViewItemParam(g_hContextMenuList, idx);
                if (itemIdx >= 0 && itemIdx < (int)g_contextMenuItems.size() && DisableContextMenuItem(g_contextMenuItems[itemIdx])) success++;
            }
            wchar_t result[128]; swprintf_s(result, L"成功禁用 %d 项", success);
            MessageBoxW(hWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
            RefreshContextMenuList();
            break;
        }
        case 3003: {
            int selected = ListView_GetSelectedCount(g_hContextMenuList);
            if (selected == 0) { MessageBoxW(hWnd, L"请先选择要恢复的菜单项", L"提示", MB_OK | MB_ICONWARNING); break; }
            wchar_t msg[128]; swprintf_s(msg, L"确定恢复选中的 %d 个右键菜单项吗？", selected);
            if (MessageBoxW(hWnd, msg, L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            int success = 0;
            int idx = -1;
            while ((idx = ListView_GetNextItem(g_hContextMenuList, idx, LVNI_SELECTED)) != -1) {
                int itemIdx = (int)GetListViewItemParam(g_hContextMenuList, idx);
                if (itemIdx >= 0 && itemIdx < (int)g_contextMenuItems.size() && EnableContextMenuItem(g_contextMenuItems[itemIdx])) success++;
            }
            wchar_t result[128]; swprintf_s(result, L"成功恢复 %d 项", success);
            MessageBoxW(hWnd, result, L"完成", MB_OK | MB_ICONINFORMATION);
            RefreshContextMenuList();
            break;
        }
        case 3005: { // 右键菜单状态筛选框
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_contextMenuFilterType = (int)SendMessageW(GetDlgItem(g_hTab, 3005), CB_GETCURSEL, 0, 0);
                RefreshContextMenuList();
            }
            break;
        }

        case 5001:
            if (g_isRegScanning) break;
            g_isRegScanning = true;
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在扫描注册表...");
            {
                HANDLE hRegThread = CreateThread(NULL, 0, RegScanThreadProc, hWnd, 0, NULL);
                if (hRegThread) CloseHandle(hRegThread);
            }
            break;
        case 5002:
            CleanSelectedRegistry();
            break;

        case 4001:
            if (g_isSpeedTesting) break;
            g_isSpeedTesting = true;
            g_stopSpeedTest = false;
            SetWindowTextW(g_hSpeedResult, L"准备测速...");
            SendMessageW(g_hSpeedProgress, PBM_SETPOS, 0, 0);
            ShowWindow(g_hSpeedProgress, SW_SHOW);
            {
                HANDLE hSpeedThread = CreateThread(NULL, 0, SpeedTestThreadProc, hWnd, 0, NULL);
                if (hSpeedThread) CloseHandle(hSpeedThread);
            }
            break;
        case 4002:
            if (g_isSpeedTesting) g_stopSpeedTest = true;
            break;
        case 4003:
            if (g_isNetworkRepairing) break;
            if (MessageBoxW(hWnd, L"确定执行一键网络修复吗？\n将重置Winsock、TCP/IP、DNS、代理和Hosts。", L"确认",
                MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            g_isNetworkRepairing = true;
            EnableWindow(GetDlgItem(g_hTab, 4003), FALSE);
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在修复网络（需数十秒，请耐心等待）...");
            {
                HANDLE hRepairThread = CreateThread(NULL, 0, NetworkRepairThreadProc, hWnd, 0, NULL);
                if (hRepairThread) CloseHandle(hRepairThread);
            }
            break;
        case 4004:
            SaveHostsFile();
            break;
        case 4005:
            if (MessageBoxW(hWnd, L"确定恢复Hosts为系统默认吗？", L"确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::wstring defaultHosts = L"# Copyright (c) 1993-2009 Microsoft Corp.\r\n#\r\n# 127.0.0.1       localhost\r\n# ::1             localhost\r\n\r\n127.0.0.1       localhost\r\n::1             localhost\r\n";
                SetWindowTextW(g_hHostsEdit, defaultHosts.c_str());
                SaveHostsFile();
            }
            break;

        // ========== 实用工具页事件 ==========
        case 6010: { // 功能选择下拉框切换
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (!g_toolsReady) break;
                int funcId = (int)SendMessageW(g_hToolFuncCombo, CB_GETCURSEL, 0, 0);
                SwitchToolFunction(funcId);
                // Force re-layout
                RECT tabRect;
                GetClientRect(g_hTab, &tabRect);
                TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
                LayoutToolControls(tabRect);
            }
            break;
        }
        case 6011: // 路径编辑框（EN_CHANGE时记录当前路径）
            if (HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[MAX_PATH] = { 0 };
                GetWindowTextW(g_hToolPathEdit, buf, MAX_PATH);
                g_toolCurrentPath = buf;
            }
            break;
        case 6012: { // 浏览按钮 - 选择目录
            BROWSEINFOW bi = { 0 };
            bi.hwndOwner = hWnd;
            bi.lpszTitle = L"选择要扫描的目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = { 0 };
                if (SHGetPathFromIDListW(pidl, path)) {
                    SetWindowTextW(g_hToolPathEdit, path);
                    g_toolCurrentPath = path;
                }
                CoTaskMemFree(pidl);
            }
            break;
        }
        case 6013: { // 扫描/刷新按钮
            switch (g_toolFunction) {
            case 1: RefreshSystemInfo(); break;
            case 2: StartDuplicateScan(); break;
            case 3: RefreshProcessList(); break;
            case 4: StartShortcutScan(); break;
            case 5: StartDiskCleanupScan(); break;
            }
            break;
        }
        case 6014: { // 操作按钮（删除/结束/修复）
            switch (g_toolFunction) {
            case 2: CleanSelectedDuplicates(); break;
            case 3: KillSelectedProcess(); break;
            case 4: FixSelectedShortcuts(); break;
            case 5: CleanSelectedDiskFiles(); break;
            }
            break;
        }

        // ========== 文件粉碎（保留原有） ==========
        case 6001: {
            OPENFILENAMEW ofn = { 0 };
            wchar_t szFile[MAX_PATH] = { 0 };
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"所有文件\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(g_hShredPath, szFile);
            }
            break;
        }
        case 6002: {
            wchar_t path[MAX_PATH] = { 0 };
            GetWindowTextW(g_hShredPath, path, MAX_PATH);
            if (wcslen(path) == 0) { MessageBoxW(hWnd, L"请先选择要粉碎的文件", L"提示", MB_OK | MB_ICONWARNING); break; }
            if (MessageBoxW(hWnd, L"文件粉碎后无法恢复，确定继续吗？", L"警告", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) break;

            if (ShredFile(path)) {
                MessageBoxW(hWnd, L"文件粉碎成功！", L"完成", MB_OK | MB_ICONINFORMATION);
                SetWindowTextW(g_hShredPath, L"");
            }
            else {
                MessageBoxW(hWnd, L"粉碎失败，请检查文件是否被占用。", L"错误", MB_OK | MB_ICONERROR);
            }
            break;
        }

        case 7001: {
            if (g_isUninstalling) break;
            InitUninstallList();
            break;
        }
        case 7002: {
            if (g_isUninstalling) break;
            int selectedCount = 0;
            bool hasSystemApp = false;
            int listItemCount = ListView_GetItemCount(g_hUninstallList);
            EnterCriticalSection(&g_uninstallItemsLock);
            // 先复位全部勾选：ListView 只含筛选/搜索后可见的项，
            // 若不复位，"先勾选再被筛选隐藏"的项会残留 isChecked=true 而被误卸载
            for (auto& it : g_uninstallItems) it.isChecked = false;
            for (int i = 0; i < listItemCount; i++) {
                BOOL checked = ListView_GetCheckState(g_hUninstallList, i);
                int itemIdx = (int)GetListViewItemParam(g_hUninstallList, i);
                if (itemIdx < 0 || itemIdx >= (int)g_uninstallItems.size()) continue;
                g_uninstallItems[itemIdx].isChecked = (checked == TRUE);
                if (checked) {
                    selectedCount++;
                    if (g_uninstallItems[itemIdx].type == L"系统软件") hasSystemApp = true;
                }
            }
            if (selectedCount == 0) {
                LeaveCriticalSection(&g_uninstallItemsLock);
                MessageBoxW(hWnd, L"请先勾选要卸载的软件", L"提示", MB_OK | MB_ICONWARNING);
                break;
            }

            if (hasSystemApp) {
                if (MessageBoxW(hWnd, L"警告：您勾选了系统软件！\n卸载系统软件可能导致系统不稳定！\n确定继续吗？", L"危险警告",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                    LeaveCriticalSection(&g_uninstallItemsLock);
                    break;
                }
            }

            // 用 std::wstring 拼接：软件名来自注册表，固定 wchar_t[512] + wcscat_s
            // 在名称较长时会触发 invalid parameter handler 直接终止进程
            std::wstring msg = L"即将卸载以下 " + std::to_wstring(selectedCount) + L" 项软件：\n\n";
            int shown = 0;
            for (int i = 0; i < (int)g_uninstallItems.size() && shown < 5; i++) {
                if (g_uninstallItems[i].isChecked) {
                    msg += L"  • " + g_uninstallItems[i].displayName + L"\n";
                    shown++;
                }
            }
            LeaveCriticalSection(&g_uninstallItemsLock);
            if (selectedCount > 5) {
                msg += L"  • ... 等共 " + std::to_wstring(selectedCount) + L" 项\n";
            }

            g_keepUserData = (SendMessageW(GetDlgItem(g_hTab, 7003), BM_GETCHECK, 0, 0) == BST_CHECKED);

            if (!g_keepUserData) {
                msg += L"\n⚠ 注意：已选择不保留用户数据，卸载后将清理所有残留文件！\n";
            }

            msg += L"\n确定要继续卸载吗？";
            if (MessageBoxW(hWnd, msg.c_str(), L"卸载确认", MB_YESNO | MB_ICONQUESTION) != IDYES) break;

            g_isUninstalling = true;
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在卸载...");
            {
                HANDLE hUninstThread = CreateThread(NULL, 0, UninstallThreadProc, hWnd, 0, NULL);
                if (hUninstThread) CloseHandle(hUninstThread);
            }
            break;
        }
        case 7003:
            g_keepUserData = (SendMessageW(GetDlgItem(g_hTab, 7003), BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case 7008: { // 搜索框
            if (HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[512];
                GetWindowTextW(g_hUninstallSearch, buf, 512);
                g_uninstallSearchText = buf;

                // Debounce: start/reset timer, only refresh after user stops typing
                g_uninstallDebounceForceRescan = false;
                SetTimer(hWnd, TIMER_UNINSTALL_DEBOUNCE, UNINSTALL_DEBOUNCE_MS, NULL);
                g_uninstallDebouncePending = true;
            }
            break;
        }
        case 7004: {
            if (g_isUninstalling) break;
            int selectedCount = 0;
            int blockedCount = 0;   // Windows 自带组件：强卸会蓝屏/无法启动，必须挡下
            bool hasSystemApp = false;
            int listItemCount = ListView_GetItemCount(g_hUninstallList);
            EnterCriticalSection(&g_uninstallItemsLock);
            // 同 7002：先复位全部勾选，避免被筛选隐藏的旧勾选项被强制卸载
            for (auto& it : g_uninstallItems) it.isChecked = false;
            for (int i = 0; i < listItemCount; i++) {
                BOOL checked = ListView_GetCheckState(g_hUninstallList, i);
                int itemIdx = (int)GetListViewItemParam(g_hUninstallList, i);
                if (itemIdx < 0 || itemIdx >= (int)g_uninstallItems.size()) continue;
                g_uninstallItems[itemIdx].isChecked = (checked == TRUE);
                if (checked) {
                    selectedCount++;
                    if (g_uninstallItems[itemIdx].isSystemComponent) blockedCount++;
                    if (g_uninstallItems[itemIdx].type == L"系统软件") hasSystemApp = true;
                }
            }
            if (selectedCount == 0) {
                LeaveCriticalSection(&g_uninstallItemsLock);
                MessageBoxW(hWnd, L"请先勾选要强制卸载的软件", L"提示", MB_OK | MB_ICONWARNING);
                break;
            }

            // 全是系统组件时直接拒绝：否则用户点完两次「确定」才发现什么都没删
            if (blockedCount == selectedCount) {
                LeaveCriticalSection(&g_uninstallItemsLock);
                MessageBoxW(hWnd, L"勾选的条目全部是 Windows 自带组件（驱动/系统服务），\n"
                    L"删除它们会导致系统蓝屏或无法启动，已拒绝执行。",
                    L"禁止操作", MB_OK | MB_ICONERROR);
                break;
            }

            // 用 std::wstring 拼接：软件名来自注册表，固定 wchar_t[512] + wcscat_s
            // 在名称较长时会触发 invalid parameter handler 直接终止进程
            std::wstring msg = L"即将强制卸载以下 " + std::to_wstring(selectedCount) + L" 项软件：\n\n";
            int shown = 0;
            for (int i = 0; i < (int)g_uninstallItems.size() && shown < 5; i++) {
                if (g_uninstallItems[i].isChecked) {
                    msg += L"  • " + g_uninstallItems[i].displayName + L"\n";
                    shown++;
                }
            }
            LeaveCriticalSection(&g_uninstallItemsLock);
            if (selectedCount > 5) {
                msg += L"  • ... 等共 " + std::to_wstring(selectedCount) + L" 项\n";
            }

            if (hasSystemApp) {
                msg += L"\n⚠ 其中包含「系统软件」，强制卸载可能导致系统不稳定！\n";
            }
            // 原实现只弹一句「此操作不可恢复」，勾选了系统驱动也照样删 → 直接蓝屏
            if (blockedCount > 0) {
                msg += L"\n⚠ 其中 " + std::to_wstring(blockedCount) +
                    L" 项是 Windows 自带组件，将被自动跳过（删除会蓝屏）。\n";
            }
            msg += L"\n强制卸载不会调用软件自带的卸载程序，且会删除用户数据。\n此操作不可恢复！确定继续吗？";
            if (MessageBoxW(hWnd, msg.c_str(), L"强制卸载警告", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) break;

            g_isUninstalling = true;
            g_keepUserData = false;
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在强制卸载...");
            {
                HANDLE hForceThread = CreateThread(NULL, 0, ForceUninstallThreadProc, hWnd, 0, NULL);
                if (hForceThread) CloseHandle(hForceThread);
            }
            break;
        }
        case 7005: { // 类型筛选框
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                // 卸载进行中不得重扫：那会清空 g_uninstallItems，让工作线程手上的索引失效
                if (g_isUninstalling) break;
                g_uninstallFilterType = (int)SendMessageW(g_hUninstallFilter, CB_GETCURSEL, 0, 0);

                // 立即让旧扫描线程停止
                g_uninstallScanCancelled = true;
                ListView_DeleteAllItems(g_hUninstallList);

                // Debounce: 合并快速切换，停止操作后才真正启动新扫描
                g_uninstallDebounceForceRescan = true;
                SetTimer(hWnd, TIMER_UNINSTALL_DEBOUNCE, UNINSTALL_DEBOUNCE_MS, NULL);
                g_uninstallDebouncePending = true;
            }
            break;
        }
        }
        break;
    }

    case WM_SCAN_PROGRESS: {
        int progressIdx = (int)wParam;
        int itemIdx = (int)lParam;
        long long size = g_cleanItems[itemIdx].totalSize;
        SendMessageW(g_hCleanProgress, PBM_SETPOS, progressIdx + 1, 0);

        std::wstring sizeStr = FormatSize(size);
        ListView_SetItemText(g_hCleanList, itemIdx, 2, (LPWSTR)sizeStr.c_str());

        wchar_t countStr[16];
        swprintf_s(countStr, L"%d", g_cleanItems[itemIdx].fileCount);
        ListView_SetItemText(g_hCleanList, itemIdx, 3, countStr);

        wchar_t status[128];
        swprintf_s(status, L"正在扫描: %s", g_cleanItems[itemIdx].name.c_str());
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)status);
        break;
    }

    case WM_SCAN_DONE: {
        g_isScanning = false;
        EnableWindow(GetDlgItem(g_hTab, 1005), FALSE);
        // 总大小由工作线程经 LPARAM 堆指针传来（WPARAM 在 Win32 构建下只有 32 位）
        long long* pTotal = (long long*)lParam;
        long long total = pTotal ? *pTotal : 0;
        std::wstring totalStr = FormatSize(total);
        std::wstring status;
        if (g_stopScan) {
            g_stopScan = false;
            status = L"扫描已取消";
        }
        else {
            status = L"扫描完成，共可清理 " + totalStr;
        }
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)status.c_str());
        delete pTotal;
        break;
    }

    case WM_CLEAN_PROGRESS: {
        int cur = (int)wParam;
        SendMessageW(g_hCleanProgress, PBM_SETPOS, cur + 1, 0);
        break;
    }

    case WM_CLEAN_DONE: {
        g_isCleaning = false;
        EnableWindow(GetDlgItem(g_hTab, 1005), FALSE);
        // 释放总量由工作线程经 LPARAM 堆指针传来（WPARAM 在 Win32 构建下只有 32 位）
        long long* pFreed = (long long*)lParam;
        long long total = pFreed ? *pFreed : 0;
        std::wstring totalStr = FormatSize(total);

        WriteCleanFailureLog();

        bool wasCancelled = g_stopClean;
        g_stopClean = false;

        if (wasCancelled) {
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"清理已取消");
            delete pFreed;
            break;
        }

        wchar_t msg[256];
        if (!g_cleanFailures.empty()) {
            swprintf_s(msg, L"清理完成！\n共释放空间：%s\n有 %d 个文件因被占用或其他原因跳过，详见日志文件",
                totalStr.c_str(), (int)g_cleanFailures.size());
        }
        else {
            swprintf_s(msg, L"清理完成！\n共释放空间：%s", totalStr.c_str());
        }
        MessageBoxW(g_hMainWnd, msg, L"完成", MB_OK | MB_ICONINFORMATION);

        // 自动重新扫描：必须在 UI 线程重新采集勾选快照，否则工作线程会沿用
        // 上一次扫描遗留的快照（勾选已变化时结果错误，快照为空时进度直接"完成"）
        int rescanCount = CollectScanSnapshot();
        if (rescanCount == 0) {
            g_isScanning = false;
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"清理完成");
            delete pFreed;
            break;
        }

        g_isScanning = true;
        g_stopScan = false;
        // 自动重新扫描期间必须重新启用取消按钮：之前保持禁用，用户无法取消这次扫描
        EnableWindow(GetDlgItem(g_hTab, 1005), TRUE);
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在重新扫描...");
        // 进度条范围必须按本次重扫项数重置，否则沿用清理阶段的范围导致进度显示错误
        SendMessageW(g_hCleanProgress, PBM_SETRANGE, 0, MAKELPARAM(0, rescanCount));
        SendMessageW(g_hCleanProgress, PBM_SETPOS, 0, 0);
        HANDLE hRescanThread = CreateThread(NULL, 0, ScanThreadProc, g_hMainWnd, 0, NULL);
        if (hRescanThread) CloseHandle(hRescanThread);
        delete pFreed;
        break;
    }

    case WM_SPEED_UPDATE: {
        int progress = (int)wParam;
        SendMessageW(g_hSpeedProgress, PBM_SETPOS, progress, 0);

        if (lParam != 0) {
            SpeedUpdateInfo* info = (SpeedUpdateInfo*)lParam;
            wchar_t text[256];
            if (info->phase == 0) {
                std::wstring sizeStr = FormatSize(info->bytes);
                std::wstring speedStr = FormatSpeed(info->speed);
                swprintf_s(text, L"【下行测速中】\n进度：%d%% | 已下载：%s | 速度：%s\n剩余约 %d 秒",
                    progress, sizeStr.c_str(), speedStr.c_str(), info->remainSec);
            }
            else {
                std::wstring sizeStr = FormatSize(info->bytes);
                std::wstring speedStr = FormatSpeed(info->speed);
                swprintf_s(text, L"【上行测速中】\n进度：%d%% | 已上传：%s | 速度：%s",
                    progress, sizeStr.c_str(), speedStr.c_str());
            }
            SetWindowTextW(g_hSpeedResult, text);
            delete info;
        }
        break;
    }

    case WM_SPEED_DONE: {
        g_isSpeedTesting = false;
        g_stopSpeedTest = false;
        ShowWindow(g_hSpeedProgress, SW_HIDE);
        std::wstring* result = (std::wstring*)lParam;
        if (result) {
            SetWindowTextW(g_hSpeedResult, result->c_str());
            delete result;
        }
        break;
    }

    case WM_REG_SCAN_DONE: {
        g_isRegScanning = false;
        if (lParam == 1) {
            MessageBoxW(g_hMainWnd,
                L"扫描注册表需要管理员权限！\n请右键以管理员身份运行本程序。",
                L"权限不足", MB_OK | MB_ICONWARNING);
            break;
        }
        RefreshRegCleanList();
        wchar_t buf[128];
        swprintf_s(buf, L"注册表扫描完成，发现 %d 项无效项", (int)wParam);
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)buf);
        break;
    }

    case WM_NET_REPAIR_DONE: {
        g_isNetworkRepairing = false;
        EnableWindow(GetDlgItem(g_hTab, 4003), TRUE);
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"网络修复完成");
        MessageBoxW(g_hMainWnd, L"网络修复完成！\n建议重启电脑使全部设置生效。", L"完成", MB_OK | MB_ICONINFORMATION);
        break;
    }

    case WM_UNINSTALL_PROGRESS: {
        int cur = (int)wParam;
        int total = (int)lParam;
        wchar_t buf[128];
        swprintf_s(buf, L"正在卸载... (%d/%d)", cur, total);
        SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)buf);
        break;
    }

    case WM_UNINSTALL_DONE: {
        int scanResult = (int)wParam;
        if (scanResult == 0) {
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"扫描已取消");
        }
        else {
            RefreshUninstallList();
            wchar_t buf[128];
            swprintf_s(buf, L"扫描完成，共发现 %zu 项软件", g_uninstallItems.size());
            SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)buf);
        }
        break;
    }

    case WM_UNINSTALL_FINISHED: {
        g_isUninstalling = false;
        int successCount = (int)wParam;
        int failCount = (int)lParam;

        WriteUninstallFailureLog();

        wchar_t msg[256];
        if (failCount > 0) {
            swprintf_s(msg, L"卸载完成！\n成功卸载 %d 项，%d 项失败\n失败详情已保存到日志文件",
                successCount, failCount);
        }
        else {
            swprintf_s(msg, L"卸载完成！\n成功卸载 %d 项", successCount);
        }
        MessageBoxW(g_hMainWnd, msg, L"卸载完成", MB_OK | MB_ICONINFORMATION);

        InitUninstallList();
        break;
    }

    case WM_TIMER: {
        if (wParam == TIMER_UNINSTALL_DEBOUNCE) {
            KillTimer(hWnd, TIMER_UNINSTALL_DEBOUNCE);
            g_uninstallDebouncePending = false;

            if (g_uninstallDebounceForceRescan) {
                // 类型筛选：停止旧扫描并启动新扫描（代次递增，旧线程静默退出）
                g_uninstallDebounceForceRescan = false;
                StartUninstallScan();
            }
            else {
                // 搜索过滤：只基于现有数据刷新，不重新扫描
                ListView_DeleteAllItems(g_hUninstallList);
                RefreshUninstallList();
            }
        }
        break;
    }

    case WM_DESTROY:
        g_stopSpeedTest = true;
        g_stopScan = true;
        g_stopClean = true;
        g_uninstallScanCancelled = true;
        g_uninstallScanGeneration++; // 让卸载扫描线程尽快自行退出
        KillTimer(hWnd, TIMER_UNINSTALL_DEBOUNCE);
        // 不在此处 DeleteCriticalSection：工作线程可能仍在 EnterCriticalSection 中,
        // 删除使用中的临界区是 UB；进程退出时系统会回收该内核对象
        RemoveWindowSubclass(g_hTab, TabSubclassProc, 1);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

/**
 * @brief 应用程序入口函数
 */
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"SystemCleanerClass";

    InitializeCriticalSection(&g_uninstallItemsLock);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        0, CLASS_NAME, L"系统清理工具箱", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 620,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}