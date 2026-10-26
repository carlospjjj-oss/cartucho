// Cartucho - rewind: going back a few seconds in a game.
//
// Every few frames the session hands a save state to Push(). Each one is kept
// as the XOR difference from the state before it, deflate-compressed: frames
// a moment apart differ in little, so a GBA state of ~400 KB usually shrinks
// to a few KB. Only the newest full state is kept; Pop() walks back by
// applying differences in reverse. The oldest entries are dropped to stay
// within the memory budget.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace cartucho {

class Rewind {
public:
    explicit Rewind(size_t budget_bytes = 48u << 20);
    ~Rewind();

    void Clear();
    // Records the state that follows the last one pushed. The diff and
    // compression run on a worker thread so big states (DS) do not stall the
    // game; if the worker is still busy with the previous state, this one is
    // skipped (the history just gets a little coarser).
    void Push(const std::vector<uint8_t>& state);
    // Steps one entry back. On success `state` holds the previous state.
    bool Pop(std::vector<uint8_t>* state);

    size_t entries();
    size_t bytes();

private:
    void Worker();
    void WaitIdle(std::unique_lock<std::mutex>& lock);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<uint8_t> pending_;
    bool has_pending_ = false;
    bool busy_ = false;
    bool quit_ = false;

    size_t budget_;
    size_t bytes_ = 0;
    std::vector<uint8_t> latest_;            // newest full state
    std::deque<std::vector<uint8_t>> deltas_;  // compressed latest[i] ^ latest[i-1]
    std::vector<uint8_t> scratch_;
    // Last: the thread starts once every other member exists.
    std::thread worker_;
};

}  // namespace cartucho
