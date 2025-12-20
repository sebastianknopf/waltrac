#include "Messages.h"

#include <cstring>
#include <cmath>
#include <mbedtls/md.h>

namespace Messages {

// --- helpers -----------------------------------------------------------------

static void push_u8(std::vector<uint8_t>& out, uint8_t v) {
    out.push_back(v);
}

static void push_be_i32(std::vector<uint8_t>& out, int32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v) & 0xFF));
}

static void push_be_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v) & 0xFF));
}

static int32_t read_be_i32(const std::vector<uint8_t>& src, size_t offset) {
    int32_t v = (static_cast<int32_t>(src[offset]) << 24) |
                (static_cast<int32_t>(src[offset+1]) << 16) |
                (static_cast<int32_t>(src[offset+2]) << 8) |
                (static_cast<int32_t>(src[offset+3]));
    return v;
}

static uint32_t read_be_u32(const std::vector<uint8_t>& src, size_t offset) {
    uint32_t v = (static_cast<uint32_t>(src[offset]) << 24) |
                 (static_cast<uint32_t>(src[offset+1]) << 16) |
                 (static_cast<uint32_t>(src[offset+2]) << 8) |
                 (static_cast<uint32_t>(src[offset+3]));
    return v;
}

static void compute_hmac_sha256_trunc(const uint8_t* key, size_t keylen,
                                      const uint8_t* data, size_t datalen,
                                      uint8_t out16[16]) {
    unsigned char full[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr) {
        throw std::runtime_error("mbedtls md info not available");
    }
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md_info, 1) != 0) {
        mbedtls_md_free(&ctx);
        throw std::runtime_error("mbedtls_md_setup failed");
    }
    if (mbedtls_md_hmac_starts(&ctx, key, static_cast<int>(keylen)) != 0 ||
        mbedtls_md_hmac_update(&ctx, data, datalen) != 0 ||
        mbedtls_md_hmac_finish(&ctx, full) != 0) {
        mbedtls_md_free(&ctx);
        throw std::runtime_error("mbedtls hmac failed");
    }
    mbedtls_md_free(&ctx);
    memcpy(out16, full, 16);
}

// --- Payload ---------------------------------------------------------------

std::vector<uint8_t> Payload::serialize(const std::string* key) {
    std::vector<uint8_t> fields = this->_serialize_fields();
    if (key == nullptr) {
        // append 16 zero bytes
        fields.insert(fields.end(), 16, 0);
        return fields;
    }

    uint8_t sig[16];
    compute_hmac_sha256_trunc(reinterpret_cast<const uint8_t*>(key->data()), key->size(),
                              fields.data(), fields.size(), sig);
    std::copy(sig, sig + 16, this->hmac_.begin());
    fields.insert(fields.end(), sig, sig + 16);
    return fields;
}

std::array<uint8_t, 16> Payload::sign(const std::string& key) {
    std::vector<uint8_t> fields = this->_serialize_fields();
    uint8_t sig[16];
    compute_hmac_sha256_trunc(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                              fields.data(), fields.size(), sig);
    std::array<uint8_t, 16> out;
    std::copy(sig, sig + 16, out.begin());
    this->hmac_ = out;
    return out;
}

bool Payload::verify(const std::string& key) const {
    // hmac_ must be set
    uint8_t expected[16];
    std::vector<uint8_t> fields = this->_serialize_fields();
    compute_hmac_sha256_trunc(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                              fields.data(), fields.size(), expected);
    return std::equal(expected, expected + 16, this->hmac_.begin());
}

// --- Position --------------------------------------------------------------

Position Position::fromBytes(const std::vector<uint8_t>& data) {
    // min_fixed = 1(header) +1(interval)+6(device)+4(lat)+4(lon)+4(ts)+1(namelen)+16(hmac)
    const size_t min_fixed = 1 + 1 + 6 + 4 + 4 + 4 + 1 + 16;
    if (data.size() < min_fixed) {
        throw std::runtime_error("data too short for Position");
    }
    size_t offset = 0;
    Position p;

    p.header = data[offset++];
    p.interval = data[offset++];

    for (size_t i = 0; i < 6; ++i) p.device[i] = data[offset++];

    int32_t lat_int = read_be_i32(data, offset);
    offset += 4;
    p.latitude = static_cast<double>(lat_int) / Position::SCALE;

    int32_t lon_int = read_be_i32(data, offset);
    offset += 4;
    p.longitude = static_cast<double>(lon_int) / Position::SCALE;

    p.timestamp = read_be_u32(data, offset);
    offset += 4;

    uint8_t namelen = data[offset++];

    if (data.size() < offset + namelen + 16) {
        throw std::runtime_error("data too short for name length and hmac");
    }

    p.name.assign(reinterpret_cast<const char*>(&data[offset]), namelen);
    offset += namelen;

    // read hmac
    for (size_t i = 0; i < 16; ++i) p.hmac_[i] = data[offset++];

    if (offset != data.size()) {
        throw std::runtime_error("extra or missing bytes after parsing hmac");
    }

    return p;
}

std::vector<uint8_t> Position::_serialize_fields() const {
    std::vector<uint8_t> parts;
    // header
    push_u8(parts, header);
    // interval
    push_u8(parts, interval);
    // device (6 bytes)
    parts.insert(parts.end(), device.begin(), device.end());
    // latitude
    int32_t lat_int = static_cast<int32_t>(round(latitude * SCALE));
    push_be_i32(parts, lat_int);
    // longitude
    int32_t lon_int = static_cast<int32_t>(round(longitude * SCALE));
    push_be_i32(parts, lon_int);
    // timestamp
    push_be_u32(parts, timestamp);
    // name
    if (name.size() > 255) throw std::runtime_error("name too long; max 255 bytes");
    push_u8(parts, static_cast<uint8_t>(name.size()));
    parts.insert(parts.end(), name.begin(), name.end());
    return parts;
}

std::vector<uint8_t> Position::serialize(const std::string* key) {
    return Payload::serialize(key);
}

std::string Position::toString() const {
    char buf[200];
    snprintf(buf, sizeof(buf), "Position(header=%u, interval=%u, device=[%02x%02x%02x%02x%02x%02x], lat=%.7f, lon=%.7f, ts=%u, name=%s)",
             header, interval,
             device[0], device[1], device[2], device[3], device[4], device[5],
             latitude, longitude, timestamp, name.c_str());
    return std::string(buf);
}

// --- Command ---------------------------------------------------------------

Command Command::fromBytes(const std::vector<uint8_t>& data) {
    const size_t min_fixed = 1 + 1 + 16; // header + arglen + hmac
    if (data.size() < min_fixed) throw std::runtime_error("data too short for Command");
    size_t offset = 0;
    Command c;
    c.header = data[offset++];
    uint8_t arglen = data[offset++];
    if (data.size() < offset + arglen + 16) throw std::runtime_error("data too short for arg length and hmac");
    c.arg.assign(reinterpret_cast<const char*>(&data[offset]), arglen);
    offset += arglen;
    
    for (size_t i = 0; i < 16; ++i) {
        c.hmac_[i] = data[offset++];
    }

    if (offset != data.size()) {
        throw std::runtime_error("extra or missing bytes after parsing hmac");
    }

    return c;
}

std::vector<uint8_t> Command::_serialize_fields() const {
    std::vector<uint8_t> parts;
    push_u8(parts, header);
    if (arg.size() > 255) {
        throw std::runtime_error("arg too long; max 255 bytes");
    }

    push_u8(parts, static_cast<uint8_t>(arg.size()));
    parts.insert(parts.end(), arg.begin(), arg.end());
    
    return parts;
}

std::vector<uint8_t> Command::serialize(const std::string* key) {
    return Payload::serialize(key);
}

std::string Command::toString() const {
    char buf[200];
    snprintf(buf, sizeof(buf), "Command(header=%u, arglen=%zu, arg=%s)", header, arg.size(), arg.c_str());
    
    return std::string(buf);
}

} // namespace Messages
