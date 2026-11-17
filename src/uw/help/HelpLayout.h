#pragma once

// HelpLayout: places a HelpDoc Document at a width, as a list of things to
// draw (text runs, boxes, rules, the logo) and things to click (links, the
// disclosures' titles). It knows nothing of any toolkit: text is measured
// through a Measure the window gives it, and the window draws each Item, so
// the same layout serves NanoVG (the plugins, StageHand) and Dear ImGui
// (KeyboardEDO). Header-only, standard C++17 only.
//
// Coordinates are in the window's units, y down, the page starting at 0.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "uw/help/HelpDoc.h"

namespace uw::help {

// How wide a run of text is in a style (kBold, kItalic, kCode, or'd) and size.
struct Measure {
    virtual ~Measure() = default;
    virtual float width(std::string_view text, int style, float size) const = 0;
};

enum class Role {
    Text,          // body text
    Muted,         // the note, a disclosure's title, bullets and numbers
    Heading,
    Link,
    CardFill,      // a card's box
    CardBorder,
    CodeFill,      // behind `code`
    RowFill,       // every other table row
    Rule,          // between table rows
    HeaderRule,    // under a table's header
};

struct Item {
    enum class Kind {
        Text,     // text in style and size, in the box (x, y, w, h): left-aligned, centred in h
        Box,      // a rounded rectangle (radius), filled in role
        Frame,    // its outline
        Line,     // a horizontal line from (x, y), w long, h thick
        Image,    // the logo, filling the box
        Arrow,    // a disclosure's triangle in the box, pointing down if open, right if not
    };
    Kind        kind  = Kind::Text;
    float       x = 0, y = 0, w = 0, h = 0;
    std::string text;
    int         style  = kPlain;
    float       size   = 0;
    Role        role   = Role::Text;
    float       radius = 0;
    bool        open   = false;
};

struct Hit {
    float       x = 0, y = 0, w = 0, h = 0;
    std::string link;            // "#id" or a URL
    int         disclosure = -1; // otherwise, the disclosure this toggles

    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Layout {
    std::vector<Item>                         items;
    std::vector<Hit>                          hits;
    std::vector<std::pair<std::string, float>> anchors;   // heading id, its y
    std::vector<std::pair<std::string, int>>   disclosureIds;   // a disclosure's id, its number
    float                                     height = 0;

    // The disclosure `#id` names, or -1: following a link to it opens it.
    int disclosure(std::string_view id) const {
        if (!id.empty() && id.front() == '#') id.remove_prefix(1);
        for (const auto& [name, n] : disclosureIds)
            if (name == id) return n;
        return -1;
    }

    // The y of `#id` (or `id`), or -1.
    float anchor(std::string_view id) const {
        if (!id.empty() && id.front() == '#') id.remove_prefix(1);
        for (const auto& [name, y] : anchors)
            if (name == id) return y;
        return -1;
    }
    const Hit* hitAt(float x, float y) const {
        for (const Hit& h : hits)
            if (h.contains(x, y)) return &h;
        return nullptr;
    }
};

struct Metrics {
    float body       = 14.0f;   // body text size
    float lineHeight = 1.5f;    // times the size
    float title      = 25.0f;   // the page's title
    float h2         = 18.0f;
    float h3         = 15.5f;
    float margin     = 22.0f;   // around the page
    float maxWidth   = 860.0f;  // the page's text column, at most
    float logo       = 52.0f;
    float cardPad    = 15.0f;
    float scale      = 1.0f;    // everything above, times this
};

// Disclosures in the order Layout numbers them (depth first, as the page reads).
inline int countDisclosures(const std::vector<Block>& blocks) {
    int n = 0;
    for (const Block& b : blocks) {
        if (b.kind == Block::Kind::Disclosure) ++n;
        n += countDisclosures(b.children);
    }
    return n;
}

namespace detail {

struct Token {
    std::string word;    // with its trailing space, if any
    int         style;
    std::string link;
};

inline std::vector<Token> tokens(const Inline& text) {
    std::vector<Token> out;
    for (const Span& s : text) {
        std::size_t from = 0;
        while (from < s.text.size()) {
            std::size_t sp = s.text.find(' ', from);
            std::size_t to = sp == std::string::npos ? s.text.size() : sp + 1;
            // A break inside `code` keeps the run together where it can, so
            // treat code as one word unless it holds spaces of its own.
            out.push_back({s.text.substr(from, to - from), s.style, s.link});
            from = to;
        }
    }
    return out;
}

inline std::string_view withoutTrailingSpace(std::string_view w) {
    while (!w.empty() && w.back() == ' ') w.remove_suffix(1);
    return w;
}

class Builder {
public:
    Builder(const Measure& m, const Metrics& metrics, const std::vector<bool>& opened)
        : measure(m), opened(opened) {
        const float s = metrics.scale;
        mx         = metrics;
        mx.body *= s; mx.title *= s; mx.h2 *= s; mx.h3 *= s; mx.margin *= s;
        mx.maxWidth *= s; mx.logo *= s; mx.cardPad *= s;
    }

    Layout out;

    float lineH(float size) const { return size * mx.lineHeight; }

    // Natural width of a run on one line, and its widest word.
    std::pair<float, float> natural(const Inline& text, float size, int extra) const {
        float total = 0, widest = 0;
        for (const Token& t : tokens(text)) {
            const float w = measure.width(t.word, t.style | extra, size);
            total += w;
            widest = std::max(widest, measure.width(withoutTrailingSpace(t.word), t.style | extra, size));
        }
        return {total + codePad(), widest + codePad()};
    }

    float codePad() const { return 4.0f * mx.scale; }

    // Flows `text` into the column [x, x + width) from y; returns the height used.
    float flow(const Inline& text, float x, float y, float width, float size, Role role, int extra) {
        const float lh   = lineH(size);
        float       cx   = x, cy = y;
        bool        any  = false;
        long        last = -1;   // the text item a word may join
        for (const Token& t : tokens(text)) {
            const int   style = t.style | extra;
            const float full  = measure.width(t.word, style, size);
            const float bare  = measure.width(withoutTrailingSpace(t.word), style, size);
            const float pad   = (style & kCode) ? codePad() : 0.0f;
            if (any && cx + bare + pad > x + width) {
                cx   = x;
                cy  += lh;
                last = -1;
            }
            const Role r = t.link.empty() ? role : Role::Link;
            if (style & kCode) {
                Item box;
                box.kind   = Item::Kind::Box;
                box.role   = Role::CodeFill;
                box.x      = cx;
                box.y      = cy + lh * 0.12f;
                box.w      = bare + pad;
                box.h      = lh * 0.76f;
                box.radius = 3.0f * mx.scale;
                out.items.push_back(box);
                last = -1;
            }
            const float tx = cx + pad * 0.5f;
            if (last >= 0 && out.items[last].style == style && out.items[last].role == r && !(style & kCode)) {
                out.items[last].text += t.word;
                out.items[last].w = tx + full - out.items[last].x;
            } else {
                Item it;
                it.kind  = Item::Kind::Text;
                it.x     = tx;
                it.y     = cy;
                it.w     = full;
                it.h     = lh;
                it.text  = t.word;
                it.style = style;
                it.size  = size;
                it.role  = r;
                out.items.push_back(it);
                last = (style & kCode) ? -1 : static_cast<long>(out.items.size()) - 1;
            }
            if (!t.link.empty()) out.hits.push_back({cx, cy, bare + pad, lh, t.link, -1});
            cx += full + pad;
            any = true;
        }
        return any ? cy + lh - y : 0.0f;
    }

    // Lays out `blocks` in the column; returns the y after them.
    float blocks(const std::vector<Block>& list, float x, float y, float width) {
        bool first = true;
        for (const Block& b : list) {
            y           = block(b, x, y, width, first);
            first       = false;
            prevWasItem = b.kind == Block::Kind::Bullet || b.kind == Block::Kind::Numbered;
        }
        return y;
    }

    float gap() const { return mx.body * 0.6f; }

    float block(const Block& b, float x, float y, float width, bool first) {
        switch (b.kind) {
        case Block::Kind::Heading: {
            const float size = b.level <= 1 ? mx.title : b.level == 2 ? mx.h2 : mx.h3;
            if (!first) y += size * 0.7f;
            out.anchors.push_back({b.id, y});
            y += flow(b.text, x, y, width, size, Role::Heading, kBold);
            return y + mx.body * 0.25f;
        }
        case Block::Kind::Paragraph:
        case Block::Kind::Note:
            if (!first) y += gap();
            return y + flow(b.text, x, y, width, mx.body, b.kind == Block::Kind::Note ? Role::Muted : Role::Text,
                            b.kind == Block::Kind::Note ? kItalic : kPlain);
        case Block::Kind::Bullet:
        case Block::Kind::Numbered: {
            // Items of one list sit closer than paragraphs do.
            if (!first) y += b.depth > 0 || prevWasItem ? mx.body * 0.25f : gap();
            const float indent = mx.body * 1.5f * (b.depth + 1);
            Item mark;
            mark.kind  = Item::Kind::Text;
            mark.role  = Role::Muted;
            mark.size  = mx.body;
            mark.h     = lineH(mx.body);
            mark.y     = y;
            if (b.kind == Block::Kind::Bullet) {
                mark.text = b.depth == 0 ? "\xE2\x80\xA2" : "\xE2\x97\xA6";   // • ◦
                mark.w    = measure.width(mark.text, kPlain, mx.body);
                mark.x    = x + indent - mx.body * 0.95f;
            } else {
                mark.text = std::to_string(b.number) + ".";
                mark.w    = measure.width(mark.text, kPlain, mx.body);
                mark.x    = x + indent - mx.body * 0.4f - mark.w;
            }
            out.items.push_back(mark);
            const float used = flow(b.text, x + indent, y, width - indent, mx.body, Role::Text, kPlain);
            return y + used;
        }
        case Block::Kind::Table:
            if (!first) y += gap();
            return table(b, x, y, width) + mx.body * 0.2f;
        case Block::Kind::Card: {
            if (!first) y += mx.body * 0.9f;
            const std::size_t at = out.items.size();
            Item fill;
            fill.kind   = Item::Kind::Box;
            fill.role   = Role::CardFill;
            fill.radius = 12.0f * mx.scale;
            fill.x      = x;
            fill.y      = y;
            fill.w      = width;
            out.items.push_back(fill);
            const float pad    = mx.cardPad;
            prevWasItem        = false;
            const float bottom = blocks(b.children, x + pad, y + pad * 0.8f, width - 2 * pad) + pad;
            out.items[at].h    = bottom - y;
            Item frame         = out.items[at];
            frame.kind         = Item::Kind::Frame;
            frame.role         = Role::CardBorder;
            out.items.push_back(frame);
            return bottom;
        }
        case Block::Kind::Disclosure: {
            if (!first) y += gap();
            const int   index = nextDisclosure++;
            const bool  isOpen = index < static_cast<int>(opened.size()) && opened[index];
            const float lh     = lineH(mx.body);
            if (!b.id.empty()) {
                out.anchors.push_back({b.id, y});
                out.disclosureIds.push_back({b.id, index});
            }
            Item arrow;
            arrow.kind = Item::Kind::Arrow;
            arrow.role = Role::Muted;
            arrow.open = isOpen;
            arrow.x    = x;
            arrow.y    = y + lh * 0.5f - mx.body * 0.35f;
            arrow.w    = arrow.h = mx.body * 0.7f;
            out.items.push_back(arrow);
            const float textX = x + mx.body * 1.1f;
            const float used  = flow(b.text, textX, y, width - (textX - x), mx.body, Role::Muted, kBold);
            out.hits.push_back({x, y, width, used, {}, index});
            y += used;
            if (isOpen) {
                prevWasItem = false;
                y = blocks(b.children, x + mx.body, y + mx.body * 0.35f, width - mx.body);
            } else {
                skipDisclosures(b.children);   // keep the numbering the same, open or closed
            }
            return y;
        }
        }
        return y;
    }

    void skipDisclosures(const std::vector<Block>& list) { nextDisclosure += countDisclosures(list); }

    float table(const Block& b, float x, float y, float width) {
        std::size_t columns = 0;
        for (const auto& row : b.rows) columns = std::max(columns, row.size());
        if (columns == 0) return y;
        const float size = mx.body * 0.95f;
        const float padX = mx.body * 0.6f, padY = mx.body * 0.3f;

        std::vector<float> want(columns, 0.0f), least(columns, 0.0f);
        for (std::size_t r = 0; r < b.rows.size(); ++r)
            for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                const auto [total, widest] = natural(b.rows[r][c], size, r == 0 && b.header ? kBold : kPlain);
                want[c]  = std::max(want[c], total + 2 * padX);
                least[c] = std::max(least[c], widest + 2 * padX);
            }
        float sumWant = 0, sumLeast = 0;
        for (std::size_t c = 0; c < columns; ++c) {
            want[c] = std::max(want[c], least[c]);
            sumWant += want[c];
            sumLeast += least[c];
        }
        std::vector<float> col(columns);
        if (sumWant <= width) {   // room for every cell on one line: share what's left
            for (std::size_t c = 0; c < columns; ++c) col[c] = want[c] + (width - sumWant) * want[c] / sumWant;
        } else if (sumLeast >= width) {
            col = least;
        } else {   // each column gets its widest word, then the rest by how much more it wants
            const float extra = width - sumLeast, wanted = sumWant - sumLeast;
            for (std::size_t c = 0; c < columns; ++c) col[c] = least[c] + extra * (want[c] - least[c]) / wanted;
        }

        for (std::size_t r = 0; r < b.rows.size(); ++r) {
            const bool        head = r == 0 && b.header;
            const std::size_t fillAt = out.items.size();
            if (!head && (r % 2 == 0)) {   // HTML's even body rows (the header is row 0)
                Item fill;
                fill.kind = Item::Kind::Box;
                fill.role = Role::RowFill;
                fill.x    = x;
                fill.y    = y;
                fill.w    = width;
                out.items.push_back(fill);
            }
            float cx = x, rowH = 0;
            for (std::size_t c = 0; c < b.rows[r].size(); ++c) {
                const float h = flow(b.rows[r][c], cx + padX, y + padY, col[c] - 2 * padX, size, Role::Text,
                                     head ? kBold : kPlain);
                rowH = std::max(rowH, h);
                cx += col[c];
            }
            rowH += 2 * padY;
            if (fillAt < out.items.size() && out.items[fillAt].role == Role::RowFill
                && out.items[fillAt].kind == Item::Kind::Box)
                out.items[fillAt].h = rowH;
            y += rowH;
            Item rule;
            rule.kind = Item::Kind::Line;
            rule.role = head ? Role::HeaderRule : Role::Rule;
            rule.x    = x;
            rule.y    = y;
            rule.w    = width;
            rule.h    = (head ? 2.0f : 1.0f) * mx.scale;
            out.items.push_back(rule);
        }
        return y;
    }

    const Measure&           measure;
    const std::vector<bool>& opened;
    Metrics                  mx;
    int                      nextDisclosure = 0;
    bool                     prevWasItem    = false;
};

}   // namespace detail

// Lays the page out `width` wide. `opened[i]` says whether disclosure i
// (countDisclosures' order) is open; missing ones are closed.
inline Layout layout(const Document& doc, float width, const Measure& measure,
                     const std::vector<bool>& opened = {}, const Metrics& metrics = {}) {
    detail::Builder b(measure, metrics, opened);
    const float     margin = b.mx.margin;
    const float     column = std::max(80.0f, std::min(width - 2 * margin, b.mx.maxWidth));
    const float     x      = std::max(margin, (width - column) * 0.5f);
    float           y      = margin;

    // The head: the logo and the title beside it.
    if (!doc.logo.empty() || !doc.title.empty()) {
        float tx = x;
        if (!doc.logo.empty()) {
            Item logo;
            logo.kind   = Item::Kind::Image;
            logo.text   = doc.logo;
            logo.x      = x;
            logo.y      = y;
            logo.w      = logo.h = b.mx.logo;
            logo.radius = b.mx.logo * 0.18f;
            b.out.items.push_back(logo);
            if (!doc.logoLink.empty()) b.out.hits.push_back({x, y, logo.w, logo.h, doc.logoLink, -1});
            tx += b.mx.logo + b.mx.body * 0.8f;
        }
        const float headH = std::max(doc.logo.empty() ? 0.0f : b.mx.logo, b.lineH(b.mx.title));
        if (!doc.title.empty()) {
            Item title;
            title.kind  = Item::Kind::Text;
            title.text  = doc.title;
            title.style = kBold;
            title.size  = b.mx.title;
            title.role  = Role::Heading;
            title.x     = tx;
            title.y     = y;
            title.w     = measure.width(doc.title, kBold, b.mx.title);
            title.h     = headH;
            b.out.items.push_back(title);
        }
        b.out.anchors.push_back({"top", 0.0f});
        y += headH + b.mx.body * 0.4f;
    }

    y = b.blocks(doc.blocks, x, y, column);
    b.out.height = y + margin;
    return std::move(b.out);
}

}   // namespace uw::help
