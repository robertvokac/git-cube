#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gitcube {

struct ProcessResult {
    int exit_code = -1;
    bool signaled = false;
    bool timed_out = false;
    bool interrupted = false;
    bool output_too_large = false;
    // Standard output is kept separate from diagnostics so binary responses (archives
    // and blobs) can never be corrupted by a warning written to standard error.
    std::string output;
    std::string error_output;
};

class ProcessRunner {
public:
    static ProcessResult run(const std::vector<std::string>& args,
                             const std::filesystem::path& working_directory = {},
                             const std::atomic<bool>* shutdown_requested = nullptr,
                             std::chrono::seconds timeout = std::chrono::seconds::zero(),
                             std::chrono::seconds shutdown_grace = std::chrono::seconds(10),
                             std::size_t output_limit = 4 * 1024 * 1024,
                             std::size_t error_output_limit = 1024 * 1024);

    // Streams stdout directly to a mode-0600 file and terminates the process if the file
    // exceeds max_file_bytes. stderr remains bounded and available in error_output.
    static ProcessResult run_to_file(
        const std::vector<std::string>& args, const std::filesystem::path& output_file,
        std::uintmax_t max_file_bytes,
        const std::filesystem::path& working_directory = {},
        const std::atomic<bool>* shutdown_requested = nullptr,
        std::chrono::seconds timeout = std::chrono::seconds::zero(),
        std::chrono::seconds shutdown_grace = std::chrono::seconds(10),
        std::size_t error_output_limit = 1024 * 1024);
};

} // namespace gitcube
