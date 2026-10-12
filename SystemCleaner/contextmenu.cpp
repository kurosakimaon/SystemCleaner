// Module: contextmenu.cpp - Right-click context menu management

#include "common.h"

/**
 * @brief 扫描指定注册表路径下的右键菜单项
 */
void ScanContextMenuReg(HKEY hive, const std::wstring& basePath, const std::wstring& typeName) {
    HKEY hBaseKey = NULL;
    if (RegOpenKeyExW(hive, basePath.c_str(), 0, KEY_READ | GetRegistryViewAccess(), &hBaseKey) != ERROR_SUCCESS) return;

    wchar_t subKeyName[256];
    DWORD nameSize;
    DWORD index = 0;
    const std::wstring suffix = L"_disabled";

    while (true) {
        nameSize = _countof(subKeyName);
        LONG ret = RegEnumKeyExW(hBaseKey, index, subKeyName, &nameSize, NULL, NULL, NULL, NULL);
        if (ret != ERROR_SUCCESS) break;

        RightMenuItem item;
        item.realKeyName = subKeyName;
        item.type = typeName;
        item.location = basePath;
        item.hive = hive;
        item.parentPath = basePath;

        if (EndsWith(item.realKeyName, suffix)) {
            item.displayName = item.realKeyName.substr(0, item.realKeyName.length() - suffix.length());
            item.isEnabled = false;
        }
        else {
            item.displayName = item.realKeyName;
            item.isEnabled = true;
        }
        g_contextMenuItems.push_back(item);
        index++;
    }

    RegCloseKey(hBaseKey);
}

/**
 * @brief 扫描所有类型的右键菜单项
 */
void ScanAllContextMenus() {
    g_contextMenuItems.clear();

    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"*\\shell", L"文件右键");
    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"文件右键扩展");
    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"Directory\\shell", L"文件夹右键");
    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers", L"文件夹右键扩展");
    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"DesktopBackground\\shell", L"桌面右键");
    ScanContextMenuReg(HKEY_CLASSES_ROOT, L"Folder\\shell", L"目录右键");
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

        wchar_t nameBuf[256];
        wcscpy_s(nameBuf, item.displayName.c_str());
        lvi.pszText = nameBuf;
        ListView_InsertItem(g_hContextMenuList, &lvi);

        ListView_SetItemText(g_hContextMenuList, displayIndex, 1, (LPWSTR)item.type.c_str());
        ListView_SetItemText(g_hContextMenuList, displayIndex, 2, (LPWSTR)item.location.c_str());
        ListView_SetItemText(g_hContextMenuList, displayIndex, 3, item.isEnabled ? strEnabled : strDisabled);

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
        swprintf_s(buf, L"共扫描到 %zu 个右键菜单项，当前显示 %d 项", g_contextMenuItems.size(), displayIndex);
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
    if (RegOpenKeyExW(item.hive, item.parentPath.c_str(), 0, KEY_WRITE | GetRegistryViewAccess(), &hParentKey) != ERROR_SUCCESS)
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
    if (RegOpenKeyExW(item.hive, item.parentPath.c_str(), 0, KEY_WRITE | GetRegistryViewAccess(), &hParentKey) != ERROR_SUCCESS)
        return false;

    LONG ret = RegRenameKey(hParentKey, item.realKeyName.c_str(), item.displayName.c_str());
    RegCloseKey(hParentKey);
    return ret == ERROR_SUCCESS;
}