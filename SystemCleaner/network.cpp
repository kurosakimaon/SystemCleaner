// Module: network.cpp - Network speed testing, hosts file management, and repair

#include "common.h"

// hosts 路径必须基于实际系统盘，硬编码 C:\Windows 在系统装在其他盘时会读错文件
static wchar_t g_hostsPath[MAX_PATH] = { 0 };
const wchar_t* HOSTS_PATH = g_hostsPath;

static void InitHostsPath() {
    if (g_hostsPath[0] == 0) {
        wchar_t sysDir[MAX_PATH] = { 0 };
        if (GetWindowsDirectoryW(sysDir, MAX_PATH) == 0)
            wcscpy_s(sysDir, L"C:\\Windows");
        swprintf_s(g_hostsPath, L"%s\\System32\\drivers\\etc\\hosts", sysDir);
    }
}

// PostMessage 失败（窗口已销毁等）时堆对象会泄漏，统一封装以便释放
static void PostSpeedUpdate(HWND hWnd, WPARAM progress, SpeedUpdateInfo* info) {
    if (!PostMessageW(hWnd, WM_SPEED_UPDATE, progress, (LPARAM)info)) {
        delete info;
    }
}

static void PostSpeedDone(HWND hWnd, WPARAM code, std::wstring* result) {
    if (!PostMessageW(hWnd, WM_SPEED_DONE, code, (LPARAM)result)) {
        delete result;
    }
}

// ===== 测速参数与测速源 =====
// 单阶段（下行 / 上行）时长。做成运行时变量而不是常量，
// 让 GUI 回归测试能把完整流程压缩到几秒跑完（默认仍为 10s）。
DWORD g_speedPhaseMs = 10000;
// 连接/收发超时。做成变量：单个请求卡住时，阶段 overrun 必须可控，
// 测试也需要把超时压缩到比阶段时长更短。
DWORD g_speedIoTimeoutMs = 15000;
static const DWORD SPEED_TICK_MS = 200;           // 进度与速度刷新节拍
static const DWORD SPEED_CHUNK_DOWN = 64 * 1024;  // 下行单次读取
static const DWORD SPEED_CHUNK_UP = 1024 * 1024;  // 上行单轮提交量

struct SpeedSource {
    const wchar_t* host;
    INTERNET_PORT port;
    const wchar_t* path;
};

// 下行测速源按顺序回退：原实现只有 speedtest.tele2.net 一个源，该站点已停服，
// 导致 InternetConnect 成功但每个 HTTP 请求都失败，测速全程 0 且没有任何提示。
// 注意 Cloudflare 的 bytes 参数有上限，256MB 会被判 403，实测 32MB 正常返回 200。
static const SpeedSource g_downSources[] = {
    { L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, L"/__down?bytes=33554432" },
    { L"proof.ovh.net",        INTERNET_DEFAULT_HTTPS_PORT, L"/files/100Mb.dat" },
    { L"speedtest.tele2.net",  INTERNET_DEFAULT_HTTP_PORT,  L"/10MB.zip" },
};
static const SpeedSource g_upSource = { L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, L"/__up" };

// [0]=下行 [1]=上行。测速线程累加、ticker 线程读取，64 位计数必须走 Interlocked，
// 否则 32 位进程下 ticker 可能读到「半更新」的字节数而算出离谱速度。
static volatile LONG64 g_speedBytes[2] = { 0, 0 };

static void SpeedAddBytes(int phase, long long bytes) {
    InterlockedExchangeAdd64(&g_speedBytes[phase], (LONG64)bytes);
}

static long long SpeedGetBytes(int phase) {
    return (long long)InterlockedCompareExchange64(&g_speedBytes[phase], 0, 0);
}

/**
 * @brief 向测速源发起一次请求（GET 下载 / POST 上传），失败返回 NULL
 * @param hOutConn 成功时回填连接句柄，由调用方负责关闭
 */
static HINTERNET SpeedOpenRequest(HINTERNET hInternet, const SpeedSource& src, const wchar_t* verb,
    const void* body, DWORD bodyLen, HINTERNET* hOutConn) {
    *hOutConn = NULL;

    HINTERNET hConn = InternetConnectW(hInternet, src.host, src.port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) return NULL;

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES;
    if (src.port == INTERNET_DEFAULT_HTTPS_PORT) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hRequest = HttpOpenRequestW(hConn, verb, src.path, NULL, NULL, NULL, flags, 0);
    if (!hRequest) {
        InternetCloseHandle(hConn);
        return NULL;
    }

    DWORD timeout = g_speedIoTimeoutMs > 0 ? g_speedIoTimeoutMs : 15000;
    InternetSetOptionW(hRequest, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hRequest, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hRequest, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hRequest, INTERNET_OPTION_DATA_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    if (body && bodyLen > 0) {
        HttpAddRequestHeadersW(hRequest, L"Content-Type: application/octet-stream\r\n", -1, HTTP_ADDREQ_FLAG_ADD);
    }

    // Content-Length 交给 WinINet 依据 lpOptional 长度自动生成，
    // 手工再加一遍会出现重复头，部分服务端会直接判 400。
    if (!HttpSendRequestW(hRequest, NULL, 0, (LPVOID)body, bodyLen)) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConn);
        return NULL;
    }

    // 必须校验状态码：HttpSendRequestW 对 4xx/5xx 同样返回 TRUE，
    // 若不加判断，一个持续返回 403 的源会被当成「连通成功」，
    // 最终又退回到用户看到的「测速完成但速度为 0」。
    DWORD status = 0, statusLen = sizeof(status);
    if (HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
        &status, &statusLen, NULL) && status >= 400) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConn);
        return NULL;
    }

    *hOutConn = hConn;
    return hRequest;
}

// ticker 线程上下文：堆分配，ticker 退出时自行回收，
// 避免主线程等待超时后访问已失效的栈对象
struct SpeedTickerCtx {
    HWND hWnd;
    HANDLE hExit;   // 置位后 ticker 立即退出，不必等满一个节拍
};

// 结束并回收 ticker：事件置位后线程最多再跑一个节拍就会自行退出，
// 这里等待其真正结束，保证之后不会再有迟到的进度消息把进度条改回去
static void StopSpeedTicker(HANDLE hTicker, HANDLE hExitEvent) {
    if (hExitEvent) SetEvent(hExitEvent);
    if (hTicker) {
        WaitForSingleObject(hTicker, INFINITE);
        CloseHandle(hTicker);
    }
    if (hExitEvent) CloseHandle(hExitEvent);
}

/**
 * @brief 由测速总耗时推算进度百分比、当前阶段与剩余秒数
 *
 * 抽成纯函数便于单测：进度只取决于墙钟时间，与网络读取是否成功无关，
 * 这是「网络卡住时进度条也会动」的根本保证。
 * @param elapsedTotal 测速开始至今的毫秒数
 * @param phase 输出：0=下行，1=上行
 * @param remainSec 输出：下行阶段剩余秒数（上行阶段为 0）
 * @return 进度百分比：下行 1..49，上行 51..99，总时长用完后 100
 */
int SpeedElapsedToProgress(DWORD elapsedTotal, int& phase, int& remainSec) {
    const DWORD phaseMs = g_speedPhaseMs > 0 ? g_speedPhaseMs : 10000;
    const DWORD totalMs = phaseMs * 2;
    phase = 0;
    remainSec = 0;
    if (elapsedTotal >= totalMs) return 100;

    if (elapsedTotal < phaseMs) {
        phase = 0;
        int progress = 1 + (int)((elapsedTotal * 48) / phaseMs); // 1..49
        if (progress > 49) progress = 49;
        remainSec = (int)((phaseMs - elapsedTotal + 999) / 1000);
        return progress;
    }

    phase = 1;
    DWORD phaseElapsed = elapsedTotal - phaseMs;
    int progress = 51 + (int)((phaseElapsed * 48) / phaseMs); // 51..99
    if (progress > 99) progress = 99;
    return progress;
}

/**
 * @brief 测速 UI 刷新线程：只按墙钟时间推进进度、按已传输字节算速度
 *
 * 关键修复：原实现的进度/速度更新写在「InternetReadFile 成功读到数据」之后，
 * 一旦测速源不可达或链路卡住，循环里一次 PostMessage 都不会发出，
 * 表现就是进度条与速度纹丝不动、结束时结果全为 0。UI 节奏与网络 IO 解耦后，
 * 网络异常时进度照常推进，速度如实显示 0，用户能立刻看出是网络问题而非程序卡死。
 */
static DWORD WINAPI SpeedTickerThreadProc(LPVOID lpParam) {
    SpeedTickerCtx* ctx = (SpeedTickerCtx*)lpParam;
    HWND hWnd = ctx->hWnd;
    const DWORD phaseMs = g_speedPhaseMs > 0 ? g_speedPhaseMs : 10000;
    DWORD start = GetTickCount();

    // ticker 不按固定总时长退出，而是活到测速线程显式结束：
    // 单个请求因超时 overrun 时，进度条仍保持可见状态而不是提前冻结在错误位置。
    // 100% 只由测速线程在真正结束时发出，这里最高只到 99%。
    for (;;) {
        if (WaitForSingleObject(ctx->hExit, SPEED_TICK_MS) == WAIT_OBJECT_0) break;
        if (g_stopSpeedTest) break;

        DWORD elapsedTotal = GetTickCount() - start;

        int phase = 0;
        int remainSec = 0;
        int progress = SpeedElapsedToProgress(elapsedTotal, phase, remainSec);
        if (progress >= 100) {
            // 已超出计划总时长（某个请求因超时 overrun）：仍按上行阶段展示，
            // 进度停在 99%，等测速线程发出真正的 100%
            progress = 99;
            phase = 1;
            remainSec = 0;
        }
        DWORD phaseElapsed = (phase == 0) ? elapsedTotal : (elapsedTotal - phaseMs);
        if (phaseElapsed > elapsedTotal) phaseElapsed = elapsedTotal;

        long long bytes = SpeedGetBytes(phase);
        long long speed = bytes * 1000 / (phaseElapsed > 0 ? (long long)phaseElapsed : 1);

        SpeedUpdateInfo* info = new (std::nothrow) SpeedUpdateInfo();
        if (info) {
            info->progress = progress;
            info->bytes = bytes;
            info->speed = speed;
            info->remainSec = remainSec;
            info->phase = phase;
            PostSpeedUpdate(hWnd, progress, info);
        }
        else {
            PostMessageW(hWnd, WM_SPEED_UPDATE, progress, 0);
        }
    }

    delete ctx; // 必须是本函数最后一笔资源操作
    return 0;
}

/**
 * @brief 网络测速线程过程函数
 */
DWORD WINAPI SpeedTestThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;

    InterlockedExchange64(&g_speedBytes[0], 0);
    InterlockedExchange64(&g_speedBytes[1], 0);

    HINTERNET hInternet = InternetOpenW(L"SystemCleaner SpeedTest", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        std::wstring* errResult = new std::wstring(L"网络初始化失败，请检查网络连接后重试。");
        PostSpeedDone(hWnd, 0, errResult);
        return 1;
    }

    DWORD connTimeout = g_speedIoTimeoutMs > 0 ? g_speedIoTimeoutMs : 15000;
    InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &connTimeout, sizeof(connTimeout));

    // 启动 UI 刷新线程：从这一刻起进度条就一定在动
    HANDLE hExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE hTicker = NULL;
    SpeedTickerCtx* tickerCtx = new (std::nothrow) SpeedTickerCtx();
    if (tickerCtx) {
        tickerCtx->hWnd = hWnd;
        tickerCtx->hExit = hExitEvent;
        hTicker = CreateThread(NULL, 0, SpeedTickerThreadProc, tickerCtx, 0, NULL);
        // 线程未能启动时上下文由本函数回收，否则归 ticker 线程
        if (!hTicker) {
            delete tickerCtx;
            tickerCtx = NULL;
        }
    }
    if (!hTicker && hExitEvent) {
        CloseHandle(hExitEvent);
        hExitEvent = NULL;
    }

    long long avgDownSpeed = 0;
    long long avgUpSpeed = 0;
    DWORD connectTime = 0;
    bool downOk = false;
    bool upOk = false;

    // ---------- 下行阶段 ----------
    const DWORD phaseMs = g_speedPhaseMs > 0 ? g_speedPhaseMs : 10000;
    DWORD downStart = GetTickCount();
    {
        BYTE* buffer = new (std::nothrow) BYTE[SPEED_CHUNK_DOWN];
        size_t sourceCount = sizeof(g_downSources) / sizeof(g_downSources[0]);
        size_t srcIdx = 0;

        while (buffer && !g_stopSpeedTest && (GetTickCount() - downStart) < phaseMs) {
            HINTERNET hConn = NULL;
            HINTERNET hRequest = SpeedOpenRequest(hInternet, g_downSources[srcIdx], L"GET", NULL, 0, &hConn);
            if (!hRequest) {
                srcIdx++;
                // 已经连通过至少一个源时不能因为末尾某个源挂掉就提前收工，
                // 否则测速会在时限前结束；回到首选源继续，循环仍受时限保护。
                if (srcIdx >= sourceCount) {
                    if (!downOk) break; // 一个源都没连通
                    srcIdx = 0;
                }
                continue;
            }

            if (connectTime == 0) connectTime = GetTickCount() - downStart;
            downOk = true;

            DWORD bytesRead = 0;
            while (!g_stopSpeedTest && (GetTickCount() - downStart) < phaseMs) {
                BOOL ok = InternetReadFile(hRequest, buffer, SPEED_CHUNK_DOWN, &bytesRead);
                if (!ok || bytesRead == 0) break; // 文件读完或链路中断，换下一个源继续
                SpeedAddBytes(0, (long long)bytesRead);
            }

            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConn);

            if ((GetTickCount() - downStart) >= phaseMs) break;
            srcIdx++;
            if (srcIdx >= sourceCount) srcIdx = 0; // 未到时限则回到首选源续传
        }
        delete[] buffer;
    }

    DWORD downTime = GetTickCount() - downStart;
    long long downBytes = SpeedGetBytes(0);
    if (downTime > 0 && downBytes > 0)
        avgDownSpeed = downBytes * 1000 / downTime;

    // 下行一个源都没连通就没必要再跑上行：直接给出明确原因，
    // 而不是让用户看到「测速完成 / 下行 0 / 上行 0」这种像程序坏掉的结果。
    if (!downOk && !g_stopSpeedTest) {
        StopSpeedTicker(hTicker, hExitEvent);
        InternetCloseHandle(hInternet);

        std::wstring* errResult = new std::wstring(
            L"测速失败：无法连接到测速服务器。\n"
            L"请检查网络连接、代理或防火墙设置后重试。");
        PostSpeedDone(hWnd, 0, errResult);
        return 1;
    }

    // 下行阶段结束，先把进度推到阶段分界，避免 ticker 退出后停在 49%
    // （已停止时不推，否则进度条会在隐藏前闪到满格）
    if (!g_stopSpeedTest) PostMessageW(hWnd, WM_SPEED_UPDATE, 50, 0);

    // ---------- 上行阶段 ----------
    DWORD upStart = GetTickCount();
    {
        BYTE* upData = new (std::nothrow) BYTE[SPEED_CHUNK_UP];
        if (upData) {
            DWORD seed = (DWORD)GetTickCount();
            for (DWORD i = 0; i < SPEED_CHUNK_UP; i++) {
                seed = seed * 1103515245 + 12345;
                upData[i] = (BYTE)((seed >> 16) & 0xFF);
            }
        }

        while (upData && !g_stopSpeedTest && (GetTickCount() - upStart) < phaseMs) {
            HINTERNET hConn = NULL;
            HINTERNET hRequest = SpeedOpenRequest(hInternet, g_upSource, L"POST", upData, SPEED_CHUNK_UP, &hConn);
            if (!hRequest) break; // 上行源不可用，保留下行结果

            upOk = true;
            SpeedAddBytes(1, (long long)SPEED_CHUNK_UP);

            // 读完响应以便复用连接；上限防止服务端不回数据时死等
            BYTE readBuf[512];
            DWORD read = 0;
            for (int guard = 0; guard < 64; guard++) {
                if (!InternetReadFile(hRequest, readBuf, sizeof(readBuf), &read)) break;
                if (read == 0) break;
            }

            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConn);
        }
        delete[] upData;
    }

    DWORD upTime = GetTickCount() - upStart;
    long long upBytes = SpeedGetBytes(1);
    if (upTime > 0 && upBytes > 0)
        avgUpSpeed = upBytes * 1000 / upTime;

    // 停止 ticker 并等待其退出，避免测速结束后再收到迟到的更新把进度条改回去
    StopSpeedTicker(hTicker, hExitEvent);

    InternetCloseHandle(hInternet);

    std::wstring result;
    if (g_stopSpeedTest) result = L"测速已停止\n";
    else result = L"测速完成\n";
    result += L"延迟：" + std::to_wstring(connectTime) + L" ms\n";
    result += L"下行速度：" + FormatSpeed(avgDownSpeed);
    if (!downOk) result += L"（未获取到有效样本）";
    result += L"\n上行速度：" + FormatSpeed(avgUpSpeed);
    if (!upOk) result += L"（上行测速源不可用）";

    if (!g_stopSpeedTest) PostMessageW(hWnd, WM_SPEED_UPDATE, 100, 0);

    std::wstring* resultHeap = new std::wstring(result);
    PostSpeedDone(hWnd, 1, resultHeap);
    return 0;
}

/**
 * @brief 在覆盖 Hosts 文件之前先备份当前内容
 */
std::wstring BackupHostsFile() {
    InitHostsPath();
    wchar_t timeBuf[64] = { 0 };
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(timeBuf, L"hosts_backup_%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // 备份必须与 hosts 同目录：硬编码 C:\Windows 在系统盘非 C 时会把备份写到不存在的路径
    std::wstring etcDir = HOSTS_PATH;
    size_t sep = etcDir.find_last_of(L"\\/");
    if (sep != std::wstring::npos) etcDir = etcDir.substr(0, sep);
    std::wstring backupPath = etcDir + L"\\" + timeBuf;

    HANDLE hRead = CreateFileW(HOSTS_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hRead == INVALID_HANDLE_VALUE) return L"";

    DWORD size = GetFileSize(hRead, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(hRead); return L""; }

    char* buf = new char[size + 1];
    DWORD read = 0;
    if (!ReadFile(hRead, buf, size, &read, NULL)) {
        delete[] buf;
        CloseHandle(hRead);
        return L"";
    }
    buf[read] = '\0';
    CloseHandle(hRead);

    HANDLE hWrite = CreateFileW(backupPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hWrite == INVALID_HANDLE_VALUE) {
        delete[] buf;
        return L"";
    }

    DWORD written = 0;
    WriteFile(hWrite, buf, read, &written, NULL);
    CloseHandle(hWrite);
    delete[] buf;

    return backupPath;
}

/**
 * @brief 一键网络修复（在工作线程中执行，避免长时间阻塞 UI）
 */
void NetworkRepairAll() {
    InitHostsPath();
    SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)L"正在修复网络...");

    ExecuteCmd(L"ipconfig /release");
    ExecuteCmd(L"ipconfig /renew");
    ExecuteCmd(L"ipconfig /flushdns");
    ExecuteCmd(L"netsh winsock reset");
    ExecuteCmd(L"netsh int ip reset");
    ExecuteCmd(L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\" /v ProxyEnable /f");
    ExecuteCmd(L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\" /v ProxyServer /f");

    std::wstring backupPath = BackupHostsFile();
    HANDLE hFile = CreateFileW(HOSTS_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        const char* defaultContent = "# Copyright (c) 1993-2009 Microsoft Corp.\r\n#\r\n# This is a sample HOSTS file used by Microsoft TCP/IP for Windows.\r\n#\r\n# For example:\r\n#\r\n#      127.0.0.1       localhost\r\n#      ::1             localhost\r\n\r\n127.0.0.1       localhost\r\n::1             localhost\r\n";
        DWORD written = 0;
        WriteFile(hFile, defaultContent, (DWORD)strlen(defaultContent), &written, NULL);
        CloseHandle(hFile);
    }
}

/**
 * @brief 一键网络修复的工作线程入口
 */
DWORD WINAPI NetworkRepairThreadProc(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    NetworkRepairAll();
    PostMessageW(hWnd, WM_NET_REPAIR_DONE, 0, 0);
    return 0;
}

/**
 * @brief 读取系统 Hosts 文件内容
 */
std::wstring ReadHostsFile() {
    InitHostsPath();
    HANDLE hFile = CreateFileW(HOSTS_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(hFile); return L""; }
    char* buf = new char[size + 1];
    DWORD read = 0;
    if (!ReadFile(hFile, buf, size, &read, NULL)) {
        delete[] buf;
        CloseHandle(hFile);
        return L"";
    }
    buf[read] = '\0';
    CloseHandle(hFile);

    int wLen = MultiByteToWideChar(CP_ACP, 0, buf, -1, NULL, 0);
    wchar_t* wBuf = new wchar_t[wLen];
    MultiByteToWideChar(CP_ACP, 0, buf, -1, wBuf, wLen);
    std::wstring result = wBuf;

    delete[] buf;
    delete[] wBuf;

    // 多行 EDIT 只认 CRLF：Unix 风格换行的 hosts 会挤成一行
    std::wstring normalized;
    normalized.reserve(result.size() + 16);
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == L'\n' && (i == 0 || result[i - 1] != L'\r')) {
            normalized += L"\r\n";
        } else {
            normalized += result[i];
        }
    }
    return normalized;
}

/**
 * @brief 保存编辑后的 Hosts 文件
 */
void SaveHostsFile() {
    InitHostsPath();
    int len = GetWindowTextLength(g_hHostsEdit);
    wchar_t* wBuf = new wchar_t[len + 1];
    GetWindowTextW(g_hHostsEdit, wBuf, len + 1);

    int aLen = WideCharToMultiByte(CP_ACP, 0, wBuf, -1, NULL, 0, NULL, NULL);
    char* aBuf = new char[aLen];
    WideCharToMultiByte(CP_ACP, 0, wBuf, -1, aBuf, aLen, NULL, NULL);

    std::wstring backupPath = BackupHostsFile();
    HANDLE hFile = CreateFileW(HOSTS_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, aBuf, (DWORD)strlen(aBuf), &written, NULL);
        CloseHandle(hFile);
        if (!backupPath.empty()) {
            wchar_t msg[256];
            swprintf_s(msg, L"Hosts文件保存成功！\n原文件已备份到：%s", backupPath.c_str());
            MessageBoxW(g_hMainWnd, msg, L"提示", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(g_hMainWnd, L"Hosts文件保存成功！", L"提示", MB_OK | MB_ICONINFORMATION);
        }
    }
    else {
        MessageBoxW(g_hMainWnd, L"保存失败，请确保以管理员身份运行。", L"错误", MB_OK | MB_ICONERROR);
    }

    delete[] wBuf;
    delete[] aBuf;
}