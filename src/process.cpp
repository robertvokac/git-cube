#include "process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gitcube {

ProcessResult ProcessRunner::run(const std::vector<std::string>& args,
                                 const std::filesystem::path& working_directory,
                                 const std::atomic<bool>* shutdown_requested,
                                 std::chrono::seconds timeout,
                                 std::chrono::seconds shutdown_grace,
                                 std::size_t output_limit) {
    if (args.empty()) throw std::invalid_argument("ProcessRunner requires a command");

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) throw std::runtime_error("pipe() failed: " + std::string(std::strerror(errno)));

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        setpgid(0, 0);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("LC_ALL", "C", 1);
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        if (pipe_fd[1] > STDERR_FILENO) close(pipe_fd[1]);
        if (!working_directory.empty() && chdir(working_directory.c_str()) != 0) {
            const std::string message = "chdir failed: " + std::string(std::strerror(errno)) + "\n";
            write(STDERR_FILENO, message.data(), message.size());
            _exit(126);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        const std::string message = "execvp failed: " + std::string(std::strerror(errno)) + "\n";
        write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    close(pipe_fd[1]);
    const int old_flags = fcntl(pipe_fd[0], F_GETFL, 0);
    fcntl(pipe_fd[0], F_SETFL, old_flags | O_NONBLOCK);

    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> shutdown_seen;
    bool term_sent = false;
    bool kill_sent = false;
    bool child_done = false;
    int status = 0;

    while (!child_done) {
        char buffer[8192];
        while (true) {
            const ssize_t count = read(pipe_fd[0], buffer, sizeof(buffer));
            if (count > 0) {
                const std::size_t available = output_limit > result.output.size() ? output_limit - result.output.size() : 0;
                if (available > 0) result.output.append(buffer, std::min<std::size_t>(static_cast<std::size_t>(count), available));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }

        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            child_done = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            close(pipe_fd[0]);
            throw std::runtime_error("waitpid() failed: " + std::string(std::strerror(errno)));
        }

        const auto now = std::chrono::steady_clock::now();
        if (timeout != std::chrono::seconds::zero() && now - started >= timeout && !term_sent) {
            result.timed_out = true;
            kill(-pid, SIGTERM);
            term_sent = true;
            shutdown_seen = now;
        }
        if (shutdown_requested && shutdown_requested->load(std::memory_order_relaxed)) {
            result.interrupted = true;
            if (!shutdown_seen) shutdown_seen = now;
            if (!term_sent && now - *shutdown_seen >= shutdown_grace) {
                kill(-pid, SIGTERM);
                term_sent = true;
            }
        }
        if (term_sent && shutdown_seen && !kill_sent && now - *shutdown_seen >= shutdown_grace + std::chrono::seconds(5)) {
            kill(-pid, SIGKILL);
            kill_sent = true;
        }

        pollfd descriptor{pipe_fd[0], POLLIN, 0};
        poll(&descriptor, 1, 100);
    }

    char buffer[8192];
    while (true) {
        const ssize_t count = read(pipe_fd[0], buffer, sizeof(buffer));
        if (count > 0) {
            const std::size_t available = output_limit > result.output.size() ? output_limit - result.output.size() : 0;
            if (available > 0) result.output.append(buffer, std::min<std::size_t>(static_cast<std::size_t>(count), available));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(pipe_fd[0]);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

} // namespace gitcube
