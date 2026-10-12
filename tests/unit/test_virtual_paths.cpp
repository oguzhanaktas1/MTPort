#include "TestHarness.hpp"
#include "mtport/NameRules.hpp"
#include "mtport/PosixErrorMapper.hpp"
#include "mtport/VirtualPathMapper.hpp"

using namespace mtport;

namespace {
FileNode node(ObjectId id, std::string name, bool dir = false) {
    FileNode n;
    n.storageId = 1;
    n.objectId = id;
    n.name = std::move(name);
    n.isDirectory = dir;
    return n;
}
}  // namespace

AF_TEST(virtual_paths, split_basic) {
    CHECK(splitVirtualPath("/").empty());
    auto p = splitVirtualPath("/Internal Storage//DCIM/Camera/");
    CHECK_EQ(p.size(), size_t{3});
    CHECK_EQ(p[0], std::string("Internal Storage"));
    CHECK_EQ(p[2], std::string("Camera"));
}

AF_TEST(virtual_paths, rejects_traversal_and_garbage) {
    CHECK_ERRNO(splitVirtualPath("relative/path"), EINVAL);
    CHECK_ERRNO(splitVirtualPath(""), EINVAL);
    CHECK_ERRNO(splitVirtualPath("/a/../b"), EINVAL);
    CHECK_ERRNO(splitVirtualPath("/a/./b"), EINVAL);
    CHECK_ERRNO(splitVirtualPath(std::string("/a\0b", 4)), EINVAL);
    CHECK_ERRNO(splitVirtualPath("/" + std::string(300, 'x')), ENAMETOOLONG);
}

AF_TEST(virtual_paths, object_name_validation) {
    CHECK(isValidObjectName("photo.jpg"));
    CHECK(
        isValidObjectName("T\xc3\xbcrk\xc3\xa7"
                          "e \xc5\x9f\xc4\xb1k.txt"));  // Türkçe şık.txt
    CHECK(isValidObjectName("\xf0\x9f\x98\x80.png"));   // emoji
    CHECK(!isValidObjectName(""));
    CHECK(!isValidObjectName("."));
    CHECK(!isValidObjectName(".."));
    CHECK(!isValidObjectName("a/b"));
    CHECK(!isValidObjectName(std::string("a\0b", 3)));
    CHECK(!isValidObjectName("\xff\xfe"));  // invalid UTF-8
    CHECK(!isValidObjectName("\xc0\xaf"));  // overlong '/'
    CHECK(!isValidObjectName(std::string(256, 'a')));
    CHECK(isValidObjectName(std::string(255, 'a')));
}

AF_TEST(virtual_paths, nfc_nfd_lookup) {
    // "é" stored precomposed on the phone, looked up decomposed by macOS tools (and vice versa).
    auto listing = buildDirListing({node(1, "caf\xc3\xa9.txt")});
    CHECK(listing.find("caf\xc3\xa9.txt") != nullptr);
    CHECK(listing.find("cafe\xcc\x81.txt") != nullptr);
    CHECK(listing.find("cafe.txt") == nullptr);
    // Turkish dotless/dotted i are NOT equivalent to ASCII i.
    auto tr = buildDirListing({node(2,
                                    "\xc4\xb1"
                                    "k")});
    CHECK(tr.find("ik") == nullptr);
}

AF_TEST(virtual_paths, duplicate_names_get_stable_suffixes) {
    auto l = buildDirListing({node(7, "a.jpg"), node(3, "a.jpg"), node(5, "a.jpg"), node(9, "b")});
    CHECK_EQ(l.entries.size(), size_t{4});
    // The lowest object id keeps the original name; later ones are numbered in id order.
    CHECK_EQ(l.find("a.jpg")->node.objectId, ObjectId{3});
    CHECK_EQ(l.find("a (2).jpg")->node.objectId, ObjectId{5});
    CHECK_EQ(l.find("a (3).jpg")->node.objectId, ObjectId{7});
    // Same input in a different order yields the same mapping.
    auto l2 = buildDirListing({node(5, "a.jpg"), node(9, "b"), node(7, "a.jpg"), node(3, "a.jpg")});
    CHECK_EQ(l2.find("a (2).jpg")->node.objectId, ObjectId{5});
}

AF_TEST(virtual_paths, suffix_does_not_collide_with_real_names) {
    auto l = buildDirListing({node(1, "x.txt"), node(2, "x (2).txt"), node(3, "x.txt")});
    CHECK_EQ(l.entries.size(), size_t{3});
    CHECK_EQ(l.find("x.txt")->node.objectId, ObjectId{1});
    CHECK_EQ(l.find("x (2).txt")->node.objectId, ObjectId{2});  // the genuine file keeps its name
    CHECK_EQ(l.find("x (3).txt")->node.objectId, ObjectId{3});
}

AF_TEST(virtual_paths, case_colliding_names_are_distinct) {
    auto l = buildDirListing({node(1, "Photo.JPG"), node(2, "photo.jpg")});
    CHECK(l.find("Photo.JPG") != nullptr);
    CHECK(l.find("photo.jpg") != nullptr);
    CHECK_EQ(l.find("Photo.JPG")->node.objectId, ObjectId{1});
    CHECK_EQ(l.find("photo.jpg")->node.objectId, ObjectId{2});
}

AF_TEST(virtual_paths, hostile_names_from_the_phone_are_made_safe) {
    auto l = buildDirListing({node(1, "a/b"), node(2, ".."), node(3, "."), node(4, ""),
                              node(5,
                                   "bad\xff"
                                   "name")});
    for (const auto& e : l.entries) {
        CHECK(isValidObjectName(e.displayName));
        CHECK(e.displayName.find('/') == std::string::npos);
    }
    CHECK(l.find("a\xe2\x88\x95"
                 "b") != nullptr);  // '/' -> U+2215
    CHECK(l.find("(..)") != nullptr);
    CHECK(l.find("bad%FFname") != nullptr);                  // invalid byte escaped, original untouched
    CHECK_EQ(l.find("(..)")->node.name, std::string(".."));  // original name preserved for device calls
}

AF_TEST(virtual_paths, storage_view_unique_and_deterministic) {
    StorageDescriptor a{2, "Internal Storage", 100, 50, false}, b{1, "Internal Storage", 100, 50, false},
        c{3, "", 10, 5, false};
    auto v = buildStorageView({a, b, c});
    CHECK_EQ(v.size(), size_t{3});
    CHECK_EQ(v[0].storage.id, StorageId{1});
    CHECK_EQ(v[0].displayName, std::string("Internal Storage"));
    CHECK_EQ(v[1].displayName, std::string("Internal Storage (2)"));
    CHECK_EQ(v[2].displayName, std::string("Storage 3"));
}

AF_TEST(virtual_paths, mac_metadata_names) {
    CHECK(isMacMetadataName(".DS_Store"));
    CHECK(isMacMetadataName("._photo.jpg"));
    CHECK(!isMacMetadataName("._"));
    CHECK(!isMacMetadataName("photo.jpg"));
    CHECK(isMacMetadataDirName(".Spotlight-V100"));
    CHECK(!isMacMetadataDirName("DCIM"));
}

AF_TEST(virtual_paths, inode_table_is_stable_within_generation) {
    InodeTable t;
    const auto a = t.forObject(1, 10), b = t.forObject(1, 11), s = t.forStorage(1);
    CHECK(a != b && a != s && b != s);
    CHECK(a != InodeTable::kRoot);
    CHECK_EQ(t.forObject(1, 10), a);
    CHECK_EQ(t.forStorage(1), s);
    t.reset();
    CHECK(t.forObject(1, 11) == a);  // restarted numbering: nothing carries over
}
