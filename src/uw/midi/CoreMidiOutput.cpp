#include "uw/midi/CoreMidiOutput.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <cstdio>
#include <future>

#include "uw/ump/Ump.h"

namespace uw {

namespace {

// As CoreMidiInput's: how often the notification thread looks at `quit`.
constexpr CFTimeInterval kNotifyLoopSlice = 0.25;

// Room for one event list: plenty for a burst of channel voice messages
// (64 bytes of header and packet overhead each way).
constexpr std::size_t kListBytes = 1024;

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

bool isHardwareDriver(const std::string& driver) {
    return driver != "com.apple.AppleMIDIIACDriver" && driver != "com.apple.AppleMIDINetworkDriver";
}

const char* protocolName(MidiProtocol protocol) {
    return protocol == MidiProtocol::Midi2 ? "MIDI 2.0" : "MIDI 1.0";
}

MIDIProtocolID protocolId(MidiProtocol protocol) {
    return protocol == MidiProtocol::Midi2 ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0;
}

} // namespace

// --- lifecycle -------------------------------------------------------------------

std::unique_ptr<CoreMidiOutput> CoreMidiOutput::create(const std::string& clientName) {
    std::unique_ptr<CoreMidiOutput> output(new CoreMidiOutput());
    CoreMidiOutput* const           self = output.get();

    // The client, made on the notification thread so its notifications come on
    // that thread's run loop (as CoreMidiInput::create).
    std::promise<OSStatus> made;
    std::future<OSStatus>  status = made.get_future();
    output->notifyThread = std::thread([self, clientName, &made] {
        const CFRunLoopRef loop = CFRunLoopGetCurrent();
        self->notifyLoop.store(loop);

        const CFStringRef name   = makeCFString(clientName);
        const OSStatus    result = MIDIClientCreateWithBlock(name, &self->client, ^(const MIDINotification* note) {
            switch (note->messageID) {
                case kMIDIMsgSetupChanged:
                case kMIDIMsgObjectAdded:
                case kMIDIMsgObjectRemoved:
                case kMIDIMsgPropertyChanged:
                    self->announced.fetch_add(1, std::memory_order_release);
                    break;
                default: break;
            }
        });
        CFRelease(name);

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
        std::fprintf(stderr, "CoreMidiOutput: can't reach CoreMIDI (error %d)\n", static_cast<int>(result));
        output->quit.store(true);
        output->notifyThread.join();
        return nullptr;
    }

    const CFStringRef name   = makeCFString(clientName);
    const OSStatus    result = MIDIOutputPortCreate(output->client, name, &output->port);
    CFRelease(name);
    if (result != noErr) {
        std::fprintf(stderr, "CoreMidiOutput: can't make the output port (error %d)\n", static_cast<int>(result));
        return nullptr;   // the destructor stops the thread
    }
    return output;
}

CoreMidiOutput::~CoreMidiOutput() {
    while (!opened.empty()) close(opened.begin()->first);
    if (port != 0) MIDIPortDispose(port);
    // The client is left to the system, as CoreMidiInput's.
    if (notifyThread.joinable()) {
        quit.store(true);
        if (const CFRunLoopRef loop = notifyLoop.load()) CFRunLoopStop(loop);
        notifyThread.join();
    }
}

// --- MidiOutput -------------------------------------------------------------------

std::vector<MidiDestinationInfo> CoreMidiOutput::enumerate() {
    std::vector<MidiDestinationInfo> infos;
    for (const Destination& d : listDestinations()) infos.push_back(d.info);
    return infos;
}

MidiDestinationHandle CoreMidiOutput::open(const std::string& destinationId) {
    Destination found;
    if (!findDestination(destinationId, 0, found)) {
        std::fprintf(stderr, "CoreMidiOutput: no MIDI destination '%s'\n", destinationId.c_str());
        return kInvalidMidiDestination;
    }
    Opened o;
    o.id       = destinationId;
    o.uniqueId = found.uniqueId;
    o.endpoint = found.endpoint;
    o.protocol = found.info.protocol;
    const MidiDestinationHandle handle = nextHandle++;
    opened[handle] = o;
    return handle;
}

MidiDestinationHandle CoreMidiOutput::openVirtual(const std::string& sourceName, MidiProtocol protocol,
                                                  std::int32_t persistentId) {
    Opened o;
    o.isVirtual = true;
    o.id        = sourceName;
    o.protocol  = protocol;
    const CFStringRef name   = makeCFString(sourceName);
    const OSStatus    result = MIDISourceCreateWithProtocol(client, name, protocolId(protocol), &o.endpoint);
    CFRelease(name);
    if (result != noErr) {
        std::fprintf(stderr, "CoreMidiOutput: can't make the virtual source '%s' (error %d)\n",
                     sourceName.c_str(), static_cast<int>(result));
        return kInvalidMidiDestination;
    }
    // CoreMIDI refuses an id another endpoint has (kMIDIIDNotUnique); then the
    // one it chose stays.
    if (persistentId != 0) MIDIObjectSetIntegerProperty(o.endpoint, kMIDIPropertyUniqueID, persistentId);
    const MidiDestinationHandle handle = nextHandle++;
    opened[handle] = o;
    return handle;
}

void CoreMidiOutput::close(MidiDestinationHandle destination) {
    const auto it = opened.find(destination);
    if (it == opened.end()) return;
    if (it->second.isVirtual && it->second.endpoint != 0) MIDIEndpointDispose(it->second.endpoint);
    opened.erase(it);
}

MidiProtocol CoreMidiOutput::protocol(MidiDestinationHandle destination) const {
    const auto it = opened.find(destination);
    return it == opened.end() ? MidiProtocol::Midi1 : it->second.protocol;
}

bool CoreMidiOutput::send(MidiDestinationHandle destination, const MidiMessage* messages, std::size_t count) {
    const auto it = opened.find(destination);
    if (it == opened.end() || it->second.endpoint == 0) return false;
    const Opened& o = it->second;

    // Each run of one protocol goes in an event list of that protocol; neutral
    // messages (utility, system, SysEx7) join the current run.
    alignas(MIDIEventList) Byte buffer[kListBytes];
    auto* const       list   = reinterpret_cast<MIDIEventList*>(buffer);
    MIDIEventPacket*  packet = nullptr;
    MidiProtocol      listProtocol = o.protocol;
    bool              ok = true;

    auto flush = [&] {
        if (packet == nullptr) return;
        const OSStatus result = o.isVirtual ? MIDIReceivedEventList(o.endpoint, list)
                                            : MIDISendEventList(port, o.endpoint, list);
        ok = ok && result == noErr;
        packet = nullptr;
    };
    auto start = [&](MidiProtocol p) {
        listProtocol = p;
        packet = MIDIEventListInit(list, protocolId(p));
    };

    for (std::size_t i = 0; i < count; ++i) {
        const MidiMessage& m = messages[i];
        if (m.wordCount == 0 || m.wordCount > 4) continue;
        const MidiProtocol p = ump::isProtocolNeutral(m) ? (packet ? listProtocol : o.protocol) : ump::protocolOf(m);
        if (packet != nullptr && p != listProtocol) flush();
        if (packet == nullptr) start(p);
        MIDIEventPacket* next = MIDIEventListAdd(list, kListBytes, packet, 0, m.wordCount, m.words);
        if (next == nullptr) {   // full: send what's there and start again
            flush();
            start(p);
            next = MIDIEventListAdd(list, kListBytes, packet, 0, m.wordCount, m.words);
            if (next == nullptr) return false;
        }
        packet = next;
    }
    flush();
    return ok;
}

bool CoreMidiOutput::sync(MidiDestinationHandle destination) {
    const auto it = opened.find(destination);
    if (it == opened.end()) return false;
    Opened& o = it->second;
    if (o.isVirtual) return true;
    Destination found;
    if (!findDestination(o.id, o.uniqueId, found)) {
        o.endpoint = 0;
        return false;
    }
    o.endpoint = found.endpoint;
    o.uniqueId = found.uniqueId;
    o.protocol = found.info.protocol;
    return true;
}

std::string CoreMidiOutput::describe(MidiDestinationHandle destination) const {
    const auto it = opened.find(destination);
    if (it == opened.end()) return "(not open)";
    const Opened& o = it->second;
    const std::string kind = o.isVirtual ? "virtual source '" : "destination '";
    return kind + o.id + "' (" + protocolName(o.protocol) + ")" + (o.endpoint == 0 ? ", gone" : "");
}

// --- helpers ------------------------------------------------------------------------

// Every online destination: devices, network sessions, the IAC bus and apps'
// virtual destinations.
std::vector<CoreMidiOutput::Destination> CoreMidiOutput::listDestinations() const {
    std::vector<Destination> destinations;
    const ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; ++i) {
        const MIDIEndpointRef endpoint = MIDIGetDestination(i);
        if (endpoint == 0 || integerProperty(endpoint, kMIDIPropertyOffline, 0) != 0) continue;

        MIDIEntityRef entity = 0;
        MIDIDeviceRef device = 0;
        if (MIDIEndpointGetEntity(endpoint, &entity) != noErr) entity = 0;
        if (entity != 0 && MIDIEntityGetDevice(entity, &device) != noErr) device = 0;

        Destination d;
        d.endpoint                   = endpoint;
        d.uniqueId                   = integerProperty(endpoint, kMIDIPropertyUniqueID, 0);
        d.info.name                  = stringProperty(endpoint, kMIDIPropertyName);
        const std::string deviceName = stringProperty(device, kMIDIPropertyName);
        d.info.id                    = deviceName.empty() ? d.info.name : deviceName + ":" + d.info.name;
        d.info.isVirtual             = entity == 0;
        d.info.isHardware            = device != 0 && isHardwareDriver(stringProperty(device, kMIDIPropertyDriverOwner));
        d.info.protocol = integerProperty(endpoint, kMIDIPropertyProtocolID, kMIDIProtocol_1_0) == kMIDIProtocol_2_0
                              ? MidiProtocol::Midi2
                              : MidiProtocol::Midi1;
        destinations.push_back(d);
    }
    return destinations;
}

// By unique id first (if given), then by id, as CoreMidiInput::findSource.
bool CoreMidiOutput::findDestination(const std::string& id, std::int32_t uniqueId, Destination& found) const {
    const std::vector<Destination> destinations = listDestinations();
    if (uniqueId != 0)
        for (const Destination& d : destinations)
            if (d.uniqueId == uniqueId) {
                found = d;
                return true;
            }
    for (const Destination& d : destinations)
        if (d.info.id == id) {
            found = d;
            return true;
        }
    return false;
}

} // namespace uw
