#pragma once

// HelpDoc: reads a product's help.md into blocks a help window can lay out and
// draw itself, so help opens in the product's own window: no browser, no web
// server, no files written anywhere. Header-only, standard C++17 only.
//
// It reads the Markdown the help pages are written in, and no more:
//   - The page's head, as HTML: a <div> holding the logo (<img src>, inside an
//     <a href>) and the <h1> title; then a <p><em>...</em></p> note.
//   - Cards: `::: {.callout ...}` ... `:::`, each opening with `## Heading {#id}`;
//     also `~~~note [card]` ... `~~~`, a heading's id then on a line before it: `(#id)`.
//   - Disclosures: <details><summary>Title</summary> ... </details>, their body
//     in `::: disclosure-content` ... `:::` (a container that adds nothing).
//   - Headings (# to ######) with an optional {#id}; without one, the id is
//     Pandoc's: lower case, spaces to hyphens, punctuation dropped.
//   - Paragraphs; "- " bullets, "  - " under them; "1. " numbered items; pipe
//     tables, the |---| row ending the header.
//   - Inline: **bold**, *italic*, `code`, [text](#anchor) and [text](url),
//     backslash escapes and a few entities (&amp; &lt; &gt; &quot; &nbsp;).
// Anything else is kept as plain text, so nothing a page says is ever lost.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace uw::help {

enum Style : int { kPlain = 0, kBold = 1, kItalic = 2, kCode = 4 };

struct Span {
    std::string text;
    int         style = kPlain;
    std::string link;   // "#id" within the page, or a URL; "" if none
};
using Inline = std::vector<Span>;

struct Block {
    enum class Kind {
        Heading,      // level, text, id
        Paragraph,    // text
        Note,         // text: the italic line under the title
        Bullet,       // depth (0, 1, ...), text
        Numbered,     // number, depth, text
        Table,        // rows (rows[0] is the header if header)
        Card,         // children
        Disclosure,   // text (its title), id (if any), children; closed until opened
    };
    Kind                             kind = Kind::Paragraph;
    Inline                           text;
    std::string                      id;
    int                              level  = 0;
    int                              depth  = 0;
    int                              number = 0;
    bool                             header = false;
    std::vector<std::vector<Inline>> rows;
    std::vector<Block>               children;
};

struct Document {
    std::string        title;      // the <h1>
    std::string        logo;       // the <img src>, e.g. "Unlikelyware.png"; "" if none
    std::string        logoLink;   // the <a href> around it
    std::vector<Block> blocks;
};

// ---------------------------------------------------------------------------

namespace detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

inline bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

inline int indentOf(std::string_view s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// The value of attribute `name` in an HTML tag's text, or "".
inline std::string attribute(std::string_view tag, std::string_view name) {
    const std::string key = std::string(name) + "=\"";
    for (std::size_t at = tag.find(key); at != std::string_view::npos; at = tag.find(key, at + 1)) {
        if (at > 0 && !std::isspace(static_cast<unsigned char>(tag[at - 1]))) continue;
        const std::size_t from = at + key.size();
        const std::size_t to   = tag.find('"', from);
        if (to == std::string_view::npos) return {};
        return std::string(tag.substr(from, to - from));
    }
    return {};
}

// Text with every <tag> removed.
inline std::string stripTags(std::string_view s) {
    std::string out;
    bool        inTag = false;
    for (char c : s) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) out += c;
    }
    return out;
}

// Pandoc's identifier for a heading without {#id}.
inline std::string autoId(std::string_view text) {
    std::string id;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-' || c == '.') id += static_cast<char>(std::tolower(u));
        else if (std::isspace(u)) id += '-';
        else if (u >= 0x80) id += c;   // keep UTF-8 letters as they are
    }
    std::size_t first = 0;
    while (first < id.size() && !std::isalpha(static_cast<unsigned char>(id[first]))) ++first;
    return id.substr(first);
}

inline void appendText(Inline& out, std::string_view text, int style, const std::string& link) {
    if (text.empty()) return;
    if (!out.empty() && out.back().style == style && out.back().link == link) out.back().text += text;
    else out.push_back({std::string(text), style, link});
}

inline bool entity(std::string_view s, std::size_t i, std::string& out, std::size_t& used) {
    static const struct { const char* name; const char* text; } kEntities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&nbsp;", "\xC2\xA0"},
    };
    for (const auto& e : kEntities) {
        const std::string_view name(e.name);
        if (s.substr(i, name.size()) == name) {
            out  = e.text;
            used = name.size();
            return true;
        }
    }
    return false;
}

inline void parseInline(std::string_view s, int style, const std::string& link, Inline& out) {
    std::string run;
    auto flush = [&] {
        appendText(out, run, style, link);
        run.clear();
    };
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            run += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '&') {
            std::string text;
            std::size_t used = 0;
            if (entity(s, i, text, used)) {
                run += text;
                i += used;
                continue;
            }
        }
        if (c == '`') {
            const std::size_t end = s.find('`', i + 1);
            if (end != std::string_view::npos) {
                flush();
                appendText(out, s.substr(i + 1, end - i - 1), style | kCode, link);
                i = end + 1;
                continue;
            }
        }
        if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            const std::size_t end = s.find("**", i + 2);
            if (end != std::string_view::npos && end > i + 2) {
                flush();
                parseInline(s.substr(i + 2, end - i - 2), style | kBold, link, out);
                i = end + 2;
                continue;
            }
        }
        if (c == '*' && i + 1 < s.size() && !std::isspace(static_cast<unsigned char>(s[i + 1]))) {
            std::size_t end = i + 1;
            while ((end = s.find('*', end)) != std::string_view::npos && end + 1 < s.size() && s[end + 1] == '*')
                end += 2;   // a ** inside isn't the close
            if (end != std::string_view::npos && end > i + 1) {
                flush();
                parseInline(s.substr(i + 1, end - i - 1), style | kItalic, link, out);
                i = end + 1;
                continue;
            }
        }
        if (c == '[') {
            int         nest = 0;
            std::size_t close = std::string_view::npos;
            for (std::size_t j = i; j < s.size(); ++j) {
                if (s[j] == '[') ++nest;
                else if (s[j] == ']' && --nest == 0) {
                    close = j;
                    break;
                }
            }
            if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
                const std::size_t end = s.find(')', close + 2);
                if (end != std::string_view::npos) {
                    flush();
                    const std::string target(trim(s.substr(close + 2, end - close - 2)));
                    parseInline(s.substr(i + 1, close - i - 1), style, target, out);
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == '<') {   // inline HTML: <em>, <strong>, <code>, <br>; other tags dropped
            const std::size_t end = s.find('>', i);
            if (end != std::string_view::npos) {
                const std::string_view tag = s.substr(i + 1, end - i - 1);
                for (const auto& [open, bit] : {std::pair<std::string_view, int>{"em", kItalic},
                                                {"strong", kBold}, {"code", kCode}}) {
                    const std::string closeTag = "</" + std::string(open) + ">";
                    if (tag == open) {
                        const std::size_t stop = s.find(closeTag, end + 1);
                        if (stop != std::string_view::npos) {
                            flush();
                            parseInline(s.substr(end + 1, stop - end - 1), style | bit, link, out);
                            i = stop + closeTag.size();
                            goto next;
                        }
                    }
                }
                if (std::isalpha(static_cast<unsigned char>(tag.empty() ? ' ' : tag[0])) || (!tag.empty() && tag[0] == '/')) {
                    i = end + 1;
                    continue;
                }
            }
        }
        run += c;
        ++i;
    next:;
    }
    flush();
}

inline bool isTableSeparator(std::string_view line) {
    line = trim(line);
    if (line.size() < 3 || line[0] != '|') return false;
    for (char c : line)
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return line.find('-') != std::string_view::npos;
}

inline std::vector<Inline> tableCells(std::string_view line) {
    line = trim(line);
    if (!line.empty() && line.front() == '|') line.remove_prefix(1);
    if (!line.empty() && line.back() == '|') line.remove_suffix(1);
    std::vector<Inline> cells;
    std::size_t         from = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size() && line[i] == '\\') {
            ++i;
            continue;
        }
        if (i == line.size() || line[i] == '|') {
            Inline cell;
            parseInline(trim(line.substr(from, i - from)), kPlain, {}, cell);
            cells.push_back(std::move(cell));
            from = i + 1;
        }
    }
    return cells;
}

// A list marker: "- " / "* " (bullet) or "12. " (numbered). Returns the text's
// offset after the marker, or 0 if the line isn't a list item.
inline std::size_t listMarker(std::string_view line, bool& numbered, int& number) {
    const std::size_t at = line.find_first_not_of(" \t");
    if (at == std::string_view::npos) return 0;
    if ((line[at] == '-' || line[at] == '*') && at + 1 < line.size() && line[at + 1] == ' ') {
        numbered = false;
        return at + 2;
    }
    std::size_t j = at;
    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
    if (j > at && j + 1 < line.size() && line[j] == '.' && line[j + 1] == ' ') {
        numbered = true;
        number   = std::stoi(std::string(line.substr(at, j - at)));
        return j + 2;
    }
    return 0;
}

}   // namespace detail

// ---------------------------------------------------------------------------

inline Document parse(std::string_view markdown) {
    using namespace detail;

    std::vector<std::string_view> lines;
    for (std::size_t from = 0; from <= markdown.size();) {
        std::size_t to = markdown.find('\n', from);
        if (to == std::string_view::npos) to = markdown.size();
        std::string_view line = markdown.substr(from, to - from);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        from = to + 1;
    }

    Document doc;
    // Where blocks go: the document, or the innermost open card or disclosure.
    // `:::` closes whatever opened last, a transparent container included (nullptr).
    std::vector<std::vector<Block>*> into{&doc.blocks};
    std::vector<Block*>              open;   // cards and disclosures, innermost last
    std::vector<bool>                fences; // per open `:::`: true if it opened a card

    std::string pendingId;   // from a `(#id)` line, for the heading after it
    std::string paragraph;   // text gathered for the block in progress
    Block*      current = nullptr;   // a paragraph or list item that lines still join

    auto target = [&]() -> std::vector<Block>& { return *into.back(); };
    auto finish = [&] {
        if (current && !paragraph.empty()) parseInline(trim(paragraph), kPlain, {}, current->text);
        paragraph.clear();
        current = nullptr;
    };
    auto push = [&](Block block) -> Block& {
        target().push_back(std::move(block));
        return target().back();
    };
    auto openContainer = [&](Block block) {
        Block& b = push(std::move(block));
        open.push_back(&b);
        into.push_back(&b.children);
    };
    auto closeContainer = [&] {
        if (open.empty()) return;
        open.pop_back();
        into.pop_back();
    };

    for (std::size_t n = 0; n < lines.size(); ++n) {
        const std::string_view raw  = lines[n];
        const std::string_view line = trim(raw);

        if (line.empty()) {
            finish();
            continue;
        }

        // --- HTML blocks: the head, the note, disclosures -----------------
        if (startsWith(line, "<div")) {
            finish();
            std::string html;
            for (; n < lines.size(); ++n) {
                html += std::string(lines[n]) + "\n";
                if (lines[n].find("</div>") != std::string_view::npos) break;
            }
            const std::size_t img = html.find("<img");
            if (img != std::string::npos) {
                doc.logo = attribute(std::string_view(html).substr(img, html.find('>', img) - img), "src");
                const std::size_t a = html.rfind("<a", img);
                if (a != std::string::npos) doc.logoLink = attribute(std::string_view(html).substr(a, html.find('>', a) - a), "href");
            }
            const std::size_t h1 = html.find("<h1");
            if (h1 != std::string::npos) {
                const std::size_t from = html.find('>', h1) + 1;
                doc.title = std::string(trim(stripTags(html.substr(from, html.find("</h1>", from) - from))));
            } else {
                const std::string text(trim(stripTags(html)));
                if (!text.empty()) {
                    Block b;
                    parseInline(text, kPlain, {}, b.text);
                    push(std::move(b));
                }
            }
            continue;
        }
        if (startsWith(line, "<p")) {
            finish();
            std::string html;
            for (; n < lines.size(); ++n) {
                html += std::string(trim(lines[n])) + " ";
                if (lines[n].find("</p>") != std::string_view::npos) break;
            }
            std::string_view body = trim(html);
            body.remove_prefix(std::min(body.size(), body.find('>') + 1));
            if (const std::size_t end = body.rfind("</p>"); end != std::string_view::npos) body = body.substr(0, end);
            body = trim(body);
            Block b;
            const bool note = startsWith(body, "<em>") && body.size() >= 9 && body.substr(body.size() - 5) == "</em>";
            b.kind          = note ? Block::Kind::Note : Block::Kind::Paragraph;
            parseInline(note ? trim(body.substr(4, body.size() - 9)) : body, kPlain, {}, b.text);
            push(std::move(b));
            continue;
        }
        if (startsWith(line, "<details")) {
            finish();
            Block b;
            b.kind = Block::Kind::Disclosure;
            b.id   = attribute(line.substr(0, line.find('>')), "id");   // a link to it opens it
            openContainer(std::move(b));   // closed by </details>, not :::
            continue;
        }
        if (startsWith(line, "<summary")) {
            finish();
            if (!open.empty() && open.back()->kind == Block::Kind::Disclosure) {
                const std::size_t from = line.find('>') + 1;
                const std::size_t to   = line.find("</summary>");
                parseInline(trim(line.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from)),
                            kPlain, {}, open.back()->text);
            }
            continue;
        }
        if (startsWith(line, "</details")) {
            finish();
            if (!open.empty() && open.back()->kind == Block::Kind::Disclosure) closeContainer();
            continue;
        }
        if (startsWith(line, "</") || startsWith(line, "<!--")) {
            finish();
            continue;
        }

        // --- (#id): the next heading's id ------------------------------------
        if (line.size() > 3 && startsWith(line, "(#") && line.back() == ')' && line.find(' ') == std::string_view::npos) {
            finish();
            pendingId = std::string(line.substr(2, line.size() - 3));
            continue;
        }

        // --- Fenced divs ---------------------------------------------------
        if (startsWith(line, ":::") || startsWith(line, "~~~")) {
            finish();
            const std::string_view rest = trim(line.substr(3));
            if (rest.empty() || rest.find_first_not_of(":~") == std::string_view::npos) {   // a close
                if (!fences.empty()) {
                    if (fences.back()) closeContainer();
                    fences.pop_back();
                }
            } else if (rest.find("card") != std::string_view::npos || rest.find("callout") != std::string_view::npos) {
                Block b;
                b.kind = Block::Kind::Card;
                openContainer(std::move(b));
                fences.push_back(true);
            } else {
                fences.push_back(false);   // disclosure-content and the like: no box of its own
            }
            continue;
        }

        // --- Headings ------------------------------------------------------
        if (line[0] == '#') {
            std::size_t level = line.find_first_not_of('#');
            if (level != std::string_view::npos && level <= 6 && line[level] == ' ') {
                finish();
                std::string_view text = trim(line.substr(level));
                Block            b;
                b.kind  = Block::Kind::Heading;
                b.level = static_cast<int>(level);
                if (text.size() > 2 && text.back() == '}') {
                    const std::size_t brace = text.rfind("{#");
                    if (brace != std::string_view::npos) {
                        b.id = std::string(text.substr(brace + 2, text.size() - brace - 3));
                        text = trim(text.substr(0, brace));
                    }
                }
                parseInline(text, kPlain, {}, b.text);
                if (b.id.empty() && !pendingId.empty()) b.id = pendingId;
                pendingId.clear();
                if (b.id.empty()) {
                    std::string plain;
                    for (const Span& s : b.text) plain += s.text;
                    b.id = autoId(plain);
                }
                push(std::move(b));
                continue;
            }
        }

        // --- Tables --------------------------------------------------------
        if (line[0] == '|') {
            finish();
            Block b;
            b.kind = Block::Kind::Table;
            for (; n < lines.size() && !trim(lines[n]).empty() && trim(lines[n])[0] == '|'; ++n) {
                if (isTableSeparator(lines[n])) {
                    b.header = b.rows.size() == 1;
                    continue;
                }
                b.rows.push_back(tableCells(lines[n]));
            }
            --n;
            push(std::move(b));
            continue;
        }

        // --- Lists ---------------------------------------------------------
        bool        numbered = false;
        int         number   = 0;
        std::size_t marker   = listMarker(raw, numbered, number);
        if (marker) {
            finish();
            Block b;
            b.kind   = numbered ? Block::Kind::Numbered : Block::Kind::Bullet;
            b.number = number;
            b.depth  = indentOf(raw) / 2;
            current  = &push(std::move(b));
            paragraph = std::string(raw.substr(marker));
            continue;
        }

        // --- Text: continues the paragraph or list item, or starts one -----
        if (current) {
            paragraph += " ";
            paragraph += line;
            continue;
        }
        Block b;
        current   = &push(std::move(b));
        paragraph = std::string(line);
    }
    finish();
    return doc;
}

// The plain text of a run of spans.
inline std::string plainText(const Inline& text) {
    std::string out;
    for (const Span& s : text) out += s.text;
    return out;
}

}   // namespace uw::help
