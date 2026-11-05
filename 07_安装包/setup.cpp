// ===========================================================================
// obs-beauty setup.exe —— 安装 / 卸载同一个可执行文件
//
// 用法：  Setup.exe              安装（需要管理员权限，manifest 已声明）
//         Setup.exe --uninstall  卸载（删文件 + 注册表项）
//         Setup.exe --quiet      不弹窗（供脚本调用）
//
// 为什么自己写而不是用 iexpress/NSIS：这台机器上没有 NSIS/Inno，
// iexpress 在非交互构建下静默失败（exit 1 且不给原因）。自写不依赖任何工具。
//
// 载荷（DLL + 6 支着色器）以 RCDATA 资源嵌在本 exe 里，安装时写盘。
//
// 3.0.1：安装目录改为 OBS 32.x 实际加载的 ProgramData 布局
//   %PROGRAMDATA%\obs-studio\plugins\obs-beauty\bin\64bit\obs-beauty.dll
//   %PROGRAMDATA%\obs-studio\plugins\obs-beauty\data\shaders\*.hlsl
// 与 03_装机\7_install_gpu.ps1 完全一致。旧版写到 Program Files 下的
// obs-plugins\64bit，OBS 根本不加载 —— 装了等于没装（N13 的第二种形态）。
// 同时去掉 _wsystem("cmd /c rmdir ...") 的路径拼接（命令注入 / 误删），
// 改为 Win32 递归删除；写文件改为「临时文件 + 原子替换」，失败不留半截 dll。
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>   // SHCreateDirectoryExW（递归建目录）
#include <string>
#include <vector>
#include <cstdio>

static bool g_quiet = false;

static void msg(const wchar_t* text)
{
    if (!g_quiet) MessageBoxW(nullptr, text, L"OBS Beauty GPU Setup", MB_OK);
}

static bool readRes(int id, std::vector<unsigned char>& out)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!r) return false;
    DWORD sz = SizeofResource(nullptr, r);
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g || !sz) return false;
    const void* p = LockResource(g);
    if (!p) return false;
    out.assign((const unsigned char*)p, (const unsigned char*)p + sz);
    return true;
}

// 3.0.1：写 .tmp 成功后原子替换目标。旧实现用 CREATE_ALWAYS 直接截断目标，
// 一旦 WriteFile 部分失败就留下半截 dll，把原本可用的文件换成了坏文件。
static bool writeFile(const std::wstring& path, const std::vector<unsigned char>& data)
{
    const DWORD n = (DWORD)data.size();
    if ((size_t)n != data.size()) return false;      // >4GB 截断保护
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(h, data.data(), n, &wr, nullptr) && wr == n;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// 3.0.1：不经过 cmd.exe 的递归删除。拒绝 ".."、不跟进 junction/symlink，
// 避免路径拼接注入或顺着链接删到插件目录外面去。
static bool removeTree(const std::wstring& dir)
{
    if (dir.empty() || dir.find(L"..") != std::wstring::npos) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isDir && isLink) {
                RemoveDirectoryW(p.c_str());
            } else if (isDir) {
                removeTree(p);
            } else {
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

static void makeDir(const std::wstring& p)
{
    SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
}

// 插件根目录。路径只由受控的 %PROGRAMDATA% + 固定后缀组成，并拒绝 cmd
// 元字符（延迟自删仍要借 cmd 起一个脱离进程，这里把输入约束死）。
static std::wstring pluginRoot()
{
    wchar_t buf[1024] = {0};
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, 1024);
    if (!n || n >= 1024) return L"";
    const std::wstring bad = L"\"%&|^<>()!";
    for (const wchar_t* p = buf; *p; ++p)
        if (bad.find(*p) != std::wstring::npos) return L"";
    return std::wstring(buf) + L"\\obs-studio\\plugins\\obs-beauty";
}

// 只用于「这台机器装没装 OBS」的存在性检查，不再参与拼安装路径。
static std::wstring findObs()
{
    const wchar_t* cand[] = {L"C:\\Program Files\\obs-studio",
                             L"C:\\Program Files (x86)\\obs-studio"};
    for (auto c : cand) {
        std::wstring exe = std::wstring(c) + L"\\bin\\64bit\\obs64.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
            return c;
    }
    HKEY k = nullptr;
    LONG rr = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\obs64.exe",
                            0, KEY_READ, &k);
    if (rr == ERROR_SUCCESS && k) {
        wchar_t buf[MAX_PATH] = {0};
        DWORD cb = sizeof(buf) - 2;
        if (RegQueryValueExW(k, nullptr, nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS) {
            std::wstring exe(buf);
            size_t pos = exe.find(L"\\bin\\64bit\\");
            if (pos != std::wstring::npos) {
                RegCloseKey(k);
                return exe.substr(0, pos);
            }
        }
        RegCloseKey(k);
    }
    return L"";
}

static bool obsRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"obs64.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    bool uninstall = false, quiet = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--uninstall") == 0) uninstall = true;
        if (_wcsicmp(argv[i], L"--quiet") == 0) quiet = true;
    }
    if (argv) LocalFree(argv);
    g_quiet = quiet;

    if (findObs().empty()) {
        msg(L"找不到 OBS Studio（C:\\Program Files\\obs-studio）。请先安装 OBS。");
        return 2;
    }
    if (obsRunning()) { msg(L"OBS 正在运行。请先关闭 OBS 再运行本程序。"); return 3; }

    const std::wstring root = pluginRoot();
    if (root.empty()) { msg(L"无法定位 %PROGRAMDATA%，安装中止。"); return 2; }

    const std::wstring plug = root + L"\\bin\\64bit\\obs-beauty.dll";
    const std::wstring shad = root + L"\\data\\shaders";
    const std::wstring selfCopy = root + L"\\uninstall.exe";
    const wchar_t* KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\obs-beauty";

    // 旧版（3.0.0 及以前）可能把文件装在 Program Files 布局里，卸载时一并清掉，
    // 否则旧 dll 会一直留在磁盘上，看起来像「装了新的却还是旧行为」。
    const std::wstring legacyDll =
        L"C:\\Program Files\\obs-studio\\obs-plugins\\64bit\\obs-beauty.dll";
    const std::wstring legacyData =
        L"C:\\Program Files\\obs-studio\\data\\obs-plugins\\obs-beauty";

    // 一键：已安装时**再双击本 exe** 就是卸载入口（不必去设置里找）。
    //   是(Y)=卸载   否(N)=重新安装   取消=退出
    // --quiet / --uninstall 走各自的非交互路径，不弹这个框。
    if (!uninstall && !quiet &&
        GetFileAttributesW(plug.c_str()) != INVALID_FILE_ATTRIBUTES) {
        int r = MessageBoxW(nullptr,
            L"已安装 OBS Beauty GPU（obs-beauty 3.0.1）。\n\n"
            L"是(Y)  = 卸载\n"
            L"否(N)  = 重新安装（覆盖）\n"
            L"取消    = 退出",
            L"OBS Beauty GPU Setup", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES) uninstall = true;
    }

    if (uninstall) {
        DeleteFileW(plug.c_str());
        DeleteFileW(selfCopy.c_str());
        removeTree(shad);
        removeTree(root + L"\\data\\obs-plugins\\obs-beauty");
        // 旧布局残留
        DeleteFileW(legacyDll.c_str());
        removeTree(legacyData + L"\\shaders");
        removeTree(legacyData);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, KEY);
        // 自己就在 <root>\uninstall.exe 里跑，删不掉自己所在的目录。
        // 交给一个脱离的 cmd：等 2 秒（本进程退出后）再删。root 已通过
        // cmd 元字符白名单校验，且命令行用动态缓冲，不再有定长截断风险。
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring me(self);
        if (me.find(root + L"\\") == 0) {
            std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \""
                             + root + L"\"";
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        msg(L"已卸载：插件文件与注册表项都已移除（残留目录会在 2 秒内自动删除）。");
        return 0;
    }

    makeDir(root + L"\\bin\\64bit");
    makeDir(shad);

    struct Item { int id; const wchar_t* file; };
    const Item items[] = {
        {101, L"obs-beauty.dll"},
        {201, L"detail.hlsl"}, {202, L"mask.hlsl"}, {203, L"region.hlsl"},
        {204, L"smooth.hlsl"}, {205, L"tone.hlsl"},  {206, L"warp.hlsl"},
    };
    // 与 7_install_gpu.ps1 对齐：机器上若还留着旧版 shader 副本，一并刷新，
    // 否则 OBS 可能挑中那份旧文件，出现「新增 uniform 找不到」的静默失效。
    const std::wstring legacyShaders[] = {
        root + L"\\bin\\64bit\\data\\shaders",
        root + L"\\bin\\data\\shaders",
        root + L"\\data\\obs-plugins\\obs-beauty\\shaders",
    };
    int okCount = 0;
    for (const auto& it : items) {
        std::vector<unsigned char> blob;
        if (!readRes(it.id, blob)) continue;
        const std::wstring dst = (it.id == 101) ? plug : (shad + L"\\" + it.file);
        if (!writeFile(dst, blob)) continue;
        ++okCount;
        if (it.id != 101) {
            for (const auto& ld : legacyShaders)
                if (GetFileAttributesW(ld.c_str()) != INVALID_FILE_ATTRIBUTES)
                    writeFile(ld + L"\\" + it.file, blob);
        }
    }
    if (okCount < 7) { msg(L"安装失败：资源写出不完整（DLL / 着色器）。"); return 4; }

    // 卸载器 = 本 exe 自身副本
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!CopyFileW(self, selfCopy.c_str(), FALSE)) {
        // 自己就是刚才那份时 CopyFile 会失败，忽略
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        == ERROR_SUCCESS) {
        const wchar_t* name = L"OBS Beauty GPU (obs-beauty 3.0.1)";
        const wchar_t* ver = L"3.0.1";
        std::wstring un = L"\"" + selfCopy + L"\" --uninstall";
        RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE*)name,
                       (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE*)ver,
                       (DWORD)((wcslen(ver) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"InstallLocation", 0, REG_SZ, (const BYTE*)root.c_str(),
                       (DWORD)((root.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"UninstallString", 0, REG_SZ, (const BYTE*)un.c_str(),
                       (DWORD)((un.size() + 1) * sizeof(wchar_t)));
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(k);
    }

    wchar_t done[512];
    swprintf(done, 512, L"安装完成：\n%s\n\n共写入 %d 个文件。\n启动 OBS → 源 → 滤镜 → + → Beauty (GPU)。",
             plug.c_str(), okCount);
    msg(done);
    return 0;
}
