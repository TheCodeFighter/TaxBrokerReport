#include "utils/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t kBlockSize = 64;

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

using HashState = std::array<std::uint32_t, 8>;

std::uint32_t rotateRight(std::uint32_t aValue, unsigned aBits) {
    return (aValue >> aBits) | (aValue << (32U - aBits));
}

/// Mixes one 64-byte block into the hash state.
void compressBlock(HashState& aState, const unsigned char* aBlock) {
    std::array<std::uint32_t, 64> schedule{};

    for (std::size_t index = 0; index < 16; ++index)
    {
        const unsigned char* word = aBlock + index * 4;

        schedule[index] = (static_cast<std::uint32_t>(word[0]) << 24U) |
                          (static_cast<std::uint32_t>(word[1]) << 16U) |
                          (static_cast<std::uint32_t>(word[2]) << 8U) |
                          static_cast<std::uint32_t>(word[3]);
    }

    for (std::size_t index = 16; index < 64; ++index)
    {
        const std::uint32_t older = schedule[index - 15];
        const std::uint32_t newer = schedule[index - 2];
        const std::uint32_t small0 = rotateRight(older, 7) ^ rotateRight(older, 18) ^ (older >> 3U);
        const std::uint32_t small1 =
            rotateRight(newer, 17) ^ rotateRight(newer, 19) ^ (newer >> 10U);

        schedule[index] = schedule[index - 16] + small0 + schedule[index - 7] + small1;
    }

    HashState work = aState;

    for (std::size_t index = 0; index < 64; ++index)
    {
        const std::uint32_t big1 =
            rotateRight(work[4], 6) ^ rotateRight(work[4], 11) ^ rotateRight(work[4], 25);
        const std::uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
        const std::uint32_t first =
            work[7] + big1 + choose + kRoundConstants[index] + schedule[index];
        const std::uint32_t big0 =
            rotateRight(work[0], 2) ^ rotateRight(work[0], 13) ^ rotateRight(work[0], 22);
        const std::uint32_t majority =
            (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
        const std::uint32_t second = big0 + majority;

        work[7] = work[6];
        work[6] = work[5];
        work[5] = work[4];
        work[4] = work[3] + first;
        work[3] = work[2];
        work[2] = work[1];
        work[1] = work[0];
        work[0] = first + second;
    }

    for (std::size_t index = 0; index < 8; ++index)
    {
        aState[index] += work[index];
    }
}

} // namespace

namespace taxbroker {

std::string sha256Hex(std::string_view aBytes) {
    HashState state{0x6a09e667,
                    0xbb67ae85,
                    0x3c6ef372,
                    0xa54ff53a,
                    0x510e527f,
                    0x9b05688c,
                    0x1f83d9ab,
                    0x5be0cd19};
    const auto* data = reinterpret_cast<const unsigned char*>(aBytes.data());
    const std::size_t fullBlocks = aBytes.size() / kBlockSize;

    for (std::size_t block = 0; block < fullBlocks; ++block)
    {
        compressBlock(state, data + block * kBlockSize);
    }

    // The rest, one 0x80 byte, zeros, and the message length in bits (8 bytes, big-endian) fill
    // one block, or two when fewer than 9 bytes are left in the block.
    const std::size_t remaining = aBytes.size() % kBlockSize;
    const std::size_t tailSize = remaining < kBlockSize - 8 ? kBlockSize : 2 * kBlockSize;
    std::array<unsigned char, 2 * kBlockSize> tail{};

    for (std::size_t index = 0; index < remaining; ++index)
    {
        tail[index] = data[fullBlocks * kBlockSize + index];
    }

    tail[remaining] = 0x80;

    const std::uint64_t bitLength = static_cast<std::uint64_t>(aBytes.size()) * 8U;

    for (std::size_t index = 0; index < 8; ++index)
    {
        tail[tailSize - 1 - index] = static_cast<unsigned char>(bitLength >> (8U * index));
    }

    for (std::size_t offset = 0; offset < tailSize; offset += kBlockSize)
    {
        compressBlock(state, tail.data() + offset);
    }

    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string hex;

    hex.reserve(64);

    for (const std::uint32_t word : state)
    {
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            hex.push_back(kDigits[(word >> shift) & 0xFU]);
        }
    }

    return hex;
}

} // namespace taxbroker
