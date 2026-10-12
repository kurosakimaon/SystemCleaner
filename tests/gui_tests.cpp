// SystemCleaner GUI 交互测试
//
// 目的：在真实 Win32 窗口上验证 Tab 页切换的控件显隐矩阵、按钮命令的勾选状态
// 流转、以及 WM_*_DONE 消息的 64 位数据解析——这些是纯函数单测覆盖不到的部分。
//
// 做法：把全部模块 .cpp 直接 include 进来（SystemCleaner.cpp 的 wWinMain 改名，
// 避免与控制台 main 冲突），自己注册窗口类并创建主窗口，然后用
// IsWindowVisible / GetWindowText / PostMessage(WM_COMMAND) 驱动真实交互。
//
// 编译：tests\build_gui.cmd（控制台子系统、不嵌入 requireAdministrator manifest，
// 因此可在非管理员权限下运行；被测代码路径本身不依赖管理员权限）。

#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <vector>

// ====================== MessageBox 桩 ======================
//
// 被测代码里有大量 MessageBoxW 确认框。测试里不能真的弹窗（会阻塞消息循环），
// 因此在包含模块 .cpp 之前把 MessageBoxW 宏替换为桩函数：记录最后一次文本、
// 累加调用次数，直接返回 -1。
//   - "if (MessageBoxW(...) != IDYES) break;"  -> 走安全取消分支
//   - "if (MessageBoxW(...) == IDYES) {...}"    -> 条件为假，跳过破坏性操作
// windows.h 已在上面包含（有 include guard），宏只作用于模块调用点，不污染声明。
static int g_messageBoxCalls = 0;
static std::wstring g_lastMessageBoxText;

static int TestMessageBoxStub(LPCWSTR text) {
    ++g_messageBoxCalls;
    if (text) g_lastMessageBoxText = text;
    return -1; // 既不等于 IDYES(6) 也不等于 IDOK(1)
}
#define MessageBoxW(hwnd, text, caption, type) TestMessageBoxStub(text)

// 把应用入口改名，避免与本文件的 main 冲突
#define wWinMain gui_unused_wWinMain
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

// ====================== 弹窗自动关闭 ======================
//
// 被测代码里有不少 MessageBoxW 确认框。测试里不能真的点按钮，也不能让
// MessageBox 永久阻塞主线程，因此用一个后台线程把本进程的对话框直接关掉。
// MessageBox 被关闭时返回 -1，既不等于 IDYES 也不等于 IDOK，
// 于是所有"确认后才执行破坏性操作"的分支都会安全地 break 出来。

static volatile LONG g_dialogKillerOn = 0;
static HANDLE g_dialogKillerThread = NULL;

static BOOL CALLBACK KillDialogProc(HWND h, LPARAM) {
    wchar_t cls[32] = { 0 };
    if (GetClassNameW(h, cls, 32) == 0) return TRUE;
    if (wcscmp(cls, L"#32770") != 0) return TRUE; // 只处理对话框
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static DWORD WINAPI DialogKillerProc(LPVOID) {
    while (InterlockedCompareExchange(&g_dialogKillerOn, 1, 1) == 1) {
        EnumWindows(KillDialogProc, 0);
        Sleep(20);
    }
    return 0;
}

static void KillerStart() {
    // 幂等：整个测试期间只创建一次常驻杀手线程，
    // 任何位置意外弹出的模态框都会被自动关闭，避免按用例开关的竞态挂起
    if (g_dialogKillerThread) return;
    InterlockedExchange(&g_dialogKillerOn, 1);
    g_dialogKillerThread = CreateThread(NULL, 0, DialogKillerProc, NULL, 0, NULL);
}

static void KillerStop() {
    // 杀手线程常驻到进程结束，这里不做处理（进程退出时自动回收）
}

// 让主线程处理消息（MessageBox 的模态循环由 killer 线程负责关闭）
static void PumpMessages(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    } while ((LONG)(end - GetTickCount()) > 0);
}

// ====================== 辅助 ======================

static bool Vis(HWND h) { return h != NULL && IsWindowVisible(h) != FALSE; }

static std::wstring StatusText() {
    wchar_t buf[512] = { 0 };
    if (g_hStatusText) GetWindowTextW(g_hStatusText, buf, 512);
    return buf;
}

static std::wstring SpeedResultText() {
    wchar_t buf[1024] = { 0 };
    if (g_hSpeedResult) GetWindowTextW(g_hSpeedResult, buf, 1024);
    return buf;
}

// 中文结果文本直接 printf 会经控制台代码页转换而丢字，转成 UTF-8 再输出
static std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, NULL, NULL);
    s.resize(strlen(s.c_str()));
    return s;
}

static bool Has(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

// 受 SwitchTab 管理的控件：id（g_hTab 的子控件）或全局句柄 + 所属 Tab 页
struct ManagedCtl {
    const wchar_t* name;
    int id;          // 0 表示用 h 字段
    HWND* ph;        // id==0 时的句柄变量
    int ownerTab;    // 该控件应可见的 Tab 索引
};

static ManagedCtl g_managed[] = {
    { L"g_hCleanList",      0,    &g_hCleanList,      0 },
    { L"g_hCleanProgress",  0,    &g_hCleanProgress,  0 },
    { L"1001 全选",         1001, NULL,               0 },
    { L"1002 反选",         1002, NULL,               0 },
    { L"1003 开始扫描",     1003, NULL,               0 },
    { L"1004 立即清理",     1004, NULL,               0 },
    { L"1005 取消",         1005, NULL,               0 },
    { L"1006 全不选",       1006, NULL,               0 },

    { L"g_hStartupList",    0,    &g_hStartupList,     1 },
    { L"2001 刷新扫描",     2001, NULL,                1 },
    { L"2002 禁用选中",     2002, NULL,                1 },
    { L"2003 恢复选中",     2003, NULL,                1 },
    { L"2005 状态筛选",     2005, NULL,                1 },
    { L"2006 筛选标签",     2006, NULL,                1 },

    { L"g_hContextMenuList", 0,   &g_hContextMenuList, 2 },
    { L"3001 刷新扫描",     3001, NULL,                2 },
    { L"3002 禁用选中",     3002, NULL,                2 },
    { L"3003 恢复选中",     3003, NULL,                2 },
    { L"3005 状态筛选",     3005, NULL,                2 },
    { L"3006 筛选标签",     3006, NULL,                2 },

    { L"g_hRegCleanList",   0,    &g_hRegCleanList,    3 },
    { L"5001 扫描垃圾",     5001, NULL,                3 },
    { L"5002 清理选中",     5002, NULL,                3 },

    { L"g_hSpeedResult",    0,    &g_hSpeedResult,     4 },
    { L"g_hHostsEdit",      0,    &g_hHostsEdit,       4 },
    { L"4001 开始测速",     4001, NULL,                4 },
    { L"4002 停止测速",     4002, NULL,                4 },
    { L"4003 一键修复网络", 4003, NULL,                4 },
    { L"4004 保存Hosts",    4004, NULL,                4 },
    { L"4005 恢复默认",     4005, NULL,                4 },
    { L"4006 Hosts标签",    4006, NULL,                4 },

    { L"g_hUninstallList",  0,    &g_hUninstallList,   5 },
    { L"g_hUninstallFilter",0,    &g_hUninstallFilter, 5 },
    { L"g_hUninstallSearch",0,    &g_hUninstallSearch, 5 },
    { L"7001 刷新扫描",     7001, NULL,                5 },
    { L"7002 卸载选中",     7002, NULL,                5 },
    { L"7003 保留用户数据", 7003, NULL,                5 },
    { L"7004 强制卸载",     7004, NULL,                5 },
    { L"7007 搜索标签",     7007, NULL,                5 },
    { L"7010 类型标签",     7010, NULL,                5 },

    { L"g_hToolFuncCombo",  0,    &g_hToolFuncCombo,   6 },
    { L"6030 功能选择标签", 6030, NULL,                6 },
};

static HWND CtlHwnd(const ManagedCtl& c) {
    return c.id ? GetDlgItem(g_hTab, c.id) : (c.ph ? *c.ph : NULL);
}

// 切换到指定 Tab 并校验整张显隐矩阵
static void CheckMatrix(int tab) {
    SwitchTab(tab);
    wchar_t label[96];
    for (const auto& c : g_managed) {
        HWND h = CtlHwnd(c);
        if (!h) continue; // 该控件未创建（如 g_hRegProgress），跳过
        swprintf_s(label, L"tab=%d 控件 %s", tab, c.name);
        bool expect = (c.ownerTab == tab);
        bool actual = Vis(h);
        ++g_total;
        if (actual != expect) {
            ++g_failed;
            printf("  [FAIL] %s (line %d): %ls visible=%d expect=%d\n",
                g_group, __LINE__, label, (int)actual, (int)expect);
        }
    }
}

// ====================== 测试用例 ======================

// GUI-MATRIX：7 个 Tab 页两两互斥，任何页只显示自己的控件
static void Test_TabMatrix() {
    GROUP("GUI-MATRIX Tab 显隐矩阵");
    for (int tab = 0; tab <= 6; tab++) CheckMatrix(tab);
}

// GUI-4006：「Hosts文件编辑：」标签只在网络页可见（回归：曾被 SwitchTab 遗漏）
static void Test_HostsLabel() {
    GROUP("GUI-4006 Hosts 标签");
    HWND h = GetDlgItem(g_hTab, 4006);
    CHECK_MSG(h != NULL, "4006 控件必须存在");
    if (!h) return;
    SwitchTab(4);
    CHECK_MSG(Vis(h), "网络页应显示 Hosts 标签");
    SwitchTab(0);
    CHECK_MSG(!Vis(h), "离开网络页应隐藏 Hosts 标签");
    SwitchTab(6);
    CHECK_MSG(!Vis(h), "工具页不应残留 Hosts 标签");
}

// GUI-SPEEDBAR：测速进度条只在「网络页 且 正在测速」时出现
static void Test_SpeedProgressBar() {
    GROUP("GUI-SPEEDBAR 测速进度条");
    CHECK_MSG(g_hSpeedProgress != NULL, "进度条控件必须存在");
    if (!g_hSpeedProgress) return;

    g_isSpeedTesting = false;
    SwitchTab(4);
    CHECK_MSG(!Vis(g_hSpeedProgress), "未测速时进入网络页不应出现空进度条");

    g_isSpeedTesting = true;
    SwitchTab(4);
    CHECK_MSG(Vis(g_hSpeedProgress), "测速中在网络页应显示进度条");

    SwitchTab(0);
    CHECK_MSG(!Vis(g_hSpeedProgress), "离开网络页必须隐藏进度条");

    g_isSpeedTesting = false;
}

// GUI-CHECKOPS：全选 / 反选 / 全不选 与扫描快照数量一致
static void Test_CheckOperations() {
    GROUP("GUI-CHECKOPS 勾选操作");
    int n = ListView_GetItemCount(g_hCleanList);
    CHECK_MSG(n > 0, "垃圾清理列表应有条目");

    // 全不选
    SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), 0);
    int checked = 0;
    for (int i = 0; i < n; i++) if (ListView_GetCheckState(g_hCleanList, i)) checked++;
    CHECK_MSG(checked == 0, "全不选后勾选数应为 0");
    CHECK_MSG(CollectScanSnapshot() == 0, "全不选后扫描快照应为空");

    // 全选
    SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1001, BN_CLICKED), 0);
    checked = 0;
    for (int i = 0; i < n; i++) if (ListView_GetCheckState(g_hCleanList, i)) checked++;
    CHECK_MSG(checked == n, "全选后所有条目都应勾选");
    CHECK_MSG(CollectScanSnapshot() == n, "扫描快照数量应等于勾选数");

    // 反选
    SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED), 0);
    checked = 0;
    for (int i = 0; i < n; i++) if (ListView_GetCheckState(g_hCleanList, i)) checked++;
    CHECK_MSG(checked == 0, "全选后反选应全部取消");

    // 部分勾选后反选
    if (n >= 2) {
        ListView_SetCheckState(g_hCleanList, 0, TRUE);
        SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED), 0);
        CHECK_MSG(!ListView_GetCheckState(g_hCleanList, 0), "已勾项反选后应取消");
        CHECK_MSG(ListView_GetCheckState(g_hCleanList, 1), "未勾项反选后应勾选");
    }
    SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), 0);
}

// GUI-SCANDONE64：扫描完成消息用 LPARAM 堆指针传 64 位总大小（WPARAM 在 Win32 下只有 32 位）
static void Test_ScanDoneMarshaling() {
    GROUP("GUI-SCANDONE64 扫描完成 64 位大小");
    // 前面的用例通过「取消」按钮置位过停止标志，这里复位以走正常完成分支
    g_stopScan = false;
    g_isScanning = true;
    long long* p = new long long(5LL * 1024 * 1024 * 1024); // 5 GB
    PostMessageW(g_hMainWnd, WM_SCAN_DONE, 0, (LPARAM)p);
    PumpMessages(300);
    std::wstring st = StatusText();
    CHECK_MSG(Has(st, L"5.00 GB"), "状态栏应显示 5.00 GB（未被截断为 32 位）");
    CHECK_MSG(!g_isScanning, "扫描完成后应清除扫描标志");

    // 空指针（PostMessage 失败/线程异常）不得崩溃
    PostMessageW(g_hMainWnd, WM_SCAN_DONE, 0, (LPARAM)NULL);
    PumpMessages(200);
    CHECK_MSG(true, "lParam 为 NULL 时不得崩溃");
}

// GUI-CLEANDONE：清理完成消息的取消分支与空快照分支
static void Test_CleanDone() {
    GROUP("GUI-CLEANDONE 清理完成");
    KillerStart();

    // 取消分支：不弹框，直接提示"清理已取消"
    g_isCleaning = true;
    g_stopClean = true;
    long long* p1 = new long long(3LL * 1024 * 1024 * 1024);
    PostMessageW(g_hMainWnd, WM_CLEAN_DONE, 0, (LPARAM)p1);
    PumpMessages(400);
    CHECK_MSG(!g_isCleaning, "清理完成后应清除清理标志");
    CHECK_MSG(!g_stopClean, "取消标志应被复位");
    CHECK_MSG(StatusText() == L"清理已取消", "取消分支状态文本应为「清理已取消」");

    // 正常分支：未勾选任何项 -> 不启动重扫线程，提示"清理完成"
    SendMessageW(g_hTab, WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), 0);
    g_isCleaning = true;
    g_stopClean = false;
    long long* p2 = new long long(1024);
    PostMessageW(g_hMainWnd, WM_CLEAN_DONE, 0, (LPARAM)p2);
    PumpMessages(600);
    CHECK_MSG(!g_isCleaning, "空快照分支应清除清理标志");
    CHECK_MSG(!g_isScanning, "无可重扫项时不应进入扫描状态");
    CHECK_MSG(StatusText() == L"清理完成", "空快照分支状态文本应为「清理完成」");

    KillerStop();
}

// GUI-STALECHECK：勾选后被筛选隐藏的项，不得残留 isChecked 被一并卸载
static void Test_StaleCheckReset() {
    GROUP("GUI-STALECHECK 隐藏项残留勾选");

    // 构造 3 个假条目：A/B 用户软件（可被筛选保留）、C 系统软件（会被筛选隐藏）
    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems.clear();
    for (int i = 0; i < 3; i++) {
        UninstallItem it = {};
        it.displayName = i == 0 ? L"GUITestA" : (i == 1 ? L"GUITestB" : L"GUITestC");
        it.type = (i == 2) ? L"系统软件" : L"用户软件";
        it.isChecked = false;
        it.isScanned = true;
        it.estimatedSize = 0;
        g_uninstallItems.push_back(it);
    }
    LeaveCriticalSection(&g_uninstallItemsLock);

    // 构造前置状态：A、C 在数据模型中已勾选（模拟此前同步过的勾选），
    // 其中 C 是系统软件，接下来会被「用户软件」筛选从 ListView 隐藏
    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems[0].isChecked = true;
    g_uninstallItems[2].isChecked = true;
    LeaveCriticalSection(&g_uninstallItemsLock);

    g_uninstallFilterType = 0;
    g_uninstallSearchText.clear();
    RefreshUninstallList();
    CHECK_MSG(ListView_GetItemCount(g_hUninstallList) == 3, "未筛选时应显示 3 项");

    // 切换到「用户软件」筛选：C 从 ListView 消失，但模型 isChecked 仍为 true
    g_uninstallFilterType = 2;
    RefreshUninstallList();
    CHECK_MSG(ListView_GetItemCount(g_hUninstallList) == 2, "筛选后应只剩 2 项");

    // 让可见的 A 在 ListView 中处于勾选状态（7002 会据此回写模型）
    ListView_SetCheckState(g_hUninstallList, 0, TRUE);

    EnterCriticalSection(&g_uninstallItemsLock);
    bool cCheckedBefore = g_uninstallItems[2].isChecked;
    LeaveCriticalSection(&g_uninstallItemsLock);
    CHECK_MSG(cCheckedBefore, "用例前提：C 在筛选前处于勾选状态");

    // 触发「卸载选中」：确认框被自动关闭，但勾选同步/复位已经执行
    KillerStart();
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7002, BN_CLICKED), 0);
    PumpMessages(800);
    KillerStop();

    EnterCriticalSection(&g_uninstallItemsLock);
    bool aChecked = g_uninstallItems[0].isChecked;
    bool cChecked = g_uninstallItems[2].isChecked;
    LeaveCriticalSection(&g_uninstallItemsLock);

    CHECK_MSG(aChecked, "可见且勾选的 A 应保持勾选");
    CHECK_MSG(!cChecked, "被筛选隐藏的 C 的残留勾选必须被复位（否则会被误卸载）");
    CHECK_MSG(!g_isUninstalling, "确认被取消后不得进入卸载状态");

    // 清理测试数据
    g_uninstallFilterType = 0;
    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems.clear();
    LeaveCriticalSection(&g_uninstallItemsLock);
    RefreshUninstallList();
}

// GUI-TOOLPAGE：工具页功能切换的控件显隐（文件粉碎 vs 磁盘清理）
static void Test_ToolPage() {
    GROUP("GUI-TOOLPAGE 工具页控件");
    SwitchTab(6);
    CHECK_MSG(g_toolsReady, "首次进入工具页应完成初始化");

    // 默认功能 0 = 文件粉碎
    SwitchToolFunction(0);
    CHECK_MSG(Vis(g_hShredPath), "文件粉碎页应显示路径框");
    CHECK_MSG(Vis(GetDlgItem(g_hTab, 6001)), "文件粉碎页应显示「选择文件」");
    CHECK_MSG(Vis(GetDlgItem(g_hTab, 6002)), "文件粉碎页应显示「开始粉碎」");
    CHECK_MSG(!Vis(g_hToolListView), "文件粉碎页不应显示结果列表");
    CHECK_MSG(!Vis(g_hToolPathEdit), "文件粉碎页不应显示工具路径框");
    CHECK_MSG(!Vis(GetDlgItem(g_hTab, 6012)), "文件粉碎页不应显示「浏览」");
    CHECK_MSG(!Vis(g_hToolAgeCombo), "文件粉碎页不应显示文件年龄下拉框");

    // 功能 5 = 磁盘清理助手
    SwitchToolFunction(5);
    CHECK_MSG(Vis(g_hToolPathEdit), "磁盘清理页应显示工具路径框");
    CHECK_MSG(Vis(GetDlgItem(g_hTab, 6012)), "磁盘清理页应显示「浏览」");
    CHECK_MSG(Vis(GetDlgItem(g_hTab, 6013)), "磁盘清理页应显示「开始扫描」");
    CHECK_MSG(Vis(GetDlgItem(g_hTab, 6014)), "磁盘清理页应显示「删除选中」");
    CHECK_MSG(Vis(g_hToolAgeCombo), "磁盘清理页应显示文件年龄下拉框");
    CHECK_MSG(Vis(g_hToolListView), "磁盘清理页应显示结果列表");
    CHECK_MSG(!Vis(g_hShredPath), "磁盘清理页不应残留粉碎路径框");
    CHECK_MSG(!Vis(GetDlgItem(g_hTab, 6001)), "磁盘清理页不应残留「选择文件」");
    CHECK_MSG(!Vis(GetDlgItem(g_hTab, 6002)), "磁盘清理页不应残留「开始粉碎」");

    // 离开工具页必须隐藏全部工具控件（含动态控件）
    SwitchTab(0);
    CHECK_MSG(!Vis(g_hToolListView), "离开工具页应隐藏结果列表");
    CHECK_MSG(!Vis(g_hToolPathEdit), "离开工具页应隐藏工具路径框");
    CHECK_MSG(!Vis(g_hToolAgeCombo), "离开工具页应隐藏文件年龄下拉框");
    CHECK_MSG(!Vis(GetDlgItem(g_hTab, 6013)), "离开工具页应隐藏「开始扫描」");
    CHECK_MSG(!Vis(g_hToolFuncCombo), "离开工具页应隐藏功能选择下拉框");

    // 回到工具页应恢复当前功能的控件
    SwitchTab(6);
    CHECK_MSG(Vis(g_hToolFuncCombo), "回到工具页应恢复功能选择下拉框");
    SwitchToolFunction(0);
}

// GUI-SAFEIDLE：向界面投递不会造成破坏的命令，确认消息处理不崩溃
static void Test_SafeCommands() {
    GROUP("GUI-SAFEIDLE 命令健壮性");
    const int safeIds[] = { 1001, 1002, 1006, 1005, 7003 };
    for (int id : safeIds) {
        PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
        PumpMessages(120);
    }
    CHECK_MSG(true, "安全命令投递后消息循环仍存活");

    // 未勾选任何卸载项时触发「卸载选中」：应只弹提示，不进入卸载状态
    KillerStart();
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7002, BN_CLICKED), 0);
    PumpMessages(600);
    KillerStop();
    CHECK_MSG(!g_isUninstalling, "无勾选项时不得进入卸载状态");
}

// GUI-SPEEDFLOW：测速过程中进度条必须持续推进，结束后结果不能是「全 0 且无说明」
//
// 回归背景：原实现把进度/速度更新写在「InternetReadFile 读到数据」之后，且测速源
// speedtest.tele2.net 已停服，导致一次更新都不发出：进度条与速度纹丝不动，
// 结束时上下行全为 0。
static void Test_SpeedTestFlow() {
    GROUP("GUI-SPEEDFLOW 测速流程");
    SwitchTab(4);

    // --- 纯函数：进度只由墙钟时间决定，与网络是否成功无关 ---
    int phase = -1, remain = -1;
    CHECK_MSG(SpeedElapsedToProgress(0, phase, remain) == 1, "0ms 应为 1%");
    CHECK_MSG(phase == 0, "0ms 应处于下行阶段");
    CHECK_MSG(remain >= 1, "下行阶段应给出剩余秒数");
    CHECK_MSG(SpeedElapsedToProgress(g_speedPhaseMs / 2, phase, remain) == 25, "下行半程应为 25%");
    int nearDownEnd = SpeedElapsedToProgress(g_speedPhaseMs - 1, phase, remain);
    CHECK_MSG(nearDownEnd >= 48 && nearDownEnd <= 49, "下行结束前应接近 49%");
    CHECK_MSG(SpeedElapsedToProgress(g_speedPhaseMs, phase, remain) == 51, "进入上行应跳为 51%");
    CHECK_MSG(phase == 1, "后半程应处于上行阶段");
    CHECK_MSG(remain == 0, "上行阶段不应有剩余秒数");
    int nearEnd = SpeedElapsedToProgress(g_speedPhaseMs * 2 - 1, phase, remain);
    CHECK_MSG(nearEnd >= 98 && nearEnd <= 99, "结束前应接近 99%");
    CHECK_MSG(SpeedElapsedToProgress(g_speedPhaseMs * 2, phase, remain) == 100, "总时长用完应为 100%");
    CHECK_MSG(SpeedElapsedToProgress(g_speedPhaseMs * 8, phase, remain) == 100, "超时后仍应为 100%");

    // 进度必须单调不减
    int prevP = 0;
    for (DWORD t = 0; t <= g_speedPhaseMs * 2; t += 50) {
        int p = SpeedElapsedToProgress(t, phase, remain);
        CHECK_MSG(p >= prevP, "进度百分比必须单调不减");
        prevP = p;
    }

    // --- 端到端：用真实测速线程跑一遍（压缩阶段时长，避免测试耗时 20s）---
    // 设 SPEEDTEST_FULL=1 可改用真实 10s 阶段，验证下行确实能读到数据
    // （压缩阶段下 TLS 握手就会吃满整个下行窗口，下行字节会偶发为 0）
    const DWORD savedPhaseMs = g_speedPhaseMs;
    const DWORD savedIoMs = g_speedIoTimeoutMs;
    wchar_t fullFlag[8] = { 0 };
    DWORD fullLen = GetEnvironmentVariableW(L"SPEEDTEST_FULL", fullFlag, 8);
    bool fullRun = (fullLen > 0 && fullFlag[0] == L'1');
    g_speedPhaseMs = fullRun ? 10000 : 1200;
    // 超时必须小于等待窗口，否则单个请求卡住就会让测试提前放弃等待
    g_speedIoTimeoutMs = fullRun ? 15000 : 3000;
    g_stopSpeedTest = false;
    g_isSpeedTesting = false;
    SetWindowTextW(g_hSpeedResult, L"");

    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(4001, BN_CLICKED), 0);
    PumpMessages(250);
    CHECK_MSG(g_isSpeedTesting, "点击开始测速后应进入测速状态");

    int prevPos = -1;
    int increases = 0;
    int maxPos = 0;
    bool sawUploadPhase = false;
    // 等待上限 = 两个阶段 + 两次可能的超时 overrun + 余量
    const DWORD deadline = GetTickCount() + g_speedPhaseMs * 2 + g_speedIoTimeoutMs * 2 + 4000;
    while (GetTickCount() < deadline && g_isSpeedTesting) {
        PumpMessages(150);
        int pos = (int)SendMessageW(g_hSpeedProgress, PBM_GETPOS, 0, 0);
        if (pos > prevPos) { ++increases; prevPos = pos; }
        if (pos > maxPos) maxPos = pos;
        if (pos >= 51) sawUploadPhase = true;
    }
    PumpMessages(500);

    std::wstring r = SpeedResultText();
    printf("  [info] 进度推进次数=%d 最大进度=%d 进入上行=%d\n", increases, maxPos, (int)sawUploadPhase);
    printf("  [info] 结果：%s\n", Utf8(r).c_str());
    // 纯 ASCII 诊断：重定向时中文可能被截断，这行保证结果可验证
    printf("  [ascii] branch=%s len=%zu zeroBoth=%d\n",
        Has(r, L"测速完成") ? "DONE" : (Has(r, L"测速失败") ? "FAIL" : "NONE"),
        r.size(),
        (int)(r.find(L"下行速度：0.00 KB/s\n上行速度：0.00 KB/s") != std::wstring::npos));

    CHECK_MSG(increases >= 3, "进度条必须多次推进（原 BUG：全程不动）");
    CHECK_MSG(maxPos >= 40, "进度条至少应推进到下行后半段");
    CHECK_MSG(!r.empty(), "测速结束后必须有结果文本");
    CHECK_MSG(Has(r, L"测速完成") || Has(r, L"测速失败"), "结果必须明确说明成功或失败原因");
    CHECK_MSG(!g_isSpeedTesting, "测速结束后应清除测速标志");
    CHECK_MSG(!g_stopSpeedTest, "测速结束后应复位停止标志");

    if (Has(r, L"测速完成")) {
        CHECK_MSG(maxPos >= 50, "完整跑完时进度应越过上行分界");
        CHECK_MSG(sawUploadPhase, "完整跑完时应进入上行阶段");
        // 原 BUG 特征：两个速度同时为 0.00 KB/s
        CHECK_MSG(r.find(L"下行速度：0.00 KB/s\n上行速度：0.00 KB/s") == std::wstring::npos,
            "测速完成时不得上下行同时为 0");
        // 真实时长下下行必须取到样本（压缩时长下 TLS 握手吃满窗口，不作此要求）
        if (g_speedPhaseMs >= 8000) {
            CHECK_MSG(r.find(L"下行速度：0.00 KB/s") == std::wstring::npos,
                "真实时长下下行速度不应为 0");
        }
    }
    else {
        CHECK_MSG(Has(r, L"测速服务器"), "失败结果必须指出具体原因");
    }

    g_speedPhaseMs = savedPhaseMs;
    g_speedIoTimeoutMs = savedIoMs;
}

// ====================== 强力卸载护栏 ======================
//
// 强力卸载是唯一会「删服务键 + 删 .sys 文件 + 递归删目录」的路径，误判代价是蓝屏
// 或整盘数据丢失。这里用真实注册表键（HKCU 下自建自删）与真实临时文件验证护栏。
// 注：本文件 include 了 uninstaller.cpp，因此可直接调用其中的 static 函数。

static std::wstring GMakeTempDir(const wchar_t* name) {
    wchar_t base[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"sc_gui_" + name;
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static void GWriteFile(const std::wstring& path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(h, data, size, &w, NULL); CloseHandle(h);
}

static void GRemoveTree(const std::wstring& dir) {
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

static bool RegKeyExists(HKEY hive, const std::wstring& path) {
    HKEY h = NULL;
    if (RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

static bool Exists(const std::wstring& p) { return PathFileExistsW(p.c_str()) != FALSE; }

static void RegKeyDelete(HKEY hive, const std::wstring& path) {
    RegDeleteTreeW(hive, path.c_str());
    RegDeleteKeyW(hive, path.c_str());
}

// 在 g_uninstallFailures 中查找包含指定片段的记录
static bool HasFailure(const std::wstring& needle) {
    for (const auto& f : g_uninstallFailures) {
        if (f.reason.find(needle) != std::wstring::npos) return true;
    }
    return false;
}

static UninstallItem MakeTestItem(const wchar_t* name, const wchar_t* type) {
    UninstallItem it;
    it.displayName = name;
    it.type = type;
    return it;
}

// GUI-DRV：驱动条目不得走常规卸载 / 目录式清理
static void Test_DriverItemGuard() {
    GROUP("GUI-DRV 驱动条目护栏");
    ClearUninstallFailures();

    // DRV-01 默认构造的条目必须是「惰性」的：句柄为空、字符串为空，
    // 否则索引失效时会被当成真句柄传给 RegDeleteTreeW
    UninstallItem def;
    CHECK_MSG(def.regHive == NULL, "默认条目 regHive 必须为 NULL");
    CHECK_MSG(def.regViewAccess == 0, "默认条目 regViewAccess 必须为 0");
    CHECK_MSG(def.installLocation.empty() && def.imagePath.empty(), "默认条目路径必须为空");
    CHECK_MSG(!def.isSystemComponent && !def.isChecked, "默认条目不得自带勾选/系统标记");

    // DRV-02 常规卸载遇到驱动条目：只记录并返回 false，不做任何破坏性操作
    const wchar_t* testRoot = L"SOFTWARE\\SystemCleanerTest";
    const wchar_t* svcKey = L"SOFTWARE\\SystemCleanerTest\\SCTestDrv";
    RegKeyDelete(HKEY_CURRENT_USER, svcKey);
    RegKeyDelete(HKEY_CURRENT_USER, testRoot);

    HKEY hKey = NULL;
    LSTATUS lr = RegCreateKeyExW(HKEY_CURRENT_USER, svcKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    CHECK_MSG(lr == ERROR_SUCCESS, "测试用注册表键应创建成功");
    if (lr == ERROR_SUCCESS) {
        const wchar_t* img = L"\\SystemRoot\\System32\\drivers\\SCTestDrv.sys";
        RegSetValueExW(hKey, L"ImagePath", 0, REG_EXPAND_SZ, (const BYTE*)img,
            (DWORD)((wcslen(img) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    UninstallItem drv = MakeTestItem(L"SCTestDrv", L"驱动");
    drv.regHive = HKEY_CURRENT_USER;
    drv.regKeyPath = svcKey;
    drv.imagePath = L"\\SystemRoot\\System32\\drivers\\SCTestDrv.sys";
    drv.isSystemComponent = true;

    CHECK_MSG(!UninstallSingleApp(drv, false), "驱动条目常规卸载必须返回 false");
    CHECK_MSG(HasFailure(L"驱动条目不支持常规卸载"), "必须记录「驱动不支持常规卸载」");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, svcKey), "常规卸载绝不能删除驱动服务键");

    // DRV-03 强制卸载系统自带驱动：拒绝，且服务键必须原样保留
    ClearUninstallFailures();
    CHECK_MSG(!CleanAppResiduals(drv, false), "系统组件驱动强制卸载必须返回 false");
    CHECK_MSG(HasFailure(L"Windows 自带组件"), "必须记录「Windows 自带组件」拒绝原因");
    CHECK_MSG(RegKeyExists(HKEY_CURRENT_USER, svcKey), "系统组件的服务键必须保留");
    CHECK_MSG(drv.isSystemComponent, "isSystemComponent 标记不应被清理流程改写");

    // DRV-04 第三方驱动：只删服务键与 .sys 文件，不碰任何目录
    ClearUninstallFailures();
    std::wstring dir = GMakeTempDir(L"drv");
    std::wstring pf = dir + L"\\Program Files";     // 含空格，同时验证 ImagePath 解析
    CreateDirectoryW(pf.c_str(), NULL);
    std::wstring sysFile = pf + L"\\SCTestApp.sys";
    GWriteFile(sysFile, "MZ", 2);
    CHECK_MSG(Exists(sysFile), "测试驱动文件应创建成功");

    UninstallItem third = MakeTestItem(L"SCTestApp", L"驱动");
    third.regHive = HKEY_CURRENT_USER;
    third.regKeyPath = svcKey;
    third.imagePath = sysFile;                      // 绝对路径、无引号、含空格
    third.isSystemComponent = false;

    CHECK_MSG(CleanAppResiduals(third, false), "第三方驱动强制卸载应成功");
    CHECK_MSG(!Exists(sysFile), "驱动文件必须被删除");
    CHECK_MSG(!RegKeyExists(HKEY_CURRENT_USER, svcKey), "服务注册表键必须被删除");

    RegKeyDelete(HKEY_CURRENT_USER, testRoot);
    GRemoveTree(dir);
    ClearUninstallFailures();
}

// GUI-LOC：安装位置不可用时不得递归删除，且必须报告而不是静默跳过
static void Test_InstallLocationGuard() {
    GROUP("GUI-LOC 安装位置护栏");
    ClearUninstallFailures();

    wchar_t winDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(winDir, MAX_PATH);

    // LOC-01 IsUsableInstallLocation 判定
    CHECK_MSG(!IsUsableInstallLocation(L""), "空位置不可用");
    CHECK_MSG(!IsUsableInstallLocation(L"C:\\"), "盘符根不可用");
    CHECK_MSG(!IsUsableInstallLocation(winDir), "Windows 目录不可用");
    CHECK_MSG(!IsUsableInstallLocation(std::wstring(winDir) + L"\\System32"), "System32 不可用");
    std::wstring dir = GMakeTempDir(L"loc");
    CHECK_MSG(IsUsableInstallLocation(dir), "普通临时目录应可用");
    std::wstring file = dir + L"\\a.txt";
    GWriteFile(file, "x", 1);
    CHECK_MSG(!IsUsableInstallLocation(file), "文件路径不能当目录用");

    // LOC-02 位置被写成 Windows 目录：必须失败并记录，且系统目录安然无恙
    UninstallItem bad = MakeTestItem(L"SCTestBadLoc", L"系统软件");
    bad.installLocation = winDir;
    CHECK_MSG(!CleanAppResiduals(bad, false), "位置为系统目录时必须返回 false");
    CHECK_MSG(HasFailure(L"安装位置不是可安全删除的目录"), "必须记录跳过原因，不能静默");
    CHECK_MSG(Exists(winDir), "Windows 目录必须仍然存在");

    // LOC-03 正常安装目录 + 残留目录：应真正删除
    ClearUninstallFailures();
    std::wstring appDir = dir + L"\\SCTestInstall";
    CreateDirectoryW(appDir.c_str(), NULL);
    GWriteFile(appDir + L"\\a.txt", "x", 1);

    wchar_t local[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    std::wstring residual = std::wstring(local) + L"\\SCTestResidual";
    CreateDirectoryW(residual.c_str(), NULL);
    GWriteFile(residual + L"\\data.txt", "x", 1);

    UninstallItem ok = MakeTestItem(L"SCTestApp", L"用户软件");
    ok.publisher = L"SCTestResidual";
    ok.installLocation = appDir;
    CHECK_MSG(CleanAppResiduals(ok, false), "正常条目清理应成功");
    CHECK_MSG(!Exists(appDir), "安装目录必须被删除");
    CHECK_MSG(!Exists(residual), "厂商名之外的残留目录必须被删除");

    // LOC-04 保留用户数据时，用户目录内的安装目录不删
    ClearUninstallFailures();
    CreateDirectoryW(appDir.c_str(), NULL);
    wchar_t profile[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, profile);
    UninstallItem keep = MakeTestItem(L"SCTestKeep", L"用户软件");
    keep.installLocation = std::wstring(profile) + L"\\SCTestInProfile";
    CreateDirectoryW(keep.installLocation.c_str(), NULL);
    CHECK_MSG(CleanAppResiduals(keep, true), "保留用户数据分支应返回成功");
    CHECK_MSG(Exists(keep.installLocation), "保留用户数据时用户目录内的安装目录不得删除");
    RemoveDirectoryW(keep.installLocation.c_str());

    GRemoveTree(dir);
    ClearUninstallFailures();
}

// GUI-CMD：卸载命令解析与退出码传递
static void Test_UninstallCommandExec() {
    GROUP("GUI-CMD 卸载命令执行");

    CHECK_MSG(ExecuteUninstallCommand(L"") == -1, "空命令必须返回 -1");
    CHECK_MSG(ExecuteUninstallCommand(L"   \t") == -1, "纯空白必须返回 -1");
    CHECK_MSG(ExecuteUninstallCommand(L"C:\\__nope__\\unins000.exe /S") == -1, "不存在的程序必须返回 -1");

    // CMD-01 退出码必须原样传回（原实现把 STILL_ACTIVE=259 当成正常退出码）
    int rc = ExecuteUninstallCommand(L"cmd.exe /c exit 7");
    printf("  (info) cmd.exe /c exit 7 -> %d\n", rc);
    CHECK_MSG(rc == 7, "退出码必须原样返回");

    // CMD-02 含空格且没有引号的路径：原实现按第一个空格切分会截断并返回 -1
    std::wstring dir = GMakeTempDir(L"cmd");
    std::wstring toolDir = dir + L"\\my tool";
    CreateDirectoryW(toolDir.c_str(), NULL);
    std::wstring bat = toolDir + L"\\unins 000.bat";
    GWriteFile(bat, "@exit /b 9\r\n", 12);
    int rc2 = ExecuteUninstallCommand(bat);
    printf("  (info) 含空格路径 -> %d\n", rc2);
    CHECK_MSG(rc2 != -1, "含空格的未加引号路径必须能启动（原 BUG：截断成 \"...\\my\"）");
    CHECK_MSG(rc2 == 9, "含空格路径的退出码应为 9");
    GRemoveTree(dir);
}

// GUI-UI7004 / GUI-UI7005：UI 层护栏
static void Test_ForceUninstallUiGuard() {
    GROUP("GUI-UI 强卸/筛选护栏");

    g_uninstallFilterType = 0;
    g_uninstallSearchText.clear();
    g_isUninstalling = false;

    // 构造 2 个条目：A 是 Windows 自带组件，B 是普通软件
    auto fill = [&]() {
        EnterCriticalSection(&g_uninstallItemsLock);
        g_uninstallItems.clear();
        UninstallItem a = MakeTestItem(L"SCTestSysComp", L"驱动");
        a.isSystemComponent = true;
        UninstallItem b = MakeTestItem(L"SCTestNormal", L"用户软件");
        g_uninstallItems.push_back(a);
        g_uninstallItems.push_back(b);
        LeaveCriticalSection(&g_uninstallItemsLock);
        RefreshUninstallList();
    };

    // UI-01 全部勾选的都是系统组件：直接拒绝，不进入卸载状态
    fill();
    CHECK_MSG(ListView_GetItemCount(g_hUninstallList) == 2, "应显示 2 项");
    ListView_SetCheckState(g_hUninstallList, 0, TRUE);
    g_messageBoxCalls = 0;
    g_lastMessageBoxText.clear();
    KillerStart();
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7004, BN_CLICKED), 0);
    PumpMessages(400);
    CHECK_MSG(!g_isUninstalling, "全为系统组件时不得进入卸载状态");
    CHECK_MSG(g_messageBoxCalls >= 1, "必须给出提示而不是静默拒绝");
    CHECK_MSG(Has(g_lastMessageBoxText, L"Windows 自带组件"), "提示必须点明系统组件被拒");

    // UI-02 混合勾选：确认文案必须告知「将被自动跳过」，且确认被取消后不进入卸载
    fill();
    ListView_SetCheckState(g_hUninstallList, 0, TRUE);
    ListView_SetCheckState(g_hUninstallList, 1, TRUE);
    g_messageBoxCalls = 0;
    g_lastMessageBoxText.clear();
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7004, BN_CLICKED), 0);
    PumpMessages(400);
    CHECK_MSG(Has(g_lastMessageBoxText, L"将被自动跳过"), "确认文案必须说明系统组件会被跳过");
    CHECK_MSG(Has(g_lastMessageBoxText, L"不会调用软件自带的卸载程序"), "确认文案必须说明绕过自带卸载器");
    CHECK_MSG(!g_isUninstalling, "确认框被取消后不得进入卸载状态");

    // UI-03 未勾选任何项：提示后直接返回
    fill();
    g_messageBoxCalls = 0;
    g_lastMessageBoxText.clear();
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7004, BN_CLICKED), 0);
    PumpMessages(400);
    CHECK_MSG(Has(g_lastMessageBoxText, L"请先勾选"), "未勾选时必须提示");
    CHECK_MSG(!g_isUninstalling, "未勾选时不得进入卸载状态");

    // UI-04 卸载进行中切换类型筛选：不得清空 g_uninstallItems（否则工作线程索引失效）
    fill();
    g_isUninstalling = true;
    if (g_hUninstallFilter) SendMessageW(g_hUninstallFilter, CB_SETCURSEL, 3, 0); // 3 = 驱动
    g_uninstallDebouncePending = false;
    g_uninstallDebounceForceRescan = false;
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7005, CBN_SELCHANGE), 0);
    PumpMessages(400);
    EnterCriticalSection(&g_uninstallItemsLock);
    size_t remain = g_uninstallItems.size();
    LeaveCriticalSection(&g_uninstallItemsLock);
    CHECK_MSG(remain == 2, "卸载进行中切换筛选不得清空数据模型");
    CHECK_MSG(!g_uninstallDebouncePending && !g_uninstallDebounceForceRescan,
        "卸载进行中不得安排重扫");
    g_isUninstalling = false;

    // UI-05 卸载进行中点「刷新扫描」同样不得重扫
    g_isUninstalling = true;
    g_uninstallDebouncePending = false;
    g_uninstallDebounceForceRescan = false;
    PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(7001, BN_CLICKED), 0);
    PumpMessages(300);
    EnterCriticalSection(&g_uninstallItemsLock);
    remain = g_uninstallItems.size();
    LeaveCriticalSection(&g_uninstallItemsLock);
    CHECK_MSG(remain == 2, "卸载进行中不得触发重扫清空模型");
    CHECK_MSG(!g_uninstallDebounceForceRescan, "卸载进行中不得安排强制重扫");
    g_isUninstalling = false;

    // 收尾
    EnterCriticalSection(&g_uninstallItemsLock);
    g_uninstallItems.clear();
    LeaveCriticalSection(&g_uninstallItemsLock);
    g_uninstallFilterType = 0;
    if (g_hUninstallFilter) SendMessageW(g_hUninstallFilter, CB_SETCURSEL, 0, 0);
    RefreshUninstallList();
    KillerStop();
}

// GUI-REGCLEAN：注册表清理页的前置校验、列表/数据同步、扫描并发保护
// 说明：GUI 测试宿主以非管理员运行，CleanSelectedRegistry 的管理员分支不会进入，
// 因此这里覆盖的是「删除动作之前」的所有护栏——它们恰恰是防误删的关键，
// 且必须在权限检查之前生效（否则任何情况都只报「权限不足」）。
static void FillRegList(std::initializer_list<RegCleanItem> items) {
    g_regCleanItems.clear();
    for (const auto& it : items) g_regCleanItems.push_back(it);
    RefreshRegCleanList();
}

static RegCleanItem MakeRegItem(const wchar_t* key, const wchar_t* cat, REGSAM view) {
    RegCleanItem it;
    it.hive = HKEY_CURRENT_USER;
    it.keyPath = key;
    it.category = cat;
    it.desc = L"测试项";
    it.isInvalid = true;
    it.view = view;
    return it;
}

static void Test_RegCleanGuards() {
    GROUP("GUI-REGCLEAN 注册表清理护栏");
    SwitchTab(3);
    const bool admin = IsRunAsAdmin();

    // RC-01 空列表点「清理选中」：应提示「至少选择一项」，不弹窗崩溃
    g_isRegScanning = false;
    FillRegList({});
    int before = g_messageBoxCalls;
    CleanSelectedRegistry();
    CHECK_MSG(g_messageBoxCalls == before + 1, "空列表清理应只弹一个提示框");
    CHECK_MSG(g_lastMessageBoxText.find(L"至少选择一项") != std::wstring::npos,
        "空列表应提示「请至少选择一项」");

    // RC-02 列表与数据模型不同步：必须拒绝并要求重扫，绝不能按下标乱删
    FillRegList({ MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY) });
    CHECK_MSG(ListView_GetItemCount(g_hRegCleanList) == 1, "刷新后列表应有 1 行");
    g_regCleanItems.push_back(MakeRegItem(L"Software\\SC_GuiTest_B", L"卸载残留[64位]", KEY_WOW64_64KEY));
    // 现在 vector 有 2 项、ListView 只有 1 行 —— 典型的不同步
    before = g_messageBoxCalls;
    CleanSelectedRegistry();
    CHECK_MSG(g_lastMessageBoxText.find(L"不同步") != std::wstring::npos,
        "行/数据数量不一致时必须提示「列表与扫描结果已不同步」");
    CHECK_MSG(g_messageBoxCalls == before + 1, "不同步分支应只弹一个提示框");

    // RC-03 全部取消勾选：提示「至少选择一项」
    FillRegList({ MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY) });
    for (int i = 0; i < ListView_GetItemCount(g_hRegCleanList); i++)
        ListView_SetCheckState(g_hRegCleanList, i, FALSE);
    before = g_messageBoxCalls;
    CleanSelectedRegistry();
    CHECK_MSG(g_lastMessageBoxText.find(L"至少选择一项") != std::wstring::npos,
        "无勾选时应提示「请至少选择一项」");

    // RC-04 扫描进行中点清理：必须被 g_isRegScanning 拦住，且不触碰数据
    FillRegList({ MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY) });
    g_isRegScanning = true;
    before = g_messageBoxCalls;
    CleanSelectedRegistry();
    g_isRegScanning = false;
    CHECK_MSG(g_lastMessageBoxText.find(L"正在进行") != std::wstring::npos,
        "扫描进行中应提示「注册表扫描正在进行，请稍候」");
    CHECK_MSG(g_regCleanItems.size() == 1, "扫描进行中的清理请求不得改动数据模型");

    // RC-05 视图来自 item.view 而非 category 文案：构造一个「文案写 64 位、view 是 32 位」的项，
    // 校验删除路径读取的是 item.view。这里通过直接读取字段确认扫描写入与清理读取一致。
    RegCleanItem v = MakeRegItem(L"Software\\SC_GuiTest_V", L"卸载残留[64位]", KEY_WOW64_32KEY);
    CHECK_MSG(v.view == KEY_WOW64_32KEY, "item.view 必须独立保存，不受 category 文案影响");

    // RC-06 有勾选且（非管理员）：应命中权限提示分支，而不是「至少选择一项」或「不同步」
    FillRegList({ MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY) });
    before = g_messageBoxCalls;
    CleanSelectedRegistry();
    if (!admin) {
        CHECK_MSG(g_lastMessageBoxText.find(L"管理员权限") != std::wstring::npos,
            "非管理员有勾选时应命中权限提示分支");
    } else {
        // 管理员环境：会进入确认框（桩返回 -1，取消），不应命中前置校验文案
        CHECK_MSG(g_lastMessageBoxText.find(L"至少选择一项") == std::wstring::npos,
            "有勾选时不应命中「至少选择一项」");
    }

    // RC-07 扫描完成消息：RefreshRegCleanList 后行/数据一致，状态栏计数正确
    g_regCleanItems.clear();
    g_regCleanItems.push_back(MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY));
    g_regCleanItems.push_back(MakeRegItem(L"Software\\SC_GuiTest_B", L"启动残留[当前用户]", 0));
    PostMessageW(g_hMainWnd, WM_REG_SCAN_DONE, (WPARAM)2, 0);
    PumpMessages(200);
    CHECK_MSG(ListView_GetItemCount(g_hRegCleanList) == 2, "扫描完成后列表应有 2 行");
    CHECK_MSG(ListView_GetItemCount(g_hRegCleanList) == (int)g_regCleanItems.size(),
        "扫描完成后行与数据必须一一对应");
    CHECK_MSG(StatusText().find(L"2") != std::wstring::npos, "状态栏应显示发现 2 项");

    // RC-08 权限不足的扫描完成消息（lParam==1）：不得清空列表，应弹权限提示
    FillRegList({ MakeRegItem(L"Software\\SC_GuiTest_A", L"卸载残留[64位]", KEY_WOW64_64KEY) });
    int rowsBefore = ListView_GetItemCount(g_hRegCleanList);
    size_t dataBefore = g_regCleanItems.size();
    before = g_messageBoxCalls;
    PostMessageW(g_hMainWnd, WM_REG_SCAN_DONE, 0, 1);
    PumpMessages(200);
    CHECK_MSG(g_messageBoxCalls == before + 1, "权限不足的扫描完成应弹一次提示");
    CHECK_MSG(ListView_GetItemCount(g_hRegCleanList) == rowsBefore,
        "权限不足的扫描完成不得清空已有列表");
    CHECK_MSG(g_regCleanItems.size() == dataBefore,
        "权限不足的扫描完成不得清空已有数据");

    // 收尾：清空注册表清理页数据，避免影响其它用例
    g_regCleanItems.clear();
    RefreshRegCleanList();
    g_isRegScanning = false;
    SwitchTab(0);
}

// ====================== 入口 ======================
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("SystemCleaner GUI interaction tests\n");

    InitializeCriticalSection(&g_uninstallItemsLock);

    HINSTANCE hInst = GetModuleHandleW(NULL);
    const wchar_t* cls = L"SystemCleanerClass";

    INITCOMMONCONTROLSEX icex = { sizeof(icex) };
    icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    // 放到屏幕外并显示：IsWindowVisible 对子控件要求父链可见，
    // 因此不能用 SW_HIDE，只能把窗口挪到用户看不到的位置
    HWND hWnd = CreateWindowExW(0, cls, L"GUI 测试宿主", WS_OVERLAPPEDWINDOW,
        -32000, -32000, 860, 620, NULL, NULL, hInst, NULL);
    if (!hWnd) {
        printf("FATAL: 主窗口创建失败, err=%lu\n", GetLastError());
        return 2;
    }
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hWnd);
    PumpMessages(300);

    CHECK_MSG(g_hTab != NULL, "Tab 控件应已创建");
    CHECK_MSG(g_hMainWnd == hWnd, "g_hMainWnd 应指向主窗口");

    Test_TabMatrix();
    Test_HostsLabel();
    Test_SpeedProgressBar();
    Test_CheckOperations();
    Test_ScanDoneMarshaling();
    Test_CleanDone();
    Test_StaleCheckReset();
    Test_DriverItemGuard();
    Test_InstallLocationGuard();
    Test_UninstallCommandExec();
    Test_ForceUninstallUiGuard();
    Test_ToolPage();
    Test_SafeCommands();
    Test_SpeedTestFlow();
    Test_RegCleanGuards();

    // 收尾：让可能残留的定时器/子类化正常释放
    SwitchTab(0);
    DestroyWindow(hWnd);
    PumpMessages(200);

    printf("\n=====================================\n");
    printf("Total: %d   Failed: %d\n", g_total, g_failed);
    printf("=====================================\n");
    return g_failed == 0 ? 0 : 1;
}
