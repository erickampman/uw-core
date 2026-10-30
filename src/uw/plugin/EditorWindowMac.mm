#include "uw/plugin/EditorWindow.h"

#import <AppKit/AppKit.h>

#include <cstdio>

// The window's delegate: forwards the close button and resizes. Its own class
// name, so it can't collide with a hosted plugin's Objective-C classes
// (decisions.md, "Hosting CLAP instruments": two of the author's own plugins
// already collide with each other this way).
@interface UwCoreEditorWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) uw::EditorWindow* owner;
@end

@implementation UwCoreEditorWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.owner != nullptr) self.owner->closeButtonPressed();   // closes the window itself
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) self.owner->windowResized();
}
@end

namespace uw {

namespace {

NSWindow* asWindow(void* window) { return (__bridge NSWindow*)window; }

} // namespace

EditorWindow::EditorWindow(HostedPlugin& editorPlugin, HostServices& loopServices, std::function<void()> quit)
    : plugin(editorPlugin), loop(loopServices), requestQuitAction(std::move(quit)) {}

std::unique_ptr<EditorWindow> EditorWindow::open(HostedPlugin& plugin, HostServices& loop, const std::string& title,
                                                 std::function<void()> requestQuit, std::function<void()> save) {
    if (!plugin.hasEditor()) {
        std::printf("%s has no editor that can be embedded in a Cocoa window.\n", plugin.descriptor().name.c_str());
        return nullptr;
    }
    std::unique_ptr<EditorWindow> self(new EditorWindow(plugin, loop, std::move(requestQuit)));
    self->saveAction = std::move(save);

    @autoreleasepool {
        // A command-line binary isn't a regular app until it says so: no Dock
        // icon, and its windows can't take the keyboard.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // A placeholder size until the editor reports its own.
        NSWindow* const window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.releasedWhenClosed = NO;
        window.title              = [NSString stringWithUTF8String:title.c_str()];
        self->window              = (__bridge_retained void*)window;

        UwCoreEditorWindowDelegate* const delegate = [[UwCoreEditorWindowDelegate alloc] init];
        delegate.owner                              = self.get();
        window.delegate                             = delegate;
        self->delegate                              = (__bridge_retained void*)delegate;

        // Embed before showing, so the window appears at the editor's size.
        EditorSize size;
        if (!plugin.openEditor(reinterpret_cast<NativeWindow>((__bridge void*)window.contentView), *self, size)) {
            std::fprintf(stderr, "EditorWindow: %s's editor failed to open\n", plugin.descriptor().name.c_str());
            self->close(false);
            return nullptr;
        }
        self->editorOpen = true;
        self->size       = size;
        self->applySizeHints();
        self->setContentSize(size);
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activate];

        // Cmd-Q and Cmd-Shift-S from this window, before the plugin sees them.
        EditorWindow* const raw = self.get();
        self->keyMonitor        = (__bridge_retained void*)[NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                         handler:^NSEvent*(NSEvent* event) {
                                             if (event.window != asWindow(raw->window)) return event;
                                             const NSEventModifierFlags flags = event.modifierFlags;
                                             const char* const chars = event.charactersIgnoringModifiers.UTF8String;
                                             return raw->handleKey(chars != nullptr ? chars : "",
                                                                   (flags & NSEventModifierFlagCommand) != 0,
                                                                   (flags & NSEventModifierFlagShift) != 0)
                                                        ? nil
                                                        : event;
                                         }];
    }
    return self;
}

EditorWindow::~EditorWindow() { close(true); }

bool EditorWindow::isOpen() const noexcept { return window != nullptr; }

void EditorWindow::close(bool closeEditor) {
    if (editorOpen && closeEditor) plugin.closeEditor();   // the editor goes before its parent window
    editorOpen = false;
    *alive     = false;
    @autoreleasepool {
        if (keyMonitor != nullptr) {
            [NSEvent removeMonitor:(__bridge_transfer id)keyMonitor];
            keyMonitor = nullptr;
        }
        NSWindow* const                   w = window != nullptr ? (__bridge_transfer NSWindow*)window : nil;
        UwCoreEditorWindowDelegate* const d =
            delegate != nullptr ? (__bridge_transfer UwCoreEditorWindowDelegate*)delegate : nil;
        window     = nullptr;
        delegate   = nullptr;
        d.owner    = nullptr;
        w.delegate = nil;
        [w orderOut:nil];
        [w close];
        // The close button lands here from inside the window's own close
        // handling (its delegate's windowShouldClose:): both must outlive it,
        // so the last references go once this event is done.
        if (w != nil || d != nil) dispatch_async(dispatch_get_main_queue(), ^{
            (void)w;
            (void)d;
        });
    }
}

bool EditorWindow::handleKey(const std::string& characters, bool command, bool shift) {
    if (!command) return false;
    if (characters == "q" && requestQuitAction) {
        requestQuitAction();
        return true;
    }
    if (shift && (characters == "s" || characters == "S") && saveAction) {
        saveAction();
        return true;
    }
    return false;
}

void EditorWindow::closeButtonPressed() { close(true); }   // the editor only: whatever else runs keeps running

// Fixed-size editors pin the content's min = max size; resizable ones only get a
// small minimum.
void EditorWindow::applySizeHints() {
    if (window == nullptr) return;
    NSWindow* const w         = asWindow(window);
    const bool      resizable = plugin.editorResizable();
    if (resizable) {
        w.styleMask |= NSWindowStyleMaskResizable;
        w.contentMinSize = NSMakeSize(64, 64);
        w.contentMaxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    } else {
        w.styleMask &= ~NSWindowStyleMaskResizable;
        w.contentMinSize = w.contentMaxSize = NSMakeSize(size.width, size.height);
    }
}

void EditorWindow::setContentSize(EditorSize wanted) {
    if (window == nullptr) return;
    resizingNow = true;
    [asWindow(window) setContentSize:NSMakeSize(wanted.width, wanted.height)];
    resizingNow = false;
}

void EditorWindow::windowResized() {
    if (window == nullptr || resizingNow || !plugin.editorResizable()) return;
    const NSSize now      = [asWindow(window) contentRectForFrameRect:asWindow(window).frame].size;
    EditorSize   adjusted{static_cast<std::uint32_t>(now.width), static_cast<std::uint32_t>(now.height)};
    if (adjusted.width == size.width && adjusted.height == size.height) return;
    if (plugin.setEditorSize(adjusted)) {   // the user resized it; the plugin may adjust
        size = adjusted;
        if (adjusted.width != static_cast<std::uint32_t>(now.width) ||
            adjusted.height != static_cast<std::uint32_t>(now.height))
            setContentSize(adjusted);
    }
}

// --- EditorHost ---------------------------------------------------------------------

bool EditorWindow::requestEditorResize(EditorSize wanted) {
    if (window == nullptr) return false;
    size = wanted;
    applySizeHints();   // a fixed-size editor's min = max must move first
    setContentSize(wanted);
    // Confirm with set_size once the plugin's request has returned (not from
    // inside it), as the X11 window does when its resize lands.
    const std::shared_ptr<bool> stillHere = alive;
    HostedPlugin* const         target    = &plugin;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!*stillHere) return;
        EditorSize confirmed = wanted;
        target->setEditorSize(confirmed);
    });
    return true;
}

bool EditorWindow::requestEditorShown(bool shown) {
    if (window == nullptr) return false;
    if (shown) [asWindow(window) makeKeyAndOrderFront:nil];
    else [asWindow(window) orderOut:nil];
    return true;
}

void EditorWindow::editorResizeHintsChanged() { applySizeHints(); }

void EditorWindow::editorClosed() {
    editorOpen = false;   // the plugin already tore it down
    close(false);
}

} // namespace uw
