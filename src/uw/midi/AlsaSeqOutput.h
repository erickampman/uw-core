#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "uw/midi/MidiOutput.h"

typedef struct _snd_seq snd_seq_t;

namespace uw {

// MidiOutput over the ALSA sequencer in UMP mode, beside AlsaSeqInput and with
// its two clients, so a receiver always sees the protocol it is sent:
//   "<name>"           UMP MIDI 1.0 client: sends to MIDI 1.0 destinations,
//                      and publishes MIDI 1.0 sources.
//   "<name> MIDI 2.0"  UMP MIDI 2.0 client: the same for MIDI 2.0.
// A published source shows subscribers the protocol it sends, and a UMP
// destination of either protocol gets messages untouched when the caller
// matches it, as the seam asks. A legacy (pre-UMP) client, FluidSynth's for
// one, gets MT 0x2 as ordinary sequencer events, converted by the kernel.
//
// open() sends straight to the destination's address (no subscription) from
// a private port on the client of its protocol: a UMP MIDI 2.0 client's port,
// unless it's marked MIDI 1.0 (a function block's), is MIDI 2.0; anything
// else, legacy clients included, MIDI 1.0. openVirtual() makes a port others
// subscribe to on the client of the protocol asked for, and sends to its
// subscribers. ALSA has no persistent id: a published port is found by its
// client and port name, as KeyEDO's are.
//
// Ids have AlsaSeqInput's shape, "<client name>:<port name>", or a numeric
// address ("128:0").
//
// Hotplug: a private port hears the system's announcements, read when
// changes() is called; there's no thread, as everything here is main-thread.
class AlsaSeqOutput final : public MidiOutput {
public:
    // Opens both sequencer clients. Returns null (after printing why) if the
    // ALSA sequencer is unavailable or has no UMP support.
    static std::unique_ptr<AlsaSeqOutput> create(const std::string& clientName);
    ~AlsaSeqOutput() override;

    AlsaSeqOutput(const AlsaSeqOutput&)            = delete;
    AlsaSeqOutput& operator=(const AlsaSeqOutput&) = delete;

    std::vector<MidiDestinationInfo> enumerate() override;
    MidiDestinationHandle            open(const std::string& destinationId) override;
    MidiDestinationHandle            openVirtual(const std::string& name, MidiProtocol protocol,
                                                 std::int32_t persistentId = 0) override;
    void                             close(MidiDestinationHandle destination) override;
    MidiProtocol                     protocol(MidiDestinationHandle destination) const override;
    bool                             send(MidiDestinationHandle destination, const MidiMessage* messages,
                                          std::size_t count) override;
    std::uint64_t                    changes() const override;
    bool                             sync(MidiDestinationHandle destination) override;
    const char*                      name() const override { return "ALSA sequencer"; }

    // Where a handle's messages go, e.g. "destination 'FluidSynth:Synth input
    // port' at 128:0 (MIDI 1.0)", or "virtual source 129:0 on 'KeyboardEDO
    // MIDI 2.0' (MIDI 2.0)".
    std::string describe(MidiDestinationHandle destination) const;

private:
    enum { kMidi1Client = 0, kMidi2Client = 1, kClientCount = 2 };

    struct Destination {
        MidiDestinationInfo info;
        int                 client = -1;
        int                 port   = -1;
    };
    struct Opened {
        bool         isVirtual   = false;
        std::string  id;                       // what open() was given, or the virtual port's name
        MidiProtocol protocol    = MidiProtocol::Midi1;
        int          clientIndex = kMidi1Client;
        int          port        = -1;         // ours: the virtual port, or the private one we send from
        int          destClient  = -1;         // the destination now; -1 if gone
        int          destPort    = -1;
    };

    AlsaSeqOutput() = default;

    std::vector<Destination> listDestinations() const;
    bool                     findDestination(const std::string& id, Destination& found) const;
    int                      senderPort(int clientIndex);
    static int               clientFor(MidiProtocol protocol) {
        return protocol == MidiProtocol::Midi2 ? kMidi2Client : kMidi1Client;
    }

    snd_seq_t*  seq[kClientCount]      = {nullptr, nullptr};
    int         clientId[kClientCount] = {-1, -1};
    std::string clientNames[kClientCount];
    int         senders[kClientCount]  = {-1, -1};   // each client's private port for open() destinations
    bool        followsAnnouncements   = false;

    std::map<MidiDestinationHandle, Opened> opened;   // main thread only
    MidiDestinationHandle                   nextHandle = 1;

    mutable std::uint64_t announced = 0;   // announcements read so far: changes()
};

} // namespace uw
