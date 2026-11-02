// process.h — run an external program with a time limit and capture its output.
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern char** environ;

namespace tetris {

// Runs args[0] with the given arguments and returns (stdout + stderr,
// whether it finished before the timeout; 0 = no limit). Kills it on timeout.
// Throws std::runtime_error if it cannot start or exits with an error.
// Safe to call from several threads.
inline std::pair<std::string, bool> runProcess(const std::vector<std::string>& args, double timeoutSec) {
    // Create the pipe and spawn under a lock with close-on-exec set, so a
    // process started by another thread never inherits this pipe (which
    // would hold it open and delay EOF until that process exits).
    static std::mutex spawnLock;
    int fds[2];
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(spawnLock);
        if (pipe(fds) != 0) throw std::runtime_error("pipe failed");
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        int err = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if (err != 0) {
            close(fds[0]);
            throw std::runtime_error("cannot run " + args[0] + ": " + std::strerror(err));
        }
    }

    std::string text;
    bool finished = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSec);
    char buf[4096];
    for (;;) {
        int wait = -1;
        if (timeoutSec > 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                finished = false;
                break;
            }
            wait = static_cast<int>(left.count());
        }
        pollfd p{fds[0], POLLIN, 0};
        if (::poll(&p, 1, wait) == 0) continue;  // re-check the deadline
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        text.append(buf, static_cast<size_t>(n));
    }
    if (!finished) kill(pid, SIGKILL);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (finished && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
        throw std::runtime_error(args[0] + " exited abnormally: " + text.substr(0, 200));
    return {text, finished};
}

}  // namespace tetris
