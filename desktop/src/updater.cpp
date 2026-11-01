#include "updater.h"

#include <json.hpp>

#include <cstdlib>
#include <vector>

#include "archive.h"
#include "http.h"
#include "os.h"
#include "paths.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef CARTUCHO_VERSION
#define CARTUCHO_VERSION "0.0.0-dev"
#endif

namespace cartucho {

namespace Updater {

namespace {
const char* kRelease = "https://api.github.com/repos/carlospjjj-oss/cartucho/releases/tags/pc";

// "Cartucho-PC-12.zip" -> 12.
int BuildOf(const std::string& name) {
    const std::string pre = "Cartucho-PC-";
    if (name.rfind(pre, 0) != 0 || name.size() < pre.size() + 5 || name.substr(name.size() - 4) != ".zip") return 0;
    return std::atoi(name.substr(pre.size(), name.size() - pre.size() - 4).c_str());
}
}  // namespace

int CurrentBuild() {
    // "1.0.<build>"
    const std::string v = CARTUCHO_VERSION;
    const size_t dot = v.find_last_of('.');
    return dot == std::string::npos ? 0 : std::atoi(v.c_str() + dot + 1);
}

Info Check() {
    Info info;
    info.current = CurrentBuild();
    const HttpResult r = HttpRequest("GET", kRelease, "", {"User-Agent: Cartucho-PC", "Accept: application/vnd.github+json"}, 15000);
    if (!r.ok()) {
        info.message = r.status == 0 ? "Sem conexão para procurar atualizações." : "Não foi possível ver a versão nova (HTTP " +
                                                                                     std::to_string(r.status) + ").";
        return info;
    }
    const auto j = nlohmann::json::parse(r.text(), nullptr, false);
    if (!j.is_object()) {
        info.message = "Resposta inesperada do GitHub.";
        return info;
    }
    for (const auto& a : j.value("assets", nlohmann::json::array())) {
        const int b = BuildOf(a.value("name", ""));
        if (b > info.build) {
            info.build = b;
            info.url = a.value("browser_download_url", "");
        }
    }
    info.ok = info.build > 0 && !info.url.empty();
    info.newer = info.ok && info.current > 0 && info.build > info.current;
    if (!info.ok) info.message = "Nenhuma versão para PC publicada.";
    else if (info.current == 0) info.message = "Versão de teste: atualização automática desligada.";
    else info.message = info.newer ? "Versão nova disponível (" + std::to_string(info.build) + ")." : "Você já tem a versão mais nova.";
    return info;
}

bool Install(const Info& info, std::string* message) {
    const std::string dir = JoinPath(Paths::Get().data, "update");
    const std::string zip = JoinPath(dir, "Cartucho-PC.zip");
    const std::string fresh = JoinPath(dir, "new");
    MakeDirs(dir);
    const HttpResult r = HttpRequest("GET", info.url, "", {"User-Agent: Cartucho-PC"}, 120000, 512u * 1024 * 1024);
    if (!r.ok() || r.body.size() < 1024) {
        *message = "Não foi possível baixar a atualização.";
        return false;
    }
    if (!WriteFileBytes(zip, r.body.data(), r.body.size())) {
        *message = "Não foi possível gravar a atualização em " + dir;
        return false;
    }
    // Unpack: the zip holds Cartucho/...; files go to new/ without that folder.
    std::string err;
    int files = 0;
    const bool ok = ReadZip(zip, [&](const std::string& name, const std::vector<uint8_t>& bytes) {
        std::string rel = name;
        if (rel.rfind("Cartucho/", 0) == 0) rel = rel.substr(9);
        if (rel.empty() || rel.find("..") != std::string::npos) return true;
        const std::string out = JoinPath(fresh, rel);
        MakeDirs(ParentDir(out));
        if (WriteFileBytes(out, bytes.data(), bytes.size())) files++;
        return true;
    }, &err);
    RemoveFile(zip);
    if (!ok || files == 0 || !FileExists(JoinPath(fresh, "Cartucho.exe"))) {
        *message = "A atualização baixada está incompleta. " + err;
        return false;
    }
#ifdef _WIN32
    const std::string exe_dir = Paths::Get().exe_dir;
    const std::string pid = std::to_string(GetCurrentProcessId());
    std::string bat = "@echo off\r\nchcp 65001 >nul\r\n:wait\r\n";
    bat += "tasklist /FI \"PID eq " + pid + "\" 2>nul | find \"" + pid + "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n";
    bat += "robocopy \"" + fresh + "\" \"" + exe_dir + "\" /E /R:5 /W:1 /NFL /NDL /NJH /NJS >nul\r\n";
    bat += "rmdir /S /Q \"" + fresh + "\"\r\n";
    bat += "start \"\" \"" + JoinPath(exe_dir, "Cartucho.exe") + "\"\r\n";
    const std::string script = JoinPath(dir, "atualizar.bat");
    if (!WriteText(script, bat) || !StartDetached("cmd.exe", {"/c", script})) {
        *message = "Não foi possível iniciar a atualização.";
        return false;
    }
    *message = "Atualizando…";
    return true;
#else
    *message = "Atualização baixada em " + fresh + " (no Linux, copie os arquivos manualmente).";
    return false;
#endif
}

}  // namespace Updater

}  // namespace cartucho
