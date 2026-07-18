#include "data_directory_lock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace gitcube {

DataDirectoryLock::DataDirectoryLock(const std::filesystem::path& data_directory) {
    const auto lock_path = data_directory / "gitcube.lock";
    descriptor_ = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor_ < 0) {
        throw std::runtime_error("Cannot open data-directory lock '" + lock_path.string() +
                                 "': " + std::strerror(errno));
    }
    if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        close(descriptor_);
        descriptor_ = -1;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            throw std::runtime_error("Another GitCube instance is already using data directory '" +
                                     data_directory.string() + "'");
        }
        throw std::runtime_error("Cannot lock data directory '" + data_directory.string() +
                                 "': " + std::strerror(error));
    }

    const std::string owner = std::to_string(static_cast<long long>(getpid())) + "\n";
    if (ftruncate(descriptor_, 0) == 0 && lseek(descriptor_, 0, SEEK_SET) >= 0) {
        const ssize_t ignored = write(descriptor_, owner.data(), owner.size());
        (void)ignored;
    }
}

DataDirectoryLock::~DataDirectoryLock() {
    if (descriptor_ >= 0) {
        flock(descriptor_, LOCK_UN);
        close(descriptor_);
    }
}

} // namespace gitcube
