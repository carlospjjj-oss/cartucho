#include "archive.h"

#include <miniz.h>

#include <cstdio>
#include <cstring>
#include <memory>

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
}

#include "paths.h"
#include "systems.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace cartucho {

namespace {

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
}

bool IsGameName(const std::string& name) {
    if (name.empty() || name.back() == '/' || name.back() == '\\')
        return false;
    const size_t slash = name.find_last_of("/\\");
    return SystemForFile(slash == std::string::npos ? name : name.substr(slash + 1), 0) != nullptr;
}

bool Is7z(const std::string& path) { return EndsWith(path, ".7z"); }

// --- 7z (LZMA SDK) -------------------------------------------------------------

const ISzAlloc kAlloc = {SzAlloc, SzFree};
const ISzAlloc kAllocTemp = {SzAllocTemp, SzFreeTemp};

class SevenZip {
public:
    ~SevenZip() {
        if (opened_) SzArEx_Free(&db_, &kAlloc);
        if (look_.buf) ISzAlloc_Free(&kAlloc, look_.buf);
        if (file_open_) File_Close(&stream_.file);
        if (out_) ISzAlloc_Free(&kAlloc, out_);
    }

    bool Open(const std::string& path) {
        static bool crc = false;
        if (!crc) {
            CrcGenerateTable();
            crc = true;
        }
#ifdef _WIN32
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
        if (InFile_OpenW(&stream_.file, w.c_str()) != 0) return false;
#else
        if (InFile_Open(&stream_.file, path.c_str()) != 0) return false;
#endif
        file_open_ = true;
        FileInStream_CreateVTable(&stream_);
        stream_.wres = 0;
        LookToRead2_CreateVTable(&look_, False);
        look_.buf = static_cast<Byte*>(ISzAlloc_Alloc(&kAlloc, 1 << 18));
        if (!look_.buf) return false;
        look_.bufSize = 1 << 18;
        look_.realStream = &stream_.vt;
        LookToRead2_INIT(&look_);
        SzArEx_Init(&db_);
        if (SzArEx_Open(&db_, &look_.vt, &kAlloc, &kAllocTemp) != SZ_OK) return false;
        opened_ = true;
        return true;
    }

    size_t count() const { return db_.NumFiles; }
    bool is_dir(size_t i) const { return SzArEx_IsDir(&db_, i); }
    uint64_t size(size_t i) const { return SzArEx_GetFileSize(&db_, i); }

    std::string name(size_t i) const {
        const size_t len = SzArEx_GetFileNameUtf16(&db_, i, nullptr);
        std::vector<UInt16> u(len);
        SzArEx_GetFileNameUtf16(&db_, i, u.data());
        std::string out;
        for (size_t k = 0; k < len && u[k]; k++) {
            uint32_t c = u[k];
            if (c >= 0xD800 && c < 0xDC00 && k + 1 < len) {
                c = 0x10000 + ((c - 0xD800) << 10) + (u[k + 1] - 0xDC00);
                k++;
            }
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
            else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63));
                out += static_cast<char>(0x80 | (c & 63));
            } else {
                out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63));
                out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63));
            }
        }
        return out;
    }

    // The file's bytes (the whole solid block is decoded in memory).
    bool Extract(size_t i, const uint8_t** data, size_t* size) {
        size_t offset = 0, processed = 0;
        if (SzArEx_Extract(&db_, &look_.vt, static_cast<UInt32>(i), &block_, &out_, &out_size_, &offset, &processed,
                           &kAlloc, &kAllocTemp) != SZ_OK)
            return false;
        *data = out_ + offset;
        *size = processed;
        return true;
    }

private:
    CFileInStream stream_{};
    CLookToRead2 look_{};
    CSzArEx db_{};
    bool file_open_ = false;
    bool opened_ = false;
    UInt32 block_ = 0xFFFFFFFF;
    Byte* out_ = nullptr;
    size_t out_size_ = 0;
};

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

bool IsArchive(const std::string& file_name) { return EndsWith(file_name, ".zip") || EndsWith(file_name, ".7z"); }

bool PeekArchive(const std::string& archive, ArchiveEntry* out) {
    if (Is7z(archive)) {
        SevenZip z;
        if (!z.Open(archive)) return false;
        for (size_t i = 0; i < z.count(); i++) {
            if (z.is_dir(i)) continue;
            const std::string n = z.name(i);
            if (IsGameName(n)) {
                *out = {n, z.size(i)};
                return true;
            }
        }
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.c_str(), 0)) return false;
    bool found = false;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && i < 8 && !found; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        if (IsGameName(st.m_filename)) {
            *out = {st.m_filename, st.m_uncomp_size};
            found = true;
        }
    }
    mz_zip_reader_end(&zip);
    return found;
}

std::string ExtractedPath(const std::string& archive, uint64_t archive_size, const std::string& entry) {
    char tag[24];
    std::snprintf(tag, sizeof(tag), "%08x", static_cast<unsigned>(Fnv(archive + "|" + std::to_string(archive_size))));
    std::string base = entry;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    return JoinPath(JoinPath(Paths::Get().data, "extracted"), std::string(tag) + "-" + base);
}

namespace {

struct ZipWriteState {
    FILE* f;
    const std::function<void(float)>* progress;
    uint64_t total, done;
    bool failed;
};

size_t ZipToFile(void* opaque, mz_uint64 /*ofs*/, const void* buf, size_t n) {
    auto* s = static_cast<ZipWriteState*>(opaque);
    if (std::fwrite(buf, 1, n, s->f) != n) {
        s->failed = true;
        return 0;
    }
    s->done += n;
    if (s->total && *s->progress) (*s->progress)(static_cast<float>(static_cast<double>(s->done) / s->total));
    return n;
}

FILE* OpenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(w.c_str(), m.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

bool ExtractEntry(const std::string& archive, const std::string& entry, const std::string& target,
                  const std::function<void(float)>& progress, std::string* error) {
    if (FileSize(target) > 0) return true;
    // Keep only the latest extraction (3DS and DS games are big).
    const std::string dir = ParentDir(target);
    MakeDirs(dir);
    for (const auto& e : ListDir(dir))
        if (!e.dir) RemoveFile(JoinPath(dir, e.name));
    const std::string tmp = target + ".part";
    FILE* f = OpenUtf8(tmp, "wb");
    if (!f) {
        *error = "Não foi possível gravar o jogo extraído em " + dir;
        return false;
    }
    bool ok = false;
    if (Is7z(archive)) {
        SevenZip z;
        if (!z.Open(archive)) {
            *error = "Não foi possível abrir o arquivo .7z.";
        } else {
            for (size_t i = 0; i < z.count(); i++) {
                if (z.is_dir(i) || z.name(i) != entry) continue;
                if (progress) progress(0.05f);
                const uint8_t* data = nullptr;
                size_t size = 0;
                if (!z.Extract(i, &data, &size)) {
                    *error = "O arquivo .7z está danificado ou usa um formato não suportado.";
                } else {
                    ok = std::fwrite(data, 1, size, f) == size;
                    if (!ok) *error = "Sem espaço para extrair o jogo.";
                }
                break;
            }
            if (!ok && error->empty()) *error = "O jogo não está mais dentro do arquivo.";
        }
    } else {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, archive.c_str(), 0)) {
            *error = "Não foi possível abrir o arquivo .zip.";
        } else {
            const int i = mz_zip_reader_locate_file(&zip, entry.c_str(), nullptr, 0);
            if (i < 0) {
                *error = "O jogo não está mais dentro do arquivo.";
            } else {
                mz_zip_archive_file_stat st;
                mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &st);
                ZipWriteState s{f, &progress, st.m_uncomp_size, 0, false};
                ok = mz_zip_reader_extract_to_callback(&zip, static_cast<mz_uint>(i), ZipToFile, &s, 0) && !s.failed;
                if (!ok) *error = s.failed ? "Sem espaço para extrair o jogo." : "O arquivo .zip está danificado.";
            }
            mz_zip_reader_end(&zip);
        }
    }
    std::fclose(f);
    if (!ok || !RenameFile(tmp, target)) {
        RemoveFile(tmp);
        if (error->empty()) *error = "Não foi possível extrair o jogo.";
        return false;
    }
    if (progress) progress(1.0f);
    return true;
}

bool WriteZip(const std::string& path, const std::vector<ZipItem>& items, std::string* error) {
    const std::string tmp = path + ".tmp";
    MakeDirs(ParentDir(path));
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmp.c_str(), 0)) {
        *error = "Não foi possível criar " + path;
        return false;
    }
    bool ok = true;
    for (const auto& it : items) {
        std::vector<uint8_t> bytes;
        if (!it.file.empty()) {
            if (!ReadFileBytes(it.file, &bytes)) continue;
        } else {
            bytes.assign(it.text.begin(), it.text.end());
        }
        if (!mz_zip_writer_add_mem(&zip, it.name.c_str(), bytes.data(), bytes.size(), MZ_BEST_SPEED)) {
            ok = false;
            *error = "Erro ao gravar " + it.name;
            break;
        }
    }
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    if (!ok) {
        RemoveFile(tmp);
        if (error->empty()) *error = "Não foi possível gravar " + path;
        return false;
    }
    RemoveFile(path);
    if (!RenameFile(tmp, path)) {
        *error = "Não foi possível substituir " + path;
        return false;
    }
    return true;
}

bool ReadZip(const std::string& path, const std::function<bool(const std::string&, const std::vector<uint8_t>&)>& fn,
             std::string* error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        *error = "Não foi possível abrir " + path;
        return false;
    }
    bool ok = true;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!p) {
            ok = false;
            *error = std::string("Arquivo danificado: ") + st.m_filename;
            break;
        }
        std::vector<uint8_t> bytes(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
        mz_free(p);
        if (!fn(st.m_filename, bytes)) break;
    }
    mz_zip_reader_end(&zip);
    return ok;
}

bool ZipHasEntry(const std::string& path, const std::string& name) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
    const bool found = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0) >= 0;
    mz_zip_reader_end(&zip);
    return found;
}

}  // namespace cartucho
