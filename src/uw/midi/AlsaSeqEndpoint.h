#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "uw/ump/MidiMessage.h"
#include "uw/ump/UmpStream.h"

typedef struct _snd_seq snd_seq_t;

namespace uw {

// An ALSA sequencer UMP endpoint of our own, for AlsaSeqOutput::openEndpoint:
// a sequencer client of its own, named after the endpoint, as the kernel
// keeps one endpoint per client and wants its port 0 (AlsaSeqOutput's clients
// have ports already). The kernel holds the endpoint and function block info
// for other clients to read (snd_seq_get_ump_endpoint_info, aconnect -l,
// /proc/asound/seq/clients), and the ports are the ones alsa-lib's
// snd_seq_create_ump_endpoint() makes:
//   port 0       "MIDI 2.0", the endpoint: everything sent goes out here, and
//                the kernel passes each message on to its group's port too.
//   ports 1-16   "Group N (<block names>)", one per group, subscribable in the
//                directions of the active blocks that hold it; hidden else.
// The info and ports are set here rather than with alsa-lib's helpers, which
// can't change a block once made: setFunctionBlocks() changes them in place,
// so the client, and other apps' subscriptions to it, stay.
//
// A thread of its own answers a peer's discovery sent to port 0
// (ump::answerDiscovery, to port 0's subscribers) and counts, in inquiries,
// those and each new subscription to the endpoint's ports: on Linux a receiver
// that starts listening subscribes, and usually never asks.
class AlsaSeqEndpoint {
public:
    // Null (after printing why) if the sequencer refuses.
    static std::unique_ptr<AlsaSeqEndpoint> create(const ump::EndpointInfo& info, MidiProtocol protocol);
    ~AlsaSeqEndpoint();

    AlsaSeqEndpoint(const AlsaSeqEndpoint&)            = delete;
    AlsaSeqEndpoint& operator=(const AlsaSeqEndpoint&) = delete;

    // The client to send from, from port() to its subscribers. Main thread; the
    // endpoint's thread writes its answers directly, never through the buffer.
    snd_seq_t* sequencer() const { return seq_; }
    int        client() const { return client_; }
    static constexpr int port() { return 0; }

    // Replaces the function blocks, in place. Appends to `notify` what to send
    // the endpoint's listeners about it: Endpoint Info and every block's Info
    // and Name, as the spec asks of an endpoint whose blocks change. False if
    // the kernel refused.
    bool setFunctionBlocks(const std::vector<ump::FunctionBlock>& blocks, std::vector<MidiMessage>& notify);

    std::uint64_t inquiries() const { return asked_.load(std::memory_order_acquire); }

private:
    AlsaSeqEndpoint() = default;

    bool declare();   // the kernel's endpoint and block info and the group ports, from info_
    void run();       // the endpoint's thread
    void answer(const MidiMessage& m);

    snd_seq_t*   seq_      = nullptr;
    int          client_   = -1;
    int          announce_ = -1;   // a private port that hears the system's subscriptions
    MidiProtocol protocol_ = MidiProtocol::Midi2;

    mutable std::mutex mutex_;   // info_: written on the main thread, answered from on the endpoint's
    ump::EndpointInfo  info_;

    std::atomic<std::uint64_t> asked_{0};
    std::thread                thread_;
    std::atomic<bool>          quit_{false};
    int                        wakeFd_ = -1;
};

} // namespace uw
