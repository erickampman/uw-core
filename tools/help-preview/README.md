# help-preview

Shows a product's `help.md` in uw-core's help window (`uw/dgl/HelpWindow.hpp`),
as the product's Help button does, so a page can be read and checked without
building or loading the product.

It isn't part of uw-core's build: it needs DGL, and uw-core doesn't carry DPF.
Build it against any product's DGL, here PlainOldSynth's (configure and build
that product first, so its `libdgl-opengl.a` exists):

```bash
P=../Dev-Audio/PlainOldSynth
clang++ -std=c++17 -O1 -DDGL_NAMESPACE=PlainOldSynthDGL -DDGL_OPENGL -DHAVE_OPENGL -DHAVE_DGL \
  -DDGL_USE_FILE_BROWSER -DGL_SILENCE_DEPRECATION \
  -I$P/dpf/dgl -I$P/dpf/distrho -Isrc tools/help-preview/main.cpp $P/build/libdgl-opengl.a \
  -framework Cocoa -framework OpenGL -framework CoreVideo -framework UniformTypeIdentifiers \
  -o build/help-preview
```

(On Linux: `g++`, and `-lGL -lX11 -lXext -lXrandr -lXcursor -ldbus-1 -lpthread -ldl` in place of the
frameworks.) Then, from uw-core:

```bash
build/help-preview $P/Resources/help.md $P/Resources/Unlikelyware-Final-Color.png
```

Options:
- `--at '#id'` opens at a section, as NoteGran's editors do.
- `--open-all` opens every disclosure.
- `--width W --height H` sizes the window (in points; 760 × 860 by default).
- `--shot out.ppm` draws one frame, saves it as a PPM image and quits: a look
  at the page without a screenshot.
- `--opener` checks `HelpOpener`'s life as an editor lives it (open, show
  again, close, open again, close) and quits.

`test-help-doc`, given the products' help.md paths, checks the same pages
without a window: no raw markup left in the text, every `#link` with a heading.
