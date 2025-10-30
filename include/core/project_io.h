#pragma once

#include <filesystem>

// Saves the current project state to a .jik project file. The provided path
// may omit the extension; the saver will ensure the custom .jik extension is
// applied. Returns true on success and false if the file could not be written.
bool saveProjectToFile(const std::filesystem::path& path);

