// help-preview: shows a help.md in uw-core's HelpWindow, as a product would,
// so a page can be read and checked without building the product.
//
//   help-preview help.md [logo.png] [--open-all] [--at '#id' | --scroll Y] [--width W --height H]
//                [--shot out.ppm] [--opener]
//
// --opener checks HelpOpener's life as a product's editor lives it: an editor
// window opens the help window, shows it again, closes it, and goes, twice.
// --shot draws one frame, saves it (binary PPM) and quits. Build it against
// any product's DGL (see README.md beside this file).

#include "Application.hpp"
#include "Window.hpp"
#include "uw/dgl/HelpWindow.hpp"

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

USE_NAMESPACE_DGL;

static std::string slurp(const char* path) {
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class PreviewView : public uwhelp::HelpView {
public:
    using HelpView::HelpView;
    std::string shot;
    Application* app = nullptr;

protected:
    void onNanoDisplay() override {
        HelpView::onNanoDisplay();
        if (shot.empty()) return;
        NanoVG::endFrame();
        const int w = int(getWidth()), h = int(getHeight());
        std::vector<unsigned char> px(size_t(w) * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        if (FILE* f = std::fopen(shot.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y) std::fwrite(&px[size_t(y) * w * 3], 1, size_t(w) * 3, f);
            std::fclose(f);
        }
        NanoVG::beginFrame(getWidth(), getHeight());
        shot.clear();
        app->quit();
    }
};

// An editor stand-in: on idle ticks it opens help, shows it again, and closes it.
class EditorStub : public NanoTopLevelWidget, public IdleCallback {
public:
    EditorStub(Window& w, const std::string& page, std::vector<uwhelp::Image> images, Application& app)
        : NanoTopLevelWidget(w), fPage(page), fImages(std::move(images)), fApp(app) {}
    void idleCallback() override {
        ++fTick;
        const auto* md = reinterpret_cast<const uchar*>(fPage.data());
        if (fTick == 2 || fTick == 30) fHelp.open(*this, "Opener Help", md, uint(fPage.size()), fImages, "#overview");
        if (fTick == 15) fHelp.open(*this, "Opener Help", md, uint(fPage.size()), fImages);   // shown again
        if (fTick == 20 || fTick == 45) fHelp.close();
        if (fTick == 50) {
            std::printf("opener: opened, reshown, closed, reopened and closed\n");
            fApp.quit();
        }
    }
protected:
    void onNanoDisplay() override {
        beginPath();
        rect(0, 0, getWidth(), getHeight());
        fillColor(Color(26, 28, 33));
        fill();
    }
private:
    std::string                fPage;
    std::vector<uwhelp::Image> fImages;
    Application&               fApp;
    uwhelp::HelpOpener         fHelp;
    int                        fTick = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s help.md [logo.png] [--open-all] [--at #id] [--scroll Y] [--shot out.ppm]\n", argv[0]);
        return 2;
    }
    std::string page = slurp(argv[1]), logo, at, shot;
    bool        openAll = false, opener = false;
    float       scroll  = 0;
    unsigned    width = 760, height = 860;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--open-all") openAll = true;
        else if (a == "--opener") opener = true;
        else if (a == "--at" && i + 1 < argc) at = argv[++i];
        else if (a == "--scroll" && i + 1 < argc) scroll = std::strtof(argv[++i], nullptr);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = unsigned(std::atoi(argv[++i]));
        else if (a == "--height" && i + 1 < argc) height = unsigned(std::atoi(argv[++i]));
        else logo = slurp(argv[i]);
    }

    Application                app;
    Window                     window(app);
    std::vector<uwhelp::Image> images;
    if (!logo.empty())
        images.push_back({"Unlikelyware.png", reinterpret_cast<const uchar*>(logo.data()), uint(logo.size())});
    if (opener) {
        Window::ScopedGraphicsContext context(window);
        EditorStub stub(window, page, images, app);
        context.done();
        window.setSize(400, 300);
        window.addIdleCallback(&stub, 30);
        window.show();
        app.exec();
        window.removeIdleCallback(&stub);
        return 0;
    }
    Window::ScopedGraphicsContext context(window);
    PreviewView                   view(window, page, images);
    context.done();
    view.shot = shot;
    view.app  = &app;
    window.setTitle((view.title() + " Help").c_str());
    window.setResizable(true);
    const double s = std::max(1.0, window.getScaleFactor());
    window.setSize(uint(width * s), uint(height * s));
    if (openAll) view.openAll(true);
    window.show();
    if (!at.empty()) view.showAnchor(at);
    else if (scroll > 0) view.setScroll(float(scroll * s));
    app.exec();
    return 0;
}
