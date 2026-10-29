#include "takedown/content/sha256.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace takedown::content {

void Sha256::block() {
    const std::array<std::uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};

    // Message schedule: 16 big-endian words from the block, extended to 64.
    std::array<std::uint32_t, 64> w{};
    for (unsigned i = 0; i < 16; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            w[i] = (w[i] << 8) | buffer_[i * 4 + j];
        }
    }
    for (unsigned i = 16; i < 64; ++i) {
        const auto x = w[i - 15];
        const auto y = w[i - 2];
        w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
               (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (unsigned i = 0; i < 64; ++i) {
        const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                        ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                        ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
    if (finished_) {
        throw std::logic_error("SHA-256 already finalized");
    }
    size_ += bytes.size();
    while (!bytes.empty()) {
        const auto count = std::min(buffer_.size() - used_, bytes.size());
        std::copy_n(bytes.begin(), count, buffer_.begin() + used_);
        used_ += count;
        bytes = bytes.subspan(count);
        if (used_ == buffer_.size()) {
            block();
            used_ = 0;
        }
    }
}

void Sha256::update(std::string_view text) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    update(std::span<const std::uint8_t>(data, text.size()));
}

std::string Sha256::finish() {
    const auto bits = size_ * 8U;
    const std::uint8_t marker = 0x80U;
    const std::uint8_t zero = 0;
    update(std::span<const std::uint8_t>(&marker, 1));
    while (used_ != 56) {
        update(std::span<const std::uint8_t>(&zero, 1));
    }
    std::array<std::uint8_t, 8> length{};
    for (unsigned i = 0; i < 8; ++i) {
        length[7 - i] = static_cast<std::uint8_t>(bits >> (i * 8U));
    }
    update(length);
    finished_ = true;

    const char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto word : state_) {
        for (int i = 7; i >= 0; --i) {
            result.push_back(digits[(word >> (i * 4)) & 15U]);
        }
    }
    return result;
}

} // namespace takedown::content
