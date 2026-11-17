#pragma once

// HelpWindow: a product's help page in a window of its own, drawn with DGL's
// NanoVG: no browser, no web server, and nothing written to disk. The page is
// the product's help.md (read by uw/help/HelpDoc.h, laid out by HelpLayout.h),
// embedded in the binary with the logo it shows.
//
// For DPF/DGL products (the plugins, StageHand). Include it where DGL's
// headers are on the include path, after DGL_NAMESPACE is set, as for any
// DGL code: the classes go in that namespace, so two products in one host
// never share them.
//
//   HelpOpener help;                              // a member of the editor
//   help.open(*this, "PlainOldSynth Help",        // on a click of Help
//             helpMd, helpMdSize, {{"Unlikelyware.png", logo, logoSize}},
//             "#cloud");                          // optional: open at a section
//
// The window opens beside the editor, scrolls (wheel, keys, the bar), follows
// links within the page (Backspace goes back), opens and closes disclosures,
// and sends links to the web (the logo's unlikelyware.com) to the browser.
// Closing it hides it, keeping the place; the editor's closing destroys it.

#include "NanoVG.hpp"
#include "Window.hpp"

#include "uw/help/HelpDoc.h"
#include "uw/help/HelpLayout.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(DISTRHO_OS_MAC)
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#if !defined(DISTRHO_OS_WINDOWS)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

START_NAMESPACE_DGL

namespace uwhelp {

// A file the page shows, by the name it uses for it (the logo).
struct Image {
    std::string  name;
    const uchar* data = nullptr;
    uint         size = 0;
};

// Opens a web link in the default browser, without a shell. Only http(s)
// and mailto: a page can't make the product run anything else.
inline bool openUrl(const std::string& url) {
    const bool web = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 || url.rfind("mailto:", 0) == 0;
    if (!web || url.find_first_of(" \t\r\n\"'`") != std::string::npos) return false;
#if defined(DISTRHO_OS_WINDOWS)
    return false;
#else
#if defined(DISTRHO_OS_MAC)
    const char* tool = "open";
#else
    const char* tool = "xdg-open";
#endif
    char* const argv[] = {const_cast<char*>(tool), const_cast<char*>(url.c_str()), nullptr};
    pid_t       pid    = 0;
    if (posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ) != 0) return false;
    // `open` returns at once; xdg-open may stay with the browser. Don't wait
    // on it here (this is the UI thread); reap it if it's already done.
    int status = 0;
    (void)waitpid(pid, &status, WNOHANG);
    return true;
#endif
}

// Puts the help window in front of the editor's. On macOS a host's plugin
// window is often a floating one, above every ordinary window, so the help
// window takes the editor window's level: it opens in front, and a click on
// either brings that one forward. Both handles are DGL's (pugl's NSViews).
inline void bringToFront(uintptr_t helpView, uintptr_t editorView) {
#if defined(DISTRHO_OS_MAC)
    if (helpView == 0) return;
    const auto get      = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    const auto getLevel = reinterpret_cast<long (*)(id, SEL)>(objc_msgSend);
    const auto setLevel = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    const auto order    = reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend);
    id help = get(reinterpret_cast<id>(helpView), sel_registerName("window"));
    if (help == nullptr) return;
    id editor = editorView != 0 ? get(reinterpret_cast<id>(editorView), sel_registerName("window")) : nullptr;
    if (editor != nullptr) {
        const long level = getLevel(editor, sel_registerName("level"));
        if (level > getLevel(help, sel_registerName("level"))) setLevel(help, sel_registerName("setLevel:"), level);
    }
    order(help, sel_registerName("makeKeyAndOrderFront:"), nullptr);
#else
    (void)helpView;
    (void)editorView;
#endif
}

class HelpView : public NanoTopLevelWidget, private uw::help::Measure {
public:
    HelpView(Window& window, std::string_view markdown, std::vector<Image> images)
        : NanoTopLevelWidget(window), fDoc(uw::help::parse(markdown)), fImageFiles(std::move(images)) {
        fHasFont = loadSharedResources();
        fOpened.assign(static_cast<std::size_t>(uw::help::countDisclosures(fDoc.blocks)), false);
    }

    const std::string& title() const { return fDoc.title; }

    // Scrolls to `#id` (opening it, if it's a disclosure), as a link there does.
    void showAnchor(const std::string& link) {
        relayoutIfNeeded();
        jumpTo(link);
    }
    void setScroll(float y) { scrollTo(y); }
    void openAll(bool open) {
        fOpened.assign(fOpened.size(), open);
        invalidate();
    }

protected:
    // --- Colors: the help pages' own (light page, dark text) ---
    static Color color(uw::help::Role r) {
        using uw::help::Role;
        switch (r) {
        case Role::Text:       return Color(34, 34, 34);
        case Role::Muted:      return Color(96, 96, 96);
        case Role::Heading:    return Color(20, 20, 20);
        case Role::Link:       return Color(28, 92, 186);
        case Role::CardFill:   return Color(255, 255, 255);
        case Role::CardBorder: return Color(0, 0, 0, 0.10f);
        case Role::CodeFill:   return Color(236, 236, 236);
        case Role::RowFill:    return Color(0, 0, 0, 0.03f);
        case Role::Rule:       return Color(221, 221, 221);
        case Role::HeaderRule: return Color(187, 187, 187);
        }
        return Color(34, 34, 34);
    }

    float scale() const { return static_cast<float>(std::max(1.0, getWindow().getScaleFactor())); }

    // DejaVu Sans is DGL's one embedded font: bold is drawn twice, a hair
    // apart, and italic slanted. Widths are the regular ones: runs of words
    // are drawn whole, so a per-word allowance would open gaps after them.
    float boldOffset(float size) const { return std::max(0.6f, size * 0.045f); }

    float width(std::string_view text, int style, float size) const override {
        if (text.empty() || !fHasFont) return 0.0f;
        auto& self = const_cast<HelpView&>(*this);
        self.fontFace(NANOVG_DEJAVU_SANS_TTF);
        self.fontSize(size * ((style & uw::help::kCode) ? 0.92f : 1.0f));
        Rectangle<float> bounds;
        const float      advance = self.textBounds(0, 0, text.data(), text.data() + text.size(), bounds);
        return advance;
    }

    void relayoutIfNeeded() {
        const float w = static_cast<float>(getWidth());
        const float s = scale();
        if (fLaidOut && w == fLaidWidth && s == fLaidScale) return;
        uw::help::Metrics m;
        m.scale    = s;
        fHover     = nullptr;   // it pointed into the old layout
        fLayout    = uw::help::layout(fDoc, w, *this, fOpened, m);
        fLaidOut   = true;
        fLaidWidth = w;
        fLaidScale = s;
        clampScroll();
    }

    void invalidate() {
        fLaidOut = false;
        repaint();
    }

    float maxScroll() const { return std::max(0.0f, fLayout.height - static_cast<float>(getHeight())); }
    void  clampScroll() { fScroll = std::clamp(fScroll, 0.0f, maxScroll()); }

    void scrollTo(float y) {
        fScroll = y;
        clampScroll();
        repaint();
    }

    void jumpTo(const std::string& link) {
        const int d = fLayout.disclosure(link);
        if (d >= 0 && d < static_cast<int>(fOpened.size()) && !fOpened[d]) {
            fOpened[d] = true;
            fLaidOut   = false;
            relayoutIfNeeded();
        }
        const float y = fLayout.anchor(link);
        if (y < 0) return;
        fHistory.push_back(fScroll);
        scrollTo(y - 10.0f * scale());
    }

    void onNanoDisplay() override {
        const float W = static_cast<float>(getWidth()), H = static_cast<float>(getHeight());
        beginPath();
        rect(0, 0, W, H);
        fillColor(Color(251, 250, 247));
        fill();
        if (!fHasFont) return;
        relayoutIfNeeded();

        save();
        translate(0, -fScroll);
        const float top = fScroll, bottom = fScroll + H;
        for (const uw::help::Item& it : fLayout.items) {
            if (it.y > bottom || it.y + it.h < top) continue;
            drawItem(it);
        }
        restore();

        // The scroll bar, when there's more page than window.
        if (fLayout.height > H) {
            const float s = scale();
            const float barW = 6.0f * s, track = H - 8.0f * s;
            const float thumbH = std::max(28.0f * s, track * H / fLayout.height);
            const float thumbY = 4.0f * s + (track - thumbH) * (maxScroll() > 0 ? fScroll / maxScroll() : 0.0f);
            beginPath();
            roundedRect(W - barW - 4.0f * s, thumbY, barW, thumbH, barW * 0.5f);
            fillColor(Color(0, 0, 0, fDragging ? 0.45f : 0.25f));
            fill();
        }
    }

    void drawItem(const uw::help::Item& it) {
        using uw::help::Item;
        using uw::help::kBold;
        using uw::help::kCode;
        using uw::help::kItalic;
        switch (it.kind) {
        case Item::Kind::Box:
            beginPath();
            if (it.radius > 0) roundedRect(it.x, it.y, it.w, it.h, it.radius);
            else rect(it.x, it.y, it.w, it.h);
            fillColor(color(it.role));
            fill();
            break;
        case Item::Kind::Frame:
            beginPath();
            roundedRect(it.x + 0.5f, it.y + 0.5f, it.w - 1.0f, it.h - 1.0f, it.radius);
            strokeColor(color(it.role));
            strokeWidth(1.0f);
            stroke();
            break;
        case Item::Kind::Line:
            beginPath();
            rect(it.x, it.y - it.h, it.w, it.h);
            fillColor(color(it.role));
            fill();
            break;
        case Item::Kind::Arrow: {
            beginPath();
            if (it.open) {
                moveTo(it.x, it.y + it.h * 0.2f);
                lineTo(it.x + it.w, it.y + it.h * 0.2f);
                lineTo(it.x + it.w * 0.5f, it.y + it.h * 0.9f);
            } else {
                moveTo(it.x + it.w * 0.2f, it.y);
                lineTo(it.x + it.w * 0.9f, it.y + it.h * 0.5f);
                lineTo(it.x + it.w * 0.2f, it.y + it.h);
            }
            closePath();
            fillColor(color(it.role));
            fill();
            break;
        }
        case Item::Kind::Image: {
            NanoImage* img = image(it.text);
            if (img == nullptr || !img->isValid()) break;
            beginPath();
            roundedRect(it.x, it.y, it.w, it.h, it.radius);
            fillPaint(imagePattern(it.x, it.y, it.w, it.h, 0.0f, *img, 1.0f));
            fill();
            break;
        }
        case Item::Kind::Text: {
            const float size = it.size * ((it.style & kCode) ? 0.92f : 1.0f);
            fontFace(NANOVG_DEJAVU_SANS_TTF);
            fontSize(size);
            textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
            fillColor((it.style & kCode) && it.role != uw::help::Role::Link ? Color(70, 40, 30) : color(it.role));
            const float cy = it.y + it.h * 0.5f;
            const char* begin = it.text.c_str();
            const char* end   = begin + it.text.size();
            auto draw = [&](float x) {
                if (it.style & kItalic) {
                    save();
                    translate(x, cy);
                    transform(1.0f, 0.0f, -0.2f, 1.0f, 0.0f, 0.0f);   // a slant: DGL's skewX() takes only positive angles
                    text(0, 0, begin, end);
                    restore();
                } else {
                    text(x, cy, begin, end);
                }
            };
            draw(it.x);
            if (it.style & kBold) draw(it.x + boldOffset(it.size));
            if (it.role == uw::help::Role::Link && fHover && hovering(it)) {
                beginPath();
                rect(it.x, cy + size * 0.55f, it.w, std::max(1.0f, scale()));
                fillColor(color(uw::help::Role::Link));
                fill();
            }
            break;
        }
        }
    }

    // Is this link text under the pointer's link (one link may span items)?
    bool hovering(const uw::help::Item& it) const {
        for (const uw::help::Hit& h : fLayout.hits)
            if (!h.link.empty() && h.link == fHover->link && h.contains(it.x + 1.0f, it.y + it.h * 0.5f)) return true;
        return false;
    }

    NanoImage* image(const std::string& name) {
        for (auto& [n, img] : fImages)
            if (n == name) return img.get();
        for (const Image& f : fImageFiles)
            if (f.name == name) {
                fImages.emplace_back(name, std::make_unique<NanoImage>(createImageFromMemory(f.data, f.size, 0)));
                return fImages.back().second.get();
            }
        return nullptr;
    }

    // --- Input ---------------------------------------------------------

    bool inScrollBar(float x) const {
        return fLayout.height > static_cast<float>(getHeight()) && x >= static_cast<float>(getWidth()) - 14.0f * scale();
    }

    bool onMouse(const MouseEvent& ev) override {
        if (ev.button != 1) return false;
        const float x = static_cast<float>(ev.pos.getX()), y = static_cast<float>(ev.pos.getY());
        if (!ev.press) {
            if (fDragging) {
                fDragging = false;
                repaint();
            }
            return true;
        }
        if (inScrollBar(x)) {
            fDragging    = true;
            fDragFromY   = y;
            fDragFromTop = fScroll;
            repaint();
            return true;
        }
        const uw::help::Hit* h = fLayout.hitAt(x, y + fScroll);
        if (h == nullptr) return true;
        if (h->disclosure >= 0 && h->disclosure < static_cast<int>(fOpened.size())) {
            fOpened[h->disclosure] = !fOpened[h->disclosure];
            invalidate();
        } else if (!h->link.empty() && h->link[0] == '#') {
            jumpTo(h->link);
        } else if (!h->link.empty()) {
            openUrl(h->link);
        }
        return true;
    }

    bool onMotion(const MotionEvent& ev) override {
        const float x = static_cast<float>(ev.pos.getX()), y = static_cast<float>(ev.pos.getY());
        if (fDragging) {
            const float H = static_cast<float>(getHeight());
            const float track = H - 8.0f * scale();
            const float thumbH = std::max(28.0f * scale(), track * H / std::max(1.0f, fLayout.height));
            const float room = std::max(1.0f, track - thumbH);
            scrollTo(fDragFromTop + (y - fDragFromY) * maxScroll() / room);
            return true;
        }
        const uw::help::Hit* h = inScrollBar(x) ? nullptr : fLayout.hitAt(x, y + fScroll);
        if (h != fHover) {
            fHover = h;
            setCursor(h ? kMouseCursorHand : kMouseCursorArrow);
            repaint();
        }
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override {
        scrollTo(fScroll - static_cast<float>(ev.delta.getY()) * 48.0f * scale());
        return true;
    }

    bool onKeyboard(const KeyboardEvent& ev) override {
        if (!ev.press) return false;
        const float H = static_cast<float>(getHeight()), line = 48.0f * scale();
        const bool  command = (ev.mod & kModifierSuper) || (ev.mod & kModifierControl);
        switch (ev.key) {
        case kKeyEscape:
            getWindow().close();
            return true;
        case kKeyUp:       scrollTo(fScroll - line); return true;
        case kKeyDown:     scrollTo(fScroll + line); return true;
        case kKeyPageUp:   scrollTo(fScroll - (H - line)); return true;
        case kKeyPageDown: scrollTo(fScroll + (H - line)); return true;
        case kKeyHome:     scrollTo(0); return true;
        case kKeyEnd:      scrollTo(maxScroll()); return true;
        case kKeyBackspace:
            if (!fHistory.empty()) {
                const float back = fHistory.back();
                fHistory.pop_back();
                scrollTo(back);
            }
            return true;
        case ' ':
            scrollTo(fScroll + ((ev.mod & kModifierShift) ? -1.0f : 1.0f) * (H - line));
            return true;
        case 'w':
        case 'W':
            if (command) {
                getWindow().close();
                return true;
            }
            return false;
        case '[':
            if (command && !fHistory.empty()) {
                const float back = fHistory.back();
                fHistory.pop_back();
                scrollTo(back);
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    void onResize(const ResizeEvent& ev) override {
        NanoTopLevelWidget::onResize(ev);
        fLaidOut = false;
    }

private:
    uw::help::Document                         fDoc;
    std::vector<Image>                         fImageFiles;
    std::vector<std::pair<std::string, std::unique_ptr<NanoImage>>> fImages;
    std::vector<bool>                          fOpened;
    uw::help::Layout                           fLayout;
    std::vector<float>                         fHistory;   // where each followed link left from
    const uw::help::Hit*                       fHover = nullptr;
    bool  fHasFont = false, fLaidOut = false, fDragging = false;
    float fLaidWidth = 0, fLaidScale = 0, fScroll = 0, fDragFromY = 0, fDragFromTop = 0;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HelpView)
};

// What an editor keeps: the help window, made on the first click of Help and
// shown again on the next ones, until the editor itself closes.
class HelpOpener {
public:
    HelpOpener() = default;
    ~HelpOpener() { close(); }
    HelpOpener(const HelpOpener&)            = delete;
    HelpOpener& operator=(const HelpOpener&) = delete;

    // `from` is any widget of the editor; the page is read here, so `markdown`
    // needn't outlive the call (the images must: they're decoded on first draw).
    // `anchor` ("#id") is where the page opens the first time; nullptr for the top.
    void open(Widget& from, const char* title, const uchar* markdown, uint size, std::vector<Image> images,
              const char* anchor = nullptr) {
        if (fWindow) {
            fWindow->show();
            fWindow->focus();
            bringToFront(fWindow->getNativeWindowHandle(), fOwner->getNativeWindowHandle());
            return;
        }
        fOwner = &from.getWindow();
        const double scale = std::max(1.0, fOwner->getScaleFactor());
        fWindow = std::make_unique<Window>(from.getApp());
        {
            Window::ScopedGraphicsContext context(*fWindow, *fOwner);
            fView = std::make_unique<HelpView>(
                *fWindow, std::string_view(reinterpret_cast<const char*>(markdown), size), std::move(images));
        }
        fWindow->setTitle(title);
        fWindow->setResizable(true);
        fWindow->setGeometryConstraints(uint(360 * scale), uint(300 * scale), false, false);
        fWindow->setSize(uint(760 * scale), uint(860 * scale));
        if (anchor != nullptr) fView->showAnchor(anchor);
        fWindow->show();
        fWindow->focus();
        bringToFront(fWindow->getNativeWindowHandle(), fOwner->getNativeWindowHandle());
    }

    void close() {
        if (!fWindow) return;
        {   // the view's NanoVG context goes with its own GL context current
            Window::ScopedGraphicsContext context(*fWindow, *fOwner);
            fView.reset();
        }
        fWindow->close();
        fWindow.reset();
    }

private:
    Window*                   fOwner = nullptr;
    std::unique_ptr<Window>   fWindow;
    std::unique_ptr<HelpView> fView;
};

}   // namespace uwhelp

END_NAMESPACE_DGL
