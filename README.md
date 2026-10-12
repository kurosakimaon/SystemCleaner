# SystemCleaner

一个基于纯 Win32 API 开发的 Windows 系统清理与维护工具，单文件 EXE，无任何第三方依赖。

集成垃圾清理、开机启动项管理、右键菜单管理、注册表清理、网络工具、强力卸载和实用工具七大模块，界面无模态 Tab 布局，操作前有确认与备份护栏。

## 功能

| 模块 | 说明 |
| --- | --- |
| 垃圾清理 | 扫描并清理系统/用户临时文件、浏览器缓存（Chrome / Edge / Firefox）、缩略图缓存、系统日志、Windows 更新缓存、传递优化缓存、DirectX 着色器缓存、崩溃转储、Prefetch、回收站等。分为「安全清理」（默认勾选）、「谨慎清理」和「隐私数据」（默认不勾选）三档 |
| 开机启动项 | 枚举注册表启动项与启动文件夹，可启用 / 禁用 |
| 右键菜单 | 浏览与清理文件 / 文件夹右键菜单注册项 |
| 注册表清理 | 扫描无效注册表项，删除前自动用 `reg.exe` 导出 `.reg` 备份到 `reg_backup` 目录 |
| 网络工具 | 网络诊断与配置辅助（WinINet / IP 相关操作） |
| 强力卸载 | 读取已安装程序列表，支持常规卸载与卸载残留清理 |
| 实用工具 | 文件粉碎、系统信息、重复文件查找、进程管理、快捷方式修复、磁盘清理助手 |

## 技术栈

- C++20，纯 Win32 API + Common Controls（无 Qt / MFC / 第三方库）
- 依赖系统库：`comctl32`、`shell32`、`shlwapi`、`wininet`、`advapi32`、`psapi`、`ole32` 等（通过 `#pragma comment(lib, ...)` 自动链接）
- Unicode 编译，`/utf-8` 源码编码
- 清单要求管理员权限（`UACExecutionLevel = RequireAdministrator`）

## 环境要求

- Windows 10 / 11（x64 或 Win32）
- Visual Studio 2026（工具集 v145）+「使用 C++ 的桌面开发」工作负载
  - 工程使用 `PlatformToolset = v145`、`WindowsTargetPlatformVersion = 10.0`
  - 若使用 Visual Studio 2022，打开工程后把平台工具集改为 `v143` 即可（C++20 均支持）

## 编译

### 方式一：Visual Studio

1. 打开 `SystemCleaner.sln`（或新版格式的 `SystemCleaner.slnx`）
2. 选择配置（`Debug` / `Release`）与平台（`x64` / `Win32`），四个组合均可正常编译
3. 菜单「生成 → 生成解决方案」

### 方式二：命令行（Developer PowerShell / 开发者命令提示符）

```powershell
msbuild SystemCleaner.sln /p:Configuration=Release /p:Platform=x64 /m
```

产物位于 `x64\Release\SystemCleaner.exe`（或对应平台目录）。

## 运行

程序清单声明了 `requireAdministrator`，双击即会弹出 UAC 提权对话框，无需手动「以管理员身份运行」。

## 测试

`tests/` 目录包含两套自测程序（无需第三方测试框架）：

| 脚本 | 内容 |
| --- | --- |
| `build_baseline.cmd` | 编译并运行 `baseline_tests.cpp`，336 项基础逻辑断言 |
| `build_gui.cmd` | 编译并运行 `gui_tests.cpp`，839 项 GUI 交互断言（控制台子系统，非提权运行） |
| `build_app.cmd <配置> <平台>` | 用 msbuild 编译主工程，例如 `build_app.cmd Release x64` |

测试用例清单与执行记录见 [tests/TEST_CASES.md](tests/TEST_CASES.md)。

> 注意：`tests/*.cmd` 中 `vcvars64.bat` 的路径为本机 Visual Studio 安装路径，换机器使用请先改成自己的安装路径（或直接在 Developer Prompt 中运行并删除 `call` 行）。
>
> `tests/unit_tests.cpp` 是早期草稿，未纳入编译，仅作参考。

## 安全设计

- 所有清理 / 删除操作前均有确认对话框；谨慎项与隐私项默认不勾选
- 注册表清理在删除前自动导出 `.reg` 备份，备份失败时中止删除
- 文件粉碎内置安全闸门：拒绝系统关键路径与系统/用户关键数据文件
- 子键被安全闸门拦截时其父键同样保留，防止绕过闸门

## 免责声明

本工具会直接操作临时文件、注册表与已安装程序。尽管内置了确认与备份机制，使用前仍建议自行创建系统还原点或注册表备份。因使用本工具造成的任何数据丢失或系统异常，作者不承担任何责任。

## License

未附带许可证文件。如需开源许可，请自行添加（如 MIT / GPL）。
