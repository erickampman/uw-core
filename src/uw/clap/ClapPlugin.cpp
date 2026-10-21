#include "uw/clap/ClapPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "uw/ump/Ump.h"
#include "uw/clap/ClapModule.h"

namespace uw {

namespace {

// Most input events handed to the plugin per process() call. The engine's own
// per-slot limit is lower (EngineGraph, 1024 by default).
constexpr std::uint32_t kMaxEventsPerBlock = 2048;

// Set on the thread that calls process(): CLAP's is_audio_thread().
thread_local bool tlsAudioThread = false;

clap_event_header_t eventHeader(std::size_t size, std::uint32_t time, std::uint16_t type) noexcept {
    clap_event_header_t header{};
    header.size     = static_cast<std::uint32_t>(size);
    header.time     = time;
    header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    header.type     = type;
    header.flags    = 0;
    return header;
}

std::uint64_t allChannels(std::uint32_t count) noexcept {
    return count >= 64 ? ~0ull : ((1ull << count) - 1);
}

} // namespace

// --- host extension tables -----------------------------------------------------------

const clap_host_log_t ClapPlugin::kHostLog = {&ClapPlugin::hostLog};
const clap_host_thread_check_t ClapPlugin::kHostThreadCheck = {&ClapPlugin::hostIsMainThread,
                                                               &ClapPlugin::hostIsAudioThread};
const clap_host_params_t ClapPlugin::kHostParams = {
    &ClapPlugin::hostParamsRescan, &ClapPlugin::hostParamsClear, &ClapPlugin::hostParamsRequestFlush};
const clap_host_state_t   ClapPlugin::kHostState   = {&ClapPlugin::hostMarkDirty};
const clap_host_latency_t ClapPlugin::kHostLatency = {&ClapPlugin::hostLatencyChanged};
const clap_host_audio_ports_t ClapPlugin::kHostAudioPorts = {
    &ClapPlugin::hostAudioPortsRescanFlagSupported, &ClapPlugin::hostAudioPortsRescan};
const clap_host_note_ports_t ClapPlugin::kHostNotePorts = {&ClapPlugin::hostNotePortsDialects,
                                                           &ClapPlugin::hostNotePortsRescan};
const clap_host_gui_t ClapPlugin::kHostGui = {
    &ClapPlugin::hostGuiResizeHintsChanged, &ClapPlugin::hostGuiRequestResize,
    &ClapPlugin::hostGuiRequestShow, &ClapPlugin::hostGuiRequestHide, &ClapPlugin::hostGuiClosed};
const clap_host_timer_support_t ClapPlugin::kHostTimerSupport = {&ClapPlugin::hostRegisterTimer,
                                                                 &ClapPlugin::hostUnregisterTimer};
const clap_host_posix_fd_support_t ClapPlugin::kHostPosixFdSupport = {
    &ClapPlugin::hostRegisterFd, &ClapPlugin::hostModifyFd, &ClapPlugin::hostUnregisterFd};

// --- creation / destruction -------------------------------------------------------------

ClapPlugin::ClapPlugin(std::shared_ptr<ClapModule> loadedModule, const PluginDescriptor& pluginDesc,
                       HostServices& host)
    : module(std::move(loadedModule)), desc(pluginDesc), hostServices(host), hostInfo(host.hostInfo()),
      mainThread(std::this_thread::get_id()) {
    clapHost.clap_version     = CLAP_VERSION;
    clapHost.host_data        = this;
    clapHost.name             = hostInfo.name.c_str();
    clapHost.vendor           = hostInfo.vendor.c_str();
    clapHost.url              = hostInfo.url.c_str();
    clapHost.version          = hostInfo.version.c_str();
    clapHost.get_extension    = &ClapPlugin::hostGetExtension;
    clapHost.request_restart  = &ClapPlugin::hostRequestRestart;
    clapHost.request_process  = &ClapPlugin::hostRequestProcess;
    clapHost.request_callback = &ClapPlugin::hostRequestCallback;

    inList.ctx      = this;
    inList.size     = &ClapPlugin::inEventsSize;
    inList.get      = &ClapPlugin::inEventsGet;
    outList.ctx     = this;
    outList.try_push = &ClapPlugin::outEventsTryPush;

    inEvents.assign(kMaxEventsPerBlock, EventStorage{});
}

std::unique_ptr<ClapPlugin> ClapPlugin::create(std::shared_ptr<ClapModule> loadedModule,
                                               const PluginDescriptor& pluginDesc, HostServices& host) {
    if (!loadedModule || loadedModule->factory() == nullptr) return nullptr;
    std::unique_ptr<ClapPlugin> instance(new ClapPlugin(std::move(loadedModule), pluginDesc, host));

    const clap_plugin_factory_t* factory = instance->module->factory();
    instance->plugin = factory->create_plugin(factory, &instance->clapHost, pluginDesc.id.c_str());
    if (instance->plugin == nullptr) {
        instance->report(LogLevel::Error, "create_plugin failed");
        return nullptr;
    }
    if (!instance->plugin->init(instance->plugin)) {
        instance->report(LogLevel::Error, "init failed");
        instance->plugin->destroy(instance->plugin);
        instance->plugin = nullptr;
        return nullptr;
    }

    const clap_plugin_t* p = instance->plugin;
    // Name, vendor and version come from the plugin itself: the descriptor we
    // were given may carry only format, id and path (e.g. from a rack file).
    if (const clap_plugin_descriptor_t* d = p->desc) {
        if (d->name != nullptr) instance->desc.name = d->name;
        if (d->vendor != nullptr) instance->desc.vendor = d->vendor;
        if (d->version != nullptr) instance->desc.version = d->version;
    }
    instance->audioPortsExt = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
    instance->notePortsExt  = static_cast<const clap_plugin_note_ports_t*>(p->get_extension(p, CLAP_EXT_NOTE_PORTS));
    instance->paramsExt     = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
    instance->stateExt      = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    instance->guiExt        = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
    instance->timerExt      = static_cast<const clap_plugin_timer_support_t*>(p->get_extension(p, CLAP_EXT_TIMER_SUPPORT));
    instance->fdExt         = static_cast<const clap_plugin_posix_fd_support_t*>(p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT));
    return instance;
}

ClapPlugin::~ClapPlugin() {
    if (plugin == nullptr) return;
    closeEditor();                   // before anything else: the editor needs a live plugin
    if (pendingGuiDestroy) guiExt->destroy(plugin);
    removeMainLoopRegistrations();   // anything the plugin forgot to unregister
    if (processing)                  // the engine's shutdown order normally prevents this
        report(LogLevel::Warning, "deactivated while processing (audio stopped first)");
    if (active) plugin->deactivate(plugin);
    plugin->destroy(plugin);
}

void ClapPlugin::removeMainLoopRegistrations() {
    for (const auto& [clapId, timerId] : timers) hostServices.removeTimer(timerId);
    timers.clear();
    for (const int fd : watchedFds) hostServices.unwatchFd(fd);
    watchedFds.clear();
}

// --- introspection (main thread) ---------------------------------------------------------

PortLayout ClapPlugin::portLayout() const {
    PortLayout layout;
    if (audioPortsExt != nullptr) {
        for (const bool isInput : {true, false}) {
            const std::uint32_t count = audioPortsExt->count(plugin, isInput);
            for (std::uint32_t i = 0; i < count; ++i) {
                clap_audio_port_info_t info{};
                if (!audioPortsExt->get(plugin, i, isInput, &info)) continue;
                AudioPortInfo port;
                port.name         = info.name;
                port.channelCount = info.channel_count;
                port.isMain       = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
                (isInput ? layout.audioInputs : layout.audioOutputs).push_back(port);
            }
        }
    }
    if (notePortsExt != nullptr) {
        for (const bool isInput : {true, false}) {
            const std::uint32_t count = notePortsExt->count(plugin, isInput);
            for (std::uint32_t i = 0; i < count; ++i) {
                clap_note_port_info_t info{};
                if (!notePortsExt->get(plugin, i, isInput, &info)) continue;
                NotePortInfo port;
                port.name              = info.name;
                port.supportedDialects = info.supported_dialects;   // same bit values
                port.preferredDialect  = info.preferred_dialect;
                (isInput ? layout.noteInputs : layout.noteOutputs).push_back(port);
            }
        }
    }
    return layout;
}

std::vector<ParameterInfo> ClapPlugin::parameters() const {
    std::vector<ParameterInfo> list;
    if (paramsExt == nullptr) return list;
    const std::uint32_t count = paramsExt->count(plugin);
    for (std::uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!paramsExt->get_info(plugin, i, &info)) continue;
        ParameterInfo param;
        param.id            = info.id;
        param.cookie        = info.cookie;
        param.name          = info.name;
        param.minValue      = info.min_value;
        param.maxValue      = info.max_value;
        param.defaultValue  = info.default_value;
        param.isAutomatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        param.isStepped     = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        param.isHidden      = (info.flags & CLAP_PARAM_IS_HIDDEN) != 0;
        param.isReadOnly    = (info.flags & CLAP_PARAM_IS_READONLY) != 0;
        param.module        = info.module;
        list.push_back(param);
    }
    return list;
}

bool ClapPlugin::paramValueText(std::uint32_t id, double value, std::string& text) const {
    if (paramsExt == nullptr || paramsExt->value_to_text == nullptr) return false;
    char buffer[256] = {};
    if (!paramsExt->value_to_text(plugin, id, value, buffer, sizeof buffer)) return false;
    buffer[sizeof buffer - 1] = '\0';
    text = buffer;
    return true;
}

double ClapPlugin::paramValue(std::uint32_t id) const {
    double value = 0.0;
    if (paramsExt != nullptr) paramsExt->get_value(plugin, id, &value);
    return value;
}

// --- lifecycle (main thread) -------------------------------------------------------------

bool ClapPlugin::activate(double sampleRate, std::uint32_t maxBlockFrames) {
    if (plugin == nullptr || active) return false;
    prepareBuffers(portLayout(), maxBlockFrames);   // port config is fixed while active
    if (!plugin->activate(plugin, sampleRate, 1, maxBlockFrames)) {
        report(LogLevel::Error, "activate failed");
        return false;
    }
    active = true;
    return true;
}

void ClapPlugin::deactivate() {
    if (!active) return;
    plugin->deactivate(plugin);
    active = false;
}

void ClapPlugin::prepareBuffers(const PortLayout& layout, std::uint32_t maxBlockFrames) {
    const std::size_t portCount = layout.audioInputs.size() + layout.audioOutputs.size();
    portAudio.clear();
    portChannels.clear();
    portAudio.reserve(portCount);      // stable addresses for the pointers below
    portChannels.reserve(portCount);
    inputBuffers.clear();
    outputBuffers.clear();
    mainOutput = mainAudioOutput(layout);

    auto scratchPort = [&](std::uint32_t channels) {
        portAudio.emplace_back(std::size_t{channels} * maxBlockFrames, 0.0f);
        portChannels.emplace_back(channels);
        for (std::uint32_t c = 0; c < channels; ++c)
            portChannels.back()[c] = portAudio.back().data() + std::size_t{c} * maxBlockFrames;
        clap_audio_buffer_t buffer{};
        buffer.data32        = portChannels.back().data();
        buffer.channel_count = channels;
        return buffer;
    };

    // Inputs: silence, marked constant, unless the engine gives the main one
    // audio (process()). The main input is the first flagged main, else the first.
    mainInput = -1;
    for (std::size_t i = 0; i < layout.audioInputs.size() && mainInput < 0; ++i)
        if (layout.audioInputs[i].isMain) mainInput = static_cast<int>(i);
    if (mainInput < 0 && !layout.audioInputs.empty()) mainInput = 0;
    for (const AudioPortInfo& port : layout.audioInputs) {
        clap_audio_buffer_t buffer = scratchPort(port.channelCount);
        buffer.constant_mask       = allChannels(port.channelCount);
        inputBuffers.push_back(buffer);
    }
    mainInputSilence = mainInput >= 0 ? inputBuffers[static_cast<std::size_t>(mainInput)].data32 : nullptr;
    // Outputs: the main port writes straight into the engine's buffer (set per
    // block in process()); any others go to scratch.
    for (std::size_t i = 0; i < layout.audioOutputs.size(); ++i) {
        const std::uint32_t channels = layout.audioOutputs[i].channelCount;
        if (static_cast<int>(i) == mainOutput) {
            clap_audio_buffer_t buffer{};
            buffer.channel_count = channels;
            outputBuffers.push_back(buffer);
        } else {
            outputBuffers.push_back(scratchPort(channels));
        }
    }

    noteDialects.clear();
    for (const NotePortInfo& port : layout.noteInputs) noteDialects.push_back(port.supportedDialects);
}

// --- audio thread -----------------------------------------------------------------------

void ClapPlugin::process(const ProcessContext& ctx) noexcept {
    tlsAudioThread = true;
    auto silence = [&] {
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            std::fill_n(ctx.audioOut.channels[c], ctx.frameCount, 0.0f);
    };
    if (!active) {
        silence();
        return;
    }
    if (!processing) {
        processing = plugin->start_processing(plugin);
        if (!processing) {
            silence();
            return;
        }
    }

    convertAll(ctx.eventsIn);
    if (mainInput >= 0) {   // the engine's audio when it gives the port's channel count, else silence
        clap_audio_buffer_t& in = inputBuffers[static_cast<std::size_t>(mainInput)];
        if (ctx.audioIn.channelCount == in.channel_count && ctx.audioIn.channelCount > 0) {
            in.data32        = const_cast<float**>(ctx.audioIn.channels);
            in.constant_mask = 0;
        } else {
            in.data32        = mainInputSilence;
            in.constant_mask = allChannels(in.channel_count);
        }
    }
    if (mainOutput >= 0) {
        clap_audio_buffer_t& main = outputBuffers[static_cast<std::size_t>(mainOutput)];
        main.data32               = const_cast<float**>(ctx.audioOut.channels);
        main.channel_count        = ctx.audioOut.channelCount;
    }
    fillTransport(ctx.time);

    clap_process_t processData{};
    processData.steady_time         = ctx.time.samplePosition;
    processData.frames_count        = ctx.frameCount;
    processData.transport           = &transport;
    processData.audio_inputs        = inputBuffers.data();
    processData.audio_inputs_count  = static_cast<std::uint32_t>(inputBuffers.size());
    processData.audio_outputs       = outputBuffers.data();
    processData.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.size());
    processData.in_events           = &inList;
    processData.out_events          = &outList;

    currentSink = ctx.eventsOut;
    const clap_process_status status = plugin->process(plugin, &processData);
    currentSink = nullptr;
    if (status == CLAP_PROCESS_ERROR) silence();
}

void ClapPlugin::stopProcessing() noexcept {
    if (!processing) return;
    plugin->stop_processing(plugin);
    processing = false;
}

void ClapPlugin::reset() noexcept {
    if (active) plugin->reset(plugin);
}

void ClapPlugin::fillTransport(const TimeInfo& time) noexcept {
    transport        = clap_event_transport_t{};
    transport.header = eventHeader(sizeof transport, 0, CLAP_EVENT_TRANSPORT);
    transport.flags  = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    if (time.playing) transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
    transport.tempo      = time.tempoBpm;
    transport.tsig_num   = static_cast<std::uint16_t>(time.timeSigNumerator);
    transport.tsig_denom = static_cast<std::uint16_t>(time.timeSigDenominator);
    // Beats timeline (quarter notes, CLAP's fixed point): what tempo-synced LFOs
    // and delays lock to.
    auto beatTime = [](double beats) {
        return static_cast<clap_beattime>(std::llround(beats * static_cast<double>(CLAP_BEATTIME_FACTOR)));
    };
    transport.song_pos_beats = beatTime(time.beatPosition);
    transport.bar_start      = beatTime(time.barStartBeats);
    transport.bar_number     = time.barNumber;
}

std::uint32_t ClapPlugin::convertAll(const EventBlock& events) noexcept {
    inEventCount = 0;
    for (std::uint32_t i = 0; i < events.count && inEventCount < inEvents.size(); ++i)
        if (convert(events.events[i], inEvents[inEventCount])) ++inEventCount;
    return inEventCount;
}

// The engine already chose a protocol this port accepts; this only maps by type:
// MIDI 1.0 channel voice (and system messages, when the port takes MIDI 1.0) as
// bytes; everything else as UMP words.
bool ClapPlugin::convert(const Event& event, EventStorage& storage) const noexcept {
    switch (event.type) {
        case EventType::Midi: {
            const ump::MessageType type     = ump::messageType(event.midi);
            const NoteDialectMask  dialects = event.portIndex < noteDialects.size() ? noteDialects[event.portIndex] : 0;
            const bool asBytes = type == ump::MessageType::Midi1ChannelVoice ||
                                 (type == ump::MessageType::System &&
                                  (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe)) != 0);
            if (asBytes) {
                clap_event_midi_t midi{};
                midi.header     = eventHeader(sizeof midi, event.sampleOffset, CLAP_EVENT_MIDI);
                midi.port_index = event.portIndex;
                if (ump::toMidi1Bytes(event.midi, midi.data) == 0) return false;
                storage.midi = midi;
                return true;
            }
            clap_event_midi2_t midi2{};
            midi2.header     = eventHeader(sizeof midi2, event.sampleOffset, CLAP_EVENT_MIDI2);
            midi2.port_index = event.portIndex;
            for (int i = 0; i < 4; ++i) midi2.data[i] = event.midi.words[i];
            storage.midi2 = midi2;
            return true;
        }
        case EventType::ParamValue: {
            clap_event_param_value_t value{};
            value.header     = eventHeader(sizeof value, event.sampleOffset, CLAP_EVENT_PARAM_VALUE);
            value.param_id   = event.param.id;
            value.cookie     = event.param.cookie;
            value.note_id    = -1;
            value.port_index = -1;
            value.channel    = -1;
            value.key        = -1;
            value.value      = event.value;
            storage.paramValue = value;
            return true;
        }
        case EventType::ParamMod: {
            clap_event_param_mod_t mod{};
            mod.header     = eventHeader(sizeof mod, event.sampleOffset, CLAP_EVENT_PARAM_MOD);
            mod.param_id   = event.param.id;
            mod.cookie     = event.param.cookie;
            mod.note_id    = -1;
            mod.port_index = -1;
            mod.channel    = -1;
            mod.key        = -1;
            mod.amount     = event.value;
            storage.paramMod = mod;
            return true;
        }
        case EventType::ParamGestureBegin:
        case EventType::ParamGestureEnd: {
            clap_event_param_gesture_t gesture{};
            gesture.header = eventHeader(sizeof gesture, event.sampleOffset,
                                         event.type == EventType::ParamGestureBegin
                                             ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                             : CLAP_EVENT_PARAM_GESTURE_END);
            gesture.param_id = event.param.id;
            storage.gesture  = gesture;
            return true;
        }
    }
    return false;
}

std::uint32_t ClapPlugin::inEventsSize(const clap_input_events_t* list) {
    return static_cast<const ClapPlugin*>(list->ctx)->inEventCount;
}

const clap_event_header_t* ClapPlugin::inEventsGet(const clap_input_events_t* list, std::uint32_t index) {
    const auto* self = static_cast<const ClapPlugin*>(list->ctx);
    return index < self->inEventCount ? &self->inEvents[index].header : nullptr;
}

// What the plugin sends goes to the engine's sink with its sample offset:
// parameter feedback (its own GUI, internal modulation), and notes and MIDI (a
// note effect's output). MIDI keeps its protocol, as on input: CLAP_EVENT_MIDI
// as MT 0x2 (0x1 for system messages), CLAP_EVENT_MIDI2 as it is. A CLAP note
// on or off becomes a MIDI 2.0 one in group 1, keeping the velocity's
// resolution; one with a wildcard key or channel has no message, and a note
// end or choke reports a voice, not a message. SysEx and note expressions are
// dropped for now.
bool ClapPlugin::outEventsTryPush(const clap_output_events_t* list, const clap_event_header_t* header) {
    auto* self = static_cast<ClapPlugin*>(list->ctx);
    if (self->currentSink == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;

    Event event;
    event.sampleOffset = header->time;
    switch (header->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* v = reinterpret_cast<const clap_event_param_value_t*>(header);
            event.type    = EventType::ParamValue;
            event.param   = ParamRef{v->param_id, v->cookie};
            event.value   = v->value;
            break;
        }
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END: {
            const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(header);
            event.type    = header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? EventType::ParamGestureBegin
                                                                           : EventType::ParamGestureEnd;
            event.param   = ParamRef{g->param_id, nullptr};
            break;
        }
        case CLAP_EVENT_MIDI: {
            const auto* m = reinterpret_cast<const clap_event_midi_t*>(header);
            if (!ump::fromMidi1Bytes(m->data, 3, 0, event.midi)) return true;
            event.portIndex = m->port_index;
            break;
        }
        case CLAP_EVENT_MIDI2: {
            const auto* m = reinterpret_cast<const clap_event_midi2_t*>(header);
            for (int i = 0; i < 4; ++i) event.midi.words[i] = m->data[i];
            event.midi.wordCount = ump::wordCountForType(static_cast<std::uint8_t>(m->data[0] >> 28));
            event.portIndex      = m->port_index;
            break;
        }
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF: {
            const auto* n = reinterpret_cast<const clap_event_note_t*>(header);
            if (n->key < 0 || n->key > 127 || n->channel < 0 || n->channel > 15) return true;
            const std::uint32_t status   = header->type == CLAP_EVENT_NOTE_ON ? 0x9u : 0x8u;
            const double        velocity = std::clamp(n->velocity, 0.0, 1.0);
            event.midi.words[0]  = (0x4u << 28) | (status << 20) | (static_cast<std::uint32_t>(n->channel) << 16) |
                                  (static_cast<std::uint32_t>(n->key) << 8);
            event.midi.words[1]  = static_cast<std::uint32_t>(std::lround(velocity * 65535.0)) << 16;
            event.midi.wordCount = 2;
            event.portIndex      = n->port_index < 0 ? 0 : static_cast<std::uint16_t>(n->port_index);
            break;
        }
        default:
            return true;
    }
    self->currentSink->push(event);
    return true;
}

// --- main-thread servicing, params, state ------------------------------------------------

void ClapPlugin::idle() {
    if (plugin == nullptr) return;
    if (pendingGuiDestroy) {   // acknowledge a gui the plugin destroyed itself (CLAP gui.closed)
        guiExt->destroy(plugin);
        pendingGuiDestroy = false;
    }
    if (callbackRequested.exchange(false)) plugin->on_main_thread(plugin);
    // While active, process() carries parameter changes both ways, and a flush
    // would belong on the audio thread (CLAP). Only flush here when inactive.
    if (flushRequested.exchange(false) && !active && paramsExt != nullptr) {
        inEventCount = 0;
        paramsExt->flush(plugin, &inList, &outList);
    }
}

void ClapPlugin::flushParams(const EventBlock& in, EventSink* out) {
    if (paramsExt == nullptr || active) return;   // see idle()
    convertAll(in);
    currentSink = out;
    paramsExt->flush(plugin, &inList, &outList);
    currentSink = nullptr;
}

bool ClapPlugin::saveState(std::vector<std::byte>& out) const {
    if (stateExt == nullptr) return false;
    clap_ostream_t stream{};
    stream.ctx   = &out;
    stream.write = [](const clap_ostream_t* s, const void* buffer, std::uint64_t size) -> std::int64_t {
        auto*       target = static_cast<std::vector<std::byte>*>(s->ctx);
        const auto* bytes  = static_cast<const std::byte*>(buffer);
        target->insert(target->end(), bytes, bytes + size);
        return static_cast<std::int64_t>(size);
    };
    return stateExt->save(plugin, &stream);
}

bool ClapPlugin::loadState(const std::byte* data, std::size_t size) {
    if (stateExt == nullptr) return false;
    struct Reader {
        const std::byte* data;
        std::size_t      size;
        std::size_t      position;
    } reader{data, size, 0};

    clap_istream_t stream{};
    stream.ctx  = &reader;
    stream.read = [](const clap_istream_t* s, void* buffer, std::uint64_t wanted) -> std::int64_t {
        auto*             r = static_cast<Reader*>(s->ctx);
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(wanted), r->size - r->position);
        std::memcpy(buffer, r->data + r->position, n);
        r->position += n;
        return static_cast<std::int64_t>(n);
    };
    return stateExt->load(plugin, &stream);
}

// --- editor (main thread) -------------------------------------------------------------
// CLAP's order: create -> (set_scale) -> can_resize -> get_size -> set_parent ->
// show ... hide -> destroy. set_scale is skipped: on X11 so the plugin picks its
// own DPI (architecture.md section 4); on macOS sizes are in points and CLAP
// says set_scale doesn't apply.

namespace {
#ifdef __APPLE__
constexpr const char* kWindowApi = CLAP_WINDOW_API_COCOA;   // the parent is an NSView*
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;     // the parent is an X11 Window
#endif
} // namespace

bool ClapPlugin::hasEditor() const {
    return guiExt != nullptr && guiExt->is_api_supported(plugin, kWindowApi, false);
}

bool ClapPlugin::openEditor(NativeWindow parent, EditorHost& host, EditorSize& size) {
    if (!hasEditor() || editorHost != nullptr || pendingGuiDestroy) return false;
    editorHost = &host;   // the plugin may call back (e.g. request_resize) from here on
    if (!guiExt->create(plugin, kWindowApi, false)) {
        report(LogLevel::Error, "gui create failed");
        editorHost = nullptr;
        return false;
    }
    std::uint32_t width = 0, height = 0;
    if (!guiExt->get_size(plugin, &width, &height) || width == 0 || height == 0) {
        width  = 640;
        height = 480;
    }
    clap_window_t window{};
    window.api = kWindowApi;
#ifdef __APPLE__
    window.cocoa = reinterpret_cast<clap_nsview>(parent);
#else
    window.x11 = static_cast<clap_xwnd>(parent);
#endif
    if (!guiExt->set_parent(plugin, &window)) {
        report(LogLevel::Error, "gui set_parent failed");
        guiExt->destroy(plugin);
        editorHost = nullptr;
        return false;
    }
    guiExt->show(plugin);
    size = EditorSize{width, height};
    return true;
}

void ClapPlugin::closeEditor() {
    if (editorHost == nullptr) return;
    editorHost = nullptr;   // no more requests to the host window
    guiExt->hide(plugin);
    guiExt->destroy(plugin);
}

bool ClapPlugin::editorResizable() const {
    return editorHost != nullptr && guiExt->can_resize(plugin);
}

bool ClapPlugin::setEditorSize(EditorSize& size) {
    if (editorHost == nullptr) return false;
    std::uint32_t width = size.width, height = size.height;
    if (guiExt->can_resize(plugin)) guiExt->adjust_size(plugin, &width, &height);
    if (!guiExt->set_size(plugin, width, height)) return false;
    size = EditorSize{width, height};
    return true;
}

void ClapPlugin::report(LogLevel level, const char* what) const {
    char line[512];
    std::snprintf(line, sizeof line, "%s: %s", desc.name.c_str(), what);
    hostServices.log(level, line);
}

// --- host side ------------------------------------------------------------------------

ClapPlugin* ClapPlugin::from(const clap_host_t* host) noexcept {
    return static_cast<ClapPlugin*>(host->host_data);
}

const void* ClapPlugin::hostGetExtension(const clap_host_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_LOG) == 0)          return &kHostLog;
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) return &kHostThreadCheck;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)       return &kHostParams;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)        return &kHostState;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)      return &kHostLatency;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)  return &kHostAudioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)   return &kHostNotePorts;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)          return &kHostGui;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)     return &kHostTimerSupport;
#ifndef __APPLE__   // macOS plugins use the main run loop, not the host's fds (StageHand's docs/macos.md)
    if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)  return &kHostPosixFdSupport;
#endif
    return nullptr;
}

void ClapPlugin::hostRequestRestart(const clap_host_t* host) {
    from(host)->hostServices.requestRestart();
}

void ClapPlugin::hostRequestProcess(const clap_host_t* host) {
    from(host)->hostServices.requestProcess();
}

void ClapPlugin::hostRequestCallback(const clap_host_t* host) {
    from(host)->callbackRequested.store(true);
}

// CLAP allows logging from any thread; from the audio thread this does I/O, so
// it's for diagnostics only.
void ClapPlugin::hostLog(const clap_host_t* host, clap_log_severity severity, const char* msg) {
    LogLevel level = LogLevel::Info;
    switch (severity) {
        case CLAP_LOG_DEBUG:   level = LogLevel::Debug; break;
        case CLAP_LOG_INFO:    level = LogLevel::Info; break;
        case CLAP_LOG_WARNING: level = LogLevel::Warning; break;
        default:               level = LogLevel::Error; break;
    }
    from(host)->report(level, msg != nullptr ? msg : "");
}

bool ClapPlugin::hostIsMainThread(const clap_host_t* host) {
    return std::this_thread::get_id() == from(host)->mainThread;
}

bool ClapPlugin::hostIsAudioThread(const clap_host_t*) {
    return tlsAudioThread;
}

void ClapPlugin::hostParamsRescan(const clap_host_t*, clap_param_rescan_flags) {}   // no host-side param cache yet
void ClapPlugin::hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}

void ClapPlugin::hostParamsRequestFlush(const clap_host_t* host) {
    ClapPlugin* self = from(host);
    self->flushRequested.store(true);
    self->hostServices.requestParamFlush();
}

void ClapPlugin::hostMarkDirty(const clap_host_t* host) {
    from(host)->hostServices.stateChanged();
}

void ClapPlugin::hostLatencyChanged(const clap_host_t* host) {
    from(host)->report(LogLevel::Info, "latency changed (not compensated yet)");
}

bool ClapPlugin::hostAudioPortsRescanFlagSupported(const clap_host_t*, std::uint32_t) {
    return false;
}

void ClapPlugin::hostAudioPortsRescan(const clap_host_t* host, std::uint32_t) {
    from(host)->report(LogLevel::Warning, "audio ports changed; reload the plugin to pick them up");
}

std::uint32_t ClapPlugin::hostNotePortsDialects(const clap_host_t*) {
    return CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI2;
}

void ClapPlugin::hostNotePortsRescan(const clap_host_t* host, std::uint32_t) {
    from(host)->report(LogLevel::Warning, "note ports changed; reload the plugin to pick them up");
}

// --- host gui: the plugin's requests about its editor go to its EditorHost.
// EditorHost is main-thread only; CLAP allows some of these from other threads,
// which are declined rather than marshalled for now.

void ClapPlugin::hostGuiResizeHintsChanged(const clap_host_t* host) {
    ClapPlugin* self = from(host);
    if (self->editorHost != nullptr && hostIsMainThread(host)) self->editorHost->editorResizeHintsChanged();
}

bool ClapPlugin::hostGuiRequestResize(const clap_host_t* host, std::uint32_t width, std::uint32_t height) {
    ClapPlugin* self = from(host);
    if (self->editorHost == nullptr || !hostIsMainThread(host)) return false;
    return self->editorHost->requestEditorResize(EditorSize{width, height});
}

bool ClapPlugin::hostGuiRequestShow(const clap_host_t* host) {
    ClapPlugin* self = from(host);
    return self->editorHost != nullptr && hostIsMainThread(host) && self->editorHost->requestEditorShown(true);
}

bool ClapPlugin::hostGuiRequestHide(const clap_host_t* host) {
    ClapPlugin* self = from(host);
    return self->editorHost != nullptr && hostIsMainThread(host) && self->editorHost->requestEditorShown(false);
}

// wasDestroyed: the plugin lost its gui and the host must acknowledge with
// gui.destroy() (done from idle(), not inside the plugin's own call).
void ClapPlugin::hostGuiClosed(const clap_host_t* host, bool wasDestroyed) {
    ClapPlugin* self = from(host);
    EditorHost* editor = self->editorHost;
    if (editor == nullptr || !hostIsMainThread(host)) return;
    if (!wasDestroyed) {
        editor->requestEditorShown(false);
        return;
    }
    self->editorHost        = nullptr;
    self->pendingGuiDestroy = true;
    editor->editorClosed();
}

// --- host timer-support / posix-fd-support: the host's main loop, on behalf of
// the plugin. Every registration is tracked so none outlives the plugin.

bool ClapPlugin::hostRegisterTimer(const clap_host_t* host, std::uint32_t periodMs, clap_id* timerId) {
    ClapPlugin* self = from(host);
    if (self->timerExt == nullptr || timerId == nullptr) return false;
    const clap_id id    = self->nextTimerId++;
    const TimerId timer = self->hostServices.addTimer(periodMs, [self, id] {
        self->timerExt->on_timer(self->plugin, id);
    });
    if (timer == kInvalidTimer) return false;
    self->timers[id] = timer;
    *timerId         = id;
    return true;
}

bool ClapPlugin::hostUnregisterTimer(const clap_host_t* host, clap_id timerId) {
    ClapPlugin* self = from(host);
    const auto  it   = self->timers.find(timerId);
    if (it == self->timers.end()) return false;
    self->hostServices.removeTimer(it->second);
    self->timers.erase(it);
    return true;
}

bool ClapPlugin::hostRegisterFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
    ClapPlugin* self = from(host);
    if (self->fdExt == nullptr) return false;
    const bool watched = self->hostServices.watchFd(fd, flags, [self, fd](std::uint32_t events) {
        self->fdExt->on_fd(self->plugin, fd, events);
    });
    if (watched) self->watchedFds.insert(fd);
    return watched;
}

bool ClapPlugin::hostModifyFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
    ClapPlugin* self = from(host);
    return self->watchedFds.count(fd) != 0 && self->hostServices.modifyFd(fd, flags);
}

bool ClapPlugin::hostUnregisterFd(const clap_host_t* host, int fd) {
    ClapPlugin* self = from(host);
    if (self->watchedFds.erase(fd) == 0) return false;
    self->hostServices.unwatchFd(fd);
    return true;
}

} // namespace uw
