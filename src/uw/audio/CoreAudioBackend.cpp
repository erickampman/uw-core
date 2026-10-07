#include "uw/audio/CoreAudioBackend.h"

#include <CoreAudio/CoreAudio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "uw/base/Clock.h"

namespace uw {

namespace {

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector, T& value,
                 AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const AudioObjectPropertyAddress where = address(selector, scope);
    UInt32                           size  = sizeof(T);
    return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr;
}

template <typename T>
bool setProperty(AudioObjectID object, AudioObjectPropertySelector selector, const T& value) {
    const AudioObjectPropertyAddress where = address(selector);
    return AudioObjectSetPropertyData(object, &where, 0, nullptr, sizeof(T), &value) == noErr;
}

std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    if (!getProperty(object, selector, value) || value == nullptr) return {};
    std::string text;
    const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    if (CFStringGetCString(value, buffer.data(), size, kCFStringEncodingUTF8)) text = buffer.data();
    CFRelease(value);
    return text;
}

std::uint32_t outputChannelCount(AudioObjectID device) {
    const AudioObjectPropertyAddress where = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32                           size  = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<std::byte> storage(size);
    auto* const            list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr) return 0;
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool findByUid(const std::string& uid, AudioDeviceInfo& found) {
    for (const AudioDeviceInfo& device : CoreAudioBackend::outputDevices())
        if (device.uid == uid) {
            found = device;
            return true;
        }
    return false;
}

std::string hertz(double rate) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f Hz", rate);
    return text;
}

// Property changes come on the HAL's own notification thread (see
// useHalThread()), never the IOProc's.
OSStatus propertyListener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress* addresses, void* client) {
    auto* const self = static_cast<CoreAudioBackend*>(client);
    for (UInt32 i = 0; i < count; ++i) self->onPropertyChanged(object, addresses[i].mSelector);
    return noErr;
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp* now, const AudioBufferList*, const AudioTimeStamp*,
                AudioBufferList* output, const AudioTimeStamp*, void* client) {
    static_cast<CoreAudioBackend*>(client)->render(now, output);
    return noErr;
}

// The HAL posts notifications on a run loop it's given, or its own thread if
// it's given none. None: they then come whatever loop the app runs (or none).
void useHalThread() {
    static const bool done = [] {
        CFRunLoopRef none = nullptr;
        setProperty(kAudioObjectSystemObject, kAudioHardwarePropertyRunLoop, none);
        return true;
    }();
    (void)done;
}

constexpr AudioObjectPropertySelector kWatched[] = {
    kAudioDevicePropertyBufferFrameSize, kAudioDevicePropertyNominalSampleRate,
    kAudioDeviceProcessorOverload, kAudioDevicePropertyDeviceIsAlive};

// Something else setting the rate again and again: stop fighting it.
constexpr int                  kMaxRateRestores = 3;
constexpr std::chrono::seconds kRateRestoreWindow{10};

} // namespace

CoreAudioBackend::CoreAudioBackend(std::string deviceChoice, bool fallBackToDefault)
    : choice(std::move(deviceChoice)), fallBack(fallBackToDefault) {
    useHalThread();
}

CoreAudioBackend::~CoreAudioBackend() {
    close();
}

// --- devices ---------------------------------------------------------------------------

std::vector<AudioDeviceInfo> CoreAudioBackend::outputDevices() {
    std::vector<AudioDeviceInfo> devices;
    const AudioObjectPropertyAddress where = address(kAudioHardwarePropertyDevices);
    UInt32                           size  = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &where, 0, nullptr, &size) != noErr) return devices;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, ids.data()) != noErr)
        return devices;

    AudioObjectID defaultOutput = 0;
    getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, defaultOutput);
    for (const AudioObjectID id : ids) {
        AudioDeviceInfo info;
        info.id             = id;
        info.outputChannels = outputChannelCount(id);
        if (info.outputChannels == 0) continue;   // an input-only device
        info.name      = stringProperty(id, kAudioObjectPropertyName);
        info.uid       = stringProperty(id, kAudioDevicePropertyDeviceUID);
        info.isDefault = id == defaultOutput;
        Float64 rate   = 0.0;
        if (getProperty(id, kAudioDevicePropertyNominalSampleRate, rate)) info.sampleRate = rate;
        devices.push_back(info);
    }
    return devices;
}

bool CoreAudioBackend::findDevice(const std::string& wanted, AudioDeviceInfo& found, std::string& problem) {
    const std::vector<AudioDeviceInfo> devices = outputDevices();
    if (wanted.empty()) {
        for (const AudioDeviceInfo& device : devices)
            if (device.isDefault) {
                found = device;
                return true;
            }
        problem = "there's no default output device";
        return false;
    }
    for (const AudioDeviceInfo& device : devices)
        if (device.name == wanted || device.uid == wanted) {
            found = device;
            return true;
        }
    const std::string            part = lowercase(wanted);
    std::vector<AudioDeviceInfo> matches;
    for (const AudioDeviceInfo& device : devices)
        if (lowercase(device.name).find(part) != std::string::npos) matches.push_back(device);
    if (matches.size() == 1) {
        found = matches.front();
        return true;
    }
    if (matches.empty()) {
        problem = "no output device '" + wanted + "' (see --list-audio)";
    } else {
        problem = "'" + wanted + "' matches several output devices:";
        for (const AudioDeviceInfo& device : matches) problem += " '" + device.name + "'";
    }
    return false;
}

// --- AudioBackend ------------------------------------------------------------------------

bool CoreAudioBackend::open(const AudioConfig& requested, AudioConfig& negotiated) {
    if (isOpen) return false;

    std::string problem;
    pendingChoice.clear();
    if (!findDevice(choice, main, problem)) {
        if (!fallBack || choice.empty() || !findDevice({}, main, problem)) {
            std::fprintf(stderr, "CoreAudioBackend: %s\n", problem.c_str());
            return false;
        }
        pendingChoice = choice;   // service() moves there when it appears
    }

    // The device keeps its rate unless one is asked for; from here on, the rate
    // it runs at is the one we hold to (the plugins are prepared for it).
    wanted = requested;
    if (requested.sampleRate > 0.0 && requested.sampleRate != main.sampleRate &&
        !setProperty(main.id, kAudioDevicePropertyNominalSampleRate, Float64{requested.sampleRate}))
        std::fprintf(stderr, "CoreAudioBackend: '%s' won't run at %.0f Hz\n", main.name.c_str(), requested.sampleRate);
    Float64 rate = 0.0;
    getProperty(main.id, kAudioDevicePropertyNominalSampleRate, rate);
    wanted.sampleRate = rate;

    // This process's buffer on the device stays as set until set again, so
    // close() puts back what was there.
    restoreBufferSize = 0;
    if (UInt32 before = 0; requested.bufferSize != 0 && getProperty(main.id, kAudioDevicePropertyBufferFrameSize, before))
        restoreBufferSize = before;
    if (main.outputChannels < requested.outputChannels)
        std::fprintf(stderr, "CoreAudioBackend: '%s' has %u outputs; outputs %u to %u go nowhere\n",
                     main.name.c_str(), main.outputChannels, main.outputChannels + 1, requested.outputChannels);

    // The system's device list, for hotplug (the main device's return, the
    // chosen one appearing, the click's devices).
    const AudioObjectPropertyAddress devicesAddress = address(kAudioHardwarePropertyDevices);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devicesAddress, &propertyListener, this);

    lost.store(false);
    rateChanged.store(false);
    lostReported = false;
    if (!setUp()) {
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devicesAddress, &propertyListener, this);
        return false;
    }
    isOpen = true;

    negotiatedConfig                   = AudioConfig{};
    negotiatedConfig.sampleRate        = currentSampleRate.load();
    negotiatedConfig.bufferSize        = currentBufferSize.load();
    negotiatedConfig.inputChannels     = 0;   // outputs only (decision 2)
    negotiatedConfig.outputChannels    = requested.outputChannels;
    negotiatedConfig.auxOutputChannels = requested.auxOutputChannels;
    negotiated                         = negotiatedConfig;
    return true;
}

bool CoreAudioBackend::setUp() {
    mainChannels        = outputChannelCount(main.id);
    main.outputChannels = mainChannels;

    // The click: another device through an aggregate, or onto the main device's
    // first channels, or nowhere.
    ioDevice = main.id;
    auxChannels.clear();
    if (!clickUid.empty()) {
        AudioDeviceInfo other;
        if (findByUid(clickUid, other) && makeAggregate(other)) {
            ioDevice    = aggregate;
            auxChannels = {mainChannels, mainChannels + std::min<std::uint32_t>(1, other.outputChannels - 1)};
        } else {
            clickUid.clear();   // gone, or no aggregate: unrouted (the controller tries again on hotplug)
        }
    } else if (clickOnMain && mainChannels > 0) {
        auxChannels = {0, std::min<std::uint32_t>(1, mainChannels - 1)};
    }

    if (wanted.sampleRate > 0.0) {
        Float64 now = 0.0;
        if (getProperty(ioDevice, kAudioDevicePropertyNominalSampleRate, now) && now != wanted.sampleRate)
            setProperty(ioDevice, kAudioDevicePropertyNominalSampleRate, Float64{wanted.sampleRate});
    }
    AudioValueRange range{};
    getProperty(ioDevice, kAudioDevicePropertyBufferFrameSizeRange, range);
    if (wanted.bufferSize != 0) {
        const auto frames = static_cast<UInt32>(
            std::clamp<double>(wanted.bufferSize, range.mMinimum, range.mMaximum > 0 ? range.mMaximum : 8192));
        if (frames != wanted.bufferSize)
            std::fprintf(stderr, "CoreAudioBackend: '%s' takes %.0f to %.0f frames; using %u\n", main.name.c_str(),
                         range.mMinimum, range.mMaximum, frames);
        if (!setProperty(ioDevice, kAudioDevicePropertyBufferFrameSize, frames))
            std::fprintf(stderr, "CoreAudioBackend: couldn't set a %u-frame buffer\n", frames);
    }
    Float64 rate   = 0.0;
    UInt32  frames = 0;
    getProperty(ioDevice, kAudioDevicePropertyNominalSampleRate, rate);
    getProperty(ioDevice, kAudioDevicePropertyBufferFrameSize, frames);
    currentSampleRate.store(rate, std::memory_order_relaxed);
    currentBufferSize.store(frames, std::memory_order_relaxed);
    if (wanted.sampleRate > 0.0 && rate != wanted.sampleRate) {
        std::fprintf(stderr, "CoreAudioBackend: '%s' won't run at %.0f Hz (it's at %.0f Hz)\n", main.name.c_str(),
                     wanted.sampleRate, rate);
        tearDown();
        return false;
    }

    // Planar buffers for the largest block the device allows (the IOProc is
    // stopped: they can move).
    const std::uint32_t capacity = std::max<std::uint32_t>(frames, static_cast<std::uint32_t>(range.mMaximum));
    const std::uint32_t outputs  = wanted.outputChannels;
    const std::uint32_t aux      = wanted.auxOutputChannels;
    if (capacity != capacityFrames || outputPointers.size() != outputs || auxPointers.size() != aux) {
        capacityFrames = capacity;
        planar.assign(static_cast<std::size_t>(outputs + aux) * capacityFrames, 0.0f);
        outputPointers.clear();
        auxPointers.clear();
        for (std::uint32_t c = 0; c < outputs; ++c)
            outputPointers.push_back(planar.data() + std::size_t{c} * capacityFrames);
        for (std::uint32_t c = 0; c < aux; ++c)
            auxPointers.push_back(planar.data() + std::size_t{outputs + c} * capacityFrames);
    }

    AudioDeviceIOProcID id = nullptr;
    if (const OSStatus result = AudioDeviceCreateIOProcID(ioDevice, &ioProc, this, &id); result != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't run on '%s' (error %d)\n", main.name.c_str(),
                     static_cast<int>(result));
        tearDown();
        return false;
    }
    procId = reinterpret_cast<void*>(id);
    addListeners(main.id);
    return true;
}

void CoreAudioBackend::tearDown() {
    if (procId != nullptr) {
        AudioDeviceStop(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId));   // returns once the IOProc is done
        AudioDeviceDestroyIOProcID(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId));
        procId = nullptr;
    }
    removeListeners();
    if (aggregate != 0) {
        AudioHardwareDestroyAggregateDevice(aggregate);
        aggregate = 0;
    }
    ioDevice = 0;
}

bool CoreAudioBackend::restart() {
    tearDown();
    if (!setUp()) return false;
    if (running && AudioDeviceStart(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId)) != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't start '%s' again\n", main.name.c_str());
        return false;
    }
    return true;
}

// The main device and `other`, as one private device (decision 3): the main
// device is the clock; the other runs at its rate, drift-compensated. Not shown
// to other apps, and gone when the app quits.
bool CoreAudioBackend::makeAggregate(const AudioDeviceInfo& other) {
    if (other.sampleRate != wanted.sampleRate &&
        !setProperty(other.id, kAudioDevicePropertyNominalSampleRate, Float64{wanted.sampleRate}))
        std::fprintf(stderr, "CoreAudioBackend: '%s' won't run at %.0f Hz; the click may drift\n", other.name.c_str(),
                     wanted.sampleRate);

    const auto cfString = [](const std::string& text) {
        return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
    };
    const auto subDevice = [&](const std::string& uid, bool driftCompensated) {
        CFMutableDictionaryRef entry = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
        const CFStringRef value = cfString(uid);
        CFDictionarySetValue(entry, CFSTR(kAudioSubDeviceUIDKey), value);
        CFRelease(value);
        if (driftCompensated) {
            int               one = 1;
            const CFNumberRef on  = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
            CFDictionarySetValue(entry, CFSTR(kAudioSubDeviceDriftCompensationKey), on);
            CFRelease(on);
        }
        return entry;
    };

    CFMutableArrayRef subDevices = CFArrayCreateMutable(kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);
    for (CFMutableDictionaryRef entry : {subDevice(main.uid, false), subDevice(other.uid, true)}) {
        CFArrayAppendValue(subDevices, entry);
        CFRelease(entry);
    }
    CFMutableDictionaryRef description = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const CFStringRef name      = cfString(aggregateName);
    const CFStringRef uid       = cfString(aggregateUidPrefix + ".aggregate." + std::to_string(getpid()));
    const CFStringRef mainUid   = cfString(main.uid);
    int               one       = 1;
    const CFNumberRef isPrivate = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), name);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey), uid);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), isPrivate);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDevices);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), mainUid);
    const OSStatus result = AudioHardwareCreateAggregateDevice(description, &aggregate);
    for (CFTypeRef object : {static_cast<CFTypeRef>(name), static_cast<CFTypeRef>(uid), static_cast<CFTypeRef>(mainUid),
                             static_cast<CFTypeRef>(isPrivate), static_cast<CFTypeRef>(subDevices),
                             static_cast<CFTypeRef>(description)})
        CFRelease(object);
    if (result != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't join '%s' and '%s' (error %d)\n", main.name.c_str(),
                     other.name.c_str(), static_cast<int>(result));
        aggregate = 0;
        return false;
    }
    // A new aggregate takes a moment to gather its devices' channels.
    const std::uint32_t expected = mainChannels + other.outputChannels;
    for (int i = 0; i < 50 && outputChannelCount(aggregate) < expected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (outputChannelCount(aggregate) <= mainChannels) {
        std::fprintf(stderr, "CoreAudioBackend: the joined device for the click didn't come up\n");
        AudioHardwareDestroyAggregateDevice(aggregate);
        aggregate = 0;
        return false;
    }
    return true;
}

void CoreAudioBackend::setProcessor(AudioProcessor* audioProcessor) {
    if (running) {
        std::fprintf(stderr, "CoreAudioBackend: setProcessor() ignored while running\n");
        return;
    }
    processor = audioProcessor;
}

bool CoreAudioBackend::start() {
    if (!isOpen || running || procId == nullptr) return false;
    if (const OSStatus result = AudioDeviceStart(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId));
        result != noErr) {
        std::fprintf(stderr, "CoreAudioBackend: can't start '%s' (error %d)\n", main.name.c_str(),
                     static_cast<int>(result));
        return false;
    }
    running = true;
    return true;
}

void CoreAudioBackend::stop() {
    if (!running) return;
    if (procId != nullptr) AudioDeviceStop(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId));
    running = false;
}

void CoreAudioBackend::close() {
    if (!isOpen) return;
    stop();
    tearDown();
    const AudioObjectPropertyAddress devicesAddress = address(kAudioHardwarePropertyDevices);
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devicesAddress, &propertyListener, this);
    if (restoreBufferSize != 0 && !lost.load())
        setProperty(main.id, kAudioDevicePropertyBufferFrameSize, UInt32{restoreBufferSize});
    restoreBufferSize = 0;
    isOpen            = false;
    planar.clear();
    outputPointers.clear();
    auxPointers.clear();
    capacityFrames = 0;
}

AudioConfig CoreAudioBackend::config() const {
    AudioConfig current = negotiatedConfig;
    current.bufferSize  = currentBufferSize.load(std::memory_order_relaxed);
    current.sampleRate  = currentSampleRate.load(std::memory_order_relaxed);
    return current;
}

// From a period's start to its sound at the outputs: the buffer, then the
// device's safety offset and latency, and the stream's.
double CoreAudioBackend::outputLatencySeconds() const {
    const double rate = currentSampleRate.load(std::memory_order_relaxed);
    if (procId == nullptr || rate <= 0.0) return 0.0;
    UInt32 deviceLatency = 0, safetyOffset = 0, streamLatency = 0;
    getProperty(ioDevice, kAudioDevicePropertyLatency, deviceLatency, kAudioObjectPropertyScopeOutput);
    getProperty(ioDevice, kAudioDevicePropertySafetyOffset, safetyOffset, kAudioObjectPropertyScopeOutput);
    AudioStreamID stream = 0;   // the first output stream's
    if (getProperty(ioDevice, kAudioDevicePropertyStreams, stream, kAudioObjectPropertyScopeOutput) && stream != 0)
        getProperty(stream, kAudioStreamPropertyLatency, streamLatency);
    const std::uint32_t frames = currentBufferSize.load(std::memory_order_relaxed) + deviceLatency + safetyOffset +
                                 streamLatency;
    return frames / rate;
}

// --- AuxRouting ------------------------------------------------------------------

std::vector<PlaybackDevice> CoreAudioBackend::playbackDevices() {
    std::vector<PlaybackDevice> devices;
    for (const AudioDeviceInfo& info : outputDevices()) {
        PlaybackDevice device{info.name, {}};
        for (std::uint32_t c = 1; c <= info.outputChannels; ++c) device.ports.push_back(std::to_string(c));
        devices.push_back(std::move(device));
    }
    return devices;
}

bool CoreAudioBackend::connectAux(const PlaybackDevice& device) {
    if (!isOpen || lost.load()) return false;
    AudioDeviceInfo target;
    bool            found = false;
    for (const AudioDeviceInfo& info : outputDevices())
        if (info.name == device.name) {
            target = info;
            found  = true;
            break;
        }
    if (!found) return false;
    if (target.uid == main.uid) {   // onto the main outputs' device: no aggregate
        if (clickOnMain && clickUid.empty()) return true;
        clickOnMain = true;
        clickUid.clear();
        return restart();
    }
    if (clickUid == target.uid && aggregate != 0) return true;
    clickOnMain = false;
    clickUid    = target.uid;
    return restart() && aggregate != 0;
}

void CoreAudioBackend::disconnectAux() {
    if (!clickOnMain && clickUid.empty()) return;
    clickOnMain = false;
    clickUid.clear();
    if (isOpen && !lost.load()) restart();
}

bool CoreAudioBackend::takeDevicesChanged() {
    return devicesChanged.exchange(false, std::memory_order_relaxed);
}

std::string CoreAudioBackend::mainDevice() {
    return main.name;
}

// --- main-thread service -----------------------------------------------------------------

std::vector<std::string> CoreAudioBackend::service() {
    std::vector<std::string> notices;
    if (!isOpen) return notices;
    const bool listChanged = devicesChangedHere.exchange(false, std::memory_order_relaxed);

    // The main device went away: silent until it's back (not the laptop's
    // speakers instead: on stage, sound from the wrong place is worse).
    if (lost.load()) {
        if (!lostReported) {
            tearDown();
            lostReported = true;
            notices.push_back("The output device '" + main.name + "' went away: silent until it's back.");
        }
        AudioDeviceInfo back;
        if (listChanged && findByUid(main.uid, back)) {
            main = back;
            lost.store(false);
            lostReported = false;
            if (setUp() &&
                (!running || AudioDeviceStart(ioDevice, reinterpret_cast<AudioDeviceIOProcID>(procId)) == noErr)) {
                notices.push_back("The output device '" + main.name + "' is back: playing.");
                devicesChanged.store(true);   // so the click is routed again
            } else {
                lost.store(true);   // try again on the next change
                lostReported = true;
                notices.push_back("The output device '" + main.name + "' is back but won't start; still waiting.");
            }
        }
        return notices;
    }

    // The device chosen for this machine, stood in for until now (decision 12).
    if (listChanged && !pendingChoice.empty()) {
        AudioDeviceInfo chosen;
        std::string     problem;
        if (findDevice(pendingChoice, chosen, problem)) {
            const std::string before = main.name;
            main                     = chosen;
            pendingChoice.clear();
            if (restart())
                notices.push_back("The output device '" + main.name + "' is here: playing on it (not '" + before + "').");
            else
                notices.push_back("The output device '" + main.name + "' is here but won't start.");
        }
    }

    // The click's other device went: back to the main device alone (the
    // controller routes it again when it returns).
    if (listChanged && !clickUid.empty()) {
        AudioDeviceInfo other;
        if (!findByUid(clickUid, other)) {
            clickUid.clear();
            restart();
        }
    }

    // The rate is ours while we play: the plugins were prepared for it.
    if (rateChanged.exchange(false, std::memory_order_relaxed)) {
        const double now = currentSampleRate.load();
        if (now != wanted.sampleRate) {
            const auto t = std::chrono::steady_clock::now();
            if (rateRestores == 0 || t - firstRateRestore > kRateRestoreWindow) {
                rateRestores     = 0;
                firstRateRestore = t;
            }
            if (++rateRestores <= kMaxRateRestores &&
                setProperty(ioDevice, kAudioDevicePropertyNominalSampleRate, Float64{wanted.sampleRate}))
                notices.push_back("Something set '" + main.name + "' to " + hertz(now) + "; set back to " +
                                  hertz(wanted.sampleRate) + ", the rate the plugins run at.");
            else if (rateRestores == kMaxRateRestores + 1)
                notices.push_back("Something keeps setting '" + main.name + "' to " + hertz(now) +
                                  ": the plugins run at " + hertz(wanted.sampleRate) +
                                  ", so pitch and tempo are off until it's set back.");
        }
    }
    return notices;
}

// --- notifications -----------------------------------------------------------------------

void CoreAudioBackend::addListeners(AudioObjectID object) {
    for (const AudioObjectPropertySelector selector : kWatched) {
        const AudioObjectPropertyAddress where = address(selector);
        AudioObjectAddPropertyListener(object, &where, &propertyListener, this);
    }
    listenedTo = object;
}

void CoreAudioBackend::removeListeners() {
    if (listenedTo == 0) return;
    for (const AudioObjectPropertySelector selector : kWatched) {
        const AudioObjectPropertyAddress where = address(selector);
        AudioObjectRemovePropertyListener(listenedTo, &where, &propertyListener, this);
    }
    listenedTo = 0;
}

void CoreAudioBackend::onPropertyChanged(AudioObjectID object, std::uint32_t selector) noexcept {
    switch (selector) {
        case kAudioHardwarePropertyDevices:
            devicesChanged.store(true, std::memory_order_relaxed);
            devicesChangedHere.store(true, std::memory_order_relaxed);
            break;
        case kAudioDevicePropertyBufferFrameSize: {
            UInt32 frames = 0;
            if (getProperty(object, kAudioDevicePropertyBufferFrameSize, frames))
                currentBufferSize.store(frames, std::memory_order_relaxed);
            break;
        }
        case kAudioDevicePropertyNominalSampleRate: {
            Float64 rate = 0.0;
            if (getProperty(object, kAudioDevicePropertyNominalSampleRate, rate) &&
                rate != currentSampleRate.load(std::memory_order_relaxed)) {
                currentSampleRate.store(rate, std::memory_order_relaxed);
                rateChanged.store(true, std::memory_order_relaxed);
            }
            break;
        }
        case kAudioDeviceProcessorOverload:
            overloads.fetch_add(1, std::memory_order_relaxed);
            break;
        case kAudioDevicePropertyDeviceIsAlive: {
            UInt32 alive = 1;
            if (!getProperty(object, kAudioDevicePropertyDeviceIsAlive, alive) || alive == 0)
                lost.store(true, std::memory_order_relaxed);
            break;
        }
        default: break;
    }
}

// --- the IOProc (CoreAudio's real-time thread) -------------------------------------------

// No allocation, locks or system calls. The streams are the HAL's virtual
// format: Float32, interleaved within each stream.
void CoreAudioBackend::render(const void* nowStamp, void* outputBufferList) noexcept {
    const auto* const now    = static_cast<const AudioTimeStamp*>(nowStamp);
    auto* const       output = static_cast<AudioBufferList*>(outputBufferList);
    if (output == nullptr || output->mNumberBuffers == 0) return;

    const ::AudioBuffer& first = output->mBuffers[0];
    const std::uint32_t  frames =
        first.mNumberChannels == 0 ? 0 : first.mDataByteSize / (first.mNumberChannels * sizeof(float));
    if (frames == 0 || frames > capacityFrames) {   // can't happen: the range's maximum was allocated
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
            std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
        return;
    }

    const std::int64_t   position = frameCounter.load(std::memory_order_relaxed);
    AudioCallbackContext ctx;
    ctx.frameCount     = frames;
    ctx.blockStartTime = now != nullptr && (now->mFlags & kAudioTimeStampHostTimeValid) != 0
                             ? hostTimeToNanoseconds(now->mHostTime)
                             : nowNanoseconds();
    ctx.time.sampleRate     = currentSampleRate.load(std::memory_order_relaxed);
    ctx.time.samplePosition = position;
    ctx.output = uw::AudioBuffer{outputPointers.data(), static_cast<std::uint32_t>(outputPointers.size()), frames};
    ctx.aux    = uw::AudioBuffer{auxPointers.data(), static_cast<std::uint32_t>(auxPointers.size()), frames};

    if (processor != nullptr) {
        processor->process(ctx);
    } else {
        for (float* channel : outputPointers) std::fill_n(channel, frames, 0.0f);
        for (float* channel : auxPointers) std::fill_n(channel, frames, 0.0f);
    }

    // Our outputs onto the device's channels, in order across its streams, and
    // the click onto its channels, mixed in; any other channel is silent.
    // (auxChannels only changes while the IOProc is stopped.)
    std::uint32_t deviceChannel = 0;
    for (UInt32 b = 0; b < output->mNumberBuffers; ++b) {
        ::AudioBuffer&      buffer   = output->mBuffers[b];
        auto* const         samples  = static_cast<float*>(buffer.mData);
        const std::uint32_t channels = buffer.mNumberChannels;
        if (samples == nullptr || channels == 0) continue;
        const std::uint32_t bufferFrames = std::min<std::uint32_t>(frames, buffer.mDataByteSize / (channels * sizeof(float)));
        for (std::uint32_t c = 0; c < channels; ++c, ++deviceChannel) {
            const float* const ours = deviceChannel < outputPointers.size() ? outputPointers[deviceChannel] : nullptr;
            const float*       aux  = nullptr;
            for (std::size_t a = 0; a < auxChannels.size() && a < auxPointers.size(); ++a)
                if (auxChannels[a] == deviceChannel) aux = auxPointers[a];
            for (std::uint32_t f = 0; f < bufferFrames; ++f)
                samples[f * channels + c] = (ours != nullptr ? ours[f] : 0.0f) + (aux != nullptr ? aux[f] : 0.0f);
        }
    }
    frameCounter.store(position + frames, std::memory_order_relaxed);
}

} // namespace uw
