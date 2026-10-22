# SystemCleaner 自测用例清单（最终结果）

> 自测方式：
> - **UT**：`tests/baseline_tests.cpp`（控制台工程，直接包含 `utils.cpp` 被测源码，可自动化运行，336 项断言）
> - **FUNC**：`tests/functional_tests.cpp`（真实文件系统 / HKCU 注册表 / 进程与快捷方式行为测试，246 项断言，测试数据自动清理）
> - **GUI**：`tests/gui_tests.cpp`（控制台宿主进程，真实创建主窗口，驱动 Tab 显隐矩阵、按钮命令流转、`WM_*_DONE` 64 位消息解析、网络测速完整流程、强力卸载护栏、注册表清理护栏等交互，1126 项断言）。编译期用宏把 `MessageBoxW` 替换为桩函数消除模态阻塞，可在非提升环境自动化运行。
> - **静态**：源码走查 + 编译诊断（发现即修复）
> - **UI**：需管理员权限手工验证的破坏性操作（真正卸载/清理/粉碎），标注为 `MANUAL`
>
> 编译验证：Release|x64、Release|Win32、Debug|x64、Debug|Win32 四个配置全部 0 error。
> 自动化回归：`baseline_tests.exe` → **Total: 336  Failed: 0**；`functional_tests.exe` → **Total: 246  Failed: 0**；`gui_tests.exe` → **Total: 1126  Failed: 0**

## 1. 安全护栏（最高优先级 —— 误删系统文件即致命）

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| SEC-01 | `IsSystemCriticalPath("C:\Windows\System32\drivers\etc\hosts")` | true | UT | ✅（原 FAIL：反斜杠形式漏判，已修） |
| SEC-02 | `IsSystemCriticalPath("C:\Windows\System32")` | true | UT | ✅ |
| SEC-03 | `IsSystemCriticalPath("C:\Windows\Temp")` | false（可清理） | UT | ✅ |
| SEC-04 | Windows 装在非 C 盘时系统目录仍被保护 | true | UT | ✅（原 FAIL：硬编码 `C:\`，改为 `GetWindowsDirectoryW`/`SystemDrive` 动态前缀） |
| SEC-05 | `IsSystemCriticalPath("C:\Windows\System323\x")` | false（不前缀误伤） | UT | ✅ |
| SEC-06 | 短文件名 `C:\PROGRA~1\...` 被识别为受保护 | true | UT | ✅（原 FAIL：新增 `GetLongPathNameW` 8.3 展开） |
| SEC-07 | `IsPathTraversal("C:\a\..\..\Windows")` | true | UT | ✅ |
| SEC-08 | `IsPathTraversal("C:\a\b..c")` | false | UT | ✅ |
| SEC-09 | 正斜杠形式 `C:/Windows/System32` 也必须受保护 | true | UT | ✅（原 FAIL：可绕过护栏，已加路径规范化） |
| SEC-10 | 尾部反斜杠 / 大小写混写形式 | 判定一致 | UT | ✅ |
| SEC-11 | 残留目录名含 `..`、分隔符、盘符、通配符、保留设备名 | 拒绝删除 | UT | ✅（原 FAIL：未校验，新增 `IsSafeResidualName`） |

## 1b. 强力卸载安全护栏（本轮新增，`baseline_tests.exe`）

> 强力卸载会「递归删目录 + 删服务键 + 删 .sys 文件」，误判一处即蓝屏或整盘数据丢失。
> 本节全部为纯函数 UT，直接调用 `utils.cpp` 中的实现。

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| VENDOR-01 | `IsSharedVendorDirName("Microsoft")` / `"Google"` / `"Temp"` / `"Packages"` | true（公共/厂商目录名） | UT | ✅ |
| VENDOR-02 | `IsSharedVendorDirName("MyApp")` | false（真实软件目录放行） | UT | ✅ |
| VENDOR-03 | 空串 / 纯空格 / 纯点 | true（会拼出 AppData 本身） | UT | ✅ |
| VENDOR-04 | 大小写、尾点尾空格形式 `"MICROSOFT."` | true（归一化后仍命中） | UT | ✅ |
| UNDER-01 | `IsPathUnderDirectory("C:\A\B", "C:\A")` | true | UT | ✅ |
| UNDER-02 | `IsPathUnderDirectory("C:\AB", "C:\A")` | false（原 FAIL：`find` 子串匹配误判） | UT | ✅ |
| UNDER-03 | 正斜杠 / 大小写 / 尾斜杠形式 | 判定一致 | UT | ✅ |
| UNDER-04 | parent 为空 | false（空串是任意串前缀） | UT | ✅ |
| ENVX-01 | `ExpandEnvVars("%SystemRoot%\System32")` | 展开为真实系统目录 | UT | ✅ |
| ENVX-02 | 无 `%` 的普通串 | 原样返回 | UT | ✅ |
| ENVX-03 | 超长输入（超出展开缓冲区） | 返回 false 且保留原值，不截断 | UT | ✅ |
| CMD-01 | `"C:\Program Files\App\unins000.exe" /S` | 正确拆出 exe 与参数 | UT | ✅ |
| CMD-02 | `C:\Program Files\App\unins000.exe /S`（含空格无引号） | 取最长真实存在的文件前缀（原 FAIL：截成 `C:\Program`） | UT | ✅ |
| CMD-03 | `msiexec.exe /x {GUID} /qn` | exe=`msiexec.exe` | UT | ✅ |
| CMD-04 | 空串 / 纯空白 | false | UT | ✅ |
| SHROOT-01 | `IsSharedProgramRootPath("%ProgramFiles%")`、`%LOCALAPPDATA%`、`%ProgramData%` | true | UT | ✅ |
| SHROOT-02 | `X:\Users` 用户配置根 | true（原 FAIL：只挡 profile 本身，漏掉整个用户根） | UT | ✅ |
| SHROOT-03 | `C:\Program Files\Google\Chrome` | false（真实应用目录放行） | UT | ✅ |
| SAFEDEL-01 | 空 / `C:\` / `C:` / `D:\` | false | UT | ✅ |
| SAFEDEL-02 | UNC 共享根 `\\server\share`、主机 `\\server` | false（原 FAIL：`NormalizePathForCompare` 折叠掉前导 `\\`，整棵网络共享可被删） | UT | ✅ |
| SAFEDEL-03 | `C:\Windows`、`System32`、`C:\Program Files` | false | UT | ✅ |
| SAFEDEL-04 | `..\..\Windows` 遍历绕过 | false | UT | ✅ |
| SAFEDEL-05 | 实际系统盘根 / `X:\Users`（非硬编码 C:） | false | UT | ✅ |
| SYSCMP-01 | `\SystemRoot\...`、`\System32\...`、`%SystemRoot%\...` | true | UT | ✅ |
| SYSCMP-02 | 空 ImagePath / 非绝对路径（`svchost.exe -k ...`） | true（最保守） | UT | ✅ |
| SYSCMP-03 | `\??\C:\Windows\System32\drivers\x.sys` | true（原 FAIL：不认设备前缀） | UT | ✅ |
| SYSCMP-04 | `C:\Program Files\App\app.sys`（含引号/参数形式） | false（原 FAIL：`\??\` 第三方驱动被误判为系统组件） | UT | ✅ |
| DRVPATH-01 | `\SystemRoot\System32\drivers\ntfs.sys` | 展开为真实 Windows 目录 | UT | ✅ |
| DRVPATH-02 | `\??\` / `\\.\` / `\\?\` 设备前缀 | 正确剥离（原 FAIL：`L"\\.\"` 字面量只有 3 字符，前缀从未匹配） | UT | ✅ |
| DRVPATH-03 | `svchost.exe -k netsvcs`、`drivers\x.sys`、`C:x` | 返回空串（不得当当前目录文件删） | UT | ✅ |
| DRVPATH-04 | 未加引号含空格的真实文件路径 | 完整保留，不截成 `C:\Program` | UT | ✅ |
| DRVPATH-05 | 含空格但整串都不存在 | 保留整串（宁可因不存在而跳过删除） | UT | ✅ |
| REGDEL-01 | `SOFTWARE`、`SYSTEM`、`SYSTEM\CurrentControlSet` | false（容器键禁止整棵删除） | UT | ✅（原缺失：注册表侧无删除总闸） |
| REGDEL-02 | `SYSTEM\CurrentControlSet\Services`、`Control`、`Enum` | false | UT | ✅ |
| REGDEL-03 | `SOFTWARE\Classes`、`SOFTWARE\Microsoft\Windows\CurrentVersion`、`...\Uninstall`、`...\App Paths` | false | UT | ✅ |
| REGDEL-04 | HKCU `...\CurrentVersion\Run`、`Environment`、`Control Panel` | false | UT | ✅ |
| REGDEL-05 | 大小写 / 正斜杠 / 尾斜杠 / 首尾空白形式 | 一律拦下（按段比较 + 归一化） | UT | ✅ |
| REGDEL-06 | `...\Uninstall\MyApp`、`...\Services\SCTestDrv`、`ServicesMyApp` | true（真实条目与同名兄弟键放行） | UT | ✅ |
| REGDEL-07 | `SOFTWARE\Classes\.exe`、`SOFTWARE\Policies\Microsoft\Windows\System`、`...\Policies\System`、`...\ProfileList\S-1-5-21-1`、`...\ShellIconOverlayIdentifiers\x`、HKCU `...\FileExts\.txt` | false（原 FAIL：闸门只比「整串是否等于容器键」，深层子键全部放行，删掉即文件关联失效/组策略被清/用户配置损坏） | UT | ✅ |
| REGDEL-08 | `SYSTEM\CurrentControlSet\Services\Tcpip\Parameters`、`...\Services\ntfs\Security` | false（关键服务内部，即使在 Services 白名单下也拦） | UT | ✅ |
| REGDEL-09 | `SOFTWARE\WOW6432Node\...\Uninstall\OldApp`、`...\App Paths\tool.exe` | true（32 位视图真实条目可清理） | UT | ✅ |
| REGDEL-10 | `SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion` | false；`SOFTWARE\WOW6432Node\Whatever\Deep\Key` | true（第三方 32 位键等价 `SOFTWARE\Whatever`） | UT | ✅ |
| REGDEL-11 | hive 为 `NULL` / 栈垃圾句柄 | false（原 FAIL：非 HKCU 一律当 HKLM 放行） | UT | ✅ |
| REGDEL-12 | `HKEY_USERS\S-1-5-21-1\Software\...\Uninstall\App` | true；`...\Software\Classes\.exe` | false（原 FAIL：HKU 路径以 SID 开头，相对 hive 的保护名单永不命中） | UT | ✅ |

## 1c. 注册表清理判定与路径映射（本轮新增，`baseline_tests.exe`）

> 注册表清理页（Tab 3）扫描「卸载残留 / 启动残留」并递归删除。误判一条即可能删掉仍在使用的软件记录或系统组件。

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| REGPATH-01 | `RegRealPath(HKLM, SOFTWARE\...\Uninstall\App, KEY_WOW64_32KEY)` | 映射为 `SOFTWARE\WOW6432Node\...`（原 FAIL：展示/备份用未加前缀路径，用户按列表找不到键，`reg export` 导出的还是 64 位视图） | UT | ✅ |
| REGPATH-02 | 64 位视图 / `view=0` / 非 HKLM / 非 `SOFTWARE` 开头 | 原样返回 | UT | ✅ |
| REGPATH-03 | 已带 `WOW6432Node` 前缀 | 幂等，不二次叠加 | UT | ✅ |
| REGEXIST-01 | `NULL` hive / 空路径 | false | UT | ✅ |
| REGEXIST-02 | `HKCU\Software`、`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion` | true | UT | ✅ |
| REGEXIST-03 | 不存在的键 | false（区分「删除失败」与「本来就不存在」） | UT | ✅ |
| LEFTOVER-01 | `InstallLocation` 写了却不存在 | true（残留） | UT | ✅ |
| LEFTOVER-02 | `InstallLocation` 目录仍存在 | false（即便卸载器缺失） | UT | ✅ |
| LEFTOVER-03 | `InstallLocation` 指向文件 / 尾斜杠 / 含通配符 | 分别是残留 / 归一化后非残留 / 不判定 | UT | ✅ |
| LEFTOVER-04 | 无 `InstallLocation`，卸载器绝对路径不存在 | true（原 FAIL：只看 `InstallLocation`，大量不写该值的 MSI 残留完全扫不到） | UT | ✅ |
| LEFTOVER-05 | `msiexec.exe /x {GUID}`、`Windows\Installer\*.Msi`、相对路径卸载器 | false（卸载器是系统程序/缓存清理属正常，不构成残留信号） | UT | ✅ |
| LEFTOVER-06 | 位置与命令都为空 / 纯空白 | false（无法判定，保守放过） | UT | ✅ |
| SYSCOMP-05 | `SystemComponent=1` / `ReleaseType` 非空 / `ParentKeyName` 非空 | true（系统组件与 MSI 补丁，原 FAIL：完全未排除，`InstallLocation` 缺失即被列进清理列表，删后组件无法卸载/修复） | UT | ✅ |
| STARTUPCMD-01 | 空命令 / 绝对路径不存在（含带参、带引号） | true（无效启动项，原 FAIL：valueName 分支是死代码，启动项残留根本扫不到） | UT | ✅ |
| STARTUPCMD-02 | 绝对路径存在 / 相对命令名 / 含通配符 / 未定义变量 | false（放过） | UT | ✅ |

## 2. 文件大小 / 数值计算

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| NUM-01 | `high=1, low=0`（4 GiB） | 4294967296 | UT | ✅（原 FAIL：`high*MAXDWORD+low` 少算，改 `FileSizeFromFindData`） |
| NUM-02 | `high=0, low=1000` | 1000 | UT | ✅ |
| NUM-03 | `high=3, low=7` | 12884901895 | UT | ✅ |
| NUM-04 | `FormatSize(0)` | `0 B` | UT | ✅ |
| NUM-05 | `FormatSize(1024)` | `1.00 KB` | UT | ✅ |
| NUM-06 | `FormatSize(1024^3)` | `1.00 GB` | UT | ✅ |
| NUM-07 | `FormatSize(-1)` | 不崩溃、不显示负数 | UT | ✅（原 FAIL：负数产生乱码单位，已钳位） |

## 3. 受保护数据文件

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| PROT-01 | `IsProtectedDataFile("login.db")` | true | UT | ✅ |
| PROT-02 | `IsProtectedDataFile("thumbcache_abc.db")` | false | UT | ✅ |
| PROT-03 | `IsProtectedDataFile("a.log")` | false | UT | ✅ |
| PROT-04 | `IsProtectedDataFile("config.json")` | true | UT | ✅ |

## 4. 子目录通配匹配

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| GLOB-01 | `MatchSubDirPattern("Default\Cache", "*\Cache")` | true | UT | ✅ |
| GLOB-02 | `MatchSubDirPattern("Cache", "*\Cache")` | false | UT | ✅ |
| GLOB-03 | `MatchSubDirPattern("A\B\Cache", "*\Cache")` | true | UT | ✅ |
| GLOB-04 | `ParseSubDirPattern("a\b/c")` | 3 段 | UT | ✅ |

## 5. 只读文件处理（清理有效性）

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| RO-01 | 只读文件 `IsFileLocked()` | false（只读 ≠ 被占用） | UT | ✅（原 FAIL：`ACCESS_DENIED` 一律当占用） |
| RO-02 | 删除只读缓存文件 | 成功 | UT | ✅（原 FAIL：只读文件永远删不掉，新增 `ForceDeleteFile`） |
| RO-03 | 真正被其他进程占用的文件 | 判定为占用并跳过 | UT | ✅ |

## 6. 缓冲区安全（`*_s` 溢出即进程终止）

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| BUF-01 | 卸载/强卸确认框拼接超长软件名 | 不崩溃 | 静态 | ✅（原 FAIL：`wchar_t[512]/[1024]`+`wcscat_s`，改 `std::wstring`） |
| BUF-02 | `KillProcessesInDirectory` 传入 300 字符路径 | 不崩溃 | 静态 | ✅（原 FAIL：`wcscpy_s` 触发 invalid parameter 终止，改动态串） |
| BUF-03 | `RemoveServicesInDirectory`/`CleanAppPathsEntry` 超长路径 | 不崩溃 | 静态 | ✅（同上） |
| BUF-04 | `ExecuteUninstallCommand` 解析超长 `UninstallString` | 不崩溃、安全截断 | 静态 | ✅（改用 `SafeCopy`） |
| BUF-05 | `DeleteMatchingShortcuts` 扩展名长于缓冲区 | 不崩溃 | 静态 | ✅（原 `wcscpy_s(ext, dot)` 溢出，改 `_wcsicmp`） |
| BUF-06 | `RefreshUninstallList` 显示超长软件名 | 不崩溃 | 静态 | ✅ |
| BUF-07 | `WriteUninstallFailureLog` 路径接近 MAX_PATH | 不崩溃 | 静态 | ✅（去掉 `wcscat_s`） |
| BUF-08 | `_wsplitpath_s` 重复文件路径 | 不崩溃 | 静态 | ✅（改动态长度缓冲区） |

## 7. 未初始化 / 生命周期 / 并发

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| MEM-01 | `ScanInstalledDrivers` 的 `regViewAccess` | 确定值 0 | 静态 | ✅（原 FAIL：未赋值，强删时用栈垃圾） |
| MEM-02 | `WM_CLEAN_DONE` 自动重扫期间取消按钮 | 可用 | 静态 | ✅（原 FAIL：保持禁用，无法取消） |
| MEM-03 | `EnableStartupItem` 文件夹项名称异常 | 不崩溃 | 静态 | ✅（原 FAIL：`substr(len-9)` 下溢，改 `TryStripDisabledSuffix`） |
| MEM-04 | `SpeedTestThreadProc` PostMessage 失败 | 不泄漏 | 静态 | ✅（新增 `PostSpeedUpdate/PostSpeedDone` 失败即 `delete`） |
| MEM-05 | 扫描/清理线程读取 ListView 勾选状态 | 不跨线程访问控件 | 静态 | ✅（新增 `CollectScanSnapshot`/`CollectCleanSnapshot` UI 线程快照） |
| MEM-06 | 快速切换筛选/搜索导致并发扫描 | 旧线程结果不写入列表 | 静态 | ✅（原 FAIL：线程启动即 `g_uninstallScanCancelled=false` 覆盖取消请求；改为代际 + `thread_local`） |
| MEM-07 | `WM_DESTROY` 时工作线程仍在运行 | 不删除使用中的临界区 | 静态 | ✅（原 FAIL：`DeleteCriticalSection` UB） |
| MEM-08 | 点击「强制卸载」 | 线程句柄被关闭 | 静态 | ✅（原 FAIL：句柄泄漏） |
| MEM-09 | `ExecuteCmd` 子进程挂死 | 5 分钟超时后终止 | 静态 | ✅（原 FAIL：UI 永久等待） |

## 8. 消息 / 交互逻辑

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| MSG-01 | 注册表清理完成后自动重扫 | 触发扫描 | 静态 | ✅（原 FAIL：`PostMessageW(hwnd,5001,...)`，改 `WM_COMMAND+MAKEWPARAM`） |
| MSG-02 | 切离「网络工具」页 | 「Hosts文件编辑：」标签隐藏 | 静态 | ✅（原 FAIL：4006 未纳入 `SwitchTab`） |
| MSG-03 | 切离「强力卸载」页 | 「类型:」标签隐藏 | 静态 | ✅（原 FAIL：7010 未纳入） |
| MSG-04 | 工具页反复切换功能 5 次 | 列数保持不变 | 静态 | ✅（原 FAIL：列累积重复，新增 `DeleteToolListColumns`） |
| MSG-05 | 工具页多选 3 项点「删除选中」 | 处理 3 项 | 静态 | ✅（原 FAIL：`LVS_SINGLESEL` 只能选 1 项，已移除） |
| MSG-06 | 系统信息页重复刷新 | 不出现重复行 | 静态 | ✅（`PopulateSystemInfo` 先清空列表与列） |

## 9. 扫描 / 清理正确性

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| SCAN-01 | HKCU 与 HKLM 同名同值启动项 | 两条都显示 | 静态 | ✅（原 FAIL：去重 key 缺 hive） |
| SCAN-02 | 系统信息「CPU 厂商」 | 有值 | 静态 | ✅（原 FAIL：`cpuSize` 复用未重置） |
| SCAN-03 | 显卡型号检测 | 任意机器可检出 | 静态 | ✅（原 FAIL：硬编码 GUID + 枚举错句柄，改按 `ClassGUID` 过滤 `Control\Video`） |
| SCAN-04 | 核心数 vs 线程数 | 来源不同 | 静态 | ✅（核心数取 `CentralProcessor\Core Information`，线程数取 `dwNumberOfProcessors`） |
| SCAN-05 | 首尾相同、中间不同的两个文件 | 判为不重复 | 静态 | ✅（原 FAIL：采样哈希误判 → 用户删错文件；改全量 128-bit 摘要） |
| SCAN-06 | 符号链接环 / 超深目录 | 不栈溢出 | 静态 | ✅（全部递归扫描加深度上限 + 跳过 reparse point） |
| SCAN-07 | 回收站清空失败 | 不虚报释放空间 | 静态 | ✅（改为清空后二次统计差值） |
| SCAN-08 | 磁盘清理扫描目录与删除基准目录 | 一致 | 静态 | ✅（原 FAIL：`path` vs `g_toolCurrentPath` 不一致会删错文件） |
| SCAN-09 | 自定义路径扫出的失效快捷方式 | 可删除 | 静态 | ✅（原 FAIL：按文件名回退到 4 个默认根目录查找，改 lParam 记录完整路径） |
| SCAN-10 | `DetermineAppType` 环境变量读取失败 | 不误判为系统软件 | 静态 | ✅（原 FAIL：`find("")==0` 使全部软件判为「系统软件」） |
| SCAN-11 | 清理项路径在系统盘非 C: 的机器上 | 指向真实系统目录 | 静态 | ✅（`InitCleanItems` 全部改动态 `sysRoot`/`ProgramData`） |
| SCAN-12 | `MEMORY.DMP` / `Minidump` 定位 | 使用真实系统盘 | 静态 | ✅ |
| SCAN-13 | hosts 文件路径 | 使用真实系统盘 | 静态 | ✅（原 FAIL：硬编码 `C:\Windows`） |
| SCAN-14 | 强删目录遇到只读文件 | 能删除 | 静态 | ✅（原 FAIL：`ACCESS_DENIED` 当占用跳过，目录残留） |
| SCAN-15 | 强删目录遇到 junction/符号链接 | 只断开链接，不进入目标 | 静态 | ✅（原 FAIL：会顺着链接删到目录外真实数据） |
| SCAN-16 | 注册表备份目录 | 位于 exe 目录，可写 | 静态 | ✅ |
| SCAN-17 | 清理失败日志重复写入 | 时间戳格式正确、BOM 不重复 | 静态 | ✅ |

## 10. 编译 / 静态诊断

| 编号 | 用例 | 预期 | 方式 | 结果 |
|---|---|---|---|---|
| BLD-01 | Release \| x64 | 0 error | 编译 | ✅ `x64\Release\SystemCleaner.exe` |
| BLD-02 | Release \| Win32 | 0 error | 编译 | ✅ `Release\SystemCleaner.exe` |
| BLD-03 | Debug \| x64 | 0 error | 编译 | ✅ `x64\Debug\SystemCleaner.exe` |
| BLD-04 | Debug \| Win32 | 0 error | 编译 | ✅ `Debug\SystemCleaner.exe` |
| BLD-05 | `baseline_tests.exe` 自动化回归 | Failed = 0 | UT | ✅ Total 336 / Failed 0 |
| BLD-06 | `gui_tests.exe` GUI 交互自动化回归 | Failed = 0 | GUI | ✅ Total 1126 / Failed 0 |
| BLD-07 | `functional_tests.exe` 功能行为回归 | Failed = 0 | FUNC | ✅ Total 246 / Failed 0 |

## 11. GUI 交互自动化（`gui_tests.exe`，真实窗口驱动）

> 宿主进程真实注册窗口类并创建主窗口（置于屏幕外但可见，使子控件 `IsWindowVisible` 有效），
> 用 `PostMessage(WM_COMMAND)` / `SendMessage` 驱动按钮命令，用 `IsWindowVisible` / `GetWindowText` 校验交互结果。
> `MessageBoxW` 在编译期被替换为桩函数（返回 -1），破坏性确认框一律走安全取消分支，可在非提升环境全自动运行。

| 编号 | 用例 | 预期 | 结果 |
|---|---|---|---|
| GUI-01 | 遍历 Tab 0–6 的控件显隐矩阵 | 每页只显示本页控件，其余全部隐藏 | ✅ |
| GUI-02 | 4006「Hosts文件编辑：」标签 | 仅网络页可见，切走即隐藏 | ✅ |
| GUI-03 | 测速进度条 | 仅「网络页 且 正在测速」时可见 | ✅ |
| GUI-04 | 全选/反选/全不选 + 扫描快照 | 勾选数与 `CollectScanSnapshot()` 一致 | ✅ |
| GUI-05 | `WM_SCAN_DONE` 经 LPARAM 堆指针传 5 GB | 状态栏显示「5.00 GB」，不被截断为 32 位 | ✅（原 FAIL：读 WPARAM 高 32 位丢失，显示 0 B 且泄漏堆） |
| GUI-06 | `WM_SCAN_DONE` lParam=NULL | 不崩溃 | ✅ |
| GUI-07 | `WM_CLEAN_DONE` 取消分支 | 状态「清理已取消」，标志复位 | ✅ |
| GUI-08 | `WM_CLEAN_DONE` 空快照分支 | 状态「清理完成」，不启动重扫 | ✅ |
| GUI-09 | 勾选后被筛选隐藏的项触发卸载 | 隐藏项 `isChecked` 被复位，不误卸载 | ✅（原 FAIL：仅同步可见项，隐藏项残留勾选被一并卸载） |
| GUI-10 | 工具页功能切换（粉碎 vs 磁盘清理） | 各自控件正确显隐，互不残留 | ✅ |
| GUI-11 | 离开工具页 | 全部工具控件（含动态）隐藏 | ✅ |
| GUI-12 | 投递安全命令 / 无勾选触发卸载 | 消息循环存活，不进入危险状态 | ✅ |
| GUI-13 | 网络测速完整流程（`SpeedElapsedToProgress` 纯函数 + 真实测速线程端到端） | 进度条多次推进至 100%、进入上行阶段；结束结果明确「测速完成」或「测速失败+原因」，不得上下行同时为 0 | ✅（原 FAIL：进度/速度更新挂在网络读取成功之后，测速源停服时一次更新都不发出，进度条与速度全程不动、结果全 0） |
| GUI-13b | `SpeedElapsedToProgress` 进度换算 | 0ms→1%、半程→25%、下行末→49%、上行首→51%、结束→100%，且单调不减 | ✅ |

### 11b. 强力卸载护栏（本轮新增，真实注册表键 + 真实临时文件）

> `gui_tests.cpp` 直接 include 了 `uninstaller.cpp`，因此其中的 `static` 函数
> （`IsUsableInstallLocation` / `CleanDriverResiduals` / `CleanAppResiduals`）可被直接调用；
> 测试在 `HKCU\SOFTWARE\SystemCleanerTest` 下自建自删注册表键，不触碰真实系统服务。

| 编号 | 用例 | 预期 | 结果 |
|---|---|---|---|
| GUI-DRV-01 | 默认构造的 `UninstallItem` | `regHive==NULL`、`regViewAccess==0`、路径为空、无勾选/系统标记（索引失效时不会被当成真句柄传给 `RegDeleteTreeW`） | ✅ |
| GUI-DRV-02 | 对 `type="驱动"` 条目调用常规卸载 | 返回 false、记录「驱动条目不支持常规卸载」、服务键原样保留 | ✅（原 FAIL：驱动条目走目录式清理，把 .sys 文件当目录递归删） |
| GUI-DRV-03 | 强制卸载 `isSystemComponent=true` 的驱动 | 返回 false、记录「Windows 自带组件」、服务键保留 | ✅ |
| GUI-DRV-04 | 强制卸载第三方驱动（含空格绝对 ImagePath） | 返回 true、.sys 文件被删、服务键被删、不触碰任何目录 | ✅ |
| GUI-LOC-01 | `IsUsableInstallLocation` 对空 / `C:\` / winDir / System32 / 普通目录 / 文件路径 | 依次 false/false/false/false/true/false | ✅ |
| GUI-LOC-02 | `installLocation` 被写成 Windows 目录 | 返回 false、记录「安装位置不是可安全删除的目录」、Windows 目录完好 | ✅（原 FAIL：静默跳过，用户以为已清理） |
| GUI-LOC-03 | 正常安装目录 + 厂商名残留目录（keepUserData=false） | 两者均被真正删除 | ✅ |
| GUI-LOC-04 | keepUserData=true 且安装目录位于用户配置目录内 | 安装目录保留 | ✅ |
| GUI-CMD-01 | 空 / 纯空白 / 不存在的程序 | 返回 -1 | ✅ |
| GUI-CMD-02 | `cmd.exe /c exit 7` | 返回 7（退出码原样传回） | ✅（原 FAIL：`WAIT_TIMEOUT` 时把 `STILL_ACTIVE=259` 当退出码返回） |
| GUI-CMD-03 | 含空格且未加引号的真实批处理 | 能启动且退出码正确（原 FAIL：按第一个空格截断成 `...\my` 直接失败） | ✅ |
| GUI-UI-01 | 勾选全为系统组件后点「强制卸载」 | 不进入卸载状态、弹「禁止操作」并点明系统组件 | ✅ |
| GUI-UI-02 | 混合勾选（系统组件 + 普通软件） | 确认文案含「将被自动跳过」与「不会调用软件自带的卸载程序」；取消后不进入卸载状态 | ✅ |
| GUI-UI-03 | 未勾选任何项点「强制卸载」 | 提示「请先勾选」，不进入卸载状态 | ✅ |
| GUI-UI-04 | 卸载进行中切换类型筛选（7005） | `g_uninstallItems` 不被清空、不安排 debounce 重扫（否则工作线程索引失效） | ✅（原 FAIL：立即 `ListView_DeleteAllItems` + 重扫） |
| GUI-UI-05 | 卸载进行中点「刷新扫描」（7001） | 同样不触发重扫清空数据模型 | ✅ |

> 说明：GUI-13 端到端默认把单阶段时长压缩到 1.2s（`g_speedPhaseMs`）以快速跑完；设环境变量 `SPEEDTEST_FULL=1` 可按真实 10s 阶段运行，额外校验下行速度非 0（压缩时长下 TLS 握手会吃满下行窗口）。

### 11c. 注册表清理护栏（本轮新增，`gui_tests.exe`）

> GUI 宿主以非管理员运行，`CleanSelectedRegistry` 的管理员分支不进入；
> 本节覆盖的是「删除动作之前」的全部前置护栏——它们恰是防误删的关键，且必须在权限检查之前生效。

| 编号 | 用例 | 预期 | 结果 |
|---|---|---|---|
| GUI-REGCLEAN-01 | 空列表点「清理选中」(5002) | 只弹一个提示框，文案「请至少选择一项」 | ✅ |
| GUI-REGCLEAN-02 | ListView 行数与 `g_regCleanItems` 不一致时清理 | 拒绝并提示「列表与扫描结果已不同步」，绝不按下标乱删 | ✅（原 FAIL：行/数据下标错位，「勾的是 A 删的是 B」） |
| GUI-REGCLEAN-03 | 全部取消勾选后清理 | 提示「请至少选择一项」 | ✅ |
| GUI-REGCLEAN-04 | `g_isRegScanning=true` 时清理 | 被拦住并提示「扫描正在进行」，不改动数据模型 | ✅（原 FAIL：工作线程 push_back 与 UI 按下标读并发，可越界） |
| GUI-REGCLEAN-05 | 视图来源 | 删除用 `item.view`，与 `category` 中文字串无关 | ✅（原 FAIL：靠 `category.find("[32位]")` 判视图，文案一改就用错视图） |
| GUI-REGCLEAN-06 | 有勾选且非管理员 | 命中「管理员权限」分支（而非「至少选择一项」/「不同步」） | ✅（原 FAIL：权限检查在最前，任何情况都只报权限不足） |
| GUI-REGCLEAN-07 | `WM_REG_SCAN_DONE(wParam=2,lParam=0)` | 列表 2 行且与数据一一对应，状态栏含「2」 | ✅ |
| GUI-REGCLEAN-08 | `WM_REG_SCAN_DONE(lParam=1)` 权限不足 | 弹一次提示，**不清空**已有列表与数据 | ✅（原 FAIL：扫描线程先 `clear` 再判权限，普通权限点一次扫描就把上次结果抹掉） |

## 12. 本轮修复的真实缺陷（源码走查 + GUI 验证）

| 编号 | 缺陷 | 影响 | 修复 |
|---|---|---|---|
| FIX-01 | `WM_SCAN_DONE`/`WM_CLEAN_DONE` 从 `wParam` 读 64 位总量 | 工作线程实际把总量放在 `LPARAM` 堆指针里；UI 读错位置 → 恒显示「0 B」，且堆指针泄漏 | 改从 `lParam` 取 `long long*`，三条退出路径均 `delete`；发送端 `new(std::nothrow)` + `PostMessage` 失败即 `delete` |
| FIX-02 | 「卸载选中」(7002)/「强制卸载」(7004) 只同步可见项 | 先勾选再被筛选/搜索隐藏的项残留 `isChecked=true`，会被一并卸载 | 命令处理先在锁内 `for (auto& it) it.isChecked=false` 全量复位，再按可见项回写 |
| FIX-03 | `RefreshRegCleanList` 用 `wchar_t buf[256]`+`wcscpy_s` | 注册表 keyPath（根前缀+子键）可超 256 → invalid parameter 终止进程 | 直接用 `keyPath.c_str()` 赋 `pszText` |
| FIX-04 | `LoadRegistryStartup` 遇 `ERROR_MORE_DATA` 即 `break` | 超长命令行(>1024)/超长值名会静默丢弃其后全部启动项 | 改为 `ERROR_NO_MORE_ITEMS` 才 break，其余错误 `index++; continue` |
| FIX-05 | 磁盘清理 `scanDir` 递归 | 无深度上限 + 不跳过 reparse point → 符号链接环栈溢出；固定 `wchar_t[MAX_PATH]` 搜索串溢出 | 加 `depth>40` 上限、跳过 `FILE_ATTRIBUTE_REPARSE_POINT`、搜索串改 `std::wstring` |
| FIX-06 | `WM_SPEED_DONE` 未判空即解引用 `lParam` | 发送端 `new` 失败或异常时崩溃 | 加 `if (result)` 判空后再用并 `delete` |
| FIX-07 | 测速进度/速度更新写在 `InternetReadFile` 成功之后 | 测速源不可达或链路卡住时循环内一次 `PostMessage` 都不发出：进度条与速度纹丝不动，结束时上下行全为 0 | 新增独立 ticker 线程按墙钟时间驱动进度与速度，UI 节奏与网络 IO 解耦；进度换算抽成纯函数 `SpeedElapsedToProgress` 便于单测 |
| FIX-08 | 唯一下行测速源 `speedtest.tele2.net` 已停服 | 连接成功但每个 HTTP 请求都失败，测速恒 0 且无任何提示 | 改为多源回退（Cloudflare / OVH / tele2），并在一个源都连不通时明确提示「无法连接到测速服务器」 |
| FIX-09 | `HttpSendRequestW` 对 4xx/5xx 也返回 TRUE | 持续返回 403 的坏源被当成「连通成功」，仍退化为「测速完成但速度 0」 | 发送后用 `HttpQueryInfoW(HTTP_QUERY_STATUS_CODE)` 判 `status>=400` 即视为该源失败并回退 |

## 13. 本轮修复的真实缺陷（强力卸载专项审计）

> 本轮聚焦「强力卸载 / 残留清理」，用新增的 UT + GUI 用例反向暴露缺陷，全部已修复并被回归覆盖。

| 编号 | 缺陷 | 影响 | 修复 |
|---|---|---|---|
| FIX-10 | `ExecuteUninstallCommand` 按第一个空格切分 `UninstallString` | `C:\Program Files\App\unins000.exe /S`（注册表里没加引号，真实存在）被截成 `C:\Program` → 卸载直接失败 | 改用 `SplitUninstallCommand` 取「最长的真实存在的文件前缀」，支持带引号 / 无引号含空格 / PATH 三种形式 |
| FIX-11 | `WaitForSingleObject` 超时后仍走 `GetExitCodeProcess` | 卸载程序弹框等人点击时，`STILL_ACTIVE(259)` 被当成「卸载程序返回 259」，且随后照常执行递归删除 → 把正在卸载的目录删掉 | 超时返回独立错误码 `-2`；调用方据此记录「卸载程序超时未退出，已跳过残留清理」并直接返回，不做任何破坏性操作 |
| FIX-12 | `ExecuteUninstallCommand` 无条件设置工作目录 | 目录不存在时 `ShellExecuteExW` 直接失败 | 仅当目录真实存在才设置 `lpDirectory`；缓冲区由固定小数组改为 `MAX_PATH*4` + `SafeCopy` |
| FIX-13 | `NormalizePathForCompare` 无条件折叠重复分隔符 | 前导 `\\` 被吃掉，`\\server\share` 变成 `\server\share`，`IsDriveOrShareRoot` 认不出 UNC 共享根 → **整棵网络共享目录可被递归删除** | 保留前导 `\\`（UNC 主机/共享与 `\\?\` 前缀），其余分隔符照常折叠 |
| FIX-14 | `IsSharedProgramRootPath` 只挡用户配置目录本身 | `X:\Users` 整个用户配置根不在黑名单 → 强卸时可整棵删除所有用户数据 | 追加 `CSIDL_PROFILE` 的父目录（用户配置根）到受保护根列表 |
| FIX-15 | `ResolveDriverFilePath` 设备前缀字面量写成 `L"\\.\"`（实际 3 字符） | `\??\`、`\\.\` 前缀从未被剥离，绝对路径解析失败 | 修正为 `L"\\??\\"` / `L"\\\\.\\"`，并补 `\\?\` 支持 |
| FIX-16 | `ResolveDriverFilePath` 对未加引号含空格的串直接按首个空格截断 | 截出的前缀（如 `C:\Program`）若恰好存在即是一个真实目录 → **删错对象** | 逐个空格前缀试探 `GetFileAttributesW`，取真实存在的文件前缀；一个都不存在时保留整串（宁可因路径不存在而跳过删除） |
| FIX-17 | `IsSystemComponentImagePath` 不认设备前缀、不解析引号 | `\??\C:\Program Files\App\x.sys` 这类第三方驱动被误判为「Windows 自带组件」，用户无法清理；反之相对路径判定也不稳 | 统一委托 `ResolveDriverFilePath` 解析后再与真实 Windows 目录做整段前缀比较 |
| FIX-18 | `ResolveDriverFilePath` 定义在 `uninstaller.cpp` 内 | `utils.cpp` 侧的 `IsSystemComponentImagePath` 无法复用，形成重复实现 | 迁移到 `utils.cpp`，`uninstaller.cpp` 改为调用 |
| FIX-19 | 注册表侧没有递归删除总闸（目录侧有 `IsSafeToDeleteDirectory`） | `DeleteRegistryKeyRecursive` / `RecursiveDeleteKeyEx` 拿到 `SOFTWARE`、`SYSTEM\CurrentControlSet\Services` 等容器键时会 `RegDeleteTreeW` 抹掉整棵子树（等价于重装系统） | 新增 `IsSafeToDeleteRegistryKey(hive, keyPath)`：归一化后按段比较 HKLM/HKCU 受保护容器键名单，两个删除入口均先过闸 |

## 14. 本轮修复的真实缺陷（注册表清理专项审计）

> 本轮聚焦「注册表清理」（Tab 3，扫描垃圾 5001 / 清理选中 5002），用新增 UT + GUI 用例反向暴露缺陷，全部已修复并被回归覆盖。

| 编号 | 缺陷 | 影响 | 修复 |
|---|---|---|---|
| FIX-20 | `IsSafeToDeleteRegistryKey` 只比「整串是否等于容器键」 | `SOFTWARE\Classes\.exe`、`...\Policies\System`、`...\ProfileList\*`、关键服务内部条目等深层键全部放行，删掉即文件关联失效 / 组策略被清 / 用户配置损坏 / 系统服务损坏 | 重写为分层规则集：整棵子树保护 → 白名单容器直接子项放行 → 受保护根等于判定 → 微软/系统容器祖先拒绝（均按段、忽略大小写） |
| FIX-21 | hive 白名单缺失 | 句柄为 `NULL` / 栈垃圾时被当成 HKLM 直接放行 | 新增 `IsSupportedRegistryHive`，未知句柄一律拒绝；`HKEY_USERS` 剥掉首段 SID 后按 HKCU 规则处理 |
| FIX-22 | 扫描只看 `InstallLocation` 非空且不存在 | 大量不写 `InstallLocation` 的 MSI 残留完全扫不到；`InstallLocation` 写成文件/`C:` 也误命中 | 新增 `IsUninstallEntryLeftover`：先看安装目录，缺失再看卸载器绝对路径；`msiexec` / `Windows\Installer` 缓存 / 相对路径 / 通配符一律放过 |
| FIX-23 | 未排除系统组件 / MSI 补丁 | `SystemComponent=1`、带 `ReleaseType`/`ParentKeyName` 的更新条目只要 `InstallLocation` 缺失就被列为残留，删后组件无法卸载/修复、Windows Update 记录错乱 | 新增 `IsSystemOrPatchUninstallEntry`，扫描阶段直接跳过 |
| FIX-24 | `valueName` 分支是死代码 | 启动项（Run/RunOnce）残留根本扫不到，而它恰是注册表清理最常见需求 | 新增 `ScanStartupValueView` + `IsInvalidStartupCommand`，扫描 HKCU/HKLM 的 Run/RunOnce；删除侧 `DeleteRegistryValue` 真正处理单值 |
| FIX-25 | 展示/备份用未加 `WOW6432Node` 前缀的路径 | 32 位视图残留：用户按列表路径找不到键；`reg export` 导出的其实是 64 位视图，备份对象与被删对象无关 | 新增 `RegRealPath`，扫描写入与备份均用真实路径（幂等，不二次叠加） |
| FIX-26 | 靠 `category.find(L"[32位]")` 中文字串判视图 | 文案一改（翻译/加空格/改标签）就用错视图：32 位残留删不掉，64 位同名键可能被误删 | `RegCleanItem` 新增 `REGSAM view`，扫描时记录、删除时直读 |
| FIX-27 | `RegScanThreadProc` 先 `clear` 再判权限 | 普通权限点一次扫描就把上次结果连同 ListView 抹掉，用户看到空列表以为系统很干净 | 权限不足时不 `clear`，`WM_REG_SCAN_DONE(lParam=1)` 保留旧列表与数据 |
| FIX-28 | 清理循环中 `ListView_DeleteItem` 与 vector 下标错位 | 删行后 ListView 行号前移而 vector 不变，后续 `ListView_GetCheckState(i)` 读到错项，「勾的是 A 删的是 B」 | 清理前校验 `ListView 行数 == vector 大小`，不一致直接拒绝并要求重扫 |
| FIX-29 | `BackupRegistry` 用 `ExecuteCmd` 且丢弃退出码 | `reg.exe` 失败（项不存在/目录不可写/被拦截）时照样提示「备份已保存」，用户以为可恢复其实没有 | 新增 `ExecuteCmdChecked`（取退出码）+ `BackupRegistryKey`（reg.exe 优先、回退 `RegSaveKeyExW`）；逐项备份，备份失败则跳过该项，绝不无备份删除 |
| FIX-30 | `RegSaveKeyExW` 未启用备份/还原特权 | 管理员令牌里 `SeBackupPrivilege`/`SeRestorePrivilege` 默认未启用，直接调用得 `ERROR_PRIVILEGE_NOT_HELD(1314)` | 新增 `EnableRegistryBackupPrivileges`，回退前 `AdjustTokenPrivileges` 启用两特权 |
| FIX-31 | 枚举子键/值用固定 `wchar_t[256]` | 超长键名（GUID、带路径的补丁键）使 `RegEnumKeyExW` 返回 `ERROR_MORE_DATA` 直接 break，漏掉该项及其后所有项（扫描漏报、删除删一半） | 改用可增长 `std::vector<wchar_t>`，`ERROR_MORE_DATA` 时扩容重试，枚举出错 `index++; continue` |
| FIX-32 | 删除失败与「键本来就不存在」混为一谈 | 一堆假失败混在结果里，真正没删掉的项看不出来 | 删除前 `RegKeyExistsInView` 探测，已不存在的项直接移除不计失败；结果弹窗区分成功/失败/备份失败跳过 |
| FIX-33 | `RefreshRegCleanList` 存 `&g_regCleanItems[i]` 到 lParam | vector 后续扩容/清空即悬空指针 | 改存下标 `(LPARAM)i` |

## 遗留 / 需人工确认

| 项 | 说明 |
|---|---|
| GUI 交互验证 | Tab 显隐矩阵、按钮命令流转、`WM_*_DONE` 64 位消息解析、勾选状态同步、工具页控件切换等**交互逻辑**已由 `gui_tests.exe` 在真实窗口上自动化覆盖（见第 11 节），无需手工点击。 |
| 破坏性操作 | 真正执行卸载 / 清理 / 文件粉碎 / 一键网络修复 / Hosts 写回 会改动系统，程序清单 `requireAdministrator`，当前非提升环境不执行；这些路径的**确认前逻辑**已由 GUI 测试覆盖（桩使确认框返回取消），**确认后效果**建议在提升后的桌面环境手工验证（MSG、SCAN-07/08/09 等）。 |
