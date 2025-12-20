#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>

// Messages.h - generated from service/python/data.py
// Target: ESP32 (uses mbedTLS for HMAC-SHA256)

namespace Messages {

class Payload {
public:
    virtual ~Payload() = default;

    // Serialize full message including trailing 16-byte HMAC.
    // If key == nullptr, append 16 zero bytes instead of HMAC.
    std::vector<uint8_t> serialize(const std::string* key = nullptr);

    // Compute and store HMAC using provided key, return 16-byte signature.
    std::array<uint8_t, 16> sign(const std::string& key);

    // Verify stored HMAC against provided key.
    bool verify(const std::string& key) const;

protected:
    // Subclasses implement this to return the bytes that should be signed
    // (all fields except the trailing HMAC).
    virtual std::vector<uint8_t> _serialize_fields() const = 0;

    mutable std::array<uint8_t, 16> hmac_{};
};


class Position : public Payload {
public:
    static constexpr double SCALE = 1e7;

    // fields in order: header, interval, device(6), latitude, longitude, timestamp, namelen, name, hmac
    uint8_t header = 0;
    uint8_t interval = 0;
    std::array<uint8_t, 6> device{{0}};
    double latitude = 0.0;   // stored as int32 = round(latitude * SCALE)
    double longitude = 0.0;  // stored as int32
    uint32_t timestamp = 0;  // 4 bytes unsigned
    std::string name;

    // HMAC is in Payload.hmac_

    Position() = default;

    // Parse from raw bytes (throws std::runtime_error on error)
    static Position fromBytes(const std::vector<uint8_t>& data);

    // Serialize full message (fields + 16-byte HMAC) — delegates to Payload::serialize
    std::vector<uint8_t> serialize(const std::string* key = nullptr);

protected:
    std::vector<uint8_t> _serialize_fields() const override;

public:
    std::string toString() const;
};


class Command : public Payload {
public:
    uint8_t header = 0;
    std::string arg;

    Command() = default;

    static Command fromBytes(const std::vector<uint8_t>& data);
    std::vector<uint8_t> serialize(const std::string* key = nullptr);

protected:
    std::vector<uint8_t> _serialize_fields() const override;

public:
    std::string toString() const;
};

} // namespace Messages
