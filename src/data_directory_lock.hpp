#pragma once

#include <filesystem>

namespace gitcube {

// Prevents two GitCube processes from recovering and executing the same persistent
// queue concurrently. The lock file intentionally remains on disk; flock ownership is
// tied to the open descriptor and is released automatically when the process exits.
class DataDirectoryLock {
public:
    explicit DataDirectoryLock(const std::filesystem::path& data_directory);
    ~DataDirectoryLock();

    DataDirectoryLock(const DataDirectoryLock&) = delete;
    DataDirectoryLock& operator=(const DataDirectoryLock&) = delete;
    DataDirectoryLock(DataDirectoryLock&&) = delete;
    DataDirectoryLock& operator=(DataDirectoryLock&&) = delete;

private:
    int descriptor_ = -1;
};

} // namespace gitcube
