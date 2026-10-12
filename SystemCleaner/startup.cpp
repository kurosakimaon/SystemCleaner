// Module: startup.cpp - Startup item management (registry and folder-based)

#include "common.h"
#include <set>

// 去重集合：跨 32/64 位注册表视图扫描时，同一启动项只保留一个
static std::set<std::wstring> g_seenStartupKeys;

/**
 * @brief 从注册表指定子键加载启动项（viewFlag 指定注册表视图）
 */
void LoadRegistryStartup(HKEY hive, const std::wstring& subKey, const std::wstring& locationName, REGSAM viewFlag) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(hive, subKey.c_str(), 0, KEY_READ | viewFlag, &hKey) != ERROR_SUCCESS) return;

    wchar_t valueName[256];
    wchar_t valueData[1024];
    DWORD nameSize, dataSize;
    DWORD index = 0;

    while (true) {
        nameSize = _countof(valueName);
        dataSize = _countof(valueData);
        DWORD type = 0;

        LONG ret = RegEnumValueW(hKey, index, valueName, &nameSize, NULL, &type, (LPBYTE)valueData, &dataSize);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) {
            // ERROR_MORE_DATA：名称或数据超出缓冲区。此处必须跳过当前项继续枚举，
            // 否则 break 会丢掉本子键中其后所有启动项（超长命令行很常见）
            index++;
            continue;
        }

        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            // 去重：名称+命令+位置完全相同则跳过（32/64位视图内容可能相同）。
            // 必须包含 hive，否则 HKCU 下与 HKLM 同名的独立启动项会被误删
            std::wstring dedupKey = (hive == HKEY_LOCAL_MACHINE ? L"HKLM|" : L"HKCU|") +
                std::wstring(valueName) + L"|" + std::wstring(valueData) + L"|" + subKey;
            for (wchar_t& ch : dedupKey) ch = towlower(ch);
            if (!g_seenStartupKeys.insert(dedupKey).second) { index++; continue; }

            StartupItem item;
            item.realName = valueName;
            item.path = valueData;
            item.location = locationName;
            item.hive = hive;
            item.subKey = subKey;
            item.isRegistry = true;

            const std::wstring suffix = L"_disabled";
            if (EndsWith(item.realName, suffix)) {
                item.displayName = item.realName.substr(0, item.realName.length() - suffix.length());
                item.isEnabled = false;
            }
            else {
                item.displayName = item.realName;
                item.isEnabled = true;
            }
            g_startupItems.push_back(item);
        }
        index++;
    }

    RegCloseKey(hKey);
}

/**
 * @brief 从启动文件夹加载启动项
 */
void LoadFolderStartup(const std::wstring& folderPath, const std::wstring& locationName) {
    if (!PathFileExistsW(folderPath.c_str())) return;

    WIN32_FIND_DATAW fd;
    std::wstring search = folderPath + L"\\*.*";
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    const std::wstring suffix = L".disabled";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        StartupItem item;
        item.realName = fd.cFileName;
        item.path = folderPath + L"\\" + item.realName;
        item.location = locationName;
        item.isRegistry = false;

        if (EndsWith(item.realName, suffix)) {
            item.displayName = item.realName.substr(0, item.realName.length() - suffix.length());
            item.isEnabled = false;
        }
        else {
            item.displayName = item.realName;
            item.isEnabled = true;
        }
        g_startupItems.push_back(item);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

/**
 * @brief 应用任务管理器（StartupApproved）的禁用状态到已扫描的启动项
 */
static void ApplyStartupApproved() {
    struct ApprovedKey {
        HKEY hive;
        const wchar_t* subKey;
    };
    const ApprovedKey approvedKeys[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder" },
    };

    for (const auto& ak : approvedKeys) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(ak.hive, ak.subKey, 0, KEY_READ | GetRegistryViewAccess(), &hKey) != ERROR_SUCCESS) continue;

        wchar_t valueName[256];
        BYTE data[64];
        DWORD nameSize, dataSize;
        DWORD index = 0;

        while (true) {
            nameSize = _countof(valueName);
            dataSize = sizeof(data);
            LONG ret = RegEnumValueW(hKey, index, valueName, &nameSize, NULL, NULL, data, &dataSize);
            if (ret != ERROR_SUCCESS) break;

            // StartupApproved 值为 12 字节二进制，首字节最低位为 1 表示已禁用
            bool disabled = (dataSize >= 1) && ((data[0] & 0x01) != 0);
            if (disabled) {
                for (auto& item : g_startupItems) {
                    if (_wcsicmp(item.realName.c_str(), valueName) == 0 ||
                        _wcsicmp(item.displayName.c_str(), valueName) == 0) {
                        item.isEnabled = false;
                    }
                }
            }
            index++;
        }
        RegCloseKey(hKey);
    }
}

/**
 * @brief 扫描所有来源的开机启动项
 */
void ScanAllStartupItems() {
    g_startupItems.clear();
    g_seenStartupKeys.clear();

    // Run / RunOnce / 组策略（Policies\Explorer\Run）三个子键
    const wchar_t* regPaths[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run"
    };

    REGSAM nativeView = GetRegistryViewAccess();

    for (auto subKey : regPaths) {
        LoadRegistryStartup(HKEY_CURRENT_USER, subKey, L"HKCU: " + std::wstring(subKey), nativeView);
        LoadRegistryStartup(HKEY_LOCAL_MACHINE, subKey, L"HKLM: " + std::wstring(subKey), nativeView);
    }

    // 64 位系统：补充扫描 32 位软件视图（WOW6432Node），避免遗漏 32 位程序的启动项
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    bool osIs64Bit = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                      si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
    if (osIs64Bit && nativeView != KEY_WOW64_32KEY) {
        for (auto subKey : regPaths) {
            LoadRegistryStartup(HKEY_CURRENT_USER, subKey, L"HKCU(32位): " + std::wstring(subKey), KEY_WOW64_32KEY);
            LoadRegistryStartup(HKEY_LOCAL_MACHINE, subKey, L"HKLM(32位): " + std::wstring(subKey), KEY_WOW64_32KEY);
        }
    }

    wchar_t userStartup[MAX_PATH] = { 0 };
    wchar_t commonStartup[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, userStartup);
    SHGetFolderPathW(NULL, CSIDL_COMMON_STARTUP, NULL, 0, commonStartup);

    LoadFolderStartup(userStartup, L"当前用户启动文件夹");
    LoadFolderStartup(commonStartup, L"公共启动文件夹");

    // 应用任务管理器的禁用状态（最后应用，避免被后续加载覆盖）
    ApplyStartupApproved();
}

/**
 * @brief 检查启动项目标文件是否仍然存在（注册表项会解析引号、参数和环境变量）
 */
static bool StartupTargetExists(const StartupItem& item) {
    std::wstring target = item.path;
    if (target.empty()) return true;

    if (item.isRegistry) {
        size_t start = target.find_first_not_of(L" \t");
        if (start == std::wstring::npos) return true;
        target = target.substr(start);

        if (!target.empty() && target[0] == L'"') {
            size_t end = target.find(L'"', 1);
            if (end == std::wstring::npos) return true;
            target = target.substr(1, end - 1);
        }
        else {
            size_t space = target.find_first_of(L" \t");
            if (space != std::wstring::npos) target = target.substr(0, space);
        }
        if (target.empty()) return true;

        wchar_t expanded[MAX_PATH] = { 0 };
        DWORD expLen = ExpandEnvironmentStringsW(target.c_str(), expanded, MAX_PATH);
        if (expLen > 0 && expLen < MAX_PATH) target = expanded;
    }

    return PathFileExistsW(target.c_str()) != FALSE;
}

/**
 * @brief 刷新启动项列表控件（支持按状态筛选）
 */
void RefreshStartupList() {
    ListView_DeleteAllItems(g_hStartupList);
    ScanAllStartupItems();
    wchar_t strEnabled[] = L"已启用";
    wchar_t strDisabled[] = L"已禁用";

    int displayIndex = 0;
    for (size_t i = 0; i < g_startupItems.size(); i++) {
        const StartupItem& item = g_startupItems[i];

        if (g_startupFilterType == 1 && !item.isEnabled) continue;
        if (g_startupFilterType == 2 && item.isEnabled) continue;

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = displayIndex;
        lvi.lParam = (LPARAM)i; // 保存原始索引，供禁用/恢复操作映射

        wchar_t nameBuf[256];
        wcscpy_s(nameBuf, item.displayName.c_str());
        lvi.pszText = nameBuf;
        ListView_InsertItem(g_hStartupList, &lvi);

        std::wstring displayPath = item.path;
        if (displayPath.length() > 80) displayPath = displayPath.substr(0, 80) + L"...";

        ListView_SetItemText(g_hStartupList, displayIndex, 1, (LPWSTR)displayPath.c_str());
        ListView_SetItemText(g_hStartupList, displayIndex, 2, (LPWSTR)item.location.c_str());

        std::wstring statusStr = item.isEnabled ? strEnabled : strDisabled;
        if (!StartupTargetExists(item)) statusStr += L"（路径无效）";
        ListView_SetItemText(g_hStartupList, displayIndex, 3, (LPWSTR)statusStr.c_str());

        displayIndex++;
    }

    RECT tabRect;
    GetClientRect(g_hTab, &tabRect);
    TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
    AdjustListViewHeight(g_hStartupList, tabRect, 50, 50);

    wchar_t buf[128];
    if (g_startupFilterType == 0) {
        swprintf_s(buf, L"共扫描到 %zu 个启动项", g_startupItems.size());
    }
    else {
        swprintf_s(buf, L"共扫描到 %zu 个启动项，当前显示 %d 项", g_startupItems.size(), displayIndex);
    }
    SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)buf);
}

/**
 * @brief 禁用指定的启动项
 */
bool DisableStartupItem(const StartupItem& item) {
    if (!item.isEnabled) return true;

    if (item.isRegistry) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(item.hive, item.subKey.c_str(), 0, KEY_READ | KEY_SET_VALUE | GetRegistryViewAccess(), &hKey) != ERROR_SUCCESS)
            return false;

        DWORD type = 0;
        BYTE data[1024];
        DWORD dataSize = sizeof(data);
        if (RegQueryValueExW(hKey, item.realName.c_str(), NULL, &type, data, &dataSize) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return false;
        }

        std::wstring newName = item.realName + L"_disabled";
        if (RegSetValueExW(hKey, newName.c_str(), 0, type, data, dataSize) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return false;
        }

        RegDeleteValueW(hKey, item.realName.c_str());
        RegCloseKey(hKey);
        return true;
    }
    else {
        std::wstring newPath = item.path + L".disabled";
        return MoveFileW(item.path.c_str(), newPath.c_str()) != FALSE;
    }
}

/**
 * @brief 恢复（启用）指定的已禁用启动项
 */
bool EnableStartupItem(const StartupItem& item) {
    if (item.isEnabled) return true;

    if (item.isRegistry) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(item.hive, item.subKey.c_str(), 0, KEY_READ | KEY_SET_VALUE | GetRegistryViewAccess(), &hKey) != ERROR_SUCCESS)
            return false;

        DWORD type = 0;
        BYTE data[1024];
        DWORD dataSize = sizeof(data);
        if (RegQueryValueExW(hKey, item.realName.c_str(), NULL, &type, data, &dataSize) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return false;
        }

        if (RegSetValueExW(hKey, item.displayName.c_str(), 0, type, data, dataSize) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return false;
        }

        RegDeleteValueW(hKey, item.realName.c_str());
        RegCloseKey(hKey);
        return true;
    }
    else {
        // 必须以 ".disabled" 结尾才能还原；直接 substr(len-9) 在名称异常时
        // （len<9）会因 size_t 下溢而崩溃
        std::wstring newPath;
        if (!TryStripDisabledSuffix(item.path, newPath)) return false;
        return MoveFileW(item.path.c_str(), newPath.c_str()) != FALSE;
    }
}