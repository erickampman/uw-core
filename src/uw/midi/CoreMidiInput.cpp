#include "uw/midi/CoreMidiInput.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <cstdio>
#include <future>

#include "uw/base/Clock.h"
#include "uw/ump/Ump.h"

namespace uw {

namespace {

// How often the notification thread looks at `quit` if CFRunLoopStop() came
// before it started running (the stop is then lost).
constexpr CFTimeInterval kNotifyLoopSlice = 0.25;

std::string stringProperty(MIDIObjectRef object, CFStringRef property) {
    CFStringRef value = nullptr;
    if (object == 0 || MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr)
        return {};
    std::string text;
    if (const char* fast = CFStringGetCStringPtr(value, kCFStringEncodingUTF8)) {
        text = fast;
    } else {
        const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
        std::vector<char> buffer(static_cast<std::size_t>(size));
        if (CFStringGetCString(value, buffer.data(), size, kCFStringEncodingUTF8)) text = buffer.data();
    }
    CFRelease(value);
    return text;
}

std::int32_t integerProperty(MIDIObjectRef object, CFStringRef property, std::int32_t fallback) {
    SInt32 value = 0;
    return MIDIObjectGetIntegerProperty(object, property, &value) == noErr ? value : fallback;
}

CFStringRef makeCFString(const std::string& text) {
    return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

const char* protocolName(int portIndex) {
    return portIndex == 1 ? "MIDI 2.0" : "MIDI 1.0";
}

// Hardware is a device on a driver that talks to one: USB, Bluetooth, or a
// third party's. The IAC bus and network sessions are drivers too, but software.
bool isHardwareDriver(const std::string& driver) {
    return driver != "com.apple.AppleMIDIIACDriver" && driver != "com.apple.AppleMIDINetworkDriver";
}

} // namespace

// --- lifecycle -------------------------------------------------------------------

std::unique_ptr<CoreMidiInput> CoreMidiInput::create(const std::string& clientName) {
    std::unique_ptr<CoreMidiInput> input(new CoreMidiInput());
    CoreMidiInput* const           self = input.get();

    // The client, made on the notification thread so its notifications come on
    // that thread's run loop.
    std::promise<OSStatus> made;
    std::future<OSStatus>  status = made.get_future();
    input->notifyThread = std::thread([self, clientName, &made] {
        const CFRunLoopRef loop = CFRunLoopGetCurrent();
        self->notifyLoop.store(loop);

        const CFStringRef name   = makeCFString(clientName);
        const OSStatus    result = MIDIClientCreateWithBlock(name, &self->client, ^(const MIDINotification* note) {
            switch (note->messageID) {
                case kMIDIMsgSetupChanged:
                case kMIDIMsgObjectAdded:
                case kMIDIMsgObjectRemoved:
                case kMIDIMsgPropertyChanged:   // kMIDIPropertyOffline among them
                    self->announced.fetch_add(1, std::memory_order_release);
                    break;
                default: break;
            }
        });
        CFRelease(name);

        // A run loop with no sources returns at once; a timer that never fires
        // keeps this one waiting.
        CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreate(
            kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1e10, 1e10, 0, 0,
            [](CFRunLoopTimerRef, void*) {}, nullptr);
        CFRunLoopAddTimer(loop, keepAlive, kCFRunLoopDefaultMode);
        made.set_value(result);   // `made` isn't touched after this

        if (result == noErr)
            while (!self->quit.load())
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, kNotifyLoopSlice, false);

        CFRunLoopRemoveTimer(loop, keepAlive, kCFRunLoopDefaultMode);
        CFRelease(keepAlive);
    });
    if (const OSStatus result = status.get(); result != noErr) {
        std::fprintf(stderr, "CoreMidiInput: can't reach CoreMIDI (error %d)\n", static_cast<int>(result));
        input->quit.store(true);
        input->notifyThread.join();
        return nullptr;
    }

    // The two input ports, one per protocol (CoreMIDI fixes it per port).
    const MIDIProtocolID protocols[kPortCount] = {kMIDIProtocol_1_0, kMIDIProtocol_2_0};
    input->portNames[kMidi1Port]               = clientName;
    input->portNames[kMidi2Port]               = clientName + " MIDI 2.0";
    for (int p = 0; p < kPortCount; ++p) {
        const CFStringRef name   = makeCFString(input->portNames[p]);
        const OSStatus    result = MIDIInputPortCreateWithProtocol(
            input->client, name, protocols[p], &input->ports[p], ^(const MIDIEventList* list, void* refCon) {
                self->receive(list, static_cast<Receiver*>(refCon));
            });
        CFRelease(name);
        if (result != noErr) {
            std::fprintf(stderr, "CoreMidiInput: can't make the %s input port (error %d)\n",
                         protocolName(p), static_cast<int>(result));
            return nullptr;   // the destructor stops the thread
        }
    }
    return input;
}

CoreMidiInput::~CoreMidiInput() {
    while (!openPorts.empty()) close(openPorts.begin()->first);
    for (MIDIPortRef port : ports)
        if (port != 0) MIDIPortDispose(port);
    // The client itself is left to the system: disposing a process's last client
    // can make the MIDI server quit under it, and a later client then fails.
    // With the thread gone, nothing runs its notifications.
    if (notifyThread.joinable()) {
        quit.store(true);
        if (const CFRunLoopRef loop = notifyLoop.load()) CFRunLoopStop(loop);
        notifyThread.join();
    }
}

// --- MidiInput -------------------------------------------------------------------

void CoreMidiInput::setProtocolPreference(MidiProtocol pref) {
    preference = pref;
}

void CoreMidiInput::setSink(MidiSink* midiSink) {
    sink.store(midiSink, std::memory_order_release);
}

std::vector<MidiSourceInfo> CoreMidiInput::enumerate() {
    std::vector<MidiSourceInfo> infos;
    for (const Source& source : listSources()) infos.push_back(source.info);
    return infos;
}

MidiSourceHandle CoreMidiInput::open(const std::string& sourceId) {
    Source source;
    if (!findSource(sourceId, 0, source)) {
        std::fprintf(stderr, "CoreMidiInput: no MIDI source '%s'\n", sourceId.c_str());
        return kInvalidMidiSource;
    }
    OpenPort open;
    open.portIndex = portFor(source.info.protocol);
    open.sourceId  = sourceId;
    open.uniqueId  = source.uniqueId;
    open.portName  = portNames[open.portIndex];
    const MidiSourceHandle handle = registerPort(open);
    OpenPort&              opened = openPorts[handle];
    if (MIDIPortConnectSource(ports[opened.portIndex], source.endpoint, opened.receiver) != noErr) {
        std::fprintf(stderr, "CoreMidiInput: can't connect to '%s'\n", sourceId.c_str());
        close(handle);
        return kInvalidMidiSource;
    }
    opened.connected = source.endpoint;
    return handle;
}

bool CoreMidiInput::sync(MidiSourceHandle source) {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return false;
    OpenPort& open = it->second;
    if (open.isVirtual) return true;

    // Listed (online) under its unique id, or else its id? Then it's here. A
    // device that went offline and came back is reconnected: its connection may
    // not have survived.
    Source     found;
    const bool present = findSource(open.sourceId, open.uniqueId, found);
    if (present && found.endpoint == open.connected) return true;
    if (open.connected != 0) {
        MIDIPortDisconnectSource(ports[open.portIndex], open.connected);
        open.connected = 0;
    }
    if (!present) return false;
    if (MIDIPortConnectSource(ports[open.portIndex], found.endpoint, open.receiver) != noErr) return false;
    open.connected = found.endpoint;
    open.uniqueId  = found.uniqueId;
    return true;
}

MidiSourceHandle CoreMidiInput::openVirtual(const std::string& portName, MidiProtocol protocol) {
    OpenPort open;
    open.portIndex = portFor(protocol);
    open.isVirtual = true;
    open.portName  = portName;
    const MidiSourceHandle handle   = registerPort(open);
    OpenPort&              opened   = openPorts[handle];
    Receiver* const        receiver = opened.receiver;

    // Other apps send to it; CoreMIDI converts an app of the other protocol.
    const MIDIProtocolID protocolId = opened.portIndex == kMidi2Port ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0;
    const CFStringRef    name       = makeCFString(portName);
    const OSStatus       result     = MIDIDestinationCreateWithProtocol(
        client, name, protocolId, &opened.destination,
        ^(const MIDIEventList* list, void*) { receive(list, receiver); });
    CFRelease(name);
    if (result != noErr) {
        std::fprintf(stderr, "CoreMidiInput: can't make the virtual input '%s' (error %d)\n", portName.c_str(),
                     static_cast<int>(result));
        close(handle);
        return kInvalidMidiSource;
    }
    return handle;
}

void CoreMidiInput::close(MidiSourceHandle source) {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return;
    const OpenPort& open = it->second;
    open.receiver->handle.store(kInvalidMidiSource, std::memory_order_release);
    if (open.connected != 0) MIDIPortDisconnectSource(ports[open.portIndex], open.connected);
    if (open.destination != 0) MIDIEndpointDispose(open.destination);
    openPorts.erase(it);
}

std::string CoreMidiInput::describe(MidiSourceHandle source) const {
    const auto it = openPorts.find(source);
    if (it == openPorts.end()) return "(not open)";
    const OpenPort& open = it->second;
    if (open.isVirtual)
        return "destination '" + open.portName + "' (" + protocolName(open.portIndex) + ")";
    return "port '" + open.portName + "' (" + protocolName(open.portIndex) + ")" +
           (open.connected == 0 ? ", not connected" : "");
}

// --- helpers (main thread) -----------------------------------------------------------

// Every online source of every other client: devices, network sessions, the IAC
// bus and apps' virtual sources. (Our own virtual inputs are destinations, so
// they aren't among them.)
std::vector<CoreMidiInput::Source> CoreMidiInput::listSources() const {
    std::vector<Source> sources;
    const ItemCount     count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef endpoint = MIDIGetSource(i);
        if (endpoint == 0 || integerProperty(endpoint, kMIDIPropertyOffline, 0) != 0) continue;

        MIDIEntityRef entity = 0;
        MIDIDeviceRef device = 0;
        if (MIDIEndpointGetEntity(endpoint, &entity) != noErr) entity = 0;
        if (entity != 0 && MIDIEntityGetDevice(entity, &device) != noErr) device = 0;

        Source source;
        source.endpoint            = endpoint;
        source.uniqueId            = integerProperty(endpoint, kMIDIPropertyUniqueID, 0);
        source.info.name           = stringProperty(endpoint, kMIDIPropertyName);
        const std::string deviceName = stringProperty(device, kMIDIPropertyName);
        source.info.id             = deviceName.empty() ? source.info.name : deviceName + ":" + source.info.name;
        source.info.isVirtual      = entity == 0;
        source.info.isHardware     = device != 0 && isHardwareDriver(stringProperty(device, kMIDIPropertyDriverOwner));
        source.info.protocol       = integerProperty(endpoint, kMIDIPropertyProtocolID, kMIDIProtocol_1_0) ==
                                       kMIDIProtocol_2_0
                                         ? MidiProtocol::Midi2
                                         : MidiProtocol::Midi1;
        sources.push_back(source);
    }
    return sources;
}

// By unique id first (if given: it survives a rename), then by id.
bool CoreMidiInput::findSource(const std::string& sourceId, std::int32_t uniqueId, Source& found) const {
    const std::vector<Source> sources = listSources();
    if (uniqueId != 0)
        for (const Source& source : sources)
            if (source.uniqueId == uniqueId) {
                found = source;
                return true;
            }
    for (const Source& source : sources)
        if (source.info.id == sourceId) {
            found = source;
            return true;
        }
    return false;
}

int CoreMidiInput::portFor(MidiProtocol protocol) const {
    return (preference == MidiProtocol::Midi2 && protocol == MidiProtocol::Midi2) ? kMidi2Port : kMidi1Port;
}

CoreMidiInput::Receiver* CoreMidiInput::newReceiver() {
    receivers.push_back(std::make_unique<Receiver>());
    return receivers.back().get();
}

MidiSourceHandle CoreMidiInput::registerPort(OpenPort open) {
    const MidiSourceHandle handle = nextHandle++;
    open.receiver                 = newReceiver();
    open.receiver->handle.store(handle, std::memory_order_release);
    openPorts[handle] = std::move(open);
    return handle;
}

// --- CoreMIDI's thread -----------------------------------------------------------------

void CoreMidiInput::receive(const void* eventList, Receiver* receiver) noexcept {
    if (receiver == nullptr) return;
    const MidiSourceHandle handle = receiver->handle.load(std::memory_order_acquire);
    MidiSink* const        target = sink.load(std::memory_order_acquire);
    if (handle == kInvalidMidiSource || target == nullptr) return;

    // Stamped as it reaches us, not with the packet's own host time: that's
    // when the driver got the bytes, earlier than now by CoreMIDI's delivery,
    // and a block that starts in between would miss the message and find it
    // more than a period old ("late MIDI"; found at 64 frames with a USB
    // keyboard). The engine's contract is capture time <= queue time, as
    // ALSA's read-time stamps give (StageHand's docs/macos.md, milestone 5 notes).
    const Nanoseconds      captured = nowNanoseconds();
    const auto*            list     = static_cast<const MIDIEventList*>(eventList);
    const MIDIEventPacket* packet   = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p, packet = MIDIEventPacketNext(packet)) {
        // A packet may hold several UMP messages, all at its time.
        for (UInt32 i = 0; i < packet->wordCount;) {
            const std::uint8_t words = ump::wordCountForType(static_cast<std::uint8_t>(packet->words[i] >> 28));
            if (words == 0 || i + words > packet->wordCount) break;
            TimedMidiMessage message;
            message.captureTime       = captured;
            message.message.wordCount = words;
            for (std::uint8_t w = 0; w < words; ++w) message.message.words[w] = packet->words[i + w];
            target->onMidi(handle, message);
            i += words;
        }
    }
}

} // namespace uw
