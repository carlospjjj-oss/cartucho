#include "rewind.h"

#include <zlib.h>

#include <cstring>

namespace cartucho {

namespace {

// Compresses into `buffer` (reused, sized for the worst case) and hands back
// an exact-size copy: a delta of a mostly still picture packs to ~1 KB, and
// keeping the worst-case allocation behind each one would hold the whole
// state size per entry (the cause of running out of memory on the phone).
bool Compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* buffer, std::vector<uint8_t>* out) {
    uLongf len = compressBound(static_cast<uLong>(in.size()));
    buffer->resize(len + 4);
    // Size prefix so decompression knows how much to expect.
    const uint32_t n = static_cast<uint32_t>(in.size());
    std::memcpy(buffer->data(), &n, 4);
    if (compress2(buffer->data() + 4, &len, in.data(), static_cast<uLong>(in.size()), 1) != Z_OK)
        return false;
    out->assign(buffer->begin(), buffer->begin() + len + 4);
    return true;
}

// Memory one kept delta really costs: its allocation plus bookkeeping.
size_t Cost(const std::vector<uint8_t>& delta) {
    return delta.capacity() + 64;
}

bool Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    if (in.size() < 4)
        return false;
    uint32_t n = 0;
    std::memcpy(&n, in.data(), 4);
    out->resize(n);
    uLongf len = n;
    return uncompress(out->data(), &len, in.data() + 4, static_cast<uLong>(in.size() - 4)) == Z_OK &&
           len == n;
}

}  // namespace

Rewind::Rewind(size_t budget_bytes) : budget_(budget_bytes) {
    worker_ = std::thread([this] { Worker(); });
}

Rewind::~Rewind() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void Rewind::Worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return quit_ || has_pending_; });
        if (quit_)
            return;
        has_pending_ = false;
        busy_ = true;
        std::vector<uint8_t> state;
        state.swap(pending_);
        // Compress outside the lock: Pop()/Clear() wait for busy_ anyway, and
        // Push() only needs the lock to hand over the next state.
        lock.unlock();
        std::vector<uint8_t> packed;
        bool have_delta = false;
        if (!latest_.empty() && latest_.size() == state.size()) {
            scratch_.resize(state.size());
            for (size_t i = 0; i < state.size(); i++)
                scratch_[i] = state[i] ^ latest_[i];
            have_delta = Compress(scratch_, &compress_buffer_, &packed);
        }
        lock.lock();
        if (have_delta) {
            bytes_ += Cost(packed);
            deltas_.push_back(std::move(packed));
        } else {
            deltas_.clear();
            bytes_ = 0;
        }
        latest_.swap(state);
        while ((bytes_ + latest_.size() > budget_ || deltas_.size() > kMaxEntries) && !deltas_.empty()) {
            bytes_ -= Cost(deltas_.front());
            deltas_.pop_front();
        }
        busy_ = false;
        cv_.notify_all();
    }
}

void Rewind::WaitIdle(std::unique_lock<std::mutex>& lock) {
    cv_.wait(lock, [this] { return !busy_ && !has_pending_; });
}

size_t Rewind::entries() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitIdle(lock);
    return deltas_.size();
}

size_t Rewind::bytes() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitIdle(lock);
    return bytes_;
}

void Rewind::Clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitIdle(lock);
    latest_.clear();
    deltas_.clear();
    bytes_ = 0;
}

void Rewind::Push(const std::vector<uint8_t>& state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || has_pending_)
            return;
        pending_ = state;
        has_pending_ = true;
    }
    cv_.notify_all();
}

bool Rewind::Pop(std::vector<uint8_t>* state) {
    std::unique_lock<std::mutex> lock(mutex_);
    WaitIdle(lock);
    if (deltas_.empty())
        return false;
    if (!Decompress(deltas_.back(), &scratch_) || scratch_.size() != latest_.size()) {
        latest_.clear();
        deltas_.clear();
        bytes_ = 0;
        return false;
    }
    bytes_ -= Cost(deltas_.back());
    deltas_.pop_back();
    for (size_t i = 0; i < latest_.size(); i++)
        latest_[i] ^= scratch_[i];
    *state = latest_;
    return true;
}

}  // namespace cartucho
