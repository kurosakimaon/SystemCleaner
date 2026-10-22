// Module: contextmenu.cpp - Right-click context menu management

#include "common.h"

// 跨视图去重：HKCR 的 32 位视图与 64 位视图是两套独立注册，
// 同一菜单项可能在两边各注册一次（需要分别禁用），所以不合并；这里只防止
// 同一次扫描内重复枚举同一物理键
static std::vector<std::wstring> g_seenContextKeys;

/**
 * @brief 扫描指定注册表路径下的右键菜单项
 *        子键名可能很长（GUID、带版本号的 CLSID），固定 256 缓冲区会在
 *        ERROR_MORE_DATA 时中断枚举，漏掉该项及其后所有菜单
 */
void ScanContextMenuReg(HKEY hive, const std::wstring& basePath,
                        const std::wstring& typeName, REGSAM viewFlag) {
    HKEY hBaseKey = NULL;
    if (RegOpenKeyExW(hive, basePath.c_str(), 0, KEY_READ | viewFlag, &hBaseKey) != ERROR_SUCCESS)
        return;

    std::vector<wchar_t> nameBuf(512);
    DWORD index = 0;
    const std::wstring suffix = L"_disabled";

    for (;;) {
        DWORD nameSize = (DWORD)nameBuf.size();
        LONG ret = RegEnumKeyExW(hBaseKey, index, nameBuf.data(), &nameSize,
                                 NULL, NULL, NULL, NULL);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret == ERROR_MORE_DATA) {
            if (nameSize <= nameBuf.size()) nameBuf.resize(nameBuf.size() * 2);
            else nameBuf.resize((size_t)nameSize + 1);
            if (nameBuf.size() > 32768) break;
            continue;
        }
        if (ret != ERROR_SUCCESS) { index++; continue; }
        index++;

        std::wstring keyName(nameBuf.data(), nameSize);

        std::wstring dedupKey =
            std::to_wstring((unsigned long)viewFlag) + L"|" +
            basePath + L"|" + keyName;
        for (wchar_t& ch : dedupKey) ch = towlower(ch);
        if (std::find(g_seenContextKeys.begin(), g_seenContextKeys.end(), dedupKey) !=
            g_seenContextKeys.end()) {
            continue;
        }
        g_seenContextKeys.push_back(dedupKey);

        RightMenuItem item;
        item.realKeyName = keyName;
        item.type = typeName;
        item.location = basePath;
        item.hive = hive;
        item.parentPath = basePath;
        item.view = viewFlag;

        if (EndsWith(item.realKeyName, suffix)) {
            item.displayName = item.realKeyName.substr(0, item.realKeyName.length() - suffix.length());
            item.isEnabled = false;
        }
        else {
            item.displayName = item.realKeyName;
            item.isEnabled = true;
        }
        g_contextMenuItems.push_back(item);
    }

    RegCloseKey(hBaseKey);
}

/**
 * @brief 扫描所有类型的右键菜单项
 */
void ScanAllContextMenus() {
    g_contextMenuItems.clear();
    g_seenContextKeys.clear();

    struct MenuRoot {
        const wchar_t* path;
        const wchar_t* type;
    };
    const MenuRoot roots[] = {
        { L"*\\shell", L"文件右键" },
        { L"*\\shellex\\ContextMenuHandlers", L"文件右键扩展" },
        { L"Directory\\shell", L"文件夹右键" },
        { L"Directory\\shellex\\ContextMenuHandlers", L"文件夹右键扩展" },
        { L"DesktopBackground\\shell", L"桌面右键" },
        { L"Folder\\shell", L"目录右键" },
    };

    REGSAM nativeView = GetRegistryViewAccess();
    for (const auto& root : roots) {
        ScanContextMenuReg(HKEY_CLASSES_ROOT, root.path, root.type, nativeView);
    }

    // 64 位系统上 32 位程序的菜单扩展注册在 HKCR 的 32 位视图。
    // 用 64 位视图操作它们会找不到键（或误改 64 位同名项），因此单独扫描并标注
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    bool osIs64Bit = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                      si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
    if (osIs64Bit && !(nativeView & KEY_WOW64_32KEY)) {
        for (const auto& root : roots) {
            ScanContextMenuReg(HKEY_CLASSES_ROOT, root.path,
                std::wstring(root.type) + L"(32位)", KEY_WOW64_32KEY);
        }
    }
}

/**
 * @brief 刷新右键菜单列表控件（支持按状态筛选）
 */
void RefreshContextMenuList() {
    ListView_DeleteAllItems(g_hContextMenuList);
    ScanAllContextMenus();
    wchar_t strEnabled[] = L"已启用";
    wchar_t strDisabled[] = L"已禁用";

    int displayIndex = 0;
    for (size_t i = 0; i < g_contextMenuItems.size(); i++) {
        const RightMenuItem& item = g_contextMenuItems[i];

        if (g_contextMenuFilterType == 1 && !item.isEnabled) continue;
        if (g_contextMenuFilterType == 2 && item.isEnabled) continue;

        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = displayIndex;
        lvi.lParam = (LPARAM)i; // 保存原始索引，供禁用/恢复操作映射

        lvi.pszText = (LPWSTR)item.displayName.c_str();
        ListView_InsertItem(g_hContextMenuList, &lvi);

        ListView_SetItemText(g_hContextMenuList, displayIndex, 1, (LPWSTR)item.type.c_str());
        ListView_SetItemText(g_hContextMenuList, displayIndex, 2, (LPWSTR)item.location.c_str());
        ListView_SetItemText(g_hContextMenuList, displayIndex, 3,
            item.isEnabled ? strEnabled : strDisabled);

        displayIndex++;
    }

    RECT tabRect;
    GetClientRect(g_hTab, &tabRect);
    TabCtrl_AdjustRect(g_hTab, FALSE, &tabRect);
    AdjustListViewHeight(g_hContextMenuList, tabRect, 50, 50);

    wchar_t buf[128];
    if (g_contextMenuFilterType == 0) {
        swprintf_s(buf, L"共扫描到 %zu 个右键菜单项", g_contextMenuItems.size());
    }
    else {
        swprintf_s(buf, L"共扫描到 %zu 个右键菜单项，当前显示 %d 项",
            g_contextMenuItems.size(), displayIndex);
    }
    SendMessageW(g_hStatusText, WM_SETTEXT, 0, (LPARAM)buf);
}

/**
 * @brief 禁用指定的右键菜单项
 */
bool DisableContextMenuItem(const RightMenuItem& item) {
    if (!item.isEnabled) return true;
    std::wstring newName = item.realKeyName + L"_disabled";

    HKEY hParentKey = NULL;
    if (RegOpenKeyExW(item.hive, item.parentPath.c_str(), 0,
            KEY_WRITE | item.view, &hParentKey) != ERROR_SUCCESS)
        return false;

    LONG ret = RegRenameKey(hParentKey, item.realKeyName.c_str(), newName.c_str());
    RegCloseKey(hParentKey);
    return ret == ERROR_SUCCESS;
}

/**
 * @brief 恢复（启用）指定的已禁用右键菜单项
 */
bool EnableContextMenuItem(const RightMenuItem& item) {
    if (item.isEnabled) return true;

    HKEY hParentKey = NULL;
    if (RegOpenKeyExW(item.hive, item.parentPath.c_str(), 0,
            KEY_WRITE | item.view, &hParentKey) != ERROR_SUCCESS)
        return false;

    LONG ret = RegRenameKey(hParentKey, item.realKeyName.c_str(), item.displayName.c_str());
    RegCloseKey(hParentKey);
    return ret == ERROR_SUCCESS;
}
