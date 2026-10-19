#include "uw/midi/AlsaSeqEndpoint.h"

#include <alsa/asoundlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>

#include "uw/ump/Ump.h"

namespace uw {

// The endpoint and block info's setters are alsa-lib 1.2.13's (Ubuntu 24.04
// has 1.2.11): before it, openEndpoint() publishes a plain source.
#if SND_LIB_VERSION >= SND_LIB_VER(1, 2, 13)

namespace {

constexpr int kGroups = 16;   // a port for each, made at once; those no block holds stay hidden

std::uint32_t b7(unsigned v) { return v & 0x7F; }

// The port capabilities a block's direction gives its groups' ports (as
// alsa-lib's update_group_ports()): we send on an Output block's groups.
unsigned capsFor(ump::FunctionBlock::Direction direction) {
    constexpr unsigned read  = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SYNC_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    constexpr unsigned write = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SYNC_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    switch (direction) {
        case ump::FunctionBlock::Direction::Input: return write;
        case ump::FunctionBlock::Direction::Output: return read;
        case ump::FunctionBlock::Direction::Bidirectional: return read | write | SND_SEQ_PORT_CAP_DUPLEX;
    }
    return 0;
}

} // namespace

std::unique_ptr<AlsaSeqEndpoint> AlsaSeqEndpoint::create(const ump::EndpointInfo& info, MidiProtocol protocol) {
    std::unique_ptr<AlsaSeqEndpoint> e(new AlsaSeqEndpoint());
    e->info_     = info;
    e->protocol_ = protocol;
    snd_seq_t* s = nullptr;
    if (snd_seq_open(&s, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        std::fprintf(stderr, "AlsaSeqEndpoint: cannot open the ALSA sequencer\n");
        return nullptr;
    }
    e->seq_ = s;
    if (snd_seq_set_client_midi_version(s, protocol == MidiProtocol::Midi2 ? SND_SEQ_CLIENT_UMP_MIDI_2_0
                                                                            : SND_SEQ_CLIENT_UMP_MIDI_1_0) < 0) {
        std::fprintf(stderr, "AlsaSeqEndpoint: this ALSA sequencer does not support UMP clients\n");
        return nullptr;
    }
    snd_seq_set_client_name(s, info.name.empty() ? "UMP endpoint" : info.name.c_str());
    e->client_ = snd_seq_client_id(s);

    // Port 0, the endpoint, and a port for each group, as alsa-lib's
    // snd_seq_create_ump_endpoint() makes them.
    snd_seq_port_info_t* port = nullptr;
    snd_seq_port_info_alloca(&port);
    snd_seq_port_info_set_port(port, 0);
    snd_seq_port_info_set_port_specified(port, 1);
    snd_seq_port_info_set_name(port, "MIDI 2.0");
    snd_seq_port_info_set_capability(
        port, SND_SEQ_PORT_CAP_UMP_ENDPOINT | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SYNC_READ |
                  SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SYNC_WRITE |
                  SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_DUPLEX);
    snd_seq_port_info_set_type(port, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_MIDI_UMP |
                                         SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_PORT);
    snd_seq_port_info_set_ump_group(port, 0);
    if (snd_seq_create_port(s, port) < 0) {
        std::fprintf(stderr, "AlsaSeqEndpoint: cannot create the endpoint's port\n");
        return nullptr;
    }
    for (int g = 0; g < kGroups; ++g) {
        const std::string name = "Group " + std::to_string(g + 1);
        snd_seq_port_info_set_port(port, g + 1);
        snd_seq_port_info_set_port_specified(port, 1);
        snd_seq_port_info_set_name(port, name.c_str());
        snd_seq_port_info_set_capability(port, 0);   // declare() shows it if a block holds it
        snd_seq_port_info_set_ump_group(port, g + 1);
        if (snd_seq_create_port(s, port) < 0) {
            std::fprintf(stderr, "AlsaSeqEndpoint: cannot create the port for group %d\n", g + 1);
            return nullptr;
        }
    }
    if (!e->declare()) return nullptr;

    // New subscriptions are announced to the system announce port's subscribers.
    e->announce_ = snd_seq_create_simple_port(s, "announcements", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
                                              SND_SEQ_PORT_TYPE_APPLICATION);
    if (e->announce_ < 0 ||
        snd_seq_connect_from(s, e->announce_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE) < 0)
        std::fprintf(stderr, "AlsaSeqEndpoint: can't hear receivers subscribing; they won't get the setup again\n");

    e->wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->wakeFd_ < 0) {
        std::fprintf(stderr, "AlsaSeqEndpoint: eventfd failed\n");
        return nullptr;
    }
    e->thread_ = std::thread(&AlsaSeqEndpoint::run, e.get());
    return e;
}

AlsaSeqEndpoint::~AlsaSeqEndpoint() {
    if (thread_.joinable()) {
        quit_.store(true);
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t n = ::write(wakeFd_, &one, sizeof one);
        thread_.join();
    }
    if (seq_ != nullptr) snd_seq_close(seq_);   // the client, its ports and their subscriptions go
    if (wakeFd_ >= 0) ::close(wakeFd_);
}

bool AlsaSeqEndpoint::setFunctionBlocks(const std::vector<ump::FunctionBlock>& blocks,
                                        std::vector<MidiMessage>& notify) {
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (info_.blocks == blocks) return true;
        info_.blocks = blocks;
    }
    if (!declare()) return false;   // only the main thread writes info_, so it reads it unlocked
    ump::endpointInfoNotification(info_, notify);
    for (std::size_t i = 0; i < info_.blocks.size() && i < 32; ++i) {
        ump::functionBlockInfoNotification(info_, std::uint8_t(i), notify);
        ump::functionBlockNameNotification(info_, std::uint8_t(i), notify);
    }
    return true;
}

// The kernel's endpoint and block info (what ump::answerDiscovery says in
// band), and each group port's name and capabilities from the blocks.
bool AlsaSeqEndpoint::declare() {
    const ump::EndpointInfo& e      = info_;
    const std::size_t        blocks = std::min<std::size_t>(e.blocks.size(), 32);
    const unsigned           used   = protocol_ == MidiProtocol::Midi2 ? SND_UMP_EP_INFO_PROTO_MIDI2
                                                                        : SND_UMP_EP_INFO_PROTO_MIDI1;

    snd_ump_endpoint_info_t* ep = nullptr;
    snd_ump_endpoint_info_alloca(&ep);
    snd_ump_endpoint_info_set_name(ep, e.name.c_str());
    snd_ump_endpoint_info_set_product_id(ep, e.productInstanceId.c_str());
    snd_ump_endpoint_info_set_protocol_caps(ep, used | (e.midi1 ? SND_UMP_EP_INFO_PROTO_MIDI1 : 0) |
                                                    (e.midi2 ? SND_UMP_EP_INFO_PROTO_MIDI2 : 0));
    snd_ump_endpoint_info_set_protocol(ep, used);
    snd_ump_endpoint_info_set_flags(ep, e.staticBlocks ? SND_UMP_EP_INFO_STATIC_BLOCKS : 0);
    snd_ump_endpoint_info_set_num_blocks(ep, static_cast<unsigned>(blocks));
    snd_ump_endpoint_info_set_version(ep, SND_UMP_EP_INFO_DEFAULT_VERSION);
    // As the kernel keeps a device's (sound/core/ump.c): the Device Identity
    // message's 7-bit bytes, most significant first.
    snd_ump_endpoint_info_set_manufacturer_id(
        ep, (b7(e.manufacturer[0]) << 16) | (b7(e.manufacturer[1]) << 8) | b7(e.manufacturer[2]));
    snd_ump_endpoint_info_set_family_id(ep, (b7(e.family >> 7) << 8) | b7(e.family));
    snd_ump_endpoint_info_set_model_id(ep, (b7(e.model >> 7) << 8) | b7(e.model));
    const unsigned char revision[4] = {static_cast<unsigned char>(b7(e.revision[0])),
                                       static_cast<unsigned char>(b7(e.revision[1])),
                                       static_cast<unsigned char>(b7(e.revision[2])),
                                       static_cast<unsigned char>(b7(e.revision[3]))};
    snd_ump_endpoint_info_set_sw_revision(ep, revision);
    if (snd_seq_set_ump_endpoint_info(seq_, ep) < 0) {
        std::fprintf(stderr, "AlsaSeqEndpoint: the sequencer refused the endpoint info\n");
        return false;
    }

    for (std::size_t i = 0; i < blocks; ++i) {
        const ump::FunctionBlock& b         = e.blocks[i];
        const unsigned            direction = static_cast<unsigned>(b.direction) & 0x3;
        snd_ump_block_info_t*     info      = nullptr;
        snd_ump_block_info_alloca(&info);
        snd_ump_block_info_set_block_id(info, static_cast<unsigned>(i));
        snd_ump_block_info_set_active(info, b.active ? 1 : 0);
        snd_ump_block_info_set_direction(info, direction);
        snd_ump_block_info_set_ui_hint(info, direction);   // as answerDiscovery's
        snd_ump_block_info_set_first_group(info, b.firstGroup);
        snd_ump_block_info_set_num_groups(info, b.groupCount);
        snd_ump_block_info_set_name(info, b.name.c_str());
        if (snd_seq_set_ump_block_info(seq_, static_cast<int>(i), info) < 0) {
            std::fprintf(stderr, "AlsaSeqEndpoint: the sequencer refused function block %zu\n", i);
            return false;
        }
    }

    snd_seq_port_info_t* port = nullptr;
    snd_seq_port_info_alloca(&port);
    for (int g = 0; g < kGroups; ++g) {
        unsigned    caps = 0;
        std::string names;
        for (std::size_t i = 0; i < blocks; ++i) {
            const ump::FunctionBlock& b = e.blocks[i];
            if (!b.active || g < b.firstGroup || g >= b.firstGroup + b.groupCount) continue;
            caps |= capsFor(b.direction);
            if (b.name.empty()) continue;
            names += (names.empty() ? "" : ", ") + b.name;
        }
        std::string name = "Group " + std::to_string(g + 1);
        if (!names.empty()) name += " (" + names + ")";
        if (snd_seq_get_port_info(seq_, g + 1, port) < 0) return false;
        if (name == snd_seq_port_info_get_name(port) && caps == snd_seq_port_info_get_capability(port)) continue;
        snd_seq_port_info_set_name(port, name.c_str());
        snd_seq_port_info_set_capability(port, caps);
        if (snd_seq_set_port_info(seq_, g + 1, port) < 0) {
            std::fprintf(stderr, "AlsaSeqEndpoint: cannot update the port for group %d\n", g + 1);
            return false;
        }
    }
    return true;
}

// --- the endpoint's thread -------------------------------------------------------

void AlsaSeqEndpoint::run() {
    std::vector<pollfd> fds(static_cast<std::size_t>(snd_seq_poll_descriptors_count(seq_, POLLIN)));
    snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(fds.size()), POLLIN);
    fds.push_back(pollfd{wakeFd_, POLLIN, 0});

    while (!quit_.load(std::memory_order_relaxed)) {
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "AlsaSeqEndpoint: poll failed\n");
            return;
        }
        snd_seq_ump_event_t* event = nullptr;
        for (;;) {
            const int result = snd_seq_ump_event_input(seq_, &event);
            if (result == -ENOSPC) continue;   // overran: what's lost was asked again or is gone
            if (result < 0) break;             // -EAGAIN: nothing more queued
            if (!snd_seq_ev_is_ump(event)) {
                // A receiver subscribing to one of our ports: it starts listening.
                // Counted as announced: the kernel also tells the port itself
                // when a third client connects it, if the port can be written.
                const auto* const e = reinterpret_cast<const snd_seq_event_t*>(event);
                if (e->type == SND_SEQ_EVENT_PORT_SUBSCRIBED && e->dest.port == announce_ &&
                    e->data.connect.sender.client == client_)
                    asked_.fetch_add(1, std::memory_order_release);
                continue;
            }
            if (event->dest.port != port()) continue;
            MidiMessage m;
            m.wordCount = ump::wordCountForType(static_cast<std::uint8_t>(event->ump[0] >> 28));
            for (std::uint8_t i = 0; i < m.wordCount; ++i) m.words[i] = event->ump[i];
            answer(m);
        }
    }
}

// Answers a discovery message to port 0's subscribers, written directly so it
// never meets the main thread's output buffer.
void AlsaSeqEndpoint::answer(const MidiMessage& m) {
    std::vector<MidiMessage> replies;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (!ump::answerDiscovery(m, info_, replies)) return;
    }
    for (const MidiMessage& r : replies) {
        snd_seq_ump_event_t event;
        snd_seq_ump_ev_clear(&event);
        snd_seq_ev_set_ump_data(&event, const_cast<std::uint32_t*>(r.words), r.wordCount * sizeof r.words[0]);
        snd_seq_ev_set_source(&event, port());
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        snd_seq_ump_event_output_direct(seq_, &event);
    }
    asked_.fetch_add(1, std::memory_order_release);
}

#else

std::unique_ptr<AlsaSeqEndpoint> AlsaSeqEndpoint::create(const ump::EndpointInfo&, MidiProtocol) {
    std::fprintf(stderr, "AlsaSeqEndpoint: UMP endpoints need alsa-lib 1.2.13 or later; publishing a plain source\n");
    return nullptr;
}

AlsaSeqEndpoint::~AlsaSeqEndpoint() = default;

bool AlsaSeqEndpoint::setFunctionBlocks(const std::vector<ump::FunctionBlock>&, std::vector<MidiMessage>&) {
    return false;
}

#endif

} // namespace uw
