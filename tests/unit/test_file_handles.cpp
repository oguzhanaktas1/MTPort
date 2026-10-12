#include "TestHarness.hpp"
#include "mtport/FileHandleTable.hpp"
#include "mtport/PosixErrorMapper.hpp"

using namespace mtport;

namespace {
std::shared_ptr<OpenFile> openFile(uint64_t gen) {
    auto f = std::make_shared<OpenFile>();
    f->generation = gen;
    return f;
}
}  // namespace

AF_TEST(file_handles, insert_get_remove) {
    FileHandleTable t;
    const auto a = t.insert(openFile(1)), b = t.insert(openFile(1));
    CHECK(a != b);
    CHECK(a != 0);
    CHECK(t.get(a, 1) != nullptr);
    CHECK_EQ(t.size(), size_t{2});
    CHECK(t.remove(a) != nullptr);
    CHECK(t.remove(a) == nullptr);  // double release is harmless
    CHECK_ERRNO(t.get(a, 1), EBADF);
}

AF_TEST(file_handles, stale_after_generation_change) {
    FileHandleTable t;
    const auto h = t.insert(openFile(1));
    CHECK(t.get(h, 1) != nullptr);
    CHECK_ERRNO(t.get(h, 2), ESTALE);  // reconnect bumped the generation
    CHECK(t.remove(h) != nullptr);     // but it can still be released
}

AF_TEST(file_handles, unknown_handle) {
    FileHandleTable t;
    CHECK_ERRNO(t.get(12345, 1), EBADF);
}

AF_TEST(file_handles, handles_are_never_reused) {
    FileHandleTable t;
    const auto a = t.insert(openFile(1));
    t.remove(a);
    const auto b = t.insert(openFile(1));
    CHECK(a != b);
}
