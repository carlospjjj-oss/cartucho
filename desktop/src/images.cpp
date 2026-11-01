#include "images.h"

#include "paths.h"

namespace cartucho {

Images::Images() {
    decoder_ = std::thread(&Images::DecodeLoop, this);
    fetcher_ = std::thread(&Images::FetchLoop, this);
}

Images::~Images() {
    {
        std::lock_guard<std::mutex> a(mutex_);
        std::lock_guard<std::mutex> b(fetch_mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    fetch_cv_.notify_all();
    decoder_.join();
    fetcher_.join();
    for (auto& [p, t] : tex_) FreeTexture(&t);
}

const Texture& Images::Get(const std::string& path, int max_side) {
    if (path.empty()) return none_;
    auto it = tex_.find(path);
    if (it != tex_.end()) return it->second;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_[path]) {
        pending_[path] = true;
        jobs_.push_back({path, max_side});
        cv_.notify_one();
    }
    return none_;
}

void Images::Forget(const std::string& path) {
    auto it = tex_.find(path);
    if (it != tex_.end()) {
        FreeTexture(&it->second);
        tex_.erase(it);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(path);
}

void Images::Update() {
    std::deque<Done> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(done_);
    }
    for (auto& d : done)
        tex_[d.path] = d.rgba.empty() ? Texture{} : TextureFromRgba(d.rgba.data(), d.w, d.h);
}

void Images::DecodeLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            // Newest first: what is on screen now.
            job = jobs_.back();
            jobs_.pop_back();
        }
        Done d{job.path, {}, 0, 0};
        DecodeImage(job.path, &d.rgba, &d.w, &d.h, job.max_side);
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(d));
    }
}

std::string Images::Cover(const Game& g) {
    {
        std::lock_guard<std::mutex> lock(fetch_mutex_);
        auto it = covers_.find(g.path);
        if (it != covers_.end()) return it->second;
    }
    const std::string now = Covers::Get().Cached(g);
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    if (!now.empty()) {
        covers_[g.path] = now;
        return now;
    }
    covers_[g.path] = "";
    fetch_.push_back({g, 0});
    fetch_cv_.notify_one();
    return "";
}

std::vector<std::string> Images::Pictures(const Game& g) {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    auto it = pictures_.find(g.path);
    if (it != pictures_.end()) return it->second;
    pictures_[g.path] = {};
    fetch_.push_back({g, 1});
    fetch_cv_.notify_one();
    return {};
}

void Images::FetchLoop() {
    for (;;) {
        std::pair<Game, int> job;
        {
            std::unique_lock<std::mutex> lock(fetch_mutex_);
            fetch_cv_.wait(lock, [&] { return stop_ || !fetch_.empty(); });
            if (stop_) return;
            job = fetch_.back();
            fetch_.pop_back();
        }
        if (job.second == 0) {
            const std::string f = Covers::Get().Fetch(job.first);
            std::lock_guard<std::mutex> lock(fetch_mutex_);
            covers_[job.first.path] = f;
        } else {
            std::vector<std::string> pics;
            for (const char* kind : {"Named_Titles", "Named_Snaps"}) {
                const std::string f = Covers::Get().FetchKind(job.first, kind);
                if (!f.empty()) pics.push_back(f);
            }
            std::lock_guard<std::mutex> lock(fetch_mutex_);
            pictures_[job.first.path] = pics;
        }
    }
}

}  // namespace cartucho
