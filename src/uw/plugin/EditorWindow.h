#pragma once

#include <functional>
#include <memory>
#include <string>

#include "uw/plugin/Editor.h"
#include "uw/plugin/HostServices.h"
#include "uw/plugin/HostedPlugin.h"

#ifndef __APPLE__
typedef struct _XDisplay Display;
#endif

namespace uw {

// A top-level window hosting one plugin's editor: an X11 window on Linux
// (through XWayland under Wayland), an NSWindow on macOS, whose content is
// the plugin's own embedded editor (Editor.h). Not the app's own UI: a
// plain host window, so an app with its own windowing (DGL, Qt, a bespoke
// Xlib or Cocoa UI) can open one without sharing a toolkit. Moved here from
// StageHand (its App/EditorWindow.{h,cpp}, App/EditorWindowMac.{h,mm}) so
// Umpire can host plugin editors too (its docs/plugins.md, milestone 11).
//
// Linux services its X11 connection through `loop` (HostServices'
// watchFd()/unwatchFd() -- the same services the plugin's own editor may
// ask the host for, CLAP's timer-support and posix-fd-support); macOS's
// AppKit drains its own window's events on its own run loop and doesn't
// need it. `requestQuit`, if given, runs on Ctrl-Q (Linux) or Cmd-Q
// (macOS) from this window, as from the app's own window; `save`, if
// given, on Ctrl-Shift-S / Cmd-Shift-S. Main thread only.
//
// Closing the window (its close button) closes that editor only: whatever
// else the app is doing (audio, playback) keeps running.
class EditorWindow final : public EditorHost {
public:
    // Open a window titled `title`, embed the plugin's editor in it, and
    // show it. Returns null (after printing why) if the plugin has no
    // editor this platform's window system can embed, or it fails to open.
    static std::unique_ptr<EditorWindow> open(HostedPlugin& plugin, HostServices& loop, const std::string& title,
                                              std::function<void()> requestQuit = {},
                                              std::function<void()> save        = {});
    ~EditorWindow() override;   // closes the editor, then the window

    EditorWindow(const EditorWindow&)            = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // False once the user (or the plugin) closed it.
    bool isOpen() const noexcept;

    // --- EditorHost ---
    bool requestEditorResize(EditorSize size) override;
    bool requestEditorShown(bool shown) override;
    void editorResizeHintsChanged() override;
    void editorClosed() override;

    // Linux only: the X11 connection's fd became readable (its watchFd()
    // callback, registered in open()). Not for other callers.
    void handleEvents();
    // macOS only: from the window's delegate and key monitor
    // (EditorWindowMac.mm). Not for other callers.
    void closeButtonPressed();
    void windowResized();
    bool handleKey(const std::string& characters, bool command, bool shift);   // true: consumed

private:
    EditorWindow(HostedPlugin& editorPlugin, HostServices& loopServices, std::function<void()> quit);

    void close(bool closeEditor);
    void applySizeHints();   // fixed-size vs resizable, from the plugin

    HostedPlugin&          plugin;
    HostServices&          loop;
    std::function<void()>  requestQuitAction;
    std::function<void()>  saveAction;
    EditorSize             size;
    bool                   editorOpen = false;

#ifdef __APPLE__
    void setContentSize(EditorSize wanted);

    void*       window     = nullptr;   // NSWindow*, retained
    void*       delegate   = nullptr;   // its delegate, retained
    void*       keyMonitor = nullptr;   // an NSEvent local monitor
    bool        resizingNow = false;    // our own setContentSize, not the user's
    // Cleared when the window goes, so a confirmation queued for later finds it gone.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
#else
    Display*      display     = nullptr;
    unsigned long  window      = 0;      // X11 Window
    unsigned long  deleteAtom  = 0;      // WM_DELETE_WINDOW
    unsigned int   quitKeycode = 0;      // Q, grabbed with Control
    unsigned int   saveKeycode = 0;      // S, grabbed with Control+Shift (if saveAction)
    bool           confirmSize = false;  // a plugin-requested resize awaits setEditorSize()
#endif
};

} // namespace uw
