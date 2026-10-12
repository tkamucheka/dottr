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
    const char *argv[PACKAGE_COMMAND_MAX];
    char message[256];
    char command[512];
    if (!packageInstallCommand(manager, "git", root, argv, message,
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
    const char *argv[PACKAGE_COMMAND_MAX];
    char message[256] = "";
    if (packageInstallCommand(manager, "git", root, argv, message,
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
    const char *argv[PACKAGE_COMMAND_MAX];
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
        systemSelector("macos") != SYSTEM_BREW ||
        systemSelector("windows") != SYSTEM_WINGET ||
        systemSelector("apt") != SYSTEM_APT || systemSelector("bsd") != 0 ||
        systemSelector("Linux") != 0) {
        TEST_FAIL("package: wrong systems of a selector");
    }

    expectDescription(SYSTEM_ALL, "linux, macos, windows");
    expectDescription(SYSTEM_LINUX | SYSTEM_WINGET, "linux, windows");
    expectDescription(SYSTEM_APT | SYSTEM_DNF, "apt, dnf");
    expectDescription(SYSTEM_PACMAN | SYSTEM_BREW, "macos, pacman");
}

void runPackageTests(void)
{
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
