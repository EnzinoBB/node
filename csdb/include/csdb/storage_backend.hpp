#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <csdb/database.hpp>

namespace csdb::backend {

inline const char* const kMarkerFile = ".backend";

bool isKnown(const std::string& name);
// openEngines=false never opens an engine (another process holds the folder): small files then
// count as databases, since a running node has one
std::vector<std::string> detect(const std::string& dbPath, bool openEngines = true);
std::string readMarker(const std::string& dbPath);
bool writeMarker(const std::string& dbPath, const std::string& name);
std::shared_ptr<Database> open(const std::string& name, const std::string& dir);
std::vector<std::filesystem::path> files(const std::string& name, const std::string& dir);

}  // namespace csdb::backend
