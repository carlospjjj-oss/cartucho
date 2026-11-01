// Cartucho PC - the player's settings (data/settings.json), with the same
// keys as the Android app where they mean the same thing, and per-game
// overrides (data/pergame/<SYS>/<save name>.json, as on Android).
#pragma once

#include <json.hpp>

#include <mutex>
#include <string>

namespace cartucho {

class Settings {
public:
    static Settings& Get();
    void Load();
    void Save();

    std::string Str(const std::string& key, const std::string& def) const;
    int Int(const std::string& key, int def) const;
    bool Bool(const std::string& key, bool def) const;
    void Set(const std::string& key, const nlohmann::json& value);
    nlohmann::json Raw(const std::string& key) const;

private:
    mutable std::mutex mutex_;
    nlohmann::json j_ = nlohmann::json::object();
    std::string path_;
};

// A game's own settings over the app's.
class GameSettings {
public:
    GameSettings(const std::string& system_key, const std::string& save_name);
    std::string Str(const std::string& key, const std::string& def) const;
    bool Has(const std::string& key) const { return j_.contains(key); }
    void Set(const std::string& key, const std::string& value);  // "" removes
    const nlohmann::json& json() const { return j_; }

private:
    std::string path_;
    nlohmann::json j_ = nlohmann::json::object();
};

}  // namespace cartucho
