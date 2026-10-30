#include "uw/plugin/EditorWindow.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>

namespace uw {

EditorWindow::EditorWindow(HostedPlugin& editorPlugin, HostServices& loopServices, std::function<void()> quit)
    : plugin(editorPlugin), loop(loopServices), requestQuitAction(std::move(quit)) {}

std::unique_ptr<EditorWindow> EditorWindow::open(HostedPlugin& plugin, HostServices& loop, const std::string& title,
                                                 std::function<void()> requestQuit, std::function<void()> save) {
    if (!plugin.hasEditor()) {
        std::printf("%s has no editor that can be embedded in an X11 window.\n", plugin.descriptor().name.c_str());
        return nullptr;
    }
    std::unique_ptr<EditorWindow> self(new EditorWindow(plugin, loop, std::move(requestQuit)));
    self->saveAction = std::move(save);

    self->display = XOpenDisplay(nullptr);
    if (self->display == nullptr) {
        const char* name = std::getenv("DISPLAY");
        std::fprintf(stderr, "EditorWindow: cannot open X display '%s'\n", name != nullptr ? name : "");
        return nullptr;
    }
    Display* const d      = self->display;
    const int      screen = DefaultScreen(d);

    // A placeholder size until the editor reports its own.
    self->window = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, 400, 300, 0, BlackPixel(d, screen),
                                       BlackPixel(d, screen));
    XStoreName(d, self->window, title.c_str());
    self->deleteAtom = XInternAtom(d, "WM_DELETE_WINDOW", False);
    Atom protocols   = self->deleteAtom;
    XSetWMProtocols(d, self->window, &protocols, 1);
    XSelectInput(d, self->window, StructureNotifyMask | KeyPressMask);

    // Ctrl-Q (quit) and Ctrl-Shift-S (save now) work from any editor window. A
    // passive grab on our window fires even while focus is in the plugin's
    // embedded child window. Grab under Caps Lock / Num Lock too, which X treats
    // as modifiers.
    const unsigned int lockCombinations[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    auto grab = [&](unsigned int keycode, unsigned int modifiers) {
        if (keycode == 0) return;
        for (const unsigned int locks : lockCombinations)
            XGrabKey(d, static_cast<int>(keycode), modifiers | locks, self->window, True, GrabModeAsync, GrabModeAsync);
    };
    if (self->requestQuitAction) {
        self->quitKeycode = XKeysymToKeycode(d, XK_q);
        grab(self->quitKeycode, ControlMask);
    }
    if (self->saveAction) {
        self->saveKeycode = XKeysymToKeycode(d, XK_s);
        grab(self->saveKeycode, ControlMask | ShiftMask);
    }

    // Embed before mapping, so the window appears at the editor's size.
    EditorSize size;
    if (!plugin.openEditor(self->window, *self, size)) {
        std::fprintf(stderr, "EditorWindow: %s's editor failed to open\n", plugin.descriptor().name.c_str());
        self->close(false);
        return nullptr;
    }
    self->editorOpen = true;
    self->size       = size;
    XResizeWindow(d, self->window, size.width, size.height);
    self->applySizeHints();
    XMapWindow(d, self->window);
    XFlush(d);

    EditorWindow* const raw = self.get();
    loop.watchFd(ConnectionNumber(d), FdEvent::Read, [raw](std::uint32_t) { raw->handleEvents(); });
    return self;
}

EditorWindow::~EditorWindow() { close(true); }

bool EditorWindow::isOpen() const noexcept { return window != 0; }

void EditorWindow::close(bool closeEditor) {
    if (editorOpen && closeEditor) plugin.closeEditor();   // the editor goes before its parent window
    editorOpen = false;
    if (display != nullptr) {
        loop.unwatchFd(ConnectionNumber(display));
        if (window != 0) XDestroyWindow(display, window);
        XCloseDisplay(display);
    }
    window  = 0;
    display = nullptr;
}

void EditorWindow::handleEvents() {
    while (display != nullptr && XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        switch (event.type) {
            case KeyPress: {
                const bool control = (event.xkey.state & ControlMask) != 0;
                const bool shift   = (event.xkey.state & ShiftMask) != 0;
                if (control && event.xkey.keycode == quitKeycode && requestQuitAction) {
                    requestQuitAction();
                } else if (control && shift && event.xkey.keycode == saveKeycode && saveAction) {
                    saveAction();
                    if (display == nullptr) return;   // (defensive: the action closed us)
                }
                break;
            }
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == deleteAtom) {
                    close(true);   // the editor only: whatever else runs keeps running
                    return;
                }
                break;
            case ConfigureNotify: {
                const EditorSize now{static_cast<std::uint32_t>(event.xconfigure.width),
                                     static_cast<std::uint32_t>(event.xconfigure.height)};
                if (now.width == size.width && now.height == size.height) {
                    // Our own resize landing: confirm a size the plugin asked for.
                    if (confirmSize) {
                        EditorSize confirmed = size;
                        plugin.setEditorSize(confirmed);
                        confirmSize = false;
                    }
                    break;
                }
                if (!plugin.editorResizable()) break;   // size hints should prevent this
                EditorSize adjusted = now;               // the user resized the window
                if (plugin.setEditorSize(adjusted)) {
                    size = adjusted;
                    if (adjusted.width != now.width || adjusted.height != now.height)
                        XResizeWindow(display, window, adjusted.width, adjusted.height);
                }
                break;
            }
            default:
                break;
        }
    }
}

// Fixed-size editors pin the window's min = max size; resizable ones only get a
// small minimum.
void EditorWindow::applySizeHints() {
    if (display == nullptr || window == 0) return;
    XSizeHints* hints = XAllocSizeHints();
    if (hints == nullptr) return;
    if (plugin.editorResizable()) {
        hints->flags      = PMinSize;
        hints->min_width  = 64;
        hints->min_height = 64;
    } else {
        hints->flags      = PMinSize | PMaxSize;
        hints->min_width  = hints->max_width  = static_cast<int>(size.width);
        hints->min_height = hints->max_height = static_cast<int>(size.height);
    }
    XSetWMNormalHints(display, window, hints);
    XFree(hints);
}

// --- EditorHost ---------------------------------------------------------------------

bool EditorWindow::requestEditorResize(EditorSize wanted) {
    if (display == nullptr || window == 0) return false;
    size        = wanted;
    confirmSize = true;   // set_size once the resize lands (ConfigureNotify)
    applySizeHints();     // a fixed-size editor's min = max must move first
    XResizeWindow(display, window, wanted.width, wanted.height);
    XFlush(display);
    return true;
}

bool EditorWindow::requestEditorShown(bool shown) {
    if (display == nullptr || window == 0) return false;
    if (shown) XMapWindow(display, window);
    else XUnmapWindow(display, window);
    XFlush(display);
    return true;
}

void EditorWindow::editorResizeHintsChanged() {
    applySizeHints();
    if (display != nullptr) XFlush(display);
}

void EditorWindow::editorClosed() {
    editorOpen = false;   // the plugin already tore it down
    close(false);
}

} // namespace uw
