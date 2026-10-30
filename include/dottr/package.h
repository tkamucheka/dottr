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

/*
 * A set of systems, one bit per package manager. The statements of an `on`
 * block apply on a set of systems.
 */
typedef unsigned SystemSet;

#define SYSTEM_ARCH 0x01u
#define SYSTEM_DEBIAN 0x02u
#define SYSTEM_FEDORA 0x04u
#define SYSTEM_MACOS 0x08u
#define SYSTEM_WINDOWS 0x10u
#define SYSTEM_LINUX (SYSTEM_ARCH | SYSTEM_DEBIAN | SYSTEM_FEDORA)
#define SYSTEM_ALL (SYSTEM_LINUX | SYSTEM_MACOS | SYSTEM_WINDOWS)

/* The systems of a package manager, or 0 for PACKAGE_MANAGER_NONE. */
SystemSet packageManagerSystems(PackageManager manager);

/*
 * The systems named by an `on` selector: an operating system (linux, macos,
 * windows) or a distribution family. Returns 0 for an unknown selector.
 */
SystemSet systemSelector(const char *name);

/* Family replacement for a removed package-manager selector, or NULL. */
const char *systemSelectorReplacement(const char *name);

/* Comma-separated list of the selectors valid in an `on` block. */
#define SYSTEM_SELECTORS "linux, arch, debian, fedora, redhat, rhel, macos, windows"

/*
 * Describes a set of systems for a diagnostic, naming an operating system
 * when all of its systems are included: "linux, macos" or "debian, fedora".
 */
void describeSystems(SystemSet systems, char *buffer, size_t size);

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

/* Maximum fixed arguments plus NULL; add the number of package names. */
#define PACKAGE_COMMAND_OVERHEAD 9
/* Capacity of the fixed-size query command, including NULL. */
#define PACKAGE_QUERY_COMMAND_MAX 7

/*
 * Fills argv (capacity count + PACKAGE_COMMAND_OVERHEAD) with a command
 * that installs packages with manager, prefixed
 * with sudo where the manager requires root and root is false. Returns false
 * and writes a description to message if the package cannot be installed
 * this way, such as with Homebrew as root.
 */
bool packageInstallCommand(PackageManager manager, const char *const *packages,
                           size_t count, bool root, const char **argv, char *message,
                           size_t messageSize);

/*
 * Fills argv with a command that exits with status 0 if package is already
 * installed, for a manager that reports the installation of an installed
 * package as an error (winget). Returns false for the other managers, whose
 * installation succeeds for an installed package.
 */
bool packageQueryCommand(PackageManager manager, const char *package,
                         const char **argv);

#endif
