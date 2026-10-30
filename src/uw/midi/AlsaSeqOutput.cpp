#include "uw/midi/AlsaSeqOutput.h"

#include "uw/midi/AlsaSeqEndpoint.h"
#include "uw/base/Clock.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <vector>

namespace uw {

namespace {

// Each event sent holds a cell of the sender's pool until the receiver reads
// it; the kernel's most (SNDRV_SEQ_MAX_EVENTS) lets a burst run well ahead.
constexpr int kOutputPool = 2000;

// How long one send() waits, at most, for a slow receiver to make room.
constexpr auto kMaxWait = std::chrono::milliseconds(10);

const char* protocolName(MidiProtocol protocol) {
    return protocol == MidiProtocol::Midi2 ? "MIDI 2.0" : "MIDI 1.0";
}

// "client:port" numeric address, e.g. "128:0" (as AlsaSeqInput's).
bool parseAddress(const std::string& text, int& client, int& port) {
    unsigned c = 0, p = 0;
    char     extra = 0;
    if (std::sscanf(text.c_str(), "%u:%u%c", &c, &p, &extra) != 2) return false;
    client = static_cast<int>(c);
    port   = static_cast<int>(p);
    return true;
}

// A MIDI 2.0 client's port marked MIDI 1.0 (a function block's). alsa-lib 1.2.13 can say; before it
// (Ubuntu 24.04 has 1.2.11), every port of a MIDI 2.0 client is taken as MIDI 2.0.
bool isMidi1Port(const snd_seq_port_info_t* port) {
#if SND_LIB_VERSION >= SND_LIB_VER(1, 2, 13)
    return snd_seq_port_info_get_ump_is_midi1(port);
#else
    (void)port;
    return false;
#endif
}

bool isAnnouncement(int type) {
    switch (type) {
        case SND_SEQ_EVENT_CLIENT_START:
        case SND_SEQ_EVENT_CLIENT_EXIT:
        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_PORT_CHANGE:
            return true;
        default: return false;
    }
}

} // namespace

// --- lifecycle -------------------------------------------------------------------

std::unique_ptr<AlsaSeqOutput> AlsaSeqOutput::create(const std::string& clientName) {
    std::unique_ptr<AlsaSeqOutput> output(new AlsaSeqOutput());
    output->clientNames[kMidi1Client] = clientName;
    output->clientNames[kMidi2Client] = clientName + " MIDI 2.0";
    const int versions[kClientCount]   = {SND_SEQ_CLIENT_UMP_MIDI_1_0, SND_SEQ_CLIENT_UMP_MIDI_2_0};

    for (int c = 0; c < kClientCount; ++c) {
        if (snd_seq_open(&output->seq[c], "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
            std::fprintf(stderr, "AlsaSeqOutput: cannot open the ALSA sequencer\n");
            return nullptr;
        }
        if (snd_seq_set_client_midi_version(output->seq[c], versions[c]) < 0) {
            std::fprintf(stderr, "AlsaSeqOutput: this ALSA sequencer does not support UMP "
                                 "clients (kernel 6.5+ and alsa-lib 1.2.10+ needed)\n");
            return nullptr;
        }
        snd_seq_set_client_name(output->seq[c], output->clientNames[c].c_str());
        snd_seq_set_client_pool_output(output->seq[c], kOutputPool);   // the default if refused
        output->clientId[c] = snd_seq_client_id(output->seq[c]);
    }

    // Hotplug: a private port hears the system's announce port (changes()).
    snd_seq_t* const s    = output->seq[kMidi1Client];
    const int        port = snd_seq_create_simple_port(s, "announcements",
                                                       SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
                                                       SND_SEQ_PORT_TYPE_APPLICATION);
    output->followsAnnouncements =
        port >= 0 && snd_seq_connect_from(s, port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE) >= 0;
    if (!output->followsAnnouncements)
        std::fprintf(stderr, "AlsaSeqOutput: can't follow MIDI devices coming and going; restart to see new ones\n");
    return output;
}

AlsaSeqOutput::~AlsaSeqOutput() {
    for (snd_seq_t* s : seq)
        if (s != nullptr) snd_seq_close(s);   // also removes our ports
}

// --- MidiOutput -------------------------------------------------------------------

std::vector<MidiDestinationInfo> AlsaSeqOutput::enumerate() {
    std::vector<MidiDestinationInfo> infos;
    for (const Destination& d : listDestinations()) infos.push_back(d.info);
    return infos;
}

MidiDestinationHandle AlsaSeqOutput::open(const std::string& destinationId) {
    Destination found;
    if (!findDestination(destinationId, found)) {
        std::fprintf(stderr, "AlsaSeqOutput: no MIDI destination '%s'\n", destinationId.c_str());
        return kInvalidMidiDestination;
    }
    Opened o;
    o.id          = destinationId;
    o.protocol    = found.info.protocol;
    o.clientIndex = clientFor(o.protocol);
    o.port        = senderPort(o.clientIndex);
    o.destClient  = found.client;
    o.destPort    = found.port;
    if (o.port < 0) return kInvalidMidiDestination;
    o.tag = freeTag();
    const MidiDestinationHandle handle = nextHandle++;
    opened[handle] = std::move(o);
    return handle;
}

MidiDestinationHandle AlsaSeqOutput::openVirtual(const std::string& portName, MidiProtocol protocol,
                                                 std::int32_t persistentId) {
    (void)persistentId;   // ALSA has none: the client and port names identify it
    Opened o;
    o.isVirtual   = true;
    o.id          = portName;
    o.protocol    = protocol;
    o.clientIndex = clientFor(protocol);
    o.port        = snd_seq_create_simple_port(
        seq[o.clientIndex], portName.c_str(), SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (o.port < 0) {
        std::fprintf(stderr, "AlsaSeqOutput: cannot create port '%s'\n", portName.c_str());
        return kInvalidMidiDestination;
    }
    o.tag = freeTag();
    const MidiDestinationHandle handle = nextHandle++;
    opened[handle] = std::move(o);
    return handle;
}

MidiDestinationHandle AlsaSeqOutput::openEndpoint(const ump::EndpointInfo& endpoint, MidiProtocol protocol,
                                                  std::int32_t persistentId) {
    Opened o;
    o.isVirtual = true;
    o.id        = endpoint.name;
    o.protocol  = protocol;
    o.ump       = AlsaSeqEndpoint::create(endpoint, protocol);
    if (!o.ump) return openVirtual(endpoint.name, protocol, persistentId);
    o.port = AlsaSeqEndpoint::port();
    snd_seq_set_client_pool_output(o.ump->sequencer(), kOutputPool);   // the default if refused
    o.tag = freeTag();
    const MidiDestinationHandle handle = nextHandle++;
    opened[handle] = std::move(o);
    return handle;
}

bool AlsaSeqOutput::setFunctionBlocks(MidiDestinationHandle destination, const std::vector<ump::FunctionBlock>& blocks) {
    const auto it = opened.find(destination);
    if (it == opened.end() || !it->second.ump) return false;
    std::vector<MidiMessage> notify;
    if (!it->second.ump->setFunctionBlocks(blocks, notify)) return false;
    return notify.empty() || send(destination, notify.data(), notify.size());
}

std::uint64_t AlsaSeqOutput::inquiries(MidiDestinationHandle destination) const {
    const auto it = opened.find(destination);
    return it == opened.end() || !it->second.ump ? 0 : it->second.ump->inquiries();
}

void AlsaSeqOutput::close(MidiDestinationHandle destination) {
    const auto it = opened.find(destination);
    if (it == opened.end()) return;
    cancelScheduled(destination);
    if (it->second.ump) {
        queues.erase(it->second.ump->sequencer());
        it->second.ump.reset();   // its client goes, ports, queue and all
    }
    else if (it->second.isVirtual)
        snd_seq_delete_simple_port(seq[it->second.clientIndex], it->second.port);
    opened.erase(it);
}

MidiProtocol AlsaSeqOutput::protocol(MidiDestinationHandle destination) const {
    const auto it = opened.find(destination);
    return it == opened.end() ? MidiProtocol::Midi1 : it->second.protocol;
}

bool AlsaSeqOutput::send(MidiDestinationHandle destination, const MidiMessage* messages, std::size_t count) {
    const auto it = opened.find(destination);
    return it != opened.end() && write(it->second, messages, count, -1, 0);
}

bool AlsaSeqOutput::sendAt(MidiDestinationHandle destination, const MidiMessage* messages, std::size_t count,
                           Nanoseconds when) {
    const auto it = opened.find(destination);
    if (it == opened.end()) return false;
    const Opened& o = it->second;
    const int queue = queueFor(sequencerOf(o));
    if (queue < 0) return write(o, messages, count, -1, 0);   // can't schedule: now

    // The queue's time now, and ours, read together: `when` is as far ahead
    // on the queue's.
    snd_seq_queue_status_t* status = nullptr;
    snd_seq_queue_status_alloca(&status);
    if (snd_seq_get_queue_status(sequencerOf(o), queue, status) < 0) return write(o, messages, count, -1, 0);
    const Nanoseconds          now   = nowNanoseconds();
    const snd_seq_real_time_t* clock = snd_seq_queue_status_get_real_time(status);
    if (when <= now) return write(o, messages, count, -1, 0);
    const std::int64_t at = static_cast<std::int64_t>(clock->tv_sec) * 1'000'000'000 + clock->tv_nsec + (when - now);
    return write(o, messages, count, queue, at);
}

bool AlsaSeqOutput::schedules(MidiDestinationHandle destination) const {
    return opened.count(destination) != 0;
}

void AlsaSeqOutput::cancelScheduled(MidiDestinationHandle destination) {
    const auto it = opened.find(destination);
    if (it == opened.end() || it->second.tag == 0) return;
    snd_seq_t* const s = sequencerOf(it->second);
    if (queues.count(s) == 0) return;   // nothing was ever scheduled from it
    snd_seq_remove_events_t* remove = nullptr;
    snd_seq_remove_events_alloca(&remove);
    snd_seq_remove_events_set_condition(remove, SND_SEQ_REMOVE_OUTPUT | SND_SEQ_REMOVE_TAG_MATCH);
    snd_seq_remove_events_set_tag(remove, it->second.tag);
    snd_seq_remove_events(s, remove);
}

// Each message is an event from `o`'s port, direct, or with `queue` at
// `queueNanoseconds` on it; buffered, then written together.
bool AlsaSeqOutput::write(const Opened& o, const MidiMessage* messages, std::size_t count, int queue,
                          std::int64_t queueNanoseconds) {
    if (!o.isVirtual && o.destClient < 0) return false;
    snd_seq_t* const s = sequencerOf(o);

    // Writes what's buffered. A full pool (-EAGAIN: receivers behind on
    // reading) gets a short wait for room; anything else, a destination gone
    // for one, drops what's left, so it doesn't stand in front of the next send.
    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    auto       drain    = [&] {
        for (;;) {
            const int result = snd_seq_drain_output(s);
            if (result == 0) return true;
            if (result > 0 || result == -EAGAIN) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (left.count() > 0) {
                    std::vector<pollfd> fds(static_cast<std::size_t>(snd_seq_poll_descriptors_count(s, POLLOUT)));
                    snd_seq_poll_descriptors(s, fds.data(), static_cast<unsigned>(fds.size()), POLLOUT);
                    ::poll(fds.data(), fds.size(), static_cast<int>(left.count()));
                    continue;
                }
            }
            snd_seq_drop_output(s);
            return false;
        }
    };

    snd_seq_real_time_t at{};
    at.tv_sec  = static_cast<unsigned>(queueNanoseconds / 1'000'000'000);
    at.tv_nsec = static_cast<unsigned>(queueNanoseconds % 1'000'000'000);
    for (std::size_t i = 0; i < count; ++i) {
        const MidiMessage& m = messages[i];
        if (m.wordCount == 0 || m.wordCount > 4) continue;
        snd_seq_ump_event_t event;
        snd_seq_ump_ev_clear(&event);
        snd_seq_ev_set_ump_data(&event, const_cast<std::uint32_t*>(m.words), m.wordCount * sizeof m.words[0]);
        snd_seq_ev_set_source(&event, o.port);
        if (o.isVirtual)
            snd_seq_ev_set_subs(&event);
        else
            snd_seq_ev_set_dest(&event, o.destClient, o.destPort);
        if (queue < 0) {
            snd_seq_ev_set_direct(&event);
        } else {
            snd_seq_ev_schedule_real(&event, queue, 0, &at);
            snd_seq_ev_set_tag(&event, o.tag);
        }

        int result = snd_seq_ump_event_output_buffer(s, &event);
        if (result == -EAGAIN) {   // the buffer is full: write it out and try again
            if (!drain()) return false;
            result = snd_seq_ump_event_output_buffer(s, &event);
        }
        if (result < 0) {
            snd_seq_drop_output(s);
            return false;
        }
    }
    return drain();
}

std::uint64_t AlsaSeqOutput::changes() const {
    if (!followsAnnouncements) return announced;
    snd_seq_ump_event_t* event = nullptr;
    for (;;) {
        const int result = snd_seq_ump_event_input(seq[kMidi1Client], &event);
        if (result == -EAGAIN || (result < 0 && result != -ENOSPC)) break;
        // An overrun lost some: count it as one, so the caller looks again.
        if (result == -ENOSPC || (!snd_seq_ev_is_ump(event) && isAnnouncement(event->type))) ++announced;
    }
    return announced;
}

bool AlsaSeqOutput::sync(MidiDestinationHandle destination) {
    const auto it = opened.find(destination);
    if (it == opened.end()) return false;
    Opened& o = it->second;
    if (o.isVirtual) return true;

    // Still there, or back (usually at a new address)? Found by its id either way.
    Destination found;
    if (!findDestination(o.id, found)) {
        o.destClient = o.destPort = -1;
        return false;
    }
    const int port = senderPort(clientFor(found.info.protocol));
    if (port < 0) return false;
    o.protocol    = found.info.protocol;
    o.clientIndex = clientFor(o.protocol);
    o.port        = port;
    o.destClient  = found.client;
    o.destPort    = found.port;
    return true;
}

std::string AlsaSeqOutput::describe(MidiDestinationHandle destination) const {
    const auto it = opened.find(destination);
    if (it == opened.end()) return "(not open)";
    const Opened& o = it->second;
    if (o.ump)
        return "UMP endpoint " + std::to_string(o.ump->client()) + ":" + std::to_string(o.port) + " '" + o.id +
               "' (" + protocolName(o.protocol) + ")";
    if (o.isVirtual)
        return "virtual source " + std::to_string(clientId[o.clientIndex]) + ":" + std::to_string(o.port) +
               " on '" + clientNames[o.clientIndex] + "' (" + protocolName(o.protocol) + ")";
    if (o.destClient < 0) return "destination '" + o.id + "' (" + protocolName(o.protocol) + "), gone";
    return "destination '" + o.id + "' at " + std::to_string(o.destClient) + ":" + std::to_string(o.destPort) +
           " (" + protocolName(o.protocol) + ")";
}

// --- helpers ------------------------------------------------------------------------

// Writable, subscribable ports of every client but ours (our endpoints too):
// what aconnect lists as destinations.
std::vector<AlsaSeqOutput::Destination> AlsaSeqOutput::listDestinations() const {
    std::vector<Destination> destinations;
    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t*   portInfo   = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);
    snd_seq_t* const s = seq[kMidi1Client];

    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(s, clientInfo) >= 0) {
        const int client = snd_seq_client_info_get_client(clientInfo);
        if (client == SND_SEQ_CLIENT_SYSTEM || client == clientId[kMidi1Client] ||
            client == clientId[kMidi2Client] || ownsEndpoint(client))
            continue;
        const bool midi2Client = snd_seq_client_info_get_midi_version(clientInfo) == SND_SEQ_CLIENT_UMP_MIDI_2_0;

        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(s, portInfo) >= 0) {
            const unsigned caps     = snd_seq_port_info_get_capability(portInfo);
            const unsigned writable = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
            if ((caps & writable) != writable || (caps & SND_SEQ_PORT_CAP_NO_EXPORT) != 0) continue;

            Destination d;
            d.client          = client;
            d.port            = snd_seq_port_info_get_port(portInfo);
            d.info.name       = snd_seq_port_info_get_name(portInfo);
            d.info.id         = std::string(snd_seq_client_info_get_name(clientInfo)) + ":" + d.info.name;
            d.info.isVirtual  = snd_seq_client_info_get_type(clientInfo) == SND_SEQ_USER_CLIENT;
            d.info.isHardware = (snd_seq_port_info_get_type(portInfo) & SND_SEQ_PORT_TYPE_HARDWARE) != 0;
            d.info.protocol   = midi2Client && !isMidi1Port(portInfo) ? MidiProtocol::Midi2 : MidiProtocol::Midi1;
            destinations.push_back(d);
        }
    }
    return destinations;
}

// Whether `client` is one of our openEndpoint() endpoints'.
bool AlsaSeqOutput::ownsEndpoint(int client) const {
    for (const auto& [handle, o] : opened)
        if (o.ump && o.ump->client() == client) return true;
    return false;
}

// By id ("client name:port name") or numeric address ("128:0").
bool AlsaSeqOutput::findDestination(const std::string& id, Destination& found) const {
    int        client = -1, port = -1;
    const bool numeric = parseAddress(id, client, port);
    for (const Destination& d : listDestinations()) {
        if (numeric ? (d.client == client && d.port == port) : d.info.id == id) {
            found = d;
            return true;
        }
    }
    return false;
}

// A tag no opened handle has, or 0 if all 255 are taken.
std::uint8_t AlsaSeqOutput::freeTag() const {
    for (int tag = 1; tag <= 255; ++tag) {
        bool taken = false;
        for (const auto& [handle, o] : opened) taken = taken || o.tag == tag;
        if (!taken) return static_cast<std::uint8_t>(tag);
    }
    return 0;
}

// The sequencer client `o` sends from.
snd_seq_t* AlsaSeqOutput::sequencerOf(const Opened& o) const {
    return o.ump ? o.ump->sequencer() : seq[o.clientIndex];
}

// The queue sendAt() schedules on from client `s`, made and started the first
// time it's needed; -1 if the sequencer refused.
int AlsaSeqOutput::queueFor(snd_seq_t* s) {
    if (const auto it = queues.find(s); it != queues.end()) return it->second;
    const int queue = snd_seq_alloc_named_queue(s, "uw-core sendAt");
    if (queue < 0 || snd_seq_start_queue(s, queue, nullptr) < 0 || snd_seq_drain_output(s) < 0) {
        std::fprintf(stderr, "AlsaSeqOutput: no sequencer queue to schedule on; sending at once\n");
        if (queue >= 0) snd_seq_free_queue(s, queue);
        return -1;
    }
    queues[s] = queue;
    return queue;
}

// The client's private port that open() destinations are sent from, made the
// first time it's needed.
int AlsaSeqOutput::senderPort(int clientIndex) {
    if (senders[clientIndex] < 0) {
        senders[clientIndex] = snd_seq_create_simple_port(
            seq[clientIndex], "output", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_NO_EXPORT,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (senders[clientIndex] < 0)
            std::fprintf(stderr, "AlsaSeqOutput: cannot create the output port on '%s'\n",
                         clientNames[clientIndex].c_str());
    }
    return senders[clientIndex];
}

} // namespace uw
