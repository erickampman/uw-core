#include "uw/ump/UmpStream.h"

#include <algorithm>

namespace uw::ump {

namespace {

// UMP 1.1, the version an endpoint reports and a discovery asks with.
constexpr std::uint32_t kUmpVersion = 0x0101;

// Text messages' form (word 0, bits 27-26).
enum Form : std::uint32_t { Complete = 0, Start = 1, Continue = 2, End = 3 };

std::uint32_t word0(StreamStatus status, std::uint32_t form = Complete) {
    return 0xF0000000u | (form << 26) | (std::uint32_t(status) << 16);
}

MidiMessage stream(std::uint32_t w0, std::uint32_t w1 = 0, std::uint32_t w2 = 0, std::uint32_t w3 = 0) {
    MidiMessage m;
    m.words[0]  = w0;
    m.words[1]  = w1;
    m.words[2]  = w2;
    m.words[3]  = w3;
    m.wordCount = 4;
    return m;
}

// Puts byte `i` (0 = word 0's top byte) of a 16-byte message.
void putByte(MidiMessage& m, int i, std::uint8_t byte) {
    const int shift = 24 - 8 * (i % 4);
    m.words[i / 4] |= std::uint32_t(byte) << shift;
}

std::uint8_t getByte(const MidiMessage& m, int i) {
    return std::uint8_t(m.words[i / 4] >> (24 - 8 * (i % 4)));
}

// Text in as many messages as it takes, from byte `firstByte` of each (2, or
// 3 after a function block's number), at most `limit` bytes of it.
void text(StreamStatus status, std::uint32_t prefix, int firstByte, const std::string& s, std::size_t limit,
          std::vector<MidiMessage>& out) {
    const std::size_t length  = std::min(s.size(), limit);
    const std::size_t perMessage = std::size_t(16 - firstByte);
    for (std::size_t at = 0; at < length; at += perMessage) {
        const bool first = at == 0, last = at + perMessage >= length;
        const std::uint32_t form = first && last ? Complete : first ? Start : last ? End : Continue;
        MidiMessage m = stream(word0(status, form) | prefix);
        for (std::size_t i = 0; i < perMessage && at + i < length; ++i)
            putByte(m, firstByte + int(i), std::uint8_t(s[at + i]) & 0x7F);
        out.push_back(m);
    }
}

} // namespace

MidiMessage endpointDiscovery(std::uint8_t filter) noexcept {
    return stream(word0(StreamStatus::EndpointDiscovery) | kUmpVersion, filter);
}

MidiMessage functionBlockDiscovery(std::uint8_t block, std::uint8_t filter) noexcept {
    return stream(word0(StreamStatus::FunctionBlockDiscovery) | (std::uint32_t(block) << 8) | filter);
}

void endpointInfoNotification(const EndpointInfo& e, std::vector<MidiMessage>& out) {
    const std::uint32_t blocks = std::uint32_t(std::min<std::size_t>(e.blocks.size(), 32));
    out.push_back(stream(word0(StreamStatus::EndpointInfo) | kUmpVersion,
                         (e.staticBlocks ? 0x80000000u : 0) | (blocks << 24) | (e.midi2 ? 1u << 9 : 0) |
                             (e.midi1 ? 1u << 8 : 0)));
}

void deviceIdentityNotification(const EndpointInfo& e, std::vector<MidiMessage>& out) {
    auto b7 = [](unsigned v) { return std::uint32_t(v & 0x7F); };
    out.push_back(stream(word0(StreamStatus::DeviceIdentity),
                         (b7(e.manufacturer[0]) << 16) | (b7(e.manufacturer[1]) << 8) | b7(e.manufacturer[2]),
                         (b7(e.family) << 24) | (b7(e.family >> 7) << 16) | (b7(e.model) << 8) | b7(e.model >> 7),
                         (b7(e.revision[0]) << 24) | (b7(e.revision[1]) << 16) | (b7(e.revision[2]) << 8) |
                             b7(e.revision[3])));
}

void endpointNameNotification(const EndpointInfo& e, std::vector<MidiMessage>& out) {
    text(StreamStatus::EndpointName, 0, 2, e.name, 98, out);
}

void productInstanceIdNotification(const EndpointInfo& e, std::vector<MidiMessage>& out) {
    text(StreamStatus::ProductInstanceId, 0, 2, e.productInstanceId, 42, out);
}

void functionBlockInfoNotification(const EndpointInfo& e, std::uint8_t block, std::vector<MidiMessage>& out) {
    if (block >= e.blocks.size()) return;
    const FunctionBlock& b   = e.blocks[block];
    const std::uint32_t  dir = std::uint32_t(b.direction) & 0x3;
    // The UI hint follows the direction: a sender, a receiver, or both.
    const std::uint32_t hint = dir;
    out.push_back(stream(word0(StreamStatus::FunctionBlockInfo) | (b.active ? 0x8000u : 0) |
                             (std::uint32_t(block & 0x7F) << 8) | (hint << 4) | dir,
                         (std::uint32_t(b.firstGroup & 0x0F) << 24) | (std::uint32_t(b.groupCount) << 16)));
}

void functionBlockNameNotification(const EndpointInfo& e, std::uint8_t block, std::vector<MidiMessage>& out) {
    if (block >= e.blocks.size()) return;
    text(StreamStatus::FunctionBlockName, std::uint32_t(block) << 8, 3, e.blocks[block].name, 91, out);
}

bool answerDiscovery(const MidiMessage& m, const EndpointInfo& e, std::vector<MidiMessage>& out) {
    if (!isStream(m)) return false;
    if (streamStatus(m) == StreamStatus::EndpointDiscovery) {
        const std::uint8_t filter = std::uint8_t(m.words[1]);
        if (filter & 0x01) endpointInfoNotification(e, out);
        if (filter & 0x02) deviceIdentityNotification(e, out);
        if (filter & 0x04) endpointNameNotification(e, out);
        if (filter & 0x08) productInstanceIdNotification(e, out);
        if (filter & 0x10)
            out.push_back(stream(word0(StreamStatus::StreamConfigNotification) |
                                 (e.midi2 ? 0x0200u : 0x0100u)));
        return true;
    }
    if (streamStatus(m) == StreamStatus::FunctionBlockDiscovery) {
        const std::uint8_t asked  = std::uint8_t(m.words[0] >> 8);
        const std::uint8_t filter = std::uint8_t(m.words[0]);
        for (std::size_t i = 0; i < e.blocks.size() && i < 32; ++i) {
            if (asked != 0xFF && asked != i) continue;
            if (filter & 0x01) functionBlockInfoNotification(e, std::uint8_t(i), out);
            if (filter & 0x02) functionBlockNameNotification(e, std::uint8_t(i), out);
        }
        return true;
    }
    return false;
}

void StreamText::add(const MidiMessage& m, int firstByte) {
    const std::uint32_t form = (m.words[0] >> 26) & 0x3;
    if (form == Complete || form == Start) {
        text.clear();
        done = false;
    }
    for (int i = firstByte; i < 16; ++i)
        if (const std::uint8_t b = getByte(m, i)) text.push_back(char(b));
    done = form == Complete || form == End;
}

} // namespace uw::ump
