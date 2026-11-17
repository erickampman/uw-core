#pragma once

// HelpPanel: a product's help page in a Dear ImGui window of the app's own,
// for the ImGui apps (KeyboardEDO): no browser, no web server. The page is
// the product's help.md, read by uw/help/HelpDoc.h and laid out by
// HelpLayout.h, the same as the DGL products' HelpWindow (uw/dgl/).
//
// Header-only; include it where imgui.h is on the include path (1.92 or
// later: fonts are drawn at any size).
//
//   uw::help::HelpPanel help;          // a member of the app
//   help.load(markdown);               // once: the page's text
//   help.open();                       // on Help (F1); open("#id") for a section
//   help.draw("KeyboardEDO Help", font, logoTexture, logoAspect);   // every frame
//
// The window scrolls (wheel, keys), follows links within the page (Backspace
// goes back), opens and closes disclosures, and sends web links to
// `openUrl` (SDL_OpenURL, say). The font has no bold or italic of its own
// here: bold is drawn twice, a hair apart, and italic in the muted colour.

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imgui.h"
#include "uw/help/HelpDoc.h"
#include "uw/help/HelpLayout.h"

namespace uw::help {

class HelpPanel : private Measure {
public:
    std::function<void(const std::string&)> openUrl;   // web links; unset, they do nothing

    void load(std::string_view markdown) {
        doc_ = parse(markdown);
        opened_.assign(static_cast<std::size_t>(countDisclosures(doc_.blocks)), false);
        laidOut_ = false;
        loaded_  = true;
    }
    bool loaded() const { return loaded_; }
    bool isOpen() const { return open_; }
    // Whether the window had the keyboard last frame: the app's own keys
    // (playing notes, say) should leave it alone then.
    bool focused() const { return open_ && focused_; }
    void close() { open_ = false; }

    // Shows the window (bringing it to the front), at `anchor` ("#id") if given.
    void open(const char* anchor = nullptr) {
        open_  = true;
        focus_ = true;
        if (anchor != nullptr) pendingAnchor_ = anchor;
    }

    // Call once a frame, between ImGui::NewFrame() and ImGui::Render().
    // `font` is the body font (nullptr: the current one); `logo` the texture
    // of the image the page names (the Unlikelyware logo), or 0 for none.
    void draw(const char* title, ImFont* font = nullptr, ImTextureID logo = ImTextureID(0), float logoAspect = 1.0f) {
        if (!open_) return;
        const float scale = ImGui::GetStyle().FontScaleMain > 0 ? ImGui::GetStyle().FontScaleMain : 1.0f;
        ImGui::SetNextWindowSize(ImVec2(720 * scale, 640 * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320 * scale, 240 * scale), ImVec2(FLT_MAX, FLT_MAX));
        if (focus_) {
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowCollapsed(false);
            focus_ = false;
        }
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kPage);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPage);
        const bool visible = ImGui::Begin(title, &open_, ImGuiWindowFlags_NoCollapse);
        ImGui::PopStyleColor(2);
        focused_ = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (!visible) {
            ImGui::End();
            return;
        }
        font_      = font ? font : ImGui::GetFont();
        fontScale_ = scale;
        ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);

        const float width = ImGui::GetContentRegionAvail().x;
        if (!laidOut_ || width != laidWidth_ || scale != laidScale_) relayout(width, scale);

        if (!pendingAnchor_.empty()) {
            jumpTo(pendingAnchor_, width, scale, /*remember*/ false);
            pendingAnchor_.clear();
        }

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        const float  top    = ImGui::GetScrollY();
        const float  bottom = top + ImGui::GetWindowHeight();
        for (const Item& it : layout_.items) {
            if (it.y > bottom || it.y + it.h < top) continue;
            drawItem(dl, origin, it, logo, logoAspect);
        }
        ImGui::Dummy(ImVec2(width, layout_.height));

        // Input: clicks, the hand over links, keys.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const Hit*   hit   = nullptr;
        if (ImGui::IsWindowHovered()) hit = layout_.hitAt(mouse.x - origin.x, mouse.y - origin.y);
        if (hit) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) follow(*hit, width, scale);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) keys();

        ImGui::EndChild();
        ImGui::End();
    }

private:
    static constexpr ImU32 kPage = IM_COL32(251, 250, 247, 255);

    static ImU32 color(Role r) {
        switch (r) {
        case Role::Text:       return IM_COL32(34, 34, 34, 255);
        case Role::Muted:      return IM_COL32(96, 96, 96, 255);
        case Role::Heading:    return IM_COL32(20, 20, 20, 255);
        case Role::Link:       return IM_COL32(28, 92, 186, 255);
        case Role::CardFill:   return IM_COL32(255, 255, 255, 255);
        case Role::CardBorder: return IM_COL32(0, 0, 0, 26);
        case Role::CodeFill:   return IM_COL32(236, 236, 236, 255);
        case Role::RowFill:    return IM_COL32(0, 0, 0, 8);
        case Role::Rule:       return IM_COL32(221, 221, 221, 255);
        case Role::HeaderRule: return IM_COL32(187, 187, 187, 255);
        }
        return IM_COL32(34, 34, 34, 255);
    }

    // Layout sizes are in pixels at scale 1 times the style's font scale; the
    // font is drawn at that size.
    float width(std::string_view text, int, float size) const override {
        if (text.empty()) return 0.0f;
        return font_->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
    }

    void relayout(float width, float scale) {
        Metrics m;
        m.scale    = scale;
        m.margin   = 14.0f;
        layout_    = uw::help::layout(doc_, width, *this, opened_, m);
        laidOut_   = true;
        laidWidth_ = width;
        laidScale_ = scale;
    }

    void jumpTo(const std::string& link, float width, float scale, bool remember) {
        const int d = layout_.disclosure(link);
        if (d >= 0 && d < static_cast<int>(opened_.size()) && !opened_[d]) {
            opened_[d] = true;
            relayout(width, scale);
        }
        const float y = layout_.anchor(link);
        if (y < 0) return;
        if (remember) history_.push_back(ImGui::GetScrollY());
        ImGui::SetScrollY(std::max(0.0f, y - 8.0f * scale));
    }

    void follow(const Hit& h, float width, float scale) {
        if (h.disclosure >= 0 && h.disclosure < static_cast<int>(opened_.size())) {
            opened_[h.disclosure] = !opened_[h.disclosure];
            laidOut_              = false;
        } else if (!h.link.empty() && h.link[0] == '#') {
            jumpTo(h.link, width, scale, true);
        } else if (!h.link.empty() && openUrl) {
            openUrl(h.link);
        }
    }

    void keys() {
        const float H = ImGui::GetWindowHeight(), line = ImGui::GetTextLineHeightWithSpacing() * 3;
        const float y = ImGui::GetScrollY();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open_ = false;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ImGui::SetScrollY(y - line);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ImGui::SetScrollY(y + line);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(y - (H - line));
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_Space)) ImGui::SetScrollY(y + (H - line));
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) ImGui::SetScrollY(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !history_.empty()) {
            ImGui::SetScrollY(history_.back());
            history_.pop_back();
        }
    }

    void drawItem(ImDrawList* dl, ImVec2 o, const Item& it, ImTextureID logo, float logoAspect) {
        const ImVec2 a(o.x + it.x, o.y + it.y), b(o.x + it.x + it.w, o.y + it.y + it.h);
        switch (it.kind) {
        case Item::Kind::Box:
            dl->AddRectFilled(a, b, color(it.role), it.radius);
            break;
        case Item::Kind::Frame:
            dl->AddRect(a, b, color(it.role), it.radius);
            break;
        case Item::Kind::Line:
            dl->AddRectFilled(ImVec2(a.x, a.y - it.h), ImVec2(b.x, a.y), color(it.role));
            break;
        case Item::Kind::Arrow:
            if (it.open)
                dl->AddTriangleFilled(ImVec2(a.x, a.y + it.h * 0.2f), ImVec2(b.x, a.y + it.h * 0.2f),
                                      ImVec2(a.x + it.w * 0.5f, a.y + it.h * 0.9f), color(it.role));
            else
                dl->AddTriangleFilled(ImVec2(a.x + it.w * 0.2f, a.y), ImVec2(a.x + it.w * 0.9f, a.y + it.h * 0.5f),
                                      ImVec2(a.x + it.w * 0.2f, b.y), color(it.role));
            break;
        case Item::Kind::Image:
            if (logo != ImTextureID(0)) {
                // Fit the texture in the box, keeping its shape.
                float w = it.w, h = it.h;
                if (logoAspect > 1.0f) h = w / logoAspect;
                else w = h * logoAspect;
                const ImVec2 p(a.x + (it.w - w) * 0.5f, a.y + (it.h - h) * 0.5f);
                dl->AddImageRounded(ImTextureRef(logo), p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1),
                                    IM_COL32_WHITE, it.radius);
            }
            break;
        case Item::Kind::Text: {
            const ImU32 col = (it.style & kItalic) && it.role == Role::Text ? color(Role::Muted)
                            : (it.style & kCode) && it.role != Role::Link   ? IM_COL32(70, 40, 30, 255)
                                                                            : color(it.role);
            const float  fh = it.size;   // ImGui's line height for the size is the size itself
            const ImVec2 p(a.x, a.y + (it.h - fh) * 0.5f);
            const char*  t = it.text.c_str();
            dl->AddText(font_, it.size, p, col, t, t + it.text.size());
            if (it.style & kBold) dl->AddText(font_, it.size, ImVec2(p.x + std::max(0.6f, it.size * 0.045f), p.y), col, t,
                                              t + it.text.size());
            break;
        }
        }
    }

    Document           doc_;
    Layout             layout_;
    std::vector<bool>  opened_;
    std::vector<float> history_;
    std::string        pendingAnchor_;
    ImFont*            font_      = nullptr;
    float              fontScale_ = 1.0f;
    float              laidWidth_ = 0, laidScale_ = 0;
    bool               loaded_ = false, open_ = false, focus_ = false, focused_ = false, laidOut_ = false;
};

}   // namespace uw::help
