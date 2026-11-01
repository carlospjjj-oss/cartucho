#include "cloud.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <vector>

#include "archive.h"
#include "paths.h"
#include "settings.h"

#ifndef CARTUCHO_VERSION
#define CARTUCHO_VERSION "dev"
#endif

namespace cartucho {

namespace Backup {

namespace {

const char* const kFolders[] = {"saves", "states", "cheats", "covers_custom", "system", "play"};
const char* const kMarker = "cartucho-backup.txt";

// Every file under data/<folder>, as paths relative to data with '/'.
void Walk(const std::string& root, const std::string& rel, const std::function<void(const std::string&, const DirEntry&)>& fn) {
    for (const auto& e : ListDir(JoinPath(root, rel))) {
        const std::string r = rel + "/" + e.name;
        if (e.dir) Walk(root, r, fn);
        else fn(r, e);
    }
}

bool EndsWith(const std::string& s, const std::string& x) {
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

}  // namespace

Result Export(const std::string& zip, bool light) {
    Result r;
    const std::string root = Paths::Get().data;
    std::vector<ZipItem> items;
    items.push_back({kMarker, "", std::string("Cartucho PC ") + CARTUCHO_VERSION + "\n"});
    for (const char* folder : kFolders) {
        Walk(root, folder, [&](const std::string& rel, const DirEntry& e) {
            if (EndsWith(rel, ".tmp") || EndsWith(rel, ".part")) return;
            if (light) {
                const std::string f = folder;
                if (f == "system" && (rel.find('/', 7) != std::string::npos || e.size > 4u * 1024 * 1024)) return;
                if (f == "states" && rel.rfind("states/N3DS/", 0) == 0) return;
            }
            items.push_back({rel, JoinPath(root, rel), ""});
            r.files++;
            r.bytes += e.size;
        });
    }
    std::string err;
    r.ok = WriteZip(zip, items, &err);
    r.message = r.ok ? "Enviado: " + std::to_string(r.files) + " arquivos (" + std::to_string(r.bytes / 1024) + " KB)."
                     : err;
    return r;
}

Result Restore(const std::string& zip) {
    Result r;
    if (!FileExists(zip)) {
        r.message = "O arquivo " + zip + " não existe (ainda não foi sincronizado?).";
        return r;
    }
    if (!ZipHasEntry(zip, kMarker)) {
        r.message = "Esse arquivo não é um backup do Cartucho.";
        return r;
    }
    const std::string root = Paths::Get().data;
    std::string err;
    r.ok = ReadZip(zip, [&](const std::string& name, const std::vector<uint8_t>& bytes) {
        const std::string top = name.substr(0, name.find('/'));
        bool allowed = false;
        for (const char* f : kFolders) allowed = allowed || top == f;
        // Never outside the data folder (malformed zips).
        if (!allowed || name.find("..") != std::string::npos || name.find('\\') != std::string::npos || name.back() == '/')
            return true;
        const std::string out = JoinPath(root, name);
        MakeDirs(ParentDir(out));
        if (WriteFileBytes(out, bytes.data(), bytes.size())) {
            r.files++;
            r.bytes += bytes.size();
        }
        return true;
    }, &err);
    r.message = r.ok ? "Baixado: " + std::to_string(r.files) + " arquivos (" + std::to_string(r.bytes / 1024) + " KB)." : err;
    return r;
}

int64_t LastChange() {
    int64_t newest = 0;
    const std::string root = Paths::Get().data;
    for (const char* folder : kFolders) {
        if (std::string(folder) == "system") continue;
        Walk(root, folder, [&](const std::string& rel, const DirEntry&) {
            newest = std::max(newest, FileTime(JoinPath(root, rel)));
        });
    }
    return newest;
}

std::string DefaultName() {
    char buf[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "Cartucho-backup-%Y-%m-%d.zip", std::localtime(&t));
    return buf;
}

}  // namespace Backup

namespace Cloud {

std::string File() { return Settings::Get().Str("cloud_file", ""); }

void SetFile(const std::string& path) {
    Settings::Get().Set("cloud_file", path.empty() ? nlohmann::json() : nlohmann::json(path));
    Settings::Get().Set("cloud_last", nullptr);
}

bool Auto() { return Settings::Get().Bool("cloud_auto", true); }

int64_t LastSent() {
    const auto v = Settings::Get().Raw("cloud_last");
    return v.is_number() ? v.get<int64_t>() : 0;
}

Backup::Result Send() {
    const std::string f = File();
    if (f.empty()) {
        Backup::Result r;
        r.message = "Nenhum arquivo de nuvem escolhido.";
        return r;
    }
    Backup::Result r = Backup::Export(f, true);
    if (r.ok) Settings::Get().Set("cloud_last", NowMs());
    else r.message = "Não deu para gravar no arquivo da nuvem (" + r.message + ").";
    return r;
}

void SendIfChanged() {
    if (File().empty() || !Auto()) return;
    if (Backup::LastChange() <= LastSent()) return;
    Send();
}

Backup::Result Fetch() {
    const std::string f = File();
    if (f.empty()) {
        Backup::Result r;
        r.message = "Nenhum arquivo de nuvem escolhido.";
        return r;
    }
    Backup::Result r = Backup::Restore(f);
    if (r.ok) Settings::Get().Set("cloud_last", NowMs());
    return r;
}

}  // namespace Cloud

}  // namespace cartucho
