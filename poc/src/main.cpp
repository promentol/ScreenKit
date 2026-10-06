// SDL3 "hello world" for the iOS / tvOS simulator.
//
// Draws "HELLO, WORLD!" with a built-in 5x7 bitmap font -- every glyph pixel is
// an SDL_FRect, batched into one SDL_RenderFillRects call per colour -- over an
// animated gradient. No assets, no font library, nothing to bundle.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 5x7 bitmap font. One byte per row, low 5 bits used, MSB-of-5 is leftmost.
// ---------------------------------------------------------------------------
namespace font {

constexpr int kW = 5;
constexpr int kH = 7;
constexpr int kAdvance = kW + 1;  // one blank column between glyphs

struct Glyph {
    char ch;
    std::uint8_t rows[kH];
};

constexpr Glyph kGlyphs[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
};

// Unknown characters render blank rather than dropping out of the layout.
const std::uint8_t* rowsFor(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    for (const Glyph& g : kGlyphs) {
        if (g.ch == c) {
            return g.rows;
        }
    }
    return kGlyphs[0].rows;  // space
}

// Width of a string in font units (multiply by the pixel scale).
int widthUnits(const std::string& s)
{
    return s.empty() ? 0 : static_cast<int>(s.size()) * kAdvance - 1;
}

}  // namespace font

// ---------------------------------------------------------------------------

namespace {

#if defined(SDL_PLATFORM_TVOS)
constexpr const char* kPlatform = "TVOS";
// tvOS overscan: keep everything inside the title-safe area.
constexpr float kSafeInset = 0.05f;
#elif defined(SDL_PLATFORM_IOS)
constexpr const char* kPlatform = "IOS";
constexpr float kSafeInset = 0.04f;
#else
constexpr const char* kPlatform = "DESKTOP";
constexpr float kSafeInset = 0.03f;
#endif

SDL_Color hsv(float h, float s, float v, std::uint8_t a = 255)
{
    h -= std::floor(h);  // wrap into [0, 1)
    const float sector = std::floor(h * 6.0f);
    const float f = h * 6.0f - sector;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);

    float r = v, g = v, b = v;
    switch (static_cast<int>(sector) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    const auto to8 = [](float x) {
        return static_cast<std::uint8_t>(SDL_clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return SDL_Color{to8(r), to8(g), to8(b), a};
}

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::string rendererName = "UNKNOWN";
    std::uint64_t startMs = 0;
    std::uint64_t lastMs = 0;
    float fps = 0.0f;
    std::vector<SDL_FRect> scratch;  // reused every frame, no per-frame alloc
};

// Appends the lit pixels of `text` to `out` as rects.
void emitText(std::vector<SDL_FRect>& out, const std::string& text, float x, float y, float px)
{
    float penX = x;
    for (const char c : text) {
        const std::uint8_t* rows = font::rowsFor(c);
        for (int row = 0; row < font::kH; ++row) {
            for (int col = 0; col < font::kW; ++col) {
                if (rows[row] & (1u << (font::kW - 1 - col))) {
                    out.push_back(SDL_FRect{penX + col * px, y + row * px, px, px});
                }
            }
        }
        penX += font::kAdvance * px;
    }
}

void drawText(App& app, const std::string& text, float x, float y, float px, SDL_Color c)
{
    app.scratch.clear();
    emitText(app.scratch, text, x, y, px);
    if (app.scratch.empty()) {
        return;
    }
    SDL_SetRenderDrawColor(app.renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRects(app.renderer, app.scratch.data(), static_cast<int>(app.scratch.size()));
}

std::string upper(std::string s)
{
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// SDL3 main callbacks. On iOS/tvOS these hand control back to the UIKit run
// loop between frames, which a plain `while (running)` loop in main() does not.
// ---------------------------------------------------------------------------

SDL_AppResult SDL_AppInit(void** appstate, int /*argc*/, char** /*argv*/)
{
    SDL_SetAppMetadata("SDL Hello", "1.0", "com.example.sdlhello");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App* app = new App();

    // Size is a hint only -- iOS and tvOS always give a fullscreen window.
    // RESIZABLE matters on iOS: without it SDL locks the app to the orientation
    // implied by the requested size (1280x720 would force landscape). The layout
    // is computed from the drawable every frame, so any orientation is fine.
    if (!SDL_CreateWindowAndRenderer("SDL Hello", 1280, 720,
                                     SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                         SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        delete app;
        return SDL_APP_FAILURE;
    }

    SDL_SetRenderVSync(app->renderer, 1);

    if (const char* name = SDL_GetRendererName(app->renderer)) {
        app->rendererName = upper(name);
    }

    int w = 0, h = 0;
    SDL_GetRenderOutputSize(app->renderer, &w, &h);

    SDL_Log("Hello, world! SDL %d.%d.%d on %s",
            SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetPlatform());
    SDL_Log("renderer=%s  drawable=%dx%d", app->rendererName.c_str(), w, h);

    app->startMs = SDL_GetTicks();
    app->lastMs = app->startMs;
    app->scratch.reserve(4096);

    *appstate = app;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    (void)appstate;
    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        case SDL_EVENT_KEY_DOWN:
            SDL_Log("key down: %s", SDL_GetKeyName(event->key.key));
            if (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_AC_BACK) {
                return SDL_APP_SUCCESS;
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            SDL_Log("gamepad button %d", event->gbutton.button);
            break;

        case SDL_EVENT_FINGER_DOWN:
            SDL_Log("touch at %.3f, %.3f", event->tfinger.x, event->tfinger.y);
            break;

        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    App& app = *static_cast<App*>(appstate);

    const std::uint64_t nowMs = SDL_GetTicks();
    const float t = static_cast<float>(nowMs - app.startMs) / 1000.0f;
    const float dt = static_cast<float>(nowMs - app.lastMs) / 1000.0f;
    app.lastMs = nowMs;
    if (dt > 0.0f) {
        app.fps += (1.0f / dt - app.fps) * 0.05f;  // smoothed, avoids a jittery readout
    }

    int w = 0, h = 0;
    SDL_GetRenderOutputSize(app.renderer, &w, &h);
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    // --- background: vertical bands, hue drifting over time ------------------
    constexpr int kBands = 64;
    const float bandW = fw / kBands + 1.0f;  // +1 avoids seams from rounding
    for (int i = 0; i < kBands; ++i) {
        const float u = static_cast<float>(i) / kBands;
        const float wave = std::sin(u * 6.28318f + t * 0.9f) * 0.5f + 0.5f;
        const SDL_Color c = hsv(0.62f + u * 0.18f + t * 0.03f, 0.75f, 0.08f + wave * 0.10f);
        SDL_SetRenderDrawColor(app.renderer, c.r, c.g, c.b, 255);
        const SDL_FRect band{u * fw, 0.0f, bandW, fh};
        SDL_RenderFillRect(app.renderer, &band);
    }

    // --- layout --------------------------------------------------------------
    const float inset = SDL_min(fw, fh) * kSafeInset;
    const float usableW = fw - inset * 2.0f;

    const std::string line1 = "HELLO, WORLD!";
    const std::string line2 = std::string("SDL3 ON ") + kPlatform;
    const std::string line3 =
        std::to_string(w) + "X" + std::to_string(h) + "  " + app.rendererName + "  " +
        std::to_string(static_cast<int>(app.fps + 0.5f)) + " FPS";

    const float px1 = usableW * 0.86f / static_cast<float>(font::widthUnits(line1));
    const float px2 = px1 * 0.42f;
    const float px3 = px1 * 0.26f;

    const float w1 = font::widthUnits(line1) * px1;
    const float w2 = font::widthUnits(line2) * px2;
    const float w3 = font::widthUnits(line3) * px3;

    const float bob = std::sin(t * 1.7f) * px1 * 0.5f;
    const float blockH = font::kH * (px1 + px2 + px3) + px1 * 2.2f;
    const float y1 = (fh - blockH) * 0.5f + bob;
    const float y2 = y1 + font::kH * px1 + px1 * 1.4f;
    const float y3 = y2 + font::kH * px2 + px1 * 0.8f;

    // --- headline: shadow, then per-character hue cycling --------------------
    drawText(app, line1, (fw - w1) * 0.5f + px1 * 0.6f, y1 + px1 * 0.6f, px1,
             SDL_Color{0, 0, 0, 255});

    float penX = (fw - w1) * 0.5f;
    for (std::size_t i = 0; i < line1.size(); ++i) {
        const float phase = t * 0.35f - static_cast<float>(i) * 0.045f;
        drawText(app, std::string(1, line1[i]), penX, y1, px1, hsv(phase, 0.55f, 1.0f));
        penX += font::kAdvance * px1;
    }

    // --- underline bar, pulsing ----------------------------------------------
    const float pulse = std::sin(t * 2.4f) * 0.5f + 0.5f;
    const SDL_Color bar = hsv(t * 0.35f + 0.5f, 0.6f, 0.55f + pulse * 0.45f);
    SDL_SetRenderDrawColor(app.renderer, bar.r, bar.g, bar.b, 255);
    const float barW = w1 * (0.35f + pulse * 0.65f);
    const SDL_FRect underline{(fw - barW) * 0.5f, y1 + font::kH * px1 + px1 * 0.5f, barW, px1 * 0.25f};
    SDL_RenderFillRect(app.renderer, &underline);

    // --- subtitles ------------------------------------------------------------
    drawText(app, line2, (fw - w2) * 0.5f, y2, px2, SDL_Color{225, 232, 245, 255});
    drawText(app, line3, (fw - w3) * 0.5f, y3, px3, SDL_Color{120, 140, 175, 255});

    SDL_RenderPresent(app.renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/)
{
    SDL_Log("goodbye");
    delete static_cast<App*>(appstate);
}
