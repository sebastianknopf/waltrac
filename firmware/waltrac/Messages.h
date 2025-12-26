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
    // Accept a raw C string to avoid constructing a std::string on Arduino.
    std::vector<uint8_t> serialize(const char* key = nullptr);

    // Verify stored HMAC against provided key. Accept raw C string for Arduino.
    bool verify(const char* key) const;

protected:
    // Subclasses implement this to return the bytes that should be signed
    // (all fields except the trailing HMAC).
    virtual std::vector<uint8_t> _serialize_fields() const = 0;

    mutable std::array<uint8_t, 16> hmac_{};
};


class Position : public Payload {
public:
    static constexpr double SCALE = 1e7;

    // fields in order: header, interval, confidence, satellites, device(6), latitude, longitude, namelen, name, hmac
    uint8_t header = 0;
    uint8_t interval = 0;
    uint8_t confidence = 0;
    uint8_t satellites = 0;
    uint8_t device[6] = {0};
    double latitude = 0.0;
    double longitude = 0.0;
    std::string name;

    // HMAC is in Payload.hmac_

    Position() = default;

    // Parse from raw bytes (throws std::runtime_error on error)
    static Position init(const std::vector<uint8_t>& data);

    // Serialize full message (fields + 16-byte HMAC) — delegates to Payload::serialize
    std::vector<uint8_t> serialize(const char* key = nullptr);

    // Set the header byte by its parameters
    void setHeader(bool isValid);

    // Get the header params
    void getHeader(bool &isValid);

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

    // Parse from raw bytes (throws std::runtime_error on error)
    static Command init(const std::vector<uint8_t>& data);

    // Serialize full message (fields + 16-byte HMAC) — delegates to Payload::serialize
    std::vector<uint8_t> serialize(const char* key = nullptr);

    // Set the header byte by its parameters
    void setHeader();

    // Get the header params
    void getHeader();

protected:
    std::vector<uint8_t> _serialize_fields() const override;

public:
    std::string toString() const;
};

} // namespace Messages
