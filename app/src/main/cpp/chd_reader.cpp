#include "chd_reader.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "libchdr/chd.h"
#include "rc_hash.h"
}

namespace cartucho {

namespace {

constexpr uint32_t kFrameBytes = 2352 + 96;  // CD frame + subcode, as stored in a CHD

struct Track {
    int number = 0;
    bool audio = false;
    uint32_t data_offset = 0;   // bytes before the user data in each sector
    uint32_t chd_frame = 0;     // first frame of the track inside the CHD
    uint32_t lba = 0;           // its absolute sector number on the disc
    uint32_t frames = 0;
    uint32_t pregap_stored = 0; // pregap frames present in the CHD
};

struct Handle {
    bool chd = false;
    void* inner = nullptr;      // rcheevos' own handle for other formats
    chd_file* file = nullptr;
    Track track;
    uint32_t frames_per_hunk = 0;
    std::vector<uint8_t> hunk;
    int64_t hunk_index = -1;
};

rc_hash_cdreader g_default{};

bool IsChd(const char* path) {
    const size_t n = std::strlen(path);
    return n > 4 && (std::strcmp(path + n - 4, ".chd") == 0 || std::strcmp(path + n - 4, ".CHD") == 0);
}

// Bytes before the 2048 bytes of user data, by the CHD track type.
uint32_t DataOffset(const std::string& type) {
    if (type == "MODE1_RAW") return 16;
    if (type == "MODE2_RAW") return 24;
    if (type == "MODE2_FORM_MIX" || type == "MODE2") return 8;
    return 0;  // MODE1, MODE2_FORM1, AUDIO
}

std::vector<Track> ReadTracks(chd_file* file) {
    std::vector<Track> tracks;
    uint32_t chd_frame = 0;
    uint32_t lba = 0;
    for (uint32_t i = 0;; i++) {
        char meta[256] = {};
        uint32_t len = 0;
        if (chd_get_metadata(file, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta) - 1, &len, nullptr, nullptr) !=
            CHDERR_NONE) {
            // Older CHDs: CHTR without pregaps.
            if (chd_get_metadata(file, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta) - 1, &len, nullptr, nullptr) !=
                CHDERR_NONE)
                break;
        }
        int number = 0, frames = 0, pregap = 0;
        char type[32] = {}, subtype[32] = {}, pgtype[32] = {};
        const int got = std::sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s",
                                    &number, type, subtype, &frames, &pregap, pgtype);
        if (got < 4)
            break;
        Track t;
        t.number = number;
        t.audio = std::strcmp(type, "AUDIO") == 0;
        t.data_offset = DataOffset(type);
        t.frames = static_cast<uint32_t>(frames);
        // A pregap whose type starts with 'V' is stored in the image.
        t.pregap_stored = (got >= 6 && pgtype[0] == 'V') ? static_cast<uint32_t>(pregap) : 0;
        t.chd_frame = chd_frame;
        t.lba = lba;
        tracks.push_back(t);
        // Each track is padded to a multiple of 4 frames inside the CHD.
        chd_frame += (t.frames + 3) & ~3u;
        lba += t.frames;
    }
    return tracks;
}

void* OpenChd(const char* path, uint32_t track) {
    chd_file* file = nullptr;
    if (chd_open(path, CHD_OPEN_READ, nullptr, &file) != CHDERR_NONE)
        return nullptr;
    const std::vector<Track> tracks = ReadTracks(file);
    const Track* pick = nullptr;
    for (const Track& t : tracks) {
        if (track == RC_HASH_CDTRACK_FIRST_DATA) {
            if (!t.audio) { pick = &t; break; }
        } else if (track == RC_HASH_CDTRACK_LAST) {
            pick = &t;
        } else if (track == RC_HASH_CDTRACK_LARGEST) {
            if (!pick || t.frames > pick->frames) pick = &t;
        } else if (track == RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION) {
            // Single-session images only.
        } else if (static_cast<uint32_t>(t.number) == track) {
            pick = &t;
            break;
        }
    }
    const chd_header* header = chd_get_header(file);
    if (!pick || !header || header->hunkbytes < kFrameBytes) {
        chd_close(file);
        return nullptr;
    }
    auto* h = new Handle();
    h->chd = true;
    h->file = file;
    h->track = *pick;
    h->frames_per_hunk = header->hunkbytes / kFrameBytes;
    h->hunk.resize(header->hunkbytes);
    return h;
}

void* RC_CCONV OpenTrack(const char* path, uint32_t track, const rc_hash_iterator* iterator) {
    if (IsChd(path))
        return OpenChd(path, track);
    void* inner = nullptr;
    if (g_default.open_track_iterator)
        inner = g_default.open_track_iterator(path, track, iterator);
    else if (g_default.open_track)
        inner = g_default.open_track(path, track);
    if (!inner)
        return nullptr;
    auto* h = new Handle();
    h->inner = inner;
    return h;
}

// Only there so rc_client takes this reader (it copies readers that have
// open_track); rcheevos calls the iterator version above.
void* RC_CCONV OpenTrackPlain(const char* path, uint32_t track) {
    return IsChd(path) ? OpenChd(path, track) : nullptr;
}

size_t RC_CCONV ReadSector(void* handle, uint32_t sector, void* buffer, size_t bytes) {
    auto* h = static_cast<Handle*>(handle);
    if (!h)
        return 0;
    if (!h->chd)
        return g_default.read_sector(h->inner, sector, buffer, bytes);
    if (sector < h->track.lba)
        return 0;
    const uint32_t user = h->track.audio ? 2352 : 2048;  // bytes of each sector handed over
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        const uint32_t frame = h->track.chd_frame + (sector - h->track.lba);
        if (sector - h->track.lba >= h->track.frames)
            break;
        const int64_t hunk = frame / h->frames_per_hunk;
        if (hunk != h->hunk_index) {
            if (chd_read(h->file, static_cast<uint32_t>(hunk), h->hunk.data()) != CHDERR_NONE)
                break;
            h->hunk_index = hunk;
        }
        const size_t at = static_cast<size_t>(frame % h->frames_per_hunk) * kFrameBytes + h->track.data_offset;
        const size_t n = std::min<size_t>(user, bytes - done);
        std::memcpy(out + done, h->hunk.data() + at, n);
        done += n;
        sector++;
    }
    return done;
}

void RC_CCONV CloseTrack(void* handle) {
    auto* h = static_cast<Handle*>(handle);
    if (!h)
        return;
    if (h->chd)
        chd_close(h->file);
    else if (g_default.close_track)
        g_default.close_track(h->inner);
    delete h;
}

uint32_t RC_CCONV FirstTrackSector(void* handle) {
    auto* h = static_cast<Handle*>(handle);
    if (!h)
        return 0;
    if (!h->chd)
        return g_default.first_track_sector(h->inner);
    return h->track.lba + h->track.pregap_stored;
}

}  // namespace

void GetDiscReader(rc_hash_cdreader* reader) {
    rc_hash_get_default_cdreader(&g_default);
    reader->open_track = OpenTrackPlain;
    reader->open_track_iterator = OpenTrack;
    reader->read_sector = ReadSector;
    reader->close_track = CloseTrack;
    reader->first_track_sector = FirstTrackSector;
}

}  // namespace cartucho
