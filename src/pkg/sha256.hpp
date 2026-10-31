#pragma once

#include <cstdint>
#include <string>

namespace coyote::pkg {

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string hexDigest();  // finishes the hash; the object must not be reused afterwards

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256Hex(const std::string& data);

} // namespace coyote::pkg
