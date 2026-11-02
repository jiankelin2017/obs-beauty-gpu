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
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
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

static bool writeFile(const std::wstring& path, const std::vector<unsigned char>& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size();
    CloseHandle(h);
    return ok;
}

static std::wstring findObs()
{
    const wchar_t* cand[] = {L"C:\\Program Files\\obs-studio",
                             L"C:\\Program Files (x86)\\obs-studio"};
    for (auto c : cand) {
        std::wstring exe = std::wstring(c) + L"\\bin\\64bit\\obs64.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
            return c;
    }
    // 注册表兜底
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

struct Res { int id; const wchar_t* name; const wchar_t* sub; };

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

    std::wstring obs = findObs();
    if (obs.empty()) { msg(L"找不到 OBS Studio（C:\\Program Files\\obs-studio）。请先安装 OBS。"); return 2; }
    if (obsRunning()) { msg(L"OBS 正在运行。请先关闭 OBS 再运行本程序。"); return 3; }

    const std::wstring plug = obs + L"\\obs-plugins\\64bit\\obs-beauty.dll";
    const std::wstring data = obs + L"\\data\\obs-plugins\\obs-beauty";
    const std::wstring shad = data + L"\\shaders";
    const wchar_t* KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\obs-beauty";

    // 一键：已安装时**再双击本 exe** 就是卸载入口（不必去设置里找）。
    //   是(Y)=卸载   否(N)=重新安装   取消=退出
    // --quiet / --uninstall 走各自的非交互路径，不弹这个框。
    if (!uninstall && !quiet &&
        GetFileAttributesW(plug.c_str()) != INVALID_FILE_ATTRIBUTES) {
        int r = MessageBoxW(nullptr,
            L"已安装 OBS Beauty GPU（obs-beauty 3.1.76）。\n\n"
            L"是(Y)  = 卸载\n"
            L"否(N)  = 重新安装（覆盖）\n"
            L"取消    = 退出",
            L"OBS Beauty GPU Setup", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES) uninstall = true;
    }

    if (uninstall) {
        DeleteFileW(plug.c_str());
        DeleteFileW((data + L"\\uninstall.exe").c_str());
        std::wstring cmd = L"cmd /c rmdir /s /q \"" + shad + L"\"";
        _wsystem(cmd.c_str());
        cmd = L"cmd /c rmdir /s /q \"" + data + L"\"";
        _wsystem(cmd.c_str());
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, KEY);
        // 自己就在 <data>\uninstall.exe 里跑，删不掉自己所在的目录。
        // 交给一个脱离的 cmd：等 2 秒（本进程退出后）再删，最后自删临时批处理。
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring tmp = std::wstring(self);
        if (tmp.find(L"\\data\\obs-plugins\\obs-beauty\\") != std::wstring::npos) {
            wchar_t cmd[1024];
            swprintf(cmd, 1024,
                     L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%s\"",
                     data.c_str());
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        msg(L"已卸载：插件文件与注册表项都已移除（残留目录会在 2 秒内自动删除）。");
        return 0;
    }

    CreateDirectoryW((obs + L"\\data\\obs-plugins").c_str(), nullptr);
    CreateDirectoryW(data.c_str(), nullptr);
    CreateDirectoryW(shad.c_str(), nullptr);

    struct Item { int id; const wchar_t* file; };
    const Item items[] = {
        {101, L"obs-beauty.dll"},
        {201, L"detail.hlsl"}, {202, L"mask.hlsl"}, {203, L"region.hlsl"},
        {204, L"smooth.hlsl"}, {205, L"tone.hlsl"},  {206, L"warp.hlsl"},
    };
    int okCount = 0;
    for (const auto& it : items) {
        std::vector<unsigned char> blob;
        if (!readRes(it.id, blob)) continue;
        const bool isDll = (it.id == 101);
        std::wstring dst = isDll ? plug : (shad + L"\\" + it.file);
        if (writeFile(dst, blob)) okCount++;
    }
    if (okCount < 7) { msg(L"安装失败：资源写出不完整。"); return 4; }

    // 卸载器 = 本 exe 自身副本
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring selfCopy = data + L"\\uninstall.exe";
    if (!CopyFileW(self, selfCopy.c_str(), FALSE)) {
        // 自己就是刚才那份时 CopyFile 会失败，忽略
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
        == ERROR_SUCCESS) {
        const wchar_t* name = L"OBS Beauty GPU (obs-beauty 3.1.76)";
        const wchar_t* ver = L"3.1.76";
        std::wstring un = L"\"" + selfCopy + L"\" --uninstall";
        RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE*)name,
                       (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE*)ver,
                       (DWORD)((wcslen(ver) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"InstallLocation", 0, REG_SZ, (const BYTE*)data.c_str(),
                       (DWORD)((data.size() + 1) * sizeof(wchar_t)));
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
