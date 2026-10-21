#pragma once

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "uw/plugin/HostServices.h"
#include "uw/plugin/HostedPlugin.h"

namespace uw {

class ClapModule;

// HostedPlugin for one CLAP plugin instance: the only code that speaks CLAP's
// plugin API. It translates the engine's already-routed events into CLAP events
// (MT 0x2/0x1 -> clap_event_midi, MT 0x4 -> clap_event_midi2, params ->
// param events) and implements the host side CLAP plugins call back into (log,
// thread-check, params, state, latency, audio-ports, note-ports, gui,
// timer-support, posix-fd-support). Editors embed via X11 only.
class ClapPlugin final : public HostedPlugin {
public:
    // Create and init the plugin `desc.id` from a loaded module. Returns null
    // (after logging why) on failure.
    static std::unique_ptr<ClapPlugin> create(std::shared_ptr<ClapModule> module,
                                              const PluginDescriptor& desc, HostServices& host);
    ~ClapPlugin() override;

    ClapPlugin(const ClapPlugin&)            = delete;
    ClapPlugin& operator=(const ClapPlugin&) = delete;

    const PluginDescriptor&    descriptor() const override { return desc; }
    PortLayout                 portLayout() const override;
    std::vector<ParameterInfo> parameters() const override;
    bool paramValueText(std::uint32_t id, double value, std::string& text) const override;
    bool                       activate(double sampleRate, std::uint32_t maxBlockFrames) override;
    void                       deactivate() override;
    void                       process(const ProcessContext& ctx) noexcept override;
    void                       stopProcessing() noexcept override;
    void                       reset() noexcept override;
    double                     paramValue(std::uint32_t id) const override;
    void                       flushParams(const EventBlock& in, EventSink* out) override;
    bool                       saveState(std::vector<std::byte>& out) const override;
    bool                       loadState(const std::byte* data, std::size_t size) override;
    void                       idle() override;

    bool hasEditor() const override;
    bool openEditor(NativeWindow parent, EditorHost& host, EditorSize& size) override;
    void closeEditor() override;
    bool editorResizable() const override;
    bool setEditorSize(EditorSize& size) override;

private:
    union EventStorage {
        clap_event_header_t        header;
        clap_event_midi_t          midi;
        clap_event_midi2_t         midi2;
        clap_event_param_value_t   paramValue;
        clap_event_param_mod_t     paramMod;
        clap_event_param_gesture_t gesture;
    };

    ClapPlugin(std::shared_ptr<ClapModule> loadedModule, const PluginDescriptor& pluginDesc,
               HostServices& host);

    void          prepareBuffers(const PortLayout& layout, std::uint32_t maxBlockFrames);
    bool          convert(const Event& event, EventStorage& storage) const noexcept;
    std::uint32_t convertAll(const EventBlock& events) noexcept;
    void          fillTransport(const TimeInfo& time) noexcept;
    void          report(LogLevel level, const char* what) const;

    // --- host side (called by the plugin) ---
    static ClapPlugin*  from(const clap_host_t* host) noexcept;
    static const void*  hostGetExtension(const clap_host_t* host, const char* id);
    static void         hostRequestRestart(const clap_host_t* host);
    static void         hostRequestProcess(const clap_host_t* host);
    static void         hostRequestCallback(const clap_host_t* host);
    static void         hostLog(const clap_host_t* host, clap_log_severity severity, const char* msg);
    static bool         hostIsMainThread(const clap_host_t* host);
    static bool         hostIsAudioThread(const clap_host_t* host);
    static void         hostParamsRescan(const clap_host_t* host, clap_param_rescan_flags flags);
    static void         hostParamsClear(const clap_host_t* host, clap_id id, clap_param_clear_flags flags);
    static void         hostParamsRequestFlush(const clap_host_t* host);
    static void         hostMarkDirty(const clap_host_t* host);
    static void         hostLatencyChanged(const clap_host_t* host);
    static bool         hostAudioPortsRescanFlagSupported(const clap_host_t* host, std::uint32_t flag);
    static void         hostAudioPortsRescan(const clap_host_t* host, std::uint32_t flags);
    static std::uint32_t hostNotePortsDialects(const clap_host_t* host);
    static void         hostNotePortsRescan(const clap_host_t* host, std::uint32_t flags);
    static void         hostGuiResizeHintsChanged(const clap_host_t* host);
    static bool         hostGuiRequestResize(const clap_host_t* host, std::uint32_t width, std::uint32_t height);
    static bool         hostGuiRequestShow(const clap_host_t* host);
    static bool         hostGuiRequestHide(const clap_host_t* host);
    static void         hostGuiClosed(const clap_host_t* host, bool wasDestroyed);
    static bool         hostRegisterTimer(const clap_host_t* host, std::uint32_t periodMs, clap_id* timerId);
    static bool         hostUnregisterTimer(const clap_host_t* host, clap_id timerId);
    static bool         hostRegisterFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags);
    static bool         hostModifyFd(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags);
    static bool         hostUnregisterFd(const clap_host_t* host, int fd);

    static std::uint32_t              inEventsSize(const clap_input_events_t* list);
    static const clap_event_header_t* inEventsGet(const clap_input_events_t* list, std::uint32_t index);
    static bool outEventsTryPush(const clap_output_events_t* list, const clap_event_header_t* event);

    static const clap_host_log_t          kHostLog;
    static const clap_host_thread_check_t kHostThreadCheck;
    static const clap_host_params_t       kHostParams;
    static const clap_host_state_t        kHostState;
    static const clap_host_latency_t      kHostLatency;
    static const clap_host_audio_ports_t  kHostAudioPorts;
    static const clap_host_note_ports_t   kHostNotePorts;
    static const clap_host_gui_t              kHostGui;
    static const clap_host_timer_support_t    kHostTimerSupport;
    static const clap_host_posix_fd_support_t kHostPosixFdSupport;

    void removeMainLoopRegistrations();

    std::shared_ptr<ClapModule> module;
    PluginDescriptor            desc;
    HostServices&               hostServices;
    HostInfo                    hostInfo;      // clapHost's strings point into it
    clap_host_t                 clapHost{};
    const clap_plugin_t*        plugin = nullptr;
    std::thread::id             mainThread;

    const clap_plugin_audio_ports_t* audioPortsExt = nullptr;
    const clap_plugin_note_ports_t*  notePortsExt  = nullptr;
    const clap_plugin_params_t*      paramsExt     = nullptr;
    const clap_plugin_state_t*       stateExt      = nullptr;
    const clap_plugin_gui_t*              guiExt   = nullptr;
    const clap_plugin_timer_support_t*    timerExt = nullptr;
    const clap_plugin_posix_fd_support_t* fdExt    = nullptr;

    // Editor and main-loop registrations (main thread).
    EditorHost*                 editorHost        = nullptr;   // non-null while the editor exists
    bool                        pendingGuiDestroy = false;     // plugin reported its gui destroyed
    std::map<clap_id, TimerId>  timers;                        // the plugin's id -> the host's
    clap_id                     nextTimerId = 1;
    std::set<int>               watchedFds;

    bool              active     = false;
    bool              processing = false;   // audio thread (and the destructor, after audio stops)
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> flushRequested{false};

    // Process scratch, sized at activate() (events at construction).
    std::vector<std::vector<float>>  portAudio;
    std::vector<std::vector<float*>> portChannels;
    std::vector<clap_audio_buffer_t> inputBuffers;
    std::vector<clap_audio_buffer_t> outputBuffers;
    std::vector<NoteDialectMask>     noteDialects;
    int                              mainOutput = -1;
    // The main audio input: fed from ProcessContext::audioIn when the engine
    // gives audio (an effect on a bus), else its silent scratch buffer.
    int                              mainInput        = -1;
    float**                          mainInputSilence = nullptr;
    std::vector<EventStorage>        inEvents;
    std::uint32_t                    inEventCount = 0;
    clap_input_events_t              inList{};
    clap_output_events_t             outList{};
    clap_event_transport_t           transport{};
    EventSink*                       currentSink = nullptr;
};

} // namespace uw
