#include "utils/sha256.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace taxbroker;

// Expected values: the FIPS 180-4 / NIST example hashes, and `sha256sum` for the lengths around
// the 64-byte block edge.

TEST(Sha256Test, HashesTheEmptyMessage) {
    EXPECT_EQ(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, HashesAbc) {
    EXPECT_EQ(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, HashesTheFiftySixByteNistMessage) {
    // 448 bits: the length field no longer fits in the first block.
    EXPECT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, HashesOneMillionLowercaseA) {
    EXPECT_EQ(sha256Hex(std::string(1'000'000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, HashesASentence) {
    EXPECT_EQ(sha256Hex("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256Test, HashesLengthsAroundTheBlockEdges) {
    // 55 bytes is the longest message whose padding fits in one block; 56 needs a second block.
    EXPECT_EQ(sha256Hex(std::string(55, 'a')),
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(sha256Hex(std::string(56, 'a')),
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(sha256Hex(std::string(63, 'a')),
              "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    EXPECT_EQ(sha256Hex(std::string(64, 'a')),
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    EXPECT_EQ(sha256Hex(std::string(119, 'a')),
              "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    EXPECT_EQ(sha256Hex(std::string(120, 'a')),
              "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

TEST(Sha256Test, HashesBytesThatAreNotText) {
    const std::string bytes{"\x00\xff\x80", 3};

    EXPECT_EQ(sha256Hex(bytes), "f742b965f156c10374bc23aea96e3a8aff8facd6fc079defeaa30219ad86f211");
}

} // namespace
