// Module: startup.cpp - Startup item management (registry and folder-based)

#include "common.h"
#include <set>

// 去重集合：同一物理位置重复扫描时只保留一条。注册表视图也计入 key：
// HKLM 的 Run 在 32/64 位视图下是两个独立物理键（WOW6432Node），不能互相抵消
static std::set<std::wstring> g_seenStartupKeys;

static const wchar_t* kApprovedRoot =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
static const wchar_t* kBackupRoot = L"Software\\SystemCleaner\\DisabledStartup";

// 动态枚举注册表值：名称和数据都可能超过固定缓冲区（长命令行、长路径很常见），
// 固定 256/1024 缓冲区遇 ERROR_MORE_DATA 会静默跳过整条启动项
static LONG EnumValueAlloc(HKEY hKey, DWORD index, std::wstring& name, DWORD& type,
                           std::vector<BYTE>& data) {
    std::vector<wchar_t> nameBuf(512);
    std::vector<BYTE> dataBuf(2048);

    for (int attempt = 0; attempt < 12; attempt++) {
        DWORD nameSize = (DWORD)nameBuf.size();
        DWORD dataSize = (DWORD)dataBuf.size();
        type = 0;
        LONG ret = RegEnumValueW(hKey, index, nameBuf.data(), &nameSize, NULL, &type,
                                 dataBuf.data(), &dataSize);
        if (ret == ERROR_MORE_DATA) {
            if (nameSize >= nameBuf.size()) nameBuf.resize((size_t)nameSize + 1);
            if (dataSize > dataBuf.size()) dataBuf.resize((size_t)dataSize);
            else dataBuf.resize(dataBuf.size() * 2);
            if (nameBuf.size() > 32768 || dataBuf.size() > 1024 * 1024) return ERROR_OUTOFMEMORY;
            continue;
        }
        if (ret != ERROR_SUCCESS) return ret;

        name.assign(nameBuf.data(), nameSize);
        data.assign(dataBuf.begin(), dataBuf.begin() + dataSize);
        return ERROR_SUCCESS;
    }
    return ERROR_OUTOFMEMORY;
}

static std::wstring BytesToWString(const std::vector<BYTE>& data) {
    if (data.size() < sizeof(wchar_t)) return std::wstring();
    std::vector<wchar_t> chars(data.size() / sizeof(wchar_t));
    memcpy(chars.data(), data.data(), chars.size() * sizeof(wchar_t));
    std::wstring s(chars.data(), chars.size());
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

static bool QueryValueAlloc(HKEY hKey, const wchar_t* name, DWORD& type, std::vector<BYTE>& data) {
    DWORD size = 0;
    type = 0;
    LONG ret = RegQueryValueExW(hKey, name, NULL, &type, NULL, &size);
    if (ret != ERROR_SUCCESS) return false;

    data.resize(size ? size : 1);
    DWORD got = (DWORD)data.size();
    ret = RegQueryValueExW(hKey, name, NULL, &type, data.data(), &got);
    if (ret != ERROR_SUCCESS) {
        data.clear();
        return false;
    }
    data.resize(got);
    return true;
}

/**
 * @brief 从注册表指定子键加载启动项（viewFlag 指定注册表视图）
 */
void LoadRegistryStartup(HKEY hive, const std::wstring& subKey, const std::wstring& locationName, REGSAM viewFlag) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(hive, subKey.c_str(), 0, KEY_READ | viewFlag, &hKey) != ERROR_SUCCESS) return;

    DWORD index = 0;
    for (;;) {
        std::wstring valueName;
        DWORD type = 0;
        std::vector<BYTE> data;
        LONG ret = EnumValueAlloc(hKey, index, valueName, type, data);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) {
            // 单项读取失败时跳过该项，但不能终止整个子键的枚举
            index++;
            continue;
        }
        index++;

        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

        std::wstring valueData = BytesToWString(data);

        // 去重：名称+命令+位置+视图完全相同则跳过（同一物理键被重复扫描）。
        // 必须包含 hive 与视图，否则 HKLM 的 32 位启动项会被 64 位同名项吞掉，
        // 导致「禁用了一个，另一个仍在开机启动」
        std::wstring dedupKey =
            (hive == HKEY_LOCAL_MACHINE ? L"HKLM|" : L"HKCU|") +
            std::to_wstring((unsigned long)viewFlag) + L"|" +
            valueName + L"|" + valueData + L"|" + subKey;
        for (wchar_t& ch : dedupKey) ch = towlower(ch);
        if (!g_seenStartupKeys.insert(dedupKey).second) continue;

        StartupItem item;
        item.realName = valueName;
        item.path = valueData;
        item.location = locationName;
        item.hive = hive;
        item.subKey = subKey;
        item.view = viewFlag;
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

    RegCloseKey(hKey);
}

/**
 * @brief 从启动文件夹加载启动项
 */
void LoadFolderStartup(const std::wstring& folderPath, const std::wstring& locationName, HKEY hive) {
    if (!PathFileExistsW(folderPath.c_str())) return;

    WIN32_FIND_DATAW fd;
    std::wstring search = folderPath + L"\\*.*";
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    const std::wstring suffix = L".disabled";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // desktop.ini 是资源管理器用来定制文件夹显示的元数据，不是启动程序
        if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;

        StartupItem item;
        item.realName = fd.cFileName;
        item.path = folderPath + L"\\" + item.realName;
        item.location = locationName;
        item.hive = hive;
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

// ====================== 启动项禁用状态（StartupApproved） ======================

static bool IsPlainRunKey(const std::wstring& subKey) {
    return _wcsicmp(subKey.c_str(),
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") == 0;
}

static bool UsesBackupStore(const StartupItem& item) {
    if (!item.isRegistry) return false;
    if (EndsWith(item.subKey, L"\\RunOnce")) return true;
    return item.subKey.find(L"\\Policies\\") != std::wstring::npos;
}

static std::wstring ApprovedSubKeyForRegistry(const StartupItem& item, bool wow32) {
    return std::wstring(kApprovedRoot) + (wow32 ? L"Run32" : L"Run");
}

static bool ApprovedNameMatches(const StartupItem& item, const std::wstring& valueName) {
    return (!item.realName.empty() && _wcsicmp(item.realName.c_str(), valueName.c_str()) == 0) ||
           (!item.displayName.empty() && _wcsicmp(item.displayName.c_str(), valueName.c_str()) == 0);
}

/**
 * @brief 应用任务管理器（StartupApproved）的禁用状态到已扫描的启动项
 */
static void ApplyStartupApproved() {
    struct ApprovedSource {
        HKEY hive;
        const wchar_t* subKey;
        bool folder;
        bool wow32;
    };
    const ApprovedSource approvedKeys[] = {
        // Run 的禁用记录：64 位与 32 位分属 Run / Run32，且按 hive 隔离。
        // 原实现不区分 hive/视图，HKCU 与 HKLM 同名项会被互相标记
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", false, false },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", false, false },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32", false, true },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", true, false },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", true, false },
    };

    for (const auto& ak : approvedKeys) {
        HKEY hKey = NULL;
        if (RegOpenKeyExW(ak.hive, ak.subKey, 0, KEY_READ | GetRegistryViewAccess(), &hKey) != ERROR_SUCCESS) continue;

        DWORD index = 0;
        for (;;) {
            std::wstring valueName;
            DWORD type = 0;
            std::vector<BYTE> data;
            LONG ret = EnumValueAlloc(hKey, index, valueName, type, data);
            if (ret == ERROR_NO_MORE_ITEMS) break;
            if (ret != ERROR_SUCCESS) { index++; continue; }
            index++;

            // StartupApproved 值为 12 字节二进制，首字节最低位为 1 表示已禁用
            bool disabled = !data.empty() && ((data[0] & 0x01) != 0);
            if (!disabled) continue;

            for (auto& item : g_startupItems) {
                if (item.isBackedUp) continue;
                if (item.hive != ak.hive) continue;

                if (ak.folder) {
                    if (item.isRegistry) continue;
                }
                else {
                    // 只有普通 Run 键的值受 StartupApproved 控制；
                    // RunOnce / 策略键即使存在残留记录也不代表会被跳过
                    if (!item.isRegistry || !IsPlainRunKey(item.subKey)) continue;
                    bool itemIs32 = (item.view & KEY_WOW64_32KEY) != 0;
                    if (itemIs32 != ak.wow32) continue;
                }

                if (ApprovedNameMatches(item, valueName)) item.isEnabled = false;
            }
        }
        RegCloseKey(hKey);
    }
}

// ====================== 禁用项的注册表备份（RunOnce / 策略键） ======================
//
// RunOnce 与策略 Run 不受 StartupApproved 控制，改名同样无效（Run 类键会执行
// 所有值，与值名无关）。唯一可靠的禁用方式是先从原位置移除，恢复时再写回，
// 因此把原值（含类型与视图）完整备份到 HKCU 下本程序自己的键中。

static std::wstring HiveTag(HKEY hive, REGSAM view) {
    std::wstring tag = (hive == HKEY_LOCAL_MACHINE) ? L"HKLM"
        : (hive == HKEY_CURRENT_USER) ? L"HKCU"
        : (hive == HKEY_CLASSES_ROOT) ? L"HKCR" : L"HKU";
    if (view & KEY_WOW64_32KEY) tag += L"32";
    return tag;
}

static std::wstring BackupKeyPath(HKEY hive, const std::wstring& subKey, REGSAM view) {
    return std::wstring(kBackupRoot) + L"\\" + HiveTag(hive, view) + L"\\" + subKey;
}

// 值名允许任意字符但不能为空；'%' 转义后 "%00" 可安全表示默认值
static std::wstring EncodeBackupValueName(const std::wstring& name) {
    if (name.empty()) return L"%00";
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t ch : name) {
        if (ch == L'%') out += L"%25";
        else out += ch;
    }
    return out;
}

static bool DecodeBackupValueName(const std::wstring& encoded, std::wstring& name) {
    if (encoded == L"%00") { name.clear(); return true; }
    name.clear();
    for (size_t i = 0; i < encoded.size(); i++) {
        if (encoded[i] == L'%') {
            if (i + 2 >= encoded.size() || encoded[i + 1] != L'2' || encoded[i + 2] != L'5')
                return false;
            name += L'%';
            i += 2;
        }
        else {
            name += encoded[i];
        }
    }
    return true;
}

static const DWORD kBackupMagic = 0x5343424B; // 'SCBK'

static bool WriteStartupBackup(const StartupItem& item, DWORD type, const std::vector<BYTE>& data) {
    std::vector<BYTE> blob;
    blob.reserve(16 + item.realName.size() * sizeof(wchar_t) + data.size());
    auto appendDword = [&blob](DWORD v) {
        const BYTE* p = reinterpret_cast<const BYTE*>(&v);
        blob.insert(blob.end(), p, p + sizeof(DWORD));
    };
    appendDword(kBackupMagic);
    appendDword((DWORD)item.view);
    appendDword(type);
    appendDword((DWORD)item.realName.size());
    const BYTE* nameBytes = reinterpret_cast<const BYTE*>(item.realName.data());
    blob.insert(blob.end(), nameBytes, nameBytes + item.realName.size() * sizeof(wchar_t));
    blob.insert(blob.end(), data.begin(), data.end());

    HKEY hKey = NULL;
    std::wstring path = BackupKeyPath(item.hive, item.subKey, item.view);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring encoded = EncodeBackupValueName(item.realName);
    LONG ret = RegSetValueExW(hKey, encoded.c_str(), 0, REG_BINARY,
        blob.data(), (DWORD)blob.size());
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS;
}

static bool ReadStartupBackup(HKEY hKey, const std::wstring& encodedName,
                              std::wstring& originalName, DWORD& view, DWORD& type,
                              std::vector<BYTE>& data) {
    HKEY hValueKey = NULL;
    (void)hValueKey;
    DWORD blobType = 0;
    std::vector<BYTE> blob;
    if (!QueryValueAlloc(hKey, encodedName.c_str(), blobType, blob)) return false;
    if (blobType != REG_BINARY || blob.size() < 16) return false;

    DWORD magic = 0;
    memcpy(&magic, blob.data(), sizeof(DWORD));
    if (magic != kBackupMagic) return false;

    size_t off = sizeof(DWORD);
    memcpy(&view, blob.data() + off, sizeof(DWORD)); off += sizeof(DWORD);
    memcpy(&type, blob.data() + off, sizeof(DWORD)); off += sizeof(DWORD);
    DWORD nameLen = 0;
    memcpy(&nameLen, blob.data() + off, sizeof(DWORD)); off += sizeof(DWORD);

    if (nameLen > 16384) return false;
    size_t nameBytes = (size_t)nameLen * sizeof(wchar_t);
    if (off + nameBytes > blob.size()) return false;

    originalName.assign((const wchar_t*)(blob.data() + off), nameLen);
    off += nameBytes;
    data.assign(blob.begin() + off, blob.end());
    return true;
}

static bool DeleteStartupBackup(const StartupItem& item) {
    HKEY hKey = NULL;
    std::wstring path = BackupKeyPath(item.hive, item.subKey, item.view);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LONG ret = RegDeleteValueW(hKey, EncodeBackupValueName(item.realName).c_str());
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS || ret == ERROR_FILE_NOT_FOUND;
}

static bool MoveStartupValueToBackup(const StartupItem& item) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(item.hive, item.subKey.c_str(), 0,
            KEY_READ | KEY_SET_VALUE | item.view, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    std::vector<BYTE> data;
    if (!QueryValueAlloc(hKey, item.realName.c_str(), type, data)) {
        RegCloseKey(hKey);
        return false;
    }

    bool backedUp = WriteStartupBackup(item, type, data);
    LONG delRet = ERROR_SUCCESS;
    if (backedUp) delRet = RegDeleteValueW(hKey, item.realName.c_str());
    RegCloseKey(hKey);

    if (!backedUp) return false;
    if (delRet != ERROR_SUCCESS) {
        // 删除失败必须回滚备份，否则下次扫描会把同一项显示两次
        DeleteStartupBackup(item);
        return false;
    }
    return true;
}

static bool RestoreStartupValueFromBackup(const StartupItem& item) {
    HKEY hBackup = NULL;
    std::wstring path = BackupKeyPath(item.hive, item.subKey, item.view);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
            KEY_READ | KEY_WOW64_64KEY, &hBackup) != ERROR_SUCCESS) {
        return false;
    }

    std::wstring originalName;
    DWORD view = 0, type = 0;
    std::vector<BYTE> data;
    bool ok = ReadStartupBackup(hBackup, EncodeBackupValueName(item.realName),
        originalName, view, type, data);
    RegCloseKey(hBackup);
    if (!ok) return false;

    if (_wcsicmp(originalName.c_str(), item.realName.c_str()) != 0) return false;

    HKEY hTarget = NULL;
    if (RegCreateKeyExW(item.hive, item.subKey.c_str(), 0, NULL, 0,
            KEY_SET_VALUE | view, NULL, &hTarget, NULL) != ERROR_SUCCESS) {
        return false;
    }
    LONG ret = RegSetValueExW(hTarget, originalName.c_str(), 0, type,
        data.empty() ? NULL : data.data(), (DWORD)data.size());
    RegCloseKey(hTarget);
    if (ret != ERROR_SUCCESS) return false;

    DeleteStartupBackup(item);
    return true;
}

/**
 * @brief 扫描备份区中的禁用项，按「已禁用」状态加入列表
 */
static void LoadBackedUpStartupItems(HKEY hive, const std::wstring& subKey,
                                     REGSAM view, const std::wstring& locationName) {
    std::wstring path = BackupKeyPath(hive, subKey, view);
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
            KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    for (;;) {
        std::wstring encodedName;
        DWORD type = 0;
        std::vector<BYTE> blob;
        LONG ret = EnumValueAlloc(hKey, index, encodedName, type, blob);
        if (ret == ERROR_NO_MORE_ITEMS) break;
        if (ret != ERROR_SUCCESS) { index++; continue; }
        index++;

        std::wstring originalName;
        DWORD storedView = 0, storedType = 0;
        std::vector<BYTE> data;
        if (!ReadStartupBackup(hKey, encodedName, originalName, storedView, storedType, data))
            continue;
        if (storedType != REG_SZ && storedType != REG_EXPAND_SZ) continue;
        // 备份键按视图分目录存放；blob 中的视图必须与目录一致，否则拒绝恢复
        if (((storedView & KEY_WOW64_32KEY) != 0) != ((view & KEY_WOW64_32KEY) != 0))
            continue;

        StartupItem item;
        item.realName = originalName;
        item.displayName = originalName.empty() ? L"(默认值)" : originalName;
        item.path = BytesToWString(data);
        item.location = locationName + L"（已禁用）";
        item.hive = hive;
        item.subKey = subKey;
        item.view = storedView;
        item.isRegistry = true;
        item.isEnabled = false;
        item.isBackedUp = true;
        g_startupItems.push_back(item);
    }

    RegCloseKey(hKey);
}

// ====================== 扫描入口 ======================

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

    // 64 位系统：补充扫描 32 位软件视图（WOW6432Node）。
    // HKCU\Software 的 Run 类键在 32/64 位视图下是同一个物理键，扫描会得到
    // 完全重复的结果；只有 HKLM 的 Run/RunOnce 真正位于 WOW6432Node。
    // 策略键不受 WOW64 重定向，重复扫描只会产生同名重复项。
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    bool osIs64Bit = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                      si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
    if (osIs64Bit && !(nativeView & KEY_WOW64_32KEY)) {
        const wchar_t* regPaths32[] = {
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
        };
        for (auto subKey : regPaths32) {
            LoadRegistryStartup(HKEY_LOCAL_MACHINE, subKey,
                L"HKLM(32位): " + std::wstring(subKey), KEY_WOW64_32KEY);
        }
    }

    // 已禁用但被移入备份区的项（RunOnce / 策略键）
    LoadBackedUpStartupItems(HKEY_CURRENT_USER, regPaths[1], nativeView,
        L"HKCU: " + std::wstring(regPaths[1]));
    LoadBackedUpStartupItems(HKEY_LOCAL_MACHINE, regPaths[1], nativeView,
        L"HKLM: " + std::wstring(regPaths[1]));
    if (osIs64Bit && !(nativeView & KEY_WOW64_32KEY)) {
        LoadBackedUpStartupItems(HKEY_LOCAL_MACHINE, regPaths[1], KEY_WOW64_32KEY,
            L"HKLM(32位): " + std::wstring(regPaths[1]));
    }
    LoadBackedUpStartupItems(HKEY_CURRENT_USER, regPaths[2], nativeView,
        L"HKCU: " + std::wstring(regPaths[2]));
    LoadBackedUpStartupItems(HKEY_LOCAL_MACHINE, regPaths[2], nativeView,
        L"HKLM: " + std::wstring(regPaths[2]));

    wchar_t userStartup[MAX_PATH] = { 0 };
    wchar_t commonStartup[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, userStartup);
    SHGetFolderPathW(NULL, CSIDL_COMMON_STARTUP, NULL, 0, commonStartup);

    LoadFolderStartup(userStartup, L"当前用户启动文件夹", HKEY_CURRENT_USER);
    LoadFolderStartup(commonStartup, L"公共启动文件夹", HKEY_LOCAL_MACHINE);

    // 应用任务管理器的禁用状态（最后应用，避免被后续加载覆盖）
    ApplyStartupApproved();
}

/**
 * @brief 检查启动项目标文件是否仍然存在
 *        注册表命令支持引号、参数与环境变量；未加引号但路径含空格时，
 *        必须按「最长的真实文件前缀」解析，不能简单截断到第一个空格
 */
bool StartupTargetExists(const StartupItem& item) {
    std::wstring target = item.path;
    if (target.empty()) return true;

    if (item.isRegistry) {
        size_t start = target.find_first_not_of(L" \t");
        if (start == std::wstring::npos) return true;
        target = target.substr(start);

        std::wstring expanded;
        if (ExpandEnvVars(target, expanded)) target = expanded;

        std::wstring exe, params;
        if (!SplitUninstallCommand(target, exe, params) || exe.empty()) return true;

        bool absolute =
            (exe.size() >= 3 && exe[1] == L':' && (exe[2] == L'\\' || exe[2] == L'/')) ||
            (exe.size() >= 2 && exe[0] == L'\\' && exe[1] == L'\\');
        // 相对命令（rundll32、explorer 等）由系统 PATH 解析，不能判为无效
        if (!absolute) return true;

        DWORD attrs = GetFileAttributesW(exe.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    // 已禁用的文件项保存的是 xxx.disabled 路径，实际文件名为去掉后缀的那个
    std::wstring original;
    if (TryStripDisabledSuffix(target, original)) target = original;

    DWORD attrs = GetFileAttributesW(target.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
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

        // 值名最长可达 16383 字符，固定 256 缓冲区会触发 wcscpy_s 的参数检查中止进程
        lvi.pszText = (LPWSTR)item.displayName.c_str();
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

// ====================== 禁用 / 恢复 ======================

static bool DeleteApprovedValue(HKEY hive, const wchar_t* subKey, const std::wstring& valueName) {
    if (valueName.empty()) return true;
    HKEY hKey = NULL;
    LONG openRet = RegOpenKeyExW(hive, subKey, 0,
        KEY_SET_VALUE | GetRegistryViewAccess(), &hKey);
    if (openRet == ERROR_FILE_NOT_FOUND || openRet == ERROR_PATH_NOT_FOUND) return true;
    if (openRet != ERROR_SUCCESS) return false;
    LONG ret = RegDeleteValueW(hKey, valueName.c_str());
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS || ret == ERROR_FILE_NOT_FOUND;
}

static bool SetStartupApprovedDisabled(const StartupItem& item) {
    bool wow32 = (item.view & KEY_WOW64_32KEY) != 0;
    std::wstring subKey = ApprovedSubKeyForRegistry(item, wow32);

    HKEY hKey = NULL;
    if (RegCreateKeyExW(item.hive, subKey.c_str(), 0, NULL, 0,
            KEY_SET_VALUE | GetRegistryViewAccess(), NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return false;
    }
    BYTE data[12] = { 0x03 }; // 首字节 bit0 = 1：已禁用
    LONG ret = RegSetValueExW(hKey, item.realName.c_str(), 0, REG_BINARY,
        data, (DWORD)sizeof(data));
    RegCloseKey(hKey);
    return ret == ERROR_SUCCESS;
}

static bool ClearStartupApproved(const StartupItem& item, bool folder) {
    const wchar_t* subKeys[2] = { NULL, NULL };
    int count = 0;
    if (folder) {
        subKeys[count++] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
    }
    else {
        subKeys[count++] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        subKeys[count++] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
    }

    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (!DeleteApprovedValue(item.hive, subKeys[i], item.realName)) ok = false;
        if (!item.displayName.empty() &&
            _wcsicmp(item.displayName.c_str(), item.realName.c_str()) != 0) {
            if (!DeleteApprovedValue(item.hive, subKeys[i], item.displayName)) ok = false;
        }
    }
    return ok;
}

/**
 * @brief 把旧版本遗留下来的 xxx_disabled 值名改回原名（仅用于兼容）
 */
static bool RenameLegacyDisabledValue(const StartupItem& item) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(item.hive, item.subKey.c_str(), 0,
            KEY_READ | KEY_SET_VALUE | item.view, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    std::vector<BYTE> data;
    if (!QueryValueAlloc(hKey, item.realName.c_str(), type, data)) {
        RegCloseKey(hKey);
        return false;
    }

    // 原名已被占用时绝不能覆盖（可能是一个仍启用的不同启动项）
    DWORD existType = 0;
    if (RegQueryValueExW(hKey, item.displayName.c_str(), NULL, &existType, NULL, NULL) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return false;
    }

    LONG setRet = RegSetValueExW(hKey, item.displayName.c_str(), 0, type,
        data.empty() ? NULL : data.data(), (DWORD)data.size());
    if (setRet != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return false;
    }

    LONG delRet = RegDeleteValueW(hKey, item.realName.c_str());
    if (delRet != ERROR_SUCCESS) {
        // 回滚，避免同一个值以两个名字同时存在（等于没禁用/没恢复）
        RegDeleteValueW(hKey, item.displayName.c_str());
        RegCloseKey(hKey);
        return false;
    }
    RegCloseKey(hKey);
    return true;
}

/**
 * @brief 禁用指定的启动项
 */
bool DisableStartupItem(const StartupItem& item) {
    if (!item.isEnabled) return true;

    if (!item.isRegistry) {
        std::wstring newPath = item.path + L".disabled";
        return MoveFileW(item.path.c_str(), newPath.c_str()) != FALSE;
    }

    // 旧版本遗留的改名状态：值名已带后缀，Windows 仍会执行它，但本程序视为已禁用。
    // 恢复时走 RenameLegacyDisabledValue 即可，不需要再次禁用
    if (EndsWith(item.realName, L"_disabled")) return true;

    // RunOnce / 策略键：StartupApproved 不生效，必须移入备份区
    if (UsesBackupStore(item)) return MoveStartupValueToBackup(item);

    // 普通 Run：写入 StartupApproved 禁用记录（与任务管理器同一机制），
    // 不能只把值名改成 xxx_disabled——Run 键会执行所有值，改名根本不起作用
    return SetStartupApprovedDisabled(item);
}

/**
 * @brief 恢复（启用）指定的已禁用启动项
 */
bool EnableStartupItem(const StartupItem& item) {
    if (item.isEnabled) return true;

    if (item.isBackedUp) {
        return RestoreStartupValueFromBackup(item);
    }

    if (!item.isRegistry) {
        std::wstring original;
        bool renamed = true;
        if (TryStripDisabledSuffix(item.path, original)) {
            if (GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
            renamed = MoveFileW(item.path.c_str(), original.c_str()) != FALSE;
            if (!renamed) return false;
        }
        // 任务管理器禁用过的文件项文件名没有后缀，必须清除 StartupApproved 记录
        return ClearStartupApproved(item, true);
    }

    if (EndsWith(item.realName, L"_disabled")) {
        if (!RenameLegacyDisabledValue(item)) return false;
        // 只有普通 Run 的禁用状态来自 StartupApproved。RunOnce/策略键的同名
        // 记录属于另一个真正被禁用的 Run 项，不能在这里顺手清掉（会把别人启用）
        if (!IsPlainRunKey(item.subKey)) return true;
    }
    // 已禁用状态可能来自 StartupApproved；不清除的话恢复后仍不会开机启动
    return ClearStartupApproved(item, false);
}
