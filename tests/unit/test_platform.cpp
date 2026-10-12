#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Process.hpp"
#include "Platform.hpp"
#include "TestHarness.hpp"
#include "mtport/RuntimePaths.hpp"

extern char** environ;

using namespace mtport;

AF_TEST(platform, version_comparison) {
    CHECK(versionAtLeast("5.4.0", "5.4.0"));
    CHECK(versionAtLeast("5.4.1", "5.4.0"));
    CHECK(versionAtLeast("5.10.0", "5.4.0"));  // numeric, not lexical
    CHECK(versionAtLeast("6", "5.4.0"));
    CHECK(!versionAtLeast("5.3.3", "5.4.0"));
    CHECK(!versionAtLeast("5.4", "5.4.1"));
    CHECK(!versionAtLeast("", "5.4.0"));
    CHECK(!versionAtLeast("garbage", "5.4.0"));
    CHECK(versionAtLeast("15.4", "15.4"));
    CHECK(!versionAtLeast("15.3.2", "15.4"));
    CHECK(versionAtLeast("26.6.2", "15.4"));
    CHECK(versionAtLeast("5.4.0-beta1", "5.4.0"));  // suffix ignored
}

AF_TEST(platform, mount_points_only_directly_under_volumes) {
    CHECK(isUnderVolumes("/Volumes/MTPort-Pixel-8"));
    CHECK(!isUnderVolumes("/Volumes/"));
    CHECK(!isUnderVolumes("/Volumes/a/b"));
    CHECK(!isUnderVolumes("/Volumes/.."));
    CHECK(!isUnderVolumes("/Volumes/."));
    CHECK(!isUnderVolumes("/tmp/MTPort"));
    CHECK(!isUnderVolumes("/Users/me/mnt"));
    CHECK(!isUnderVolumes("Volumes/x"));
    CHECK_EQ(defaultMountPoint("Galaxy S21 FE"), std::string("/Volumes/MTPort-Galaxy-S21-FE"));
    CHECK(isUnderVolumes(defaultMountPoint("../../etc")));
    CHECK(isUnderVolumes(defaultMountPoint("")));
}

AF_TEST(platform, a_zombie_worker_is_not_alive) {
    // An exited-but-unreaped worker still answers kill(pid, 0); it must not be treated as a running mount.
    pid_t pid = 0;
    char* argv[] = {const_cast<char*>("/usr/bin/true"), nullptr};
    CHECK_EQ(::posix_spawn(&pid, "/usr/bin/true", nullptr, nullptr, argv, environ), 0);
    CHECK(processAlive(::getpid()));
    for (int i = 0; i < 100 && processAlive(pid); ++i) ::usleep(20 * 1000);
    CHECK(!processAlive(pid));  // exited; we have not called waitpid yet, so it is a zombie
    int status = 0;
    CHECK_EQ(::waitpid(pid, &status, 0), pid);
    CHECK(!processAlive(pid));
    CHECK(!processAlive(0));
    CHECK(!processAlive(-5));
}

AF_TEST(platform, safe_name_is_always_a_single_safe_component) {
    for (const char* in : {"", "..", "../x", "a/b", "  ", "Pixel 8 Pro",
                           "T\xc3\xbcrk\xc3\xa7"
                           "e",
                           "...hidden", "x\ny", "a--b"}) {
        const std::string n = safeName(in);
        CHECK(!n.empty());
        CHECK(n != "." && n != "..");
        CHECK(n.front() != '.');
        CHECK(n.find('/') == std::string::npos);
        for (unsigned char c : n) CHECK(std::isalnum(c) || c == '.' || c == '_' || c == '-');
    }
    CHECK_EQ(safeName("Pixel 8 Pro"), std::string("Pixel-8-Pro"));
}
