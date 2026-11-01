#include "settings.h"

#include "paths.h"

namespace cartucho {

Settings& Settings::Get() {
    static Settings s;
    return s;
}

void Settings::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = JoinPath(Paths::Get().data, "settings.json");
    std::string text;
    if (ReadText(path_, &text)) {
        auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_object())
            j_ = parsed;
    }
}

void Settings::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!path_.empty())
        WriteText(path_, j_.dump(2));
}

std::string Settings::Str(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = j_.find(key);
    if (it == j_.end())
        return def;
    if (it->is_string())
        return it->get<std::string>();
    return it->dump();
}

int Settings::Int(const std::string& key, int def) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = j_.find(key);
    if (it == j_.end())
        return def;
    if (it->is_number())
        return it->get<int>();
    if (it->is_string())
        return std::atoi(it->get<std::string>().c_str());
    return def;
}

bool Settings::Bool(const std::string& key, bool def) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = j_.find(key);
    return (it != j_.end() && it->is_boolean()) ? it->get<bool>() : def;
}

void Settings::Set(const std::string& key, const nlohmann::json& value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value.is_null())
            j_.erase(key);
        else
            j_[key] = value;
    }
    Save();
}

nlohmann::json Settings::Raw(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = j_.find(key);
    return it == j_.end() ? nlohmann::json() : *it;
}

GameSettings::GameSettings(const std::string& system_key, const std::string& save_name) {
    path_ = JoinPath(JoinPath(JoinPath(Paths::Get().data, "pergame"), system_key), save_name + ".json");
    std::string text;
    if (ReadText(path_, &text)) {
        auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_object())
            j_ = parsed;
    }
}

std::string GameSettings::Str(const std::string& key, const std::string& def) const {
    auto it = j_.find(key);
    if (it != j_.end() && it->is_string() && !it->get<std::string>().empty())
        return it->get<std::string>();
    return Settings::Get().Str(key, def);
}

void GameSettings::Set(const std::string& key, const std::string& value) {
    if (value.empty())
        j_.erase(key);
    else
        j_[key] = value;
    if (j_.empty())
        RemoveFile(path_);
    else
        WriteText(path_, j_.dump(2));
}

}  // namespace cartucho
