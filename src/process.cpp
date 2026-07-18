#include "process.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace gitcube {
namespace {

class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const { return value_; }
    int release() {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) {
        if (value_ >= 0) close(value_);
        value_ = value;
    }

private:
    int value_ = -1;
};

struct Pipe {
    FileDescriptor read;
    FileDescriptor write;
};

Pipe make_pipe() {
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) {
        throw std::runtime_error("pipe2() failed: " + std::string(std::strerror(errno)));
    }
    const int read_flags = fcntl(descriptors[0], F_GETFL, 0);
    if (read_flags < 0 || fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        const int error = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::runtime_error("Cannot make process-output pipe nonblocking: " +
                                 std::string(std::strerror(error)));
    }
    return {FileDescriptor(descriptors[0]), FileDescriptor(descriptors[1])};
}

bool excluded_environment_variable(std::string_view name) {
    return name.starts_with("GIT_") || name == "GCM_INTERACTIVE" || name == "LC_ALL" ||
           name == "ZIPOPT" || name == "CURL_HOME";
}

std::vector<std::string> controlled_environment() {
    std::vector<std::string> values;
    for (char** item = environ; item && *item; ++item) {
        const std::string_view entry(*item);
        const auto equals = entry.find('=');
        const auto name = entry.substr(0, equals);
        if (!excluded_environment_variable(name)) values.emplace_back(entry);
    }
    values.emplace_back("GIT_TERMINAL_PROMPT=0");
    values.emplace_back("GCM_INTERACTIVE=never");
    values.emplace_back("GIT_CONFIG_NOSYSTEM=1");
    values.emplace_back("GIT_CONFIG_GLOBAL=/dev/null");
    values.emplace_back("LC_ALL=C");
    values.emplace_back("ZIPOPT=");
    return values;
}

void append_available(int descriptor, std::string& destination, std::size_t limit) {
    char buffer[8192];
    while (true) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            const std::size_t available =
                limit > destination.size() ? limit - destination.size() : 0;
            if (available > 0) {
                destination.append(
                    buffer, std::min<std::size_t>(static_cast<std::size_t>(count), available));
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

void signal_process_group(pid_t pid, int signal) {
    if (kill(-pid, signal) != 0 && errno == ESRCH) {
        // POSIX_SPAWN_SETPGROUP should already have created the group. The direct signal
        // is a defensive fallback for a platform/kernel failure during that setup.
        kill(pid, signal);
    }
}

void check_spawn_action(int result, std::string_view action) {
    if (result != 0) {
        throw std::runtime_error(std::string(action) + " failed: " + std::strerror(result));
    }
}

ProcessResult run_process(const std::vector<std::string>& args,
                          const std::filesystem::path& working_directory,
                          const std::atomic<bool>* shutdown_requested,
                          std::chrono::seconds timeout,
                          std::chrono::seconds shutdown_grace,
                          std::size_t output_limit,
                          std::size_t error_output_limit,
                          const std::filesystem::path* output_file,
                          std::uintmax_t max_file_bytes) {
    if (args.empty()) throw std::invalid_argument("ProcessRunner requires a command");

    std::optional<Pipe> standard_output;
    FileDescriptor output_file_descriptor;
    if (output_file) {
        const int descriptor =
            open(output_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            throw std::runtime_error("Cannot open process output file '" +
                                     output_file->string() + "': " + std::strerror(errno));
        }
        output_file_descriptor.reset(descriptor);
    } else {
        standard_output.emplace(make_pipe());
    }
    Pipe standard_error = make_pipe();

    posix_spawn_file_actions_t actions;
    check_spawn_action(posix_spawn_file_actions_init(&actions), "posix_spawn_file_actions_init");
    bool actions_initialized = true;
    posix_spawnattr_t attributes;
    bool attributes_initialized = false;
    try {
        check_spawn_action(
            posix_spawn_file_actions_adddup2(
                &actions,
                output_file ? output_file_descriptor.get() : standard_output->write.get(),
                STDOUT_FILENO),
            "posix_spawn stdout setup");
        check_spawn_action(
            posix_spawn_file_actions_adddup2(&actions, standard_error.write.get(), STDERR_FILENO),
            "posix_spawn stderr setup");
        if (standard_output) {
            check_spawn_action(
                posix_spawn_file_actions_addclose(&actions, standard_output->read.get()),
                "posix_spawn stdout read close");
        }
        check_spawn_action(posix_spawn_file_actions_addclose(&actions, standard_error.read.get()),
                           "posix_spawn stderr read close");
        check_spawn_action(
            posix_spawn_file_actions_addclose(
                &actions, output_file ? output_file_descriptor.get()
                                      : standard_output->write.get()),
            "posix_spawn stdout write close");
        check_spawn_action(posix_spawn_file_actions_addclose(&actions, standard_error.write.get()),
                           "posix_spawn stderr write close");
        if (!working_directory.empty()) {
            check_spawn_action(
                posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str()),
                "posix_spawn working-directory setup");
        }

        check_spawn_action(posix_spawnattr_init(&attributes), "posix_spawnattr_init");
        attributes_initialized = true;
        check_spawn_action(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP),
                           "posix_spawnattr_setflags");
        check_spawn_action(posix_spawnattr_setpgroup(&attributes, 0),
                           "posix_spawnattr_setpgroup");

        std::vector<char*> arguments;
        arguments.reserve(args.size() + 1);
        for (const auto& argument : args) {
            arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        arguments.push_back(nullptr);

        auto environment_values = controlled_environment();
        std::vector<char*> environment;
        environment.reserve(environment_values.size() + 1);
        for (auto& value : environment_values) environment.push_back(value.data());
        environment.push_back(nullptr);

        pid_t pid = -1;
        const int spawn_result =
            posix_spawnp(&pid, arguments[0], &actions, &attributes, arguments.data(),
                         environment.data());
        posix_spawnattr_destroy(&attributes);
        attributes_initialized = false;
        posix_spawn_file_actions_destroy(&actions);
        actions_initialized = false;
        if (spawn_result != 0) {
            throw std::runtime_error("posix_spawnp() failed for '" + args.front() +
                                     "': " + std::strerror(spawn_result));
        }

        if (standard_output) standard_output->write.reset();
        standard_error.write.reset();

        ProcessResult result;
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> shutdown_seen;
        std::optional<std::chrono::steady_clock::time_point> term_sent_at;
        bool kill_sent = false;
        int status = 0;

        while (true) {
            if (standard_output) {
                append_available(standard_output->read.get(), result.output, output_limit);
            }
            append_available(standard_error.read.get(), result.error_output, error_output_limit);

            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) break;
            if (waited < 0 && errno != EINTR) {
                const int wait_error = errno;
                signal_process_group(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                throw std::runtime_error("waitpid() failed: " +
                                         std::string(std::strerror(wait_error)));
            }

            const auto now = std::chrono::steady_clock::now();
            if (output_file) {
                struct stat file_status {};
                if (fstat(output_file_descriptor.get(), &file_status) == 0 &&
                    file_status.st_size >= 0 &&
                    static_cast<std::uintmax_t>(file_status.st_size) > max_file_bytes) {
                    result.output_too_large = true;
                    if (!term_sent_at) {
                        signal_process_group(pid, SIGTERM);
                        term_sent_at = now;
                    }
                }
            }
            if (timeout != std::chrono::seconds::zero() && now - started >= timeout &&
                !term_sent_at) {
                result.timed_out = true;
                signal_process_group(pid, SIGTERM);
                term_sent_at = now;
            }
            if (shutdown_requested &&
                shutdown_requested->load(std::memory_order_relaxed)) {
                result.interrupted = true;
                if (!shutdown_seen) shutdown_seen = now;
                if (!term_sent_at && now - *shutdown_seen >= shutdown_grace) {
                    signal_process_group(pid, SIGTERM);
                    term_sent_at = now;
                }
            }
            if (term_sent_at && !kill_sent &&
                now - *term_sent_at >= std::chrono::seconds(5)) {
                signal_process_group(pid, SIGKILL);
                kill_sent = true;
            }

            pollfd descriptors[2]{
                {standard_output ? standard_output->read.get() : -1, POLLIN, 0},
                {standard_error.read.get(), POLLIN, 0},
            };
            const int poll_result = poll(descriptors, 2, 100);
            if (poll_result < 0 && errno != EINTR) {
                signal_process_group(pid, SIGKILL);
            }
        }

        if (standard_output) {
            append_available(standard_output->read.get(), result.output, output_limit);
        }
        append_available(standard_error.read.get(), result.error_output, error_output_limit);
        if (output_file) {
            struct stat file_status {};
            if (fstat(output_file_descriptor.get(), &file_status) == 0 &&
                file_status.st_size >= 0 &&
                static_cast<std::uintmax_t>(file_status.st_size) > max_file_bytes) {
                result.output_too_large = true;
            }
        }

        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.signaled = true;
            result.exit_code = 128 + WTERMSIG(status);
        }
        return result;
    } catch (...) {
        if (attributes_initialized) posix_spawnattr_destroy(&attributes);
        if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
        throw;
    }
}

} // namespace

ProcessResult ProcessRunner::run(const std::vector<std::string>& args,
                                 const std::filesystem::path& working_directory,
                                 const std::atomic<bool>* shutdown_requested,
                                 std::chrono::seconds timeout,
                                 std::chrono::seconds shutdown_grace,
                                 std::size_t output_limit,
                                 std::size_t error_output_limit) {
    return run_process(args, working_directory, shutdown_requested, timeout, shutdown_grace,
                       output_limit, error_output_limit, nullptr, 0);
}

ProcessResult ProcessRunner::run_to_file(
    const std::vector<std::string>& args, const std::filesystem::path& output_file,
    std::uintmax_t max_file_bytes, const std::filesystem::path& working_directory,
    const std::atomic<bool>* shutdown_requested, std::chrono::seconds timeout,
    std::chrono::seconds shutdown_grace, std::size_t error_output_limit) {
    return run_process(args, working_directory, shutdown_requested, timeout, shutdown_grace,
                       0, error_output_limit, &output_file, max_file_bytes);
}

} // namespace gitcube
