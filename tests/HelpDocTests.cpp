// HelpDoc and HelpLayout (uw/help/): the help pages' Markdown read into
// blocks, and laid out at a width with every link and disclosure clickable.
//
// Given paths to help.md files as arguments, it also reads each and checks
// that nothing is left as raw markup and every #link has its heading.

#include <fstream>
#include <sstream>
#include <string>

#include "Check.h"
#include "uw/help/HelpDoc.h"
#include "uw/help/HelpLayout.h"

using namespace uw::help;

namespace {

// Every character 7 units wide (8 in bold), whatever the size.
struct FixedMeasure : Measure {
    float width(std::string_view text, int style, float) const override {
        std::size_t chars = 0;
        for (unsigned char c : text)
            if ((c & 0xC0) != 0x80) ++chars;   // count UTF-8 characters, not bytes
        return static_cast<float>(chars) * ((style & kBold) ? 8.0f : 7.0f);
    }
};

const char* kPage = R"(<div style="display: flex;">
  <a href="https://www.unlikelyware.com" target="_blank">
    <img src="Unlikelyware.png" alt="Unlikelyware" style="width: 56px;">
  </a>
  <h1 style="margin: 0;">TestSynth</h1>
</div>

<p>
  <em>TestSynth is a CLAP plugin. Cmd is Ctrl on Linux.</em>
</p>

::: {.callout .callout-note .card}
## On This Page
[Overview](#overview) · [The Panel](#panel)
:::

::: {.callout .callout-note .card}
## Overview {#overview}
A **bold** start, *italic*, `code` and a [link](https://example.com).
A second line of the same paragraph, with an \*escaped\* star &amp; an entity.

1. **Load it** in your host.
2. **Play.**
   Still the second item.

- One
  - Under one
- Two
:::

<details class="disclosure" id="more">
<summary class="disclosure-title subtle">More on it</summary>

::: disclosure-content
Hidden until opened.
:::

</details>

::: {.callout .callout-note .card}
## The Panel {#panel}
| Control | Range | What it does |
|---|---|---|
| **Shape** | Tri, Saw | Which wave you hear, and a rather long description that will need to wrap. |
| Level | 0 to 1 | How loud. |
:::
)";

void testParse() {
    const Document doc = parse(kPage);
    CHECK(doc.title == "TestSynth");
    CHECK(doc.logo == "Unlikelyware.png");
    CHECK(doc.logoLink == "https://www.unlikelyware.com");
    CHECK(doc.blocks.size() == 5);   // note, three cards, a disclosure
    if (doc.blocks.size() != 5) return;

    CHECK(doc.blocks[0].kind == Block::Kind::Note);
    CHECK(plainText(doc.blocks[0].text) == "TestSynth is a CLAP plugin. Cmd is Ctrl on Linux.");

    const Block& contents = doc.blocks[1];
    CHECK(contents.kind == Block::Kind::Card);
    CHECK(contents.children.size() == 2);
    CHECK(contents.children[0].kind == Block::Kind::Heading);
    CHECK(contents.children[0].id == "on-this-page");   // Pandoc's id, none given
    CHECK(contents.children[1].text.size() == 3);
    CHECK(contents.children[1].text[0].link == "#overview");
    CHECK(contents.children[1].text[2].link == "#panel");

    const Block& overview = doc.blocks[2];
    CHECK(overview.children.size() == 7);   // heading, paragraph, 2 numbered, 3 bullets
    if (overview.children.size() == 7) {
        CHECK(overview.children[0].id == "overview");
        CHECK(plainText(overview.children[0].text) == "Overview");
        const Inline& p = overview.children[1].text;
        CHECK(plainText(p) == "A bold start, italic, code and a link. A second line of the same paragraph, "
                              "with an *escaped* star & an entity.");
        CHECK(p.size() >= 8);
        if (p.size() >= 8) {
            CHECK(p[1].text == "bold" && p[1].style == kBold);
            CHECK(p[3].text == "italic" && p[3].style == kItalic);
            CHECK(p[5].text == "code" && p[5].style == kCode);
            CHECK(p[7].text == "link" && p[7].link == "https://example.com");
        }
        CHECK(overview.children[2].kind == Block::Kind::Numbered && overview.children[2].number == 1);
        CHECK(plainText(overview.children[3].text) == "Play. Still the second item.");
        CHECK(overview.children[4].kind == Block::Kind::Bullet && overview.children[4].depth == 0);
        CHECK(overview.children[5].depth == 1);
        CHECK(plainText(overview.children[6].text) == "Two");
    }

    const Block& more = doc.blocks[3];
    CHECK(more.kind == Block::Kind::Disclosure);
    CHECK(more.id == "more");
    CHECK(plainText(more.text) == "More on it");
    CHECK(more.children.size() == 1 && plainText(more.children[0].text) == "Hidden until opened.");

    const Block& panel = doc.blocks[4];
    CHECK(panel.children.size() == 2);
    if (panel.children.size() == 2) {
        const Block& t = panel.children[1];
        CHECK(t.kind == Block::Kind::Table);
        CHECK(t.header);
        CHECK(t.rows.size() == 3);
        CHECK(t.rows[1].size() == 3 && plainText(t.rows[1][0]) == "Shape" && t.rows[1][0][0].style == kBold);
    }
}

void testTildeCards() {
    const Document doc = parse("~~~note [card]\n(#kit)\n## The Kit\nText.\n~~~\n~~~note [card]\n## Next\n~~~\n");
    CHECK(doc.blocks.size() == 2);
    if (doc.blocks.size() != 2) return;
    CHECK(doc.blocks[0].kind == Block::Kind::Card);
    CHECK(doc.blocks[0].children.size() == 2);
    CHECK(!doc.blocks[0].children.empty() && doc.blocks[0].children[0].id == "kit");
    CHECK(!doc.blocks[1].children.empty() && doc.blocks[1].children[0].id == "next");   // (#id) used once
}

void testLayout() {
    const Document     doc = parse(kPage);
    const FixedMeasure m;
    CHECK(countDisclosures(doc.blocks) == 1);

    const Layout closed = layout(doc, 600, m);
    const Layout opened = layout(doc, 600, m, {true});
    CHECK(opened.height > closed.height);   // the disclosure's body shows

    // Every #link has a place to go, and clicking the logo leads home.
    for (const Hit& h : closed.hits)
        if (!h.link.empty() && h.link[0] == '#') CHECK(closed.anchor(h.link) >= 0);
    CHECK(closed.anchor("#panel") > closed.anchor("#overview"));
    const Hit* logo = closed.hitAt(30, 30);
    CHECK(logo && logo->link == "https://www.unlikelyware.com");

    // The disclosure's title toggles disclosure 0.
    bool toggle = false;
    for (const Hit& h : closed.hits) toggle = toggle || h.disclosure == 0;
    CHECK(toggle);
    CHECK(closed.disclosure("#more") == 0);
    CHECK(closed.anchor("more") > closed.anchor("overview"));

    // Narrower means taller, and no text runs past the column.
    const Layout narrow = layout(doc, 320, m);
    CHECK(narrow.height > closed.height);
    for (const Item& it : narrow.items)
        if (it.kind == Item::Kind::Text && it.text.find(' ') != std::string::npos) CHECK(it.x + it.w <= 320 + 7.5f);

    // Scale makes everything bigger.
    Metrics big;
    big.scale = 2.0f;
    CHECK(layout(doc, 1200, m, {}, big).height > closed.height * 1.5f);
}

// What a real page must never show: markup the parser didn't understand.
void checkPage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    if (!in) {
        std::fprintf(stderr, "can't read %s\n", path.c_str());
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const Document doc = parse(ss.str());
    const int      before = uw::test::failures;
    CHECK(!doc.title.empty());
    CHECK(!doc.logo.empty());

    const FixedMeasure m;
    std::vector<bool>  all(static_cast<std::size_t>(countDisclosures(doc.blocks)), true);
    const Layout       l = layout(doc, 700, m, all);
    for (const Item& it : l.items) {
        if (it.kind != Item::Kind::Text || (it.style & kCode)) continue;
        if (it.text.rfind("| ", 0) == 0) {   // a table row read as text ("Voice | Operators" is fine)
            std::fprintf(stderr, "%s: table row as text: %s\n", path.c_str(), it.text.c_str());
            ++uw::test::failures;
        }
        for (const char* bad : {"**", ":::", "](", "<", "{#", "|---"})
            if (it.text.find(bad) != std::string::npos) {
                std::fprintf(stderr, "%s: raw markup \"%s\" in: %s\n", path.c_str(), bad, it.text.c_str());
                ++uw::test::failures;
            }
    }
    for (const Hit& h : l.hits)
        if (!h.link.empty() && h.link[0] == '#' && l.anchor(h.link) < 0) {
            std::fprintf(stderr, "%s: no heading for %s\n", path.c_str(), h.link.c_str());
            ++uw::test::failures;
        }
    std::printf("%s: %zu blocks, %d disclosures, %.0f tall at 700%s\n", path.c_str(), doc.blocks.size(),
                countDisclosures(doc.blocks), l.height, uw::test::failures == before ? "" : " (FAILED)");
}

}   // namespace

int main(int argc, char** argv) {
    testParse();
    testTildeCards();
    testLayout();
    for (int i = 1; i < argc; ++i) checkPage(argv[i]);
    return uw::test::finish("help-doc");
}
