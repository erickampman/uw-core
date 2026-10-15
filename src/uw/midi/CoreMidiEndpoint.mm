#include "uw/midi/CoreMidiEndpoint.h"

#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include <cstdio>
#include <vector>

namespace uw {

namespace {

NSString* string(const std::string& text) {
    NSString* const made = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return made != nil ? made : @"";
}

void report(const char* what, NSError* error) {
    std::fprintf(stderr, "CoreMidiEndpoint: %s (%s)\n", what,
                 error ? error.localizedDescription.UTF8String : "no reason given");
}

// Sends `messages` from `source` to whoever listens to it.
void sendFrom(MIDIEndpointRef source, MIDIProtocolID protocol, const std::vector<MidiMessage>& messages) {
    Byte buffer[1024];
    auto* const      list   = reinterpret_cast<MIDIEventList*>(buffer);
    MIDIEventPacket* packet = MIDIEventListInit(list, protocol);
    for (const MidiMessage& m : messages) {
        packet = MIDIEventListAdd(list, sizeof buffer, packet, 0, m.wordCount, m.words);
        if (packet == nullptr) {   // full: send what's there and start again
            MIDIReceivedEventList(source, list);
            packet = MIDIEventListInit(list, protocol);
            packet = MIDIEventListAdd(list, sizeof buffer, packet, 0, m.wordCount, m.words);
            if (packet == nullptr) return;
        }
    }
    MIDIReceivedEventList(source, list);
}

} // namespace

std::unique_ptr<CoreMidiEndpoint> CoreMidiEndpoint::create(const ump::EndpointInfo& info, MidiProtocol protocol,
                                                           std::shared_ptr<std::atomic<std::uint64_t>> inquiries) {
    if (@available(macOS 15.0, *)) {
        @autoreleasepool {
            const MIDIProtocolID protocolId = protocol == MidiProtocol::Midi2 ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0;

            MIDI2DeviceManufacturer  manufacturer = {{info.manufacturer[0], info.manufacturer[1], info.manufacturer[2]}};
            MIDI2DeviceRevisionLevel revision     = {{info.revision[0], info.revision[1], info.revision[2], info.revision[3]}};
            MIDI2DeviceInfo* const   device       = [[MIDI2DeviceInfo alloc] initWithManufacturerID:manufacturer
                                                                                         family:info.family & 0x3FFF
                                                                                    modelNumber:info.model & 0x3FFF
                                                                                  revisionLevel:revision];

            // The destination's callback answers discovery from its own copy,
            // and the source it answers on is filled in once there is one.
            auto answers = std::make_shared<const ump::EndpointInfo>(info);
            auto source  = std::make_shared<std::atomic<MIDIEndpointRef>>(0);
            MIDIReceiveBlock received = ^(const MIDIEventList* list, void*) {
                const MIDIEndpointRef from = source->load();
                if (from == 0) return;
                std::vector<MidiMessage> replies;
                const MIDIEventPacket*   packet = &list->packet[0];
                for (UInt32 p = 0; p < list->numPackets; ++p, packet = MIDIEventPacketNext(packet)) {
                    for (UInt32 w = 0; w + 4 <= packet->wordCount; w += 4) {   // stream messages are 4 words
                        MidiMessage m;
                        for (int i = 0; i < 4; ++i) m.words[i] = packet->words[w + i];
                        m.wordCount = 4;
                        if (ump::answerDiscovery(m, *answers, replies)) inquiries->fetch_add(1);
                    }
                }
                if (!replies.empty()) sendFrom(from, protocolId, replies);
            };

            MIDIUMPMutableEndpoint* const endpoint =
                [[MIDIUMPMutableEndpoint alloc] initWithName:string(info.name)
                                                  deviceInfo:device
                                           productInstanceID:string(info.productInstanceId)
                                                MIDIProtocol:protocolId
                                         destinationCallback:received];
            if (endpoint == nil) {
                report("can't make the UMP endpoint", nil);
                return nullptr;
            }

            NSMutableArray<MIDIUMPMutableFunctionBlock*>* blocks = [NSMutableArray array];
            for (const ump::FunctionBlock& b : info.blocks) {
                const auto direction = static_cast<MIDIUMPFunctionBlockDirection>(b.direction);
                const auto hint      = static_cast<MIDIUMPFunctionBlockUIHint>(b.direction);   // as answerDiscovery's
                MIDIUMPMutableFunctionBlock* const block =
                    [[MIDIUMPMutableFunctionBlock alloc] initWithName:string(b.name)
                                                            direction:direction
                                                           firstGroup:b.firstGroup
                                                   totalGroupsSpanned:b.groupCount
                                                     maxSysEx8Streams:0
                                                            MIDI1Info:kMIDIUMPFunctionBlockMIDI1InfoNotMIDI1
                                                               UIHint:hint
                                                            isEnabled:b.active];
                if (block == nil) {
                    report("can't make a function block", nil);
                    return nullptr;
                }
                [blocks addObject:block];
            }
            NSError* error = nil;
            if (blocks.count > 0 && ![endpoint registerFunctionBlocks:blocks markAsStatic:info.staticBlocks error:&error]) {
                report("can't register the function blocks", error);
                return nullptr;
            }
            if (![endpoint setEnabled:YES error:&error]) {
                report("can't enable the UMP endpoint", error);
                return nullptr;
            }

            std::unique_ptr<CoreMidiEndpoint> made(new CoreMidiEndpoint());
            made->object_      = (__bridge_retained void*)endpoint;
            made->source_      = endpoint.MIDISource;
            made->destination_ = endpoint.MIDIDestination;
            source->store(endpoint.MIDISource);
            return made;
        }
    }
    std::fprintf(stderr, "CoreMidiEndpoint: UMP endpoints need macOS 15 or later\n");
    return nullptr;
}

CoreMidiEndpoint::~CoreMidiEndpoint() {
    if (object_ == nullptr) return;
    if (@available(macOS 15.0, *)) {
        @autoreleasepool {
            MIDIUMPMutableEndpoint* const endpoint = (__bridge_transfer MIDIUMPMutableEndpoint*)object_;
            [endpoint setEnabled:NO error:nil];
        }   // released here: CoreMIDI disposes of its source and destination
    }
}

} // namespace uw
