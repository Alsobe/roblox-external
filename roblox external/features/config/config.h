#pragma once
#include <string>
#include <vector>

namespace config {
    std::string GetConfigDir();
    bool Save(const std::string& name);
    bool Load(const std::string& name);
    bool Delete(const std::string& name);
    bool Rename(const std::string& old_name, const std::string& new_name);
    std::vector<std::string> GetConfigList();
    void OpenConfigFolder();
    void EnsureConfigDir();
}