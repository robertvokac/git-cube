#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace gitcube {

struct ProcessResult {
    int exit_code = -1;
    bool signaled = false;
    bool timed_out = false;
    bool interrupted = false;
    std::string output;
};

class ProcessRunner {
public:
    static ProcessResult run(const std::vector<std::string>& args,
                             const std::filesystem::path& working_directory = {},
                             const std::atomic<bool>* shutdown_requested = nullptr,
                             std::chrono::seconds timeout = std::chrono::seconds::zero(),
                             std::chrono::seconds shutdown_grace = std::chrono::seconds(10),
                             std::size_t output_limit = 4 * 1024 * 1024);
};

} // namespace gitcube
