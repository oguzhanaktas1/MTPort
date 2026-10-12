#include "TestHarness.hpp"
#include "mtport/MetadataCache.hpp"

using namespace mtport;
using namespace std::chrono_literals;

namespace {
std::shared_ptr<const DirListing> listingOf(size_t n) {
    std::vector<FileNode> nodes;
    for (size_t i = 0; i < n; ++i) {
        FileNode f;
        f.objectId = static_cast<ObjectId>(i + 1);
        f.name = "f" + std::to_string(i);
        nodes.push_back(f);
    }
    return std::make_shared<const DirListing>(buildDirListing(std::move(nodes)));
}
}  // namespace

AF_TEST(cache, ttl_expiry_with_injected_clock) {
    auto now = std::chrono::steady_clock::now();
    MetadataCache c({}, [&] { return now; });
    c.putDir(1, 1, 0, listingOf(3));
    CHECK(c.getDir(1, 1, 0) != nullptr);
    now += 1999ms;
    CHECK(c.getDir(1, 1, 0) != nullptr);
    now += 2ms;  // 2001 ms: default TTL is 2 s
    CHECK(c.getDir(1, 1, 0) == nullptr);
    CHECK_EQ(c.directoryCount(), size_t{0});
}

AF_TEST(cache, keys_include_generation_storage_and_parent) {
    MetadataCache c;
    c.putDir(1, 1, 0, listingOf(1));
    CHECK(c.getDir(2, 1, 0) == nullptr);  // other generation
    CHECK(c.getDir(1, 2, 0) == nullptr);  // other storage
    CHECK(c.getDir(1, 1, 5) == nullptr);  // other parent
    CHECK(c.getDir(1, 1, 0) != nullptr);
}

AF_TEST(cache, invalidate_and_clear) {
    MetadataCache c;
    c.putDir(1, 1, 0, listingOf(2));
    c.putDir(1, 1, 7, listingOf(2));
    c.invalidateDir(1, 1, 0);
    CHECK(c.getDir(1, 1, 0) == nullptr);
    CHECK(c.getDir(1, 1, 7) != nullptr);
    c.clear();
    CHECK_EQ(c.directoryCount(), size_t{0});
    CHECK_EQ(c.nodeCount(), size_t{0});
}

AF_TEST(cache, bounded_by_directory_count_lru) {
    MetadataCache::Config cfg;
    cfg.maxDirectories = 3;
    MetadataCache c(cfg);
    for (ObjectId p = 1; p <= 3; ++p) c.putDir(1, 1, p, listingOf(1));
    CHECK(c.getDir(1, 1, 1) != nullptr);  // touch 1 => 2 is now least recently used
    c.putDir(1, 1, 4, listingOf(1));
    CHECK_EQ(c.directoryCount(), size_t{3});
    CHECK(c.getDir(1, 1, 2) == nullptr);
    CHECK(c.getDir(1, 1, 1) != nullptr);
    CHECK(c.getDir(1, 1, 4) != nullptr);
}

AF_TEST(cache, bounded_by_total_nodes) {
    MetadataCache::Config cfg;
    cfg.maxTotalNodes = 100;
    MetadataCache c(cfg);
    c.putDir(1, 1, 1, listingOf(60));
    c.putDir(1, 1, 2, listingOf(60));  // 120 > 100 => oldest evicted
    CHECK(c.getDir(1, 1, 1) == nullptr);
    CHECK(c.getDir(1, 1, 2) != nullptr);
    CHECK(c.nodeCount() <= 100);
    c.putDir(1, 1, 3, listingOf(500));  // larger than the whole budget: not cached at all
    CHECK(c.getDir(1, 1, 3) == nullptr);
}
