/**
 * WuWa SDK proc maps example
 *
 * Compile: g++ -std=c++17 -o example2 example2.cpp
 * Run: sudo ./example2 <pid>
 */

#include "wuwa.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>

static bool parse_pid(const char* text, pid_t& pid) {
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (value <= 0 || value > std::numeric_limits<pid_t>::max()) {
        return false;
    }

    pid = static_cast<pid_t>(value);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <pid>" << std::endl;
        return 1;
    }

    pid_t pid;
    if (!parse_pid(argv[1], pid)) {
        std::cerr << "Invalid PID: " << argv[1] << std::endl;
        return 1;
    }

    wuwa::WuWaDriver driver;
    if (!driver.connect()) {
        std::cerr << "Failed to connect to WuWa driver. Are you root?" << std::endl;
        return 1;
    }

    auto maps = driver.get_proc_maps(pid);
    if (!maps) {
        std::cerr << "Failed to get maps for PID: " << pid << std::endl;
        return 1;
    }

    std::cout << *maps;
    return 0;
}
