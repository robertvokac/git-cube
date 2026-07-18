#include "application.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <sys/stat.h>

namespace {
volatile std::sig_atomic_t signal_received = 0;

void signal_handler(int) {
    signal_received = 1;
}

void print_usage(const char* program) {
    std::cout << "GitCube 0.1.0\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --data-dir PATH   Data directory (default: $XDG_DATA_HOME/gitcube)\n"
              << "  --port PORT       HTTP port (default: 9999)\n"
              << "  --workers COUNT   Concurrent Git workers (default: 2)\n"
              << "  --bind ADDRESS    IPv4 address (default: 127.0.0.1)\n"
              << "  --help            Show this help\n"
              << "  --version         Show version\n";
}

int parse_integer(const std::string& value, const char* name, int minimum, int maximum) {
    std::size_t consumed = 0;
    int parsed = 0;
    try { parsed = std::stoi(value, &consumed); }
    catch (...) { throw std::runtime_error(std::string("Invalid ") + name + ": " + value); }
    if (consumed != value.size() || parsed < minimum || parsed > maximum) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    }
    return parsed;
}

std::filesystem::path default_data_directory() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        const std::filesystem::path path(xdg);
        if (path.is_absolute()) return path / "gitcube";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "gitcube";
    }
    return std::filesystem::current_path() / "data";
}
}

int main(int argc, char** argv) {
    gitcube::Config config;
    config.data_dir = default_data_directory();
    umask(S_IRWXG | S_IRWXO);

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* option) -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + option);
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "--version") {
                std::cout << "GitCube 0.1.0\n";
                return 0;
            } else if (arg == "--data-dir") {
                config.data_dir = next("--data-dir");
            } else if (arg == "--port") {
                config.port = parse_integer(next("--port"), "port", 1, 65535);
            } else if (arg == "--workers") {
                config.workers = parse_integer(next("--workers"), "worker count", 1, 64);
            } else if (arg == "--bind") {
                config.bind_address = next("--bind");
            } else {
                throw std::runtime_error("Unknown option: " + arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n\n";
        print_usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> watcher_done{false};
    std::thread signal_watcher([&] {
        while (!watcher_done.load(std::memory_order_relaxed)) {
            if (signal_received) {
                shutdown_requested.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    gitcube::Application application(std::move(config), shutdown_requested);
    const int result = application.run();
    watcher_done.store(true, std::memory_order_relaxed);
    if (signal_watcher.joinable()) signal_watcher.join();
    return result;
}
