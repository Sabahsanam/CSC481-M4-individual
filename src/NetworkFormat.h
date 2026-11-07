#pragma once

// Milestone 3 Section C.1: Network formats (TEXT or BINARY) for performance comparison
//
// Both formats use the same gameplay and networking logic, only the data representation changes.
//
// Choose a format when starting a client:
//   ./main 1            -> TEXT   (default, same behavior as before)
//   ./main 1 text       -> TEXT
//   ./main 1 binary     -> BINARY

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

enum class NetworkFormat {
    TEXT,
    BINARY
};

// Milestone 3 Sections C.2-C.4: Position of a moving object used in benchmarks.
struct ObjectState {
    float x;
    float y;
};
static_assert(sizeof(ObjectState) == 8, "ObjectState must be 8 bytes (no padding)");

namespace NetFormat {

// Magic byte to identify binary player packets
constexpr uint8_t BINARY_MAGIC = 0xB1;

// Raw layout of a binary player packet (exactly 10 bytes)
#pragma pack(push, 1)
struct BinaryPlayerPacket {
    uint8_t magic; // always BINARY_MAGIC
    uint8_t clientID; // Client ID 1..4
    float   x;
    float   y;
};
#pragma pack(pop)

static_assert(sizeof(BinaryPlayerPacket) == 10, "BinaryPlayerPacket must be 10 bytes");

// Parse a format name from the command line. Unknown names fall back to TEXT.
inline NetworkFormat parseFormat(const std::string& name) {
    return (name == "binary") ? NetworkFormat::BINARY : NetworkFormat::TEXT;
}

// Returns format name for logs
inline const char* formatName(NetworkFormat format) {
    return (format == NetworkFormat::BINARY) ? "BINARY" : "TEXT";
}

// Turn (clientID, x, y) into the bytes that are sent over the network
inline std::string encode(NetworkFormat format, int clientID, float x, float y) {
    if (format == NetworkFormat::BINARY) {
        BinaryPlayerPacket packet;
        packet.magic = BINARY_MAGIC;
        packet.clientID = static_cast<uint8_t>(clientID);
        packet.x = x;
        packet.y = y;
        // std::string can hold arbitrary bytes, so it works as a byte buffer
        return std::string(reinterpret_cast<const char*>(&packet), sizeof(packet));
    }

    // TEXT: exactly the same string the original code produced.
    std::ostringstream stream;
    stream << clientID << " " << x << " " << y;
    return stream.str();
}

// Decodes received bytes back into client ID and position
inline bool decode(const void* data, size_t size, int& clientID, float& x, float& y) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    // BINARY: Right size and starts with the magic byte.
    if (size == sizeof(BinaryPlayerPacket) && bytes[0] == BINARY_MAGIC) {
        BinaryPlayerPacket packet;
        std::memcpy(&packet, bytes, sizeof(packet));
        clientID = packet.clientID;
        x = packet.x;
        y = packet.y;
        return true;
    }

    // TEXT: Parse "id x y" (same parsing the original code used).
    std::stringstream stream(std::string(static_cast<const char*>(data), size));
    return static_cast<bool>(stream >> clientID >> x >> y);
}

// Milestone 3 Sections C.2-C.4: Object snapshots for the performance experiments.
//   TEXT   : "S <clientID> <count> x0 y0 x1 y1 ..."   (starts with the letter S)
//   BINARY : 4-byte header (magic, clientID, uint16 count) + count * 8 raw bytes
constexpr uint8_t BINARY_OBJECTS_MAGIC = 0xB2; // marks a binary object snapshot
constexpr char TEXT_OBJECTS_PREFIX  = 'S'; // marks a text object snapshot

// True if a received message is an object snapshot (not a single player position)
inline bool isObjectMessage(const void* data, size_t size) {
    if (size == 0) { return false; }
    const uint8_t first = static_cast<const uint8_t*>(data)[0];
    return first == BINARY_OBJECTS_MAGIC || first == static_cast<uint8_t>(TEXT_OBJECTS_PREFIX);
}

// Turn a list of objects into the bytes that are sent over the network
inline std::string encodeObjects(NetworkFormat format, int clientID, const std::vector<ObjectState>& objects) {
    if (format == NetworkFormat::BINARY) {
        const uint16_t count = static_cast<uint16_t>(objects.size());
        std::string message(4 + objects.size() * sizeof(ObjectState), '\0');
        message[0] = static_cast<char>(BINARY_OBJECTS_MAGIC);
        message[1] = static_cast<char>(clientID);
        std::memcpy(&message[2], &count, sizeof(count));
        // All objects are copied in one block, no per-object work
        if (!objects.empty()) {
            std::memcpy(&message[4], objects.data(), objects.size() * sizeof(ObjectState));
        }
        return message;
    }

    // TEXT: Every number is formatted into characters one by one.
    std::ostringstream stream;
    stream << TEXT_OBJECTS_PREFIX << " " << clientID << " " << objects.size();
    for (const ObjectState& object : objects) {
        stream << " " << object.x << " " << object.y;
    }
    return stream.str();
}

// Decodes network bytes into a list of objects
inline bool decodeObjects(const void* data, size_t size, int& clientID, std::vector<ObjectState>& objects) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    if (size >= 4 && bytes[0] == BINARY_OBJECTS_MAGIC) {
        uint16_t count;
        std::memcpy(&count, bytes + 2, sizeof(count));
        if (size != 4 + static_cast<size_t>(count) * sizeof(ObjectState)) { return false; }
        clientID = bytes[1];
        objects.resize(count);
        if (count > 0) {
            std::memcpy(objects.data(), bytes + 4, count * sizeof(ObjectState));
        }
        return true;
    }

    // TEXT: Parse "S id count x y x y ..."
    std::istringstream stream(std::string(static_cast<const char*>(data), size));
    char prefix = 0;
    size_t count = 0;
    if (!(stream >> prefix >> clientID >> count) || prefix != TEXT_OBJECTS_PREFIX) { return false; }
    objects.resize(count);
    for (size_t i = 0; i < count; i++) {
        if (!(stream >> objects[i].x >> objects[i].y)) { return false; }
    }
    return true;
}

} // namespace NetFormat