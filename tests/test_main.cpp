#include <cstring>

#include "TestHarness.hpp"

// usage: mtport-tests [--list] [suite ...]
int main(int argc, char** argv) {
    using namespace aftest;
    std::cout << std::unitbuf;  // a crash must not hide which test was running
    std::vector<std::string> suites;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list"))
            list = true;
        else
            suites.emplace_back(argv[i]);
    }
    if (list) {
        std::string last;
        for (auto& c : registry())
            if (c.suite != last) {
                std::cout << c.suite << "\n";
                last = c.suite;
            }
        return 0;
    }
    int ran = 0, failed = 0;
    for (auto& c : registry()) {
        if (!suites.empty() && std::find(suites.begin(), suites.end(), c.suite) == suites.end()) continue;
        ++ran;
        try {
            c.fn();
            std::cout << "[  OK  ] " << c.suite << "." << c.name << "\n";
        } catch (const Failure& f) {
            ++failed;
            std::cout << "[ FAIL ] " << c.suite << "." << c.name << "\n         " << f.msg << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[ FAIL ] " << c.suite << "." << c.name << "\n         unexpected exception: " << e.what()
                      << "\n";
        }
    }
    std::cout << ran - failed << "/" << ran << " tests passed\n";
    if (ran == 0) {
        std::cout << "no tests selected\n";
        return 1;
    }
    return failed ? 1 : 0;
}
