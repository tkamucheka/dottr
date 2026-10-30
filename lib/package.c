#include "dottr/package.h"

#include <stdio.h>
#include <string.h>

#include "dottr/host.h"

OperatingSystem hostOperatingSystem(void)
{
#if defined(_WIN32)
    return OS_WINDOWS;
#elif defined(__APPLE__)
    return OS_MACOS;
#elif defined(__linux__)
    return OS_LINUX;
#else
    return OS_OTHER;
#endif
}

const char *operatingSystemName(OperatingSystem os)
{
    switch (os) {
    case OS_LINUX: return "linux";
    case OS_MACOS: return "macos";
    case OS_WINDOWS: return "windows";
    case OS_OTHER: break;
    }
    return "an unsupported operating system";
}

static const struct {
    PackageManager manager;
    const char *name;
    OperatingSystem os;
} managers[] = {
    {PACKAGE_MANAGER_PACMAN, "pacman", OS_LINUX},
    {PACKAGE_MANAGER_APT, "apt", OS_LINUX},
    {PACKAGE_MANAGER_DNF, "dnf", OS_LINUX},
    {PACKAGE_MANAGER_BREW, "brew", OS_MACOS},
    {PACKAGE_MANAGER_WINGET, "winget", OS_WINDOWS},
};

#define MANAGER_COUNT (sizeof managers / sizeof managers[0])

const char *packageManagerName(PackageManager manager)
{
    for (size_t i = 0; i < MANAGER_COUNT; i++) {
        if (managers[i].manager == manager) {
            return managers[i].name;
        }
    }
    return "none";
}

PackageManager packageManagerFromName(const char *name)
{
    for (size_t i = 0; i < MANAGER_COUNT; i++) {
        if (strcmp(managers[i].name, name) == 0) {
            return managers[i].manager;
        }
    }
    return PACKAGE_MANAGER_NONE;
}

OperatingSystem packageManagerSystem(PackageManager manager)
{
    for (size_t i = 0; i < MANAGER_COUNT; i++) {
        if (managers[i].manager == manager) {
            return managers[i].os;
        }
    }
    return OS_OTHER;
}

PackageManager detectPackageManager(void)
{
    switch (hostOperatingSystem()) {
    case OS_MACOS: return PACKAGE_MANAGER_BREW;
    case OS_WINDOWS: return PACKAGE_MANAGER_WINGET;
    case OS_LINUX:
        if (hostCommandExists("pacman")) {
            return PACKAGE_MANAGER_PACMAN;
        }
        if (hostCommandExists("apt-get")) {
            return PACKAGE_MANAGER_APT;
        }
        if (hostCommandExists("dnf")) {
            return PACKAGE_MANAGER_DNF;
        }
        return PACKAGE_MANAGER_NONE;
    case OS_OTHER: break;
    }
    return PACKAGE_MANAGER_NONE;
}

bool packageInstallCommand(PackageManager manager, const char *const *packages,
                           size_t count, bool root, const char **argv, char *message,
                           size_t messageSize)
{
    size_t n = 0;
    if (count == 0 || (manager == PACKAGE_MANAGER_WINGET && count != 1)) {
        snprintf(message, messageSize,
                 "expected a nonempty package list, or one package for winget");
        return false;
    }
    switch (manager) {
    case PACKAGE_MANAGER_PACMAN:
    case PACKAGE_MANAGER_APT:
    case PACKAGE_MANAGER_DNF:
        if (!root) {
            argv[n++] = "sudo";
        }
        break;
    case PACKAGE_MANAGER_BREW:
        if (root) {
            snprintf(message, messageSize,
                     "Homebrew does not install packages as root; run "
                     "dottr as a regular user");
            return false;
        }
        break;
    case PACKAGE_MANAGER_WINGET: break;
    case PACKAGE_MANAGER_NONE:
        snprintf(message, messageSize,
                 "no supported package manager was found; dottr supports "
                 "pacman, apt, and dnf on Linux, brew on macOS, and winget "
                 "on Windows");
        return false;
    }

    switch (manager) {
    case PACKAGE_MANAGER_PACMAN:
        argv[n++] = "pacman";
        argv[n++] = "-S";
        argv[n++] = "--needed";
        argv[n++] = "--noconfirm";
        break;
    case PACKAGE_MANAGER_APT:
        argv[n++] = "apt-get";
        argv[n++] = "install";
        argv[n++] = "-y";
        break;
    case PACKAGE_MANAGER_DNF:
        argv[n++] = "dnf";
        argv[n++] = "install";
        argv[n++] = "-y";
        break;
    case PACKAGE_MANAGER_BREW:
        argv[n++] = "brew";
        argv[n++] = "install";
        break;
    case PACKAGE_MANAGER_WINGET:
        argv[n++] = "winget";
        argv[n++] = "install";
        argv[n++] = "--exact";
        argv[n++] = "--silent";
        argv[n++] = "--accept-package-agreements";
        argv[n++] = "--accept-source-agreements";
        argv[n++] = "--id";
        break;
    case PACKAGE_MANAGER_NONE: break;
    }
    for (size_t i = 0; i < count; i++) {
        argv[n++] = packages[i];
    }
    argv[n] = NULL;
    return true;
}

bool packageQueryCommand(PackageManager manager, const char *package,
                         const char **argv)
{
    if (manager != PACKAGE_MANAGER_WINGET) {
        return false;
    }
    size_t n = 0;
    argv[n++] = "winget";
    argv[n++] = "list";
    argv[n++] = "--exact";
    argv[n++] = "--accept-source-agreements";
    argv[n++] = "--id";
    argv[n++] = package;
    argv[n] = NULL;
    return true;
}

SystemSet packageManagerSystems(PackageManager manager)
{
    switch (manager) {
    case PACKAGE_MANAGER_PACMAN: return SYSTEM_PACMAN;
    case PACKAGE_MANAGER_APT: return SYSTEM_APT;
    case PACKAGE_MANAGER_DNF: return SYSTEM_DNF;
    case PACKAGE_MANAGER_BREW: return SYSTEM_BREW;
    case PACKAGE_MANAGER_WINGET: return SYSTEM_WINGET;
    case PACKAGE_MANAGER_NONE: break;
    }
    return 0;
}

static const struct {
    const char *name;
    SystemSet systems;
} selectors[] = {
    {"linux", SYSTEM_LINUX},   {"macos", SYSTEM_MACOS},
    {"windows", SYSTEM_WINDOWS}, {"pacman", SYSTEM_PACMAN},
    {"apt", SYSTEM_APT},       {"dnf", SYSTEM_DNF},
    {"brew", SYSTEM_BREW},     {"winget", SYSTEM_WINGET},
};

SystemSet systemSelector(const char *name)
{
    for (size_t i = 0; i < sizeof selectors / sizeof selectors[0]; i++) {
        if (strcmp(selectors[i].name, name) == 0) {
            return selectors[i].systems;
        }
    }
    return 0;
}

void describeSystems(SystemSet systems, char *buffer, size_t size)
{
    size_t used = 0;
    buffer[0] = '\0';
    /* Operating systems first, then the package managers not covered. */
    for (size_t i = 0; i < sizeof selectors / sizeof selectors[0]; i++) {
        SystemSet bits = selectors[i].systems;
        if ((systems & bits) != bits) {
            continue;
        }
        systems &= ~bits;
        int written = snprintf(buffer + used, size - used, "%s%s",
                               used > 0 ? ", " : "", selectors[i].name);
        if (written < 0 || (size_t)written >= size - used) {
            return;
        }
        used += (size_t)written;
    }
}
