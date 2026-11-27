#pragma once

// Scratch homes of test runs that died. Every test makes /tmp/fugue-test-home-XXXXXX and removes it at exit
// (tests/test_environment.cpp), but a test killed by a timeout, a crash or SIGKILL never gets to: 242 of them
// piled up in /tmp by 2026-11-27. Each home holds an `.owner` file with its process's pid and start time
// (field 22 of /proc/<pid>/stat); the next test to start removes the homes whose owner is gone.
//
// The sweep is one directory listing and a lstat per home. A home is removed only when it is a real folder (never a
// link) owned by this user and its owner is dead: the pid is not alive, or its start time differs (the pid was
// reused). A home with no record (made before this change, or just made and not written yet) is left alone unless
// it is older than a day.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace test_home_sweep {

inline constexpr const char *kPrefix = "fugue-test-home-";
inline constexpr const char *kOwnerFile = ".owner";
inline constexpr long kOldSeconds = 24 * 60 * 60;

// The start time of a process, in clock ticks since boot; 0 when there is no such process.
inline unsigned long long startTime(long pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file)
        return 0;
    char text[1024];
    const size_t n = std::fread(text, 1, sizeof text - 1, file);
    std::fclose(file);
    text[n] = 0;
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so count from the last ')'. Field 3 is the
    // state, field 22 the start time.
    const char *rest = std::strrchr(text, ')');
    if (!rest)
        return 0;
    ++rest;
    for (int field = 3; field < 22; ++field) {
        rest = std::strchr(rest + 1, ' ');
        if (!rest)
            return 0;
    }
    return std::strtoull(rest + 1, nullptr, 10);
}

// What this process writes into its home's `.owner`.
inline std::string ownerRecord() {
    return std::to_string(::getpid()) + " " + std::to_string(startTime(::getpid())) + "\n";
}

enum class Owner { None, Alive, Dead };

inline Owner ownerOf(const std::string &home) {
    const int fd = ::open((home + "/" + kOwnerFile).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return Owner::None;
    char text[128];
    const ssize_t n = ::read(fd, text, sizeof text - 1);
    ::close(fd);
    if (n <= 0)
        return Owner::None;
    text[n] = 0;
    char *end = nullptr;
    const long pid = std::strtol(text, &end, 10);
    const unsigned long long recorded = std::strtoull(end, nullptr, 10);
    if (pid <= 0 || recorded == 0)
        return Owner::None;
    return startTime(pid) == recorded ? Owner::Alive : Owner::Dead;
}

// Removes the homes under `root` named `prefix`* whose owner is dead (or, with no record, older than a day).
inline void sweep(const std::string &root, const std::string &prefix = kPrefix, std::time_t now = std::time(nullptr)) {
    DIR *dir = ::opendir(root.c_str());
    if (!dir)
        return;
    while (const dirent *entry = ::readdir(dir)) {
        if (std::strncmp(entry->d_name, prefix.c_str(), prefix.size()) != 0)
            continue;
        const std::string home = root + "/" + entry->d_name;
        struct stat info;
        if (::lstat(home.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid())
            continue;
        const Owner owner = ownerOf(home);
        if (owner == Owner::Alive || (owner == Owner::None && now - info.st_mtime <= kOldSeconds))
            continue;
        std::error_code ignored; // another test may be removing the same home
        std::filesystem::remove_all(home, ignored);
    }
    ::closedir(dir);
}

// Makes the home's `.owner`.
inline void claim(const std::string &home) {
    if (std::FILE *file = std::fopen((home + "/" + kOwnerFile).c_str(), "w")) {
        const std::string record = ownerRecord();
        std::fwrite(record.data(), 1, record.size(), file);
        std::fclose(file);
    }
}

} // namespace test_home_sweep
