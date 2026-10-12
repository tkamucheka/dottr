#ifndef DOTTR_PACKAGE_H
#define DOTTR_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Operating systems of the supported systems. */
typedef enum {
    OS_LINUX,
    OS_MACOS,
    OS_WINDOWS,
    OS_OTHER,
} OperatingSystem;

/* The operating system dottr was built for. */
OperatingSystem hostOperatingSystem(void);
const char *operatingSystemName(OperatingSystem os);

typedef enum {
    PACKAGE_MANAGER_NONE,
    PACKAGE_MANAGER_PACMAN,
    PACKAGE_MANAGER_APT,
    PACKAGE_MANAGER_DNF,
    PACKAGE_MANAGER_BREW,
    PACKAGE_MANAGER_WINGET,
} PackageManager;

/* The name of a package manager, as accepted by --package-manager. */
const char *packageManagerName(PackageManager manager);

/* The package manager named name, or PACKAGE_MANAGER_NONE. */
PackageManager packageManagerFromName(const char *name);

/* The operating system on which a package manager is supported. */
OperatingSystem packageManagerSystem(PackageManager manager);

/*
 * The package manager of the host: brew on macOS, winget on Windows, and on
 * Linux the first of pacman, apt-get, and dnf found in PATH. Returns
 * PACKAGE_MANAGER_NONE if there is none.
 */
PackageManager detectPackageManager(void);

/* Capacity of the argv array of packageInstallCommand, including NULL. */
#define PACKAGE_COMMAND_MAX 12

/*
 * Fills argv with the command that installs package with manager, prefixed
 * with sudo where the manager requires root and root is false. Returns false
 * and writes a description to message if the package cannot be installed
 * this way, such as with Homebrew as root.
 */
bool packageInstallCommand(PackageManager manager, const char *package,
                           bool root, const char **argv, char *message,
                           size_t messageSize);

#endif
