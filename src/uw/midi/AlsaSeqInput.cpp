#include "uw/midi/AlsaSeqInput.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <poll.h>

#include "uw/base/Clock.h"
#include "uw/ump/Ump.h"

namespace uw {

namespace {

// Below PipeWire's audio threads (rtkit's default for them is 88), so MIDI
// capture never preempts audio.
constexpr int kMidiThreadPriority = 60;

// Events queued for us before the MIDI thread reads them. ALSA's default, 200,
// overruns on a burst (a fast sender, a controller dump); the kernel's most
// (SNDRV_SEQ_MAX_EVENTS) doesn't.
constexpr int kInputPool = 2000;

const char* midiVersionName(int version) {
    switch (version) {
        case SND_SEQ_CLIENT_UMP_MIDI_1_0: return "UMP MIDI 1.0";
        case SND_SEQ_CLIENT_UMP_MIDI_2_0: return "UMP MIDI 2.0";
        default:                          return "legacy MIDI 1.0";
    }
}

// "client:port" numeric address, e.g. "14:0".
bool parseAddress(const std::string& text, int& client, int& port) {
    unsigned c = 0, p = 0;
    char     extra = 0;
    if (std::sscanf(text.c_str(), "%u:%u%c", &c, &p, &extra) != 2) return false;
    client = static_cast<int>(c);
    port   = static_cast<int>(p);
    return true;
}

} // namespace

// --- lifecycle -------------------------------------------------------------------

std::unique_ptr<AlsaSeqInput> AlsaSeqInput::create(const std::string& clientName) {
    std::unique_ptr<AlsaSeqInput> input(new AlsaSeqInput());
    input->clientNames[kMidi1Client] = clientName;
    input->clientNames[kMidi2Client] = clientName + " MIDI 2.0";
    const int versions[kClientCount]  = {SND_SEQ_CLIENT_UMP_MIDI_1_0, SND_SEQ_CLIENT_UMP_MIDI_2_0};

    for (int c = 0; c < kClientCount; ++c) {
        if (snd_seq_open(&input->seq[c], "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
            std::fprintf(stderr, "AlsaSeqInput: cannot open the ALSA sequencer\n");
            return nullptr;
        }
        if (snd_seq_set_client_midi_version(input->seq[c], versions[c]) < 0) {
            std::fprintf(stderr, "AlsaSeqInput: this ALSA sequencer does not support UMP "
                                 "clients (kernel 6.5+ and alsa-lib 1.2.10+ needed)\n");
            return nullptr;
        }
        snd_seq_set_client_name(input->seq[c], input->clientNames[c].c_str());
        snd_seq_set_client_pool_input(input->seq[c], kInputPool);   // the default if refused
        input->clientId[c] = snd_seq_client_id(input->seq[c]);
    }

    // Hotplug: the system announces clients and ports starting and exiting on
    // its announce port; a private port of ours hears them (changes()).
    if (const int port = input->createPort(kMidi1Client, "announcements", /*exported=*/false);
        port < 0 || snd_seq_connect_from(input->seq[kMidi1Client], port, SND_SEQ_CLIENT_SYSTEM,
                                         SND_SEQ_PORT_SYSTEM_ANNOUNCE) < 0)
        std::fprintf(stderr, "AlsaSeqInput: can't follow MIDI devices coming and going; restart to see new ones\n");

    input->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (input->wakeFd < 0) {
        std::fprintf(stderr, "AlsaSeqInput: eventfd failed\n");
        return nullptr;
    }

    input->thread = std::thread(&AlsaSeqInput::run, input.get());
    sched_param param{};
    param.sched_priority = kMidiThreadPriority;
    input->rtPriority =
        pthread_setschedparam(input->thread.native_handle(), SCHED_FIFO, &param) == 0;
    return input;
}

AlsaSeqInput::~AlsaSeqInput() {
    if (thread.joinable()) {
        quit.store(true);
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t n = ::write(wakeFd, &one, sizeof one);
        thread.join();
    }
    for (snd_seq_t* s : seq)
        if (s != nullptr) snd_seq_close(s);   // also removes our ports and subscriptions
    if (wakeFd >= 0) ::close(wakeFd);
}

// --- MidiInput -------------------------------------------------------------------

void AlsaSeqInput::setProtocolPreference(MidiProtocol pref) {
    preference = pref;
}

void AlsaSeqInput::setSink(MidiSink* midiSink) {
    sink.store(midiSink, std::memory_order_release);
}

std::vector<MidiSourceInfo> AlsaSeqInput::enumerate() {
    std::vector<MidiSourceInfo> infos;
    for (const Source& source : listSources()) infos.push_back(source.info);
    return infos;
}

MidiSourceHandle AlsaSeqInput::open(const std::string& sourceId) {
    Source source;
    if (!findSource(sourceId, source)) {
        std::fprintf(stderr, "AlsaSeqInput: no MIDI source '%s'\n", sourceId.c_str());
        return kInvalidMidiSource;
    }
    const int c    = clientFor(source.info.protocol);
    const int port = createPort(c, source.info.name, /*exported=*/false);
    if (port < 0) return kInvalidMidiSource;

    if (snd_seq_connect_from(seq[c], port, source.client, source.port) < 0) {
        std::fprintf(stderr, "AlsaSeqInput: cannot subscribe to '%s' (%d:%d)\n",
                     sourceId.c_str(), source.client, source.port);
        snd_seq_delete_simple_port(seq[c], port);
        return kInvalidMidiSource;
    }
    const MidiSourceHandle handle = registerPort(c, port, /*isVirtual=*/false);
    OpenPort&              opened = openPorts[handle];
    opened.sourceId               = sourceId;
    opened.sourceClient           = source.client;
    opened.sourcePort             = source.port;
    return handle;
}

bool AlsaSeqInput::sync(MidiSourceHandle source) {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return false;
    OpenPort& open = it->second;
    if (open.isVirtual) return true;
    snd_seq_t* const s = seq[open.clientIndex];

    // Still subscribed? A device that leaves takes its subscriptions with it.
    snd_seq_port_subscribe_t* subscription = nullptr;
    snd_seq_port_subscribe_alloca(&subscription);
    snd_seq_addr_t sender{}, dest{};
    sender.client = static_cast<unsigned char>(open.sourceClient);
    sender.port   = static_cast<unsigned char>(open.sourcePort);
    dest.client   = static_cast<unsigned char>(clientId[open.clientIndex]);
    dest.port     = static_cast<unsigned char>(open.port);
    snd_seq_port_subscribe_set_sender(subscription, &sender);
    snd_seq_port_subscribe_set_dest(subscription, &dest);
    if (open.sourceClient >= 0 && snd_seq_get_port_subscription(s, subscription) == 0) return true;

    // Gone: back under the same id (usually at a new address)? Then our port
    // subscribes to it again, and the handle hears it as before.
    open.sourceClient = open.sourcePort = -1;
    Source found;
    if (!findSource(open.sourceId, found)) return false;
    if (snd_seq_connect_from(s, open.port, found.client, found.port) < 0) return false;
    open.sourceClient = found.client;
    open.sourcePort   = found.port;
    return true;
}

MidiSourceHandle AlsaSeqInput::openVirtual(const std::string& portName, MidiProtocol protocol) {
    const int c    = clientFor(protocol);
    const int port = createPort(c, portName, /*exported=*/true);
    if (port < 0) return kInvalidMidiSource;
    return registerPort(c, port, /*isVirtual=*/true);
}

void AlsaSeqInput::close(MidiSourceHandle source) {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return;
    const OpenPort& open = it->second;
    portHandles[open.clientIndex][open.port].store(kInvalidMidiSource, std::memory_order_release);
    snd_seq_delete_simple_port(seq[open.clientIndex], open.port);   // drops its subscriptions
    openPorts.erase(it);
}

std::string AlsaSeqInput::describe(MidiSourceHandle source) const {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return "(not open)";
    const OpenPort& open = it->second;
    const int versions[kClientCount] = {SND_SEQ_CLIENT_UMP_MIDI_1_0, SND_SEQ_CLIENT_UMP_MIDI_2_0};
    return std::to_string(clientId[open.clientIndex]) + ":" + std::to_string(open.port) +
           " on '" + clientNames[open.clientIndex] + "' (" +
           midiVersionName(versions[open.clientIndex]) + ")";
}

// --- helpers (main thread) -----------------------------------------------------------

// Readable, subscribable ports of every other client. Main thread. (Sequencer
// queries are ioctls; they don't touch the input buffer the MIDI thread reads.)
std::vector<AlsaSeqInput::Source> AlsaSeqInput::listSources() {
    std::vector<Source> sources;
    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t*   portInfo   = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);

    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(seq[kMidi1Client], clientInfo) >= 0) {
        const int client = snd_seq_client_info_get_client(clientInfo);
        if (client == SND_SEQ_CLIENT_SYSTEM || client == clientId[kMidi1Client] ||
            client == clientId[kMidi2Client])
            continue;

        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(seq[kMidi1Client], portInfo) >= 0) {
            const unsigned caps     = snd_seq_port_info_get_capability(portInfo);
            const unsigned readable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((caps & readable) != readable || (caps & SND_SEQ_PORT_CAP_NO_EXPORT) != 0)
                continue;

            Source source;
            source.client         = client;
            source.port           = snd_seq_port_info_get_port(portInfo);
            source.info.name      = snd_seq_port_info_get_name(portInfo);
            source.info.id        = std::string(snd_seq_client_info_get_name(clientInfo)) +
                                    ":" + source.info.name;
            source.info.isVirtual = snd_seq_client_info_get_type(clientInfo) == SND_SEQ_USER_CLIENT;
            source.info.isHardware =
                (snd_seq_port_info_get_type(portInfo) & SND_SEQ_PORT_TYPE_HARDWARE) != 0;
            source.info.protocol  = snd_seq_client_info_get_midi_version(clientInfo) ==
                                            SND_SEQ_CLIENT_UMP_MIDI_2_0
                                        ? MidiProtocol::Midi2
                                        : MidiProtocol::Midi1;
            sources.push_back(source);
        }
    }
    return sources;
}

// By id ("client name:port name") or numeric address ("14:0").
bool AlsaSeqInput::findSource(const std::string& sourceId, Source& found) {
    int client = -1, port = -1;
    const bool numeric = parseAddress(sourceId, client, port);
    for (const Source& source : listSources()) {
        if (numeric ? (source.client == client && source.port == port)
                    : source.info.id == sourceId) {
            found = source;
            return true;
        }
    }
    return false;
}

int AlsaSeqInput::clientFor(MidiProtocol protocol) const {
    return (preference == MidiProtocol::Midi2 && protocol == MidiProtocol::Midi2) ? kMidi2Client
                                                                                  : kMidi1Client;
}

// `exported` ports (virtual inputs) accept connections from other apps; ports
// for an opened source are private (we subscribe them ourselves).
int AlsaSeqInput::createPort(int clientIndex, const std::string& portName, bool exported) {
    const unsigned caps = SND_SEQ_PORT_CAP_WRITE |
                          (exported ? SND_SEQ_PORT_CAP_SUBS_WRITE : SND_SEQ_PORT_CAP_NO_EXPORT);
    const int port = snd_seq_create_simple_port(
        seq[clientIndex], portName.c_str(), caps,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        std::fprintf(stderr, "AlsaSeqInput: cannot create port '%s'\n", portName.c_str());
        return -1;
    }
    if (port >= kMaxPorts) {
        std::fprintf(stderr, "AlsaSeqInput: port number %d out of range\n", port);
        snd_seq_delete_simple_port(seq[clientIndex], port);
        return -1;
    }
    return port;
}

MidiSourceHandle AlsaSeqInput::registerPort(int clientIndex, int port, bool isVirtual) {
    const MidiSourceHandle handle = nextHandle++;
    OpenPort& open   = openPorts[handle];
    open.clientIndex = clientIndex;
    open.port        = port;
    open.isVirtual   = isVirtual;
    portHandles[clientIndex][port].store(handle, std::memory_order_release);
    return handle;
}

// --- MIDI thread ---------------------------------------------------------------------

void AlsaSeqInput::run() {
    std::vector<pollfd> fds;
    for (snd_seq_t* s : seq) {
        const int count = snd_seq_poll_descriptors_count(s, POLLIN);
        const std::size_t first = fds.size();
        fds.resize(first + static_cast<std::size_t>(count));
        snd_seq_poll_descriptors(s, fds.data() + first, static_cast<unsigned>(count), POLLIN);
    }
    fds.push_back(pollfd{wakeFd, POLLIN, 0});

    while (!quit.load(std::memory_order_relaxed)) {
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "AlsaSeqInput: poll failed\n");
            break;
        }
        for (int c = 0; c < kClientCount; ++c) drain(c);
    }
}

void AlsaSeqInput::drain(int clientIndex) {
    snd_seq_ump_event_t* event = nullptr;
    for (;;) {
        const int result = snd_seq_ump_event_input(seq[clientIndex], &event);
        if (result == -EAGAIN) return;             // nothing more queued
        if (result == -ENOSPC) {                   // kernel input buffer overran
            overruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (result < 0) return;

        const Nanoseconds captured = nowNanoseconds();
        if (!snd_seq_ev_is_ump(event)) {   // non-MIDI sequencer events: hotplug announcements counted
            switch (event->type) {
                case SND_SEQ_EVENT_CLIENT_START:
                case SND_SEQ_EVENT_CLIENT_EXIT:
                case SND_SEQ_EVENT_PORT_START:
                case SND_SEQ_EVENT_PORT_EXIT:
                case SND_SEQ_EVENT_PORT_CHANGE:
                case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
                    announced.fetch_add(1, std::memory_order_release);
                    break;
                default: break;
            }
            continue;
        }

        const int port = event->dest.port;
        if (port < 0 || port >= kMaxPorts) continue;
        const MidiSourceHandle handle =
            portHandles[clientIndex][port].load(std::memory_order_acquire);
        MidiSink* const target = sink.load(std::memory_order_acquire);
        if (handle == kInvalidMidiSource || target == nullptr) continue;

        TimedMidiMessage message;
        message.captureTime       = captured;
        message.message.wordCount = ump::wordCountForType(static_cast<std::uint8_t>(event->ump[0] >> 28));
        for (std::uint8_t i = 0; i < message.message.wordCount; ++i)
            message.message.words[i] = event->ump[i];
        target->onMidi(handle, message);
    }
}

} // namespace uw
