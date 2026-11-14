#include "browser_open.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
#endif

namespace taxbroker {

#if defined(__linux__)

bool openInBrowser(const std::filesystem::path& aFile) {
    const std::string file = aFile.string();
    char program[] = "xdg-open";
    char* const arguments[] = {program, const_cast<char*>(file.c_str()), nullptr};

    // The child gets no standard streams: its messages would mix into the server output.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t child = 0;
    const int error = posix_spawnp(&child, program, &actions, nullptr, arguments, environ);

    posix_spawn_file_actions_destroy(&actions);

    if (error != 0)
    {
        return false;
    }

    std::thread([child]() {
        int status = 0;

        // Waiting ends the zombie; the result is not used.
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
    }).detach();

    return true;
}

#else

bool openInBrowser(const std::filesystem::path&) {
    return false;
}

#endif

} // namespace taxbroker
