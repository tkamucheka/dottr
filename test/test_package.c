#include <stdio.h>
#include <string.h>

#include "dottr/package.h"
#include "test.h"

/* Joins argv with spaces into buffer. */
static const char *joinCommand(const char **argv, char *buffer, size_t size)
{
    buffer[0] = '\0';
    for (size_t i = 0; argv[i]; i++) {
        if (i > 0) {
            strncat(buffer, " ", size - strlen(buffer) - 1);
        }
        strncat(buffer, argv[i], size - strlen(buffer) - 1);
    }
    return buffer;
}

static void expectCommand(PackageManager manager, bool root,
                          const char *expected)
{
    testCount++;
    const char *argv[PACKAGE_COMMAND_OVERHEAD + 1];
    const char *packages[] = {"git"};
    char message[256];
    char command[512];
    if (!packageInstallCommand(manager, packages, 1, root, argv, message,
                               sizeof message)) {
        TEST_FAIL("package: %s%s: unexpected error: %s",
                  packageManagerName(manager), root ? " as root" : "", message);
        return;
    }
    joinCommand(argv, command, sizeof command);
    if (strcmp(command, expected) != 0) {
        TEST_FAIL("package: %s%s: expected '%s', got '%s'",
                  packageManagerName(manager), root ? " as root" : "",
                  expected, command);
    }
}

static void expectError(PackageManager manager, bool root,
                        const char *expected)
{
    testCount++;
    const char *argv[PACKAGE_COMMAND_OVERHEAD + 1];
    const char *packages[] = {"git"};
    char message[256] = "";
    if (packageInstallCommand(manager, packages, 1, root, argv, message,
                              sizeof message)) {
        TEST_FAIL("package: %s%s: expected an error",
                  packageManagerName(manager), root ? " as root" : "");
    } else if (!strstr(message, expected)) {
        TEST_FAIL("package: %s%s: expected '%s' in '%s'",
                  packageManagerName(manager), root ? " as root" : "",
                  expected, message);
    }
}

static void testQueries(void)
{
    const char *argv[PACKAGE_COMMAND_OVERHEAD + 1];
    char command[512];

    testCount++;
    if (!packageQueryCommand(PACKAGE_MANAGER_WINGET, "Git.Git", argv)) {
        TEST_FAIL("package: winget must query installed packages");
    } else if (strcmp(joinCommand(argv, command, sizeof command),
                      "winget list --exact --accept-source-agreements "
                      "--id Git.Git") != 0) {
        TEST_FAIL("package: unexpected winget query '%s'", command);
    }

    static const PackageManager idempotent[] = {
        PACKAGE_MANAGER_PACMAN, PACKAGE_MANAGER_APT, PACKAGE_MANAGER_DNF,
        PACKAGE_MANAGER_BREW, PACKAGE_MANAGER_NONE,
    };
    for (size_t i = 0; i < sizeof idempotent / sizeof idempotent[0]; i++) {
        testCount++;
        if (packageQueryCommand(idempotent[i], "git", argv)) {
            TEST_FAIL("package: %s needs no query",
                      packageManagerName(idempotent[i]));
        }
    }
}

static void testNames(void)
{
    static const PackageManager all[] = {
        PACKAGE_MANAGER_PACMAN, PACKAGE_MANAGER_APT, PACKAGE_MANAGER_DNF,
        PACKAGE_MANAGER_BREW, PACKAGE_MANAGER_WINGET,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        testCount++;
        const char *name = packageManagerName(all[i]);
        if (packageManagerFromName(name) != all[i]) {
            TEST_FAIL("package: name '%s' does not map back", name);
        }
    }

    testCount++;
    if (packageManagerFromName("yum") != PACKAGE_MANAGER_NONE ||
        packageManagerFromName("apt-get") != PACKAGE_MANAGER_NONE) {
        TEST_FAIL("package: unknown names must map to none");
    }

    testCount++;
    if (packageManagerSystem(PACKAGE_MANAGER_PACMAN) != OS_LINUX ||
        packageManagerSystem(PACKAGE_MANAGER_APT) != OS_LINUX ||
        packageManagerSystem(PACKAGE_MANAGER_DNF) != OS_LINUX ||
        packageManagerSystem(PACKAGE_MANAGER_BREW) != OS_MACOS ||
        packageManagerSystem(PACKAGE_MANAGER_WINGET) != OS_WINDOWS) {
        TEST_FAIL("package: wrong operating system of a package manager");
    }
}

static void expectDescription(SystemSet systems, const char *expected)
{
    testCount++;
    char buffer[96];
    describeSystems(systems, buffer, sizeof buffer);
    if (strcmp(buffer, expected) != 0) {
        TEST_FAIL("package: systems 0x%x: expected '%s', got '%s'", systems,
                  expected, buffer);
    }
}

static void testSystems(void)
{
    testCount++;
    if (systemSelector("linux") != SYSTEM_LINUX ||
        systemSelector("macos") != SYSTEM_MACOS ||
        systemSelector("windows") != SYSTEM_WINDOWS ||
        systemSelector("arch") != SYSTEM_ARCH ||
        systemSelector("debian") != SYSTEM_DEBIAN ||
        systemSelector("fedora") != SYSTEM_FEDORA ||
        systemSelector("redhat") != SYSTEM_FEDORA ||
        systemSelector("rhel") != SYSTEM_FEDORA || systemSelector("bsd") != 0 ||
        systemSelector("Linux") != 0) {
        TEST_FAIL("package: wrong systems of a selector");
    }

    expectDescription(SYSTEM_ALL, "linux, macos, windows");
    expectDescription(SYSTEM_LINUX | SYSTEM_WINDOWS, "linux, windows");
    expectDescription(SYSTEM_DEBIAN | SYSTEM_FEDORA, "debian, fedora");
    expectDescription(SYSTEM_ARCH | SYSTEM_MACOS, "arch, macos");
    expectDescription(systemSelector("redhat") | systemSelector("rhel"), "fedora");
    static const char *const managers[] = {"pacman", "apt", "dnf", "brew", "winget"};
    static const char *const families[] = {"arch", "debian", "fedora", "macos", "windows"};
    for (size_t i = 0; i < sizeof managers / sizeof managers[0]; i++) {
        testCount++;
        const char *replacement = systemSelectorReplacement(managers[i]);
        if (systemSelector(managers[i]) || !replacement ||
            strcmp(replacement, families[i]) != 0) {
            TEST_FAIL("package: wrong replacement for %s", managers[i]);
        }
    }
    testCount++;
    if (systemSelectorReplacement("fedora") || systemSelectorReplacement("bsd")) {
        TEST_FAIL("package: only removed selectors have replacements");
    }
}

void runPackageTests(void)
{
    const char *many[64];
    const char *largeArgv[PACKAGE_COMMAND_OVERHEAD + 64];
    for (size_t i = 0; i < 64; i++) {
        many[i] = "git";
    }
    const char *packages[] = {"git", "curl", "jq"};
    const char *argv[PACKAGE_COMMAND_OVERHEAD + 3];
    char message[256];
    char command[512];
    static const struct {
        PackageManager manager;
        const char *expected;
    } batches[] = {
        {PACKAGE_MANAGER_PACMAN, "sudo pacman -S --needed --noconfirm git curl jq"},
        {PACKAGE_MANAGER_DNF, "sudo dnf install -y git curl jq"},
        {PACKAGE_MANAGER_BREW, "brew install git curl jq"},
    };
    for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) {
        testCount++;
        if (!packageInstallCommand(batches[i].manager, packages, 3, false,
                                   argv, message, sizeof message) ||
            strcmp(joinCommand(argv, command, sizeof command),
                   batches[i].expected) != 0) {
            TEST_FAIL("package: unexpected batched %s command",
                      packageManagerName(batches[i].manager));
        }
    }
    testCount++;
    if (!packageInstallCommand(PACKAGE_MANAGER_APT, many, 64, true,
                               largeArgv, message, sizeof message) ||
        largeArgv[3] != many[0] || largeArgv[66] != many[63] ||
        largeArgv[67] != NULL) {
        TEST_FAIL("package: large package list was truncated");
    }
    testCount++;
    if (!packageInstallCommand(PACKAGE_MANAGER_APT, packages, 3, false,
                               argv, message, sizeof message) ||
        strcmp(joinCommand(argv, command, sizeof command),
               "sudo apt-get install -y git curl jq") != 0) {
        TEST_FAIL("package: unexpected batched apt command");
    }
    testCount++;
    if (packageInstallCommand(PACKAGE_MANAGER_WINGET, packages, 3, false,
                              argv, message, sizeof message)) {
        TEST_FAIL("package: winget must reject several IDs per command");
    }
    testCount++;
    if (packageInstallCommand(PACKAGE_MANAGER_APT, packages, 0, false,
                              argv, message, sizeof message)) {
        TEST_FAIL("package: empty package list must fail");
    }
    testNames();
    testSystems();
    testQueries();

    expectCommand(PACKAGE_MANAGER_PACMAN, false,
                  "sudo pacman -S --needed --noconfirm git");
    expectCommand(PACKAGE_MANAGER_PACMAN, true,
                  "pacman -S --needed --noconfirm git");
    expectCommand(PACKAGE_MANAGER_APT, false, "sudo apt-get install -y git");
    expectCommand(PACKAGE_MANAGER_APT, true, "apt-get install -y git");
    expectCommand(PACKAGE_MANAGER_DNF, false, "sudo dnf install -y git");
    expectCommand(PACKAGE_MANAGER_DNF, true, "dnf install -y git");
    expectCommand(PACKAGE_MANAGER_BREW, false, "brew install git");
    expectCommand(PACKAGE_MANAGER_WINGET, false,
                  "winget install --exact --silent "
                  "--accept-package-agreements --accept-source-agreements "
                  "--id git");

    expectError(PACKAGE_MANAGER_BREW, true, "Homebrew does not install");
    expectError(PACKAGE_MANAGER_NONE, false,
                "no supported package manager was found");
}
