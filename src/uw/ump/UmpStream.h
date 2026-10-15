#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "uw/ump/MidiMessage.h"

// UMP Stream messages (MT 0xF, UMP 1.1): how a UMP endpoint says what it is
// (name, Product Instance ID, device identity, protocols) and which function
// blocks it has, in answer to a peer's Endpoint Discovery and Function Block
// Discovery. Used by the MIDI outputs that publish an endpoint
// (MidiOutput::openEndpoint), on every platform, as no system answers for
// an app's endpoint in-band. (UmpStream.cpp; tests in tests/UmpTests.cpp)
namespace uw::ump {

// Stream message status (word 0, bits 25-16).
enum class StreamStatus : std::uint16_t {
    EndpointDiscovery        = 0x000,
    EndpointInfo             = 0x001,
    DeviceIdentity           = 0x002,
    EndpointName             = 0x003,
    ProductInstanceId        = 0x004,
    StreamConfigRequest      = 0x005,
    StreamConfigNotification = 0x006,
    FunctionBlockDiscovery   = 0x010,
    FunctionBlockInfo        = 0x011,
    FunctionBlockName        = 0x012,
};

// One function block: a run of groups with one purpose ("Keyboard": groups
// 1-5, sending). Groups here are 0-based (0 is the UI's group 1).
struct FunctionBlock {
    enum class Direction : std::uint8_t { Input = 1, Output = 2, Bidirectional = 3 };

    std::string  name;              // up to 91 bytes of ASCII
    std::uint8_t firstGroup = 0;    // 0-15
    std::uint8_t groupCount = 1;    // 1-16; firstGroup + groupCount <= 16
    Direction    direction  = Direction::Output;
    bool         active     = true;
};

// What a UMP endpoint declares about itself.
struct EndpointInfo {
    std::string name;                // up to 98 bytes; also the source's name
    std::string productInstanceId;   // up to 42 bytes of ASCII; unique per unit
    // Device identity: a SysEx manufacturer ID (a one-byte ID as {id, 0, 0}),
    // family and model (14 bits), and a four-byte software revision.
    std::array<std::uint8_t, 3> manufacturer{};
    std::uint16_t               family = 0, model = 0;
    std::array<std::uint8_t, 4> revision{};
    bool midi1 = false, midi2 = true;          // protocols it can use
    std::vector<FunctionBlock> blocks;         // at most 32
    bool staticBlocks = false;                 // blocks never change
};

// (uw::ump is C++17: no defaulted comparisons.)
inline bool operator==(const FunctionBlock& a, const FunctionBlock& b) {
    return a.name == b.name && a.firstGroup == b.firstGroup && a.groupCount == b.groupCount &&
           a.direction == b.direction && a.active == b.active;
}
inline bool operator!=(const FunctionBlock& a, const FunctionBlock& b) { return !(a == b); }

constexpr bool isStream(const MidiMessage& m) noexcept { return (m.words[0] >> 28) == 0xF; }

constexpr StreamStatus streamStatus(const MidiMessage& m) noexcept {
    return static_cast<StreamStatus>((m.words[0] >> 16) & 0x3FF);
}

// Endpoint Discovery and Function Block Discovery, as a host sends them.
// `filter` is the spec's bitmap: for the endpoint, 0x01 info, 0x02 device
// identity, 0x04 name, 0x08 Product Instance ID, 0x10 stream configuration;
// for blocks, 0x01 info and 0x02 name. Block 0xFF asks for all of them.
MidiMessage endpointDiscovery(std::uint8_t filter = 0x1F) noexcept;
MidiMessage functionBlockDiscovery(std::uint8_t block = 0xFF, std::uint8_t filter = 0x03) noexcept;

// The notifications an endpoint sends, appended to `out`. Text (names, the
// Product Instance ID) takes as many messages as it needs, and none if empty.
void endpointInfoNotification(const EndpointInfo& e, std::vector<MidiMessage>& out);
void deviceIdentityNotification(const EndpointInfo& e, std::vector<MidiMessage>& out);
void endpointNameNotification(const EndpointInfo& e, std::vector<MidiMessage>& out);
void productInstanceIdNotification(const EndpointInfo& e, std::vector<MidiMessage>& out);
void functionBlockInfoNotification(const EndpointInfo& e, std::uint8_t block, std::vector<MidiMessage>& out);
void functionBlockNameNotification(const EndpointInfo& e, std::uint8_t block, std::vector<MidiMessage>& out);

// The answer to a discovery message, appended to `out`: what its filter asks
// for. Stream configuration is answered with the endpoint's one protocol and
// no jitter-reduction timestamps (Stream Configuration proper is to come).
// Returns false, appending nothing, if `m` isn't a discovery message.
bool answerDiscovery(const MidiMessage& m, const EndpointInfo& e, std::vector<MidiMessage>& out);

// Reassembles UMP Stream text (names, Product Instance ID) from messages, for
// a receiver: feed it each message of one status in order; complete() once
// the last arrived.
struct StreamText {
    std::string text;
    bool        done = false;
    // Takes one message's bytes; `firstByte` is 2 for endpoint name and
    // Product Instance ID, 3 for a function block's name.
    void add(const MidiMessage& m, int firstByte);
    bool complete() const { return done; }
};

} // namespace uw::ump
