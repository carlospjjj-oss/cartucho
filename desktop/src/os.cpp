#include "os.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "paths.h"

namespace cartucho {

#ifdef _WIN32

namespace {
std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string U(const wchar_t* w) {
    if (!w) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Quote(const std::wstring& a) {
    std::wstring o = L"\"";
    for (wchar_t c : a) { if (c == L'"') o += L'\\'; o += c; }
    return o + L"\"";
}
}  // namespace

struct Process { PROCESS_INFORMATION pi{}; };

Process* StartSelf(const std::vector<std::string>& args) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = Quote(exe);
    for (const auto& a : args) cmd += L" " + Quote(W(a));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    auto* p = new Process;
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &p->pi)) {
        delete p;
        return nullptr;
    }
    return p;
}

bool ProcessRunning(Process* p) { return p && WaitForSingleObject(p->pi.hProcess, 0) == WAIT_TIMEOUT; }

int ProcessExitCode(Process* p) {
    DWORD code = 0;
    if (p) GetExitCodeProcess(p->pi.hProcess, &code);
    return static_cast<int>(code);
}

void ProcessFree(Process* p) {
    if (!p) return;
    CloseHandle(p->pi.hProcess);
    CloseHandle(p->pi.hThread);
    delete p;
}

std::string PickFolder(const std::string& title) {
    std::string out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&d)))) {
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(W(title).c_str());
        if (SUCCEEDED(d->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = U(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        d->Release();
    }
    return out;
}

std::string PickFile(const std::string& title, const std::string& desc, const std::string& patterns) {
    std::string out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&d)))) {
        const std::wstring wd = W(desc), wp = W(patterns);
        COMDLG_FILTERSPEC spec[2] = {{wd.c_str(), wp.c_str()}, {L"Todos os arquivos", L"*.*"}};
        d->SetFileTypes(2, spec);
        d->SetTitle(W(title).c_str());
        if (SUCCEEDED(d->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = U(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        d->Release();
    }
    return out;
}

std::string PickSaveFile(const std::string& title, const std::string& default_name, const std::string& desc,
                         const std::string& patterns) {
    std::string out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileSaveDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void**>(&d)))) {
        const std::wstring wd = W(desc), wp = W(patterns);
        COMDLG_FILTERSPEC spec[1] = {{wd.c_str(), wp.c_str()}};
        d->SetFileTypes(1, spec);
        d->SetDefaultExtension(L"zip");
        d->SetFileName(W(default_name).c_str());
        d->SetTitle(W(title).c_str());
        if (SUCCEEDED(d->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(d->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = U(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        d->Release();
    }
    return out;
}

std::string VideosFolder() {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path))) {
        out = JoinPath(U(path), "Cartucho");
        CoTaskMemFree(path);
    }
    return out.empty() ? JoinPath(Paths::Get().data, "videos") : out;
}

bool StartDetached(const std::string& program, const std::vector<std::string>& args) {
    std::wstring cmd = L"\"" + W(program) + L"\"";
    for (const auto& a : args)
        cmd += (a.empty() || a.find(' ') != std::string::npos) ? L" \"" + W(a) + L"\"" : L" " + W(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                        &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void OpenUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

#else

struct Process { pid_t pid = 0; int status = -1; bool done = false; };

Process* StartSelf(const std::vector<std::string>& args) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return nullptr;
    exe[n] = 0;
    const pid_t pid = fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(exe);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(exe, argv.data());
        _exit(127);
    }
    auto* p = new Process;
    p->pid = pid;
    return p;
}

bool ProcessRunning(Process* p) {
    if (!p || p->done) return false;
    int st = 0;
    if (waitpid(p->pid, &st, WNOHANG) == p->pid) {
        p->done = true;
        p->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        return false;
    }
    return true;
}

int ProcessExitCode(Process* p) { return p ? p->status : -1; }
void ProcessFree(Process* p) { delete p; }

namespace {
std::string RunZenity(const std::string& args) {
    std::string out;
    FILE* f = popen(("zenity " + args + " 2>/dev/null").c_str(), "r");
    if (!f) return out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
}  // namespace

std::string PickFolder(const std::string& title) {
    return RunZenity("--file-selection --directory --title=\"" + title + "\"");
}

std::string PickFile(const std::string& title, const std::string&, const std::string&) {
    return RunZenity("--file-selection --title=\"" + title + "\"");
}

std::string PickSaveFile(const std::string& title, const std::string& default_name, const std::string&,
                         const std::string&) {
    return RunZenity("--file-selection --save --confirm-overwrite --filename=\"" + default_name + "\" --title=\"" + title + "\"");
}

std::string VideosFolder() { return JoinPath(Paths::Get().data, "videos"); }

bool StartDetached(const std::string& program, const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    return true;
}

void OpenUrl(const std::string& url) { SDL_OpenURL(url.c_str()); }

#endif

}  // namespace cartucho
