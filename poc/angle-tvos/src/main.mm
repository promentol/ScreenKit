// ANGLE hello-world for the tvOS simulator.
//
// Proves the M1 gate: ANGLE's EGL -> Metal backend initialises on tvOS and
// GLES2 draws to a CAMetalLayer. No SDL, no engine -- just UIKit + EGL + GLES2,
// so a failure here is unambiguously ANGLE's.

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>   // EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#include <GLES3/gl3.h>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 5x7 bitmap font, same table as the SDL3 PoC. One byte per row, low 5 bits,
// MSB-of-5 leftmost.
// ---------------------------------------------------------------------------
namespace font {
constexpr int kW = 5, kH = 7, kAdvance = 6;

struct Glyph { char ch; unsigned char rows[kH]; };

constexpr Glyph kGlyphs[] = {
    {' ', {0b00000,0b00000,0b00000,0b00000,0b00000,0b00000,0b00000}},
    {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'B', {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}},
    {'C', {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}},
    {'D', {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}},
    {'E', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}},
    {'F', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}},
    {'G', {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111}},
    {'H', {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'I', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b11111}},
    {'J', {0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100}},
    {'K', {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}},
    {'L', {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}},
    {'M', {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}},
    {'N', {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}},
    {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
    {'Q', {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}},
    {'R', {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}},
    {'S', {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'U', {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'V', {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}},
    {'W', {0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001}},
    {'X', {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}},
    {'Y', {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}},
    {'Z', {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}},
    {'0', {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}},
    {'1', {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'2', {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}},
    {'3', {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}},
    {'4', {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}},
    {'5', {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}},
    {'6', {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}},
    {'7', {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}},
    {'8', {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}},
    {'9', {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}},
    {'!', {0b00100,0b00100,0b00100,0b00100,0b00100,0b00000,0b00100}},
    {'.', {0b00000,0b00000,0b00000,0b00000,0b00000,0b01100,0b01100}},
    {'-', {0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000}},
};

const unsigned char* rowsFor(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const Glyph& g : kGlyphs) if (g.ch == c) return g.rows;
    return kGlyphs[0].rows;
}
int widthUnits(const std::string& s) {
    return s.empty() ? 0 : int(s.size()) * kAdvance - 1;
}
}  // namespace font

// ---------------------------------------------------------------------------

// All shaders are "#version 300 es": `in`/`out` instead of attribute/varying,
// explicit layout locations, and a declared fragment output. None of this
// compiles on an ES2 context.

// Background needs no vertex buffer at all -- gl_VertexID is ES3-only.
static const char* kVertBg = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFragBg = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
out vec4 fragColor;
uniform float u_time;
void main() {
    float w = sin(v_uv.x * 6.2831 + u_time * 0.9) * 0.5 + 0.5;
    vec3 c = vec3(0.05, 0.02, 0.10) + vec3(0.10, 0.03, 0.18) * w
           + vec3(0.02, 0.06, 0.12) * v_uv.y;
    fragColor = vec4(c, 1.0);
}
)";

// Glyph pixels are drawn as ONE instanced quad: a_inst advances per instance
// via glVertexAttribDivisor, which is core in ES3 and an extension in ES2.
static const char* kVertText = R"(#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_inst;   // x, y, sizeX, sizeY
out float v_x;
void main() {
    vec2 p = a_inst.xy + a_corner * a_inst.zw;
    v_x = p.x;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

static const char* kFragText = R"(#version 300 es
precision mediump float;
in float v_x;
out vec4 fragColor;
uniform float u_time;
vec3 hsv(float h, float s, float v) {
    vec3 k = mod(vec3(5.0, 3.0, 1.0) + h * 6.0, 6.0);
    return v - v * s * clamp(min(k, 4.0 - k), 0.0, 1.0);
}
void main() {
    fragColor = vec4(hsv(fract(v_x * 0.45 + 0.5 + u_time * 0.25), 0.55, 1.0), 1.0);
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        NSLog(@"[angle] shader compile failed: %s", log);
    }
    return s;
}

static GLuint linkProgram(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        NSLog(@"[angle] program link failed: %s", log);
    }
    return p;
}

// ---------------------------------------------------------------------------

@interface ANGLEView : UIView
@end

@implementation ANGLEView {
    EGLDisplay _display;
    EGLSurface _surface;
    EGLContext _context;
    CADisplayLink* _link;
    GLuint _progBg, _progText, _vboCorner, _vboInst, _vao, _vaoEmpty;
    GLsizei _instCount;
    double _start;
    BOOL _ready;
}

+ (Class)layerClass { return [CAMetalLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.contentScaleFactor = [UIScreen mainScreen].nativeScale;
    }
    return self;
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window && !_ready) [self setupEGL];
}

- (void)setupEGL {
    // Ask ANGLE for the Metal backend explicitly rather than taking the default,
    // so a fallback to some other backend shows up as a failure, not a silent pass.
    EGLint displayAttribs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE
    };
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (getPlatformDisplay) {
        _display = getPlatformDisplay(
            EGL_PLATFORM_ANGLE_ANGLE,
            reinterpret_cast<void*>(static_cast<intptr_t>(EGL_DEFAULT_DISPLAY)),
            displayAttribs);
    } else {
        NSLog(@"[angle] eglGetPlatformDisplayEXT missing, falling back");
        _display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (_display == EGL_NO_DISPLAY) { NSLog(@"[angle] no EGLDisplay"); return; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(_display, &major, &minor)) {
        NSLog(@"[angle] eglInitialize failed: 0x%04x", eglGetError());
        return;
    }
    NSLog(@"[angle] EGL %d.%d", major, minor);
    NSLog(@"[angle] EGL_VENDOR  = %s", eglQueryString(_display, EGL_VENDOR));
    NSLog(@"[angle] EGL_VERSION = %s", eglQueryString(_display, EGL_VERSION));

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(_display, cfgAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        NSLog(@"[angle] eglChooseConfig failed: 0x%04x", eglGetError());
        return;
    }

    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    layer.contentsScale = self.contentScaleFactor;
    layer.opaque = YES;

    // On Apple platforms ANGLE takes a CALayer as the native window.
    _surface = eglCreateWindowSurface(_display, config,
                                      (EGLNativeWindowType)layer, nullptr);
    if (_surface == EGL_NO_SURFACE) {
        NSLog(@"[angle] eglCreateWindowSurface failed: 0x%04x", eglGetError());
        return;
    }

    // Walk down from 3.2: ANGLE's Metal backend does not implement every ES3
    // minor, and asking for a specific one tells us exactly which we got.
    const struct { EGLint major, minor; } kTries[] = {{3, 2}, {3, 1}, {3, 0}};
    for (const auto& v : kTries) {
        const EGLint attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, v.major,
            EGL_CONTEXT_MINOR_VERSION, v.minor,
            EGL_NONE
        };
        _context = eglCreateContext(_display, config, EGL_NO_CONTEXT, attribs);
        if (_context != EGL_NO_CONTEXT) {
            NSLog(@"[angle] requested ES %d.%d -> context created", v.major, v.minor);
            break;
        }
        NSLog(@"[angle] ES %d.%d unavailable (0x%04x)", v.major, v.minor, eglGetError());
    }
    if (_context == EGL_NO_CONTEXT) {
        NSLog(@"[angle] no ES3 context could be created");
        return;
    }
    if (!eglMakeCurrent(_display, _surface, _surface, _context)) {
        NSLog(@"[angle] eglMakeCurrent failed: 0x%04x", eglGetError());
        return;
    }

    NSLog(@"[angle] Hello, world from ANGLE on tvOS!");
    NSLog(@"[angle] GL_VENDOR   = %s", glGetString(GL_VENDOR));
    NSLog(@"[angle] GL_RENDERER = %s", glGetString(GL_RENDERER));
    NSLog(@"[angle] GL_VERSION  = %s", glGetString(GL_VERSION));
    NSLog(@"[angle] GL_SHADING_LANGUAGE_VERSION = %s", glGetString(GL_SHADING_LANGUAGE_VERSION));

    // These enums and glGetStringi do not exist in ES2 -- calling them at all is
    // the proof, not the version string above.
    GLint numExt = 0, maxLayers = 0, maxUBO = 0, maxSamples = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
    glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUBO);
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    NSLog(@"[angle] ES3 limits: extensions=%d arrayTexLayers=%d uboBindings=%d maxSamples=%d",
          numExt, maxLayers, maxUBO, maxSamples);
    if (numExt > 0) {
        NSLog(@"[angle] glGetStringi(GL_EXTENSIONS, 0) = %s", glGetStringi(GL_EXTENSIONS, 0));
    }

    EGLint sw = 0, sh = 0;
    eglQuerySurface(_display, _surface, EGL_WIDTH,  &sw);
    eglQuerySurface(_display, _surface, EGL_HEIGHT, &sh);
    [self buildGeometryForWidth:sw height:sh];

    _start = CACurrentMediaTime();
    _ready = YES;
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(render)];
    [_link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
}

- (void)buildGeometryForWidth:(int)w height:(int)h {
    _progBg   = linkProgram(kVertBg,   kFragBg);
    _progText = linkProgram(kVertText, kFragText);

    const float aspect = (h > 0) ? float(w) / float(h) : 1.0f;

    // One unit quad, reused by every glyph pixel via instancing.
    const GLfloat corner[] = { 0,0, 1,0, 0,1,  1,0, 1,1, 0,1 };
    glGenBuffers(1, &_vboCorner);
    glBindBuffer(GL_ARRAY_BUFFER, _vboCorner);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corner), corner, GL_STATIC_DRAW);

    const std::string line1 = "HELLO ANGLE";
    const std::string line2 = "GLES3 ON METAL - TVOS";
    std::vector<GLfloat> inst;   // x, y, sizeX, sizeY per lit font pixel

    auto emit = [&](const std::string& str, float cy, float px) {
        const float pxx = px / aspect;          // keep glyphs square on a 16:9 panel
        const float lineW = font::widthUnits(str) * pxx;
        float penX = -lineW * 0.5f;
        const float top = cy + font::kH * px * 0.5f;
        for (char c : str) {
            const unsigned char* rows = font::rowsFor(c);
            for (int r = 0; r < font::kH; ++r) {
                for (int col = 0; col < font::kW; ++col) {
                    if (!(rows[r] & (1u << (font::kW - 1 - col)))) continue;
                    inst.push_back(penX + col * pxx);
                    inst.push_back(top - (r + 1) * px);
                    inst.push_back(pxx);
                    inst.push_back(px);
                }
            }
            penX += font::kAdvance * pxx;
        }
    };
    emit(line1,  0.10f, 0.044f);
    emit(line2, -0.18f, 0.019f);

    _instCount = GLsizei(inst.size() / 4);
    glGenBuffers(1, &_vboInst);
    glBindBuffer(GL_ARRAY_BUFFER, _vboInst);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(inst.size() * sizeof(GLfloat)),
                 inst.data(), GL_STATIC_DRAW);

    // Vertex array objects are core in ES3 (an extension in ES2).
    glGenVertexArrays(1, &_vao);
    glBindVertexArray(_vao);
    glBindBuffer(GL_ARRAY_BUFFER, _vboCorner);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, _vboInst);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glVertexAttribDivisor(1, 1);            // advance once per instance -- ES3 core
    glBindVertexArray(0);

    // The background draw binds no attributes at all, so it still needs a VAO.
    glGenVertexArrays(1, &_vaoEmpty);

    NSLog(@"[angle] %d glyph instances x 6 verts, drawn in 1 instanced call",
          _instCount);
}

- (void)render {
    if (!_ready) return;
    eglMakeCurrent(_display, _surface, _surface, _context);

    EGLint w = 0, h = 0;
    eglQuerySurface(_display, _surface, EGL_WIDTH,  &w);
    eglQuerySurface(_display, _surface, EGL_HEIGHT, &h);
    glViewport(0, 0, w, h);

    const float t = float(CACurrentMediaTime() - _start);

    glClearColor(0.02f, 0.01f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Background: three vertices, zero buffers -- positions come from gl_VertexID.
    glUseProgram(_progBg);
    glUniform1f(glGetUniformLocation(_progBg, "u_time"), t);
    glBindVertexArray(_vaoEmpty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Text: one quad, _instCount instances, one draw call.
    glUseProgram(_progText);
    glUniform1f(glGetUniformLocation(_progText, "u_time"), t);
    glBindVertexArray(_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, _instCount);

    glBindVertexArray(0);
    eglSwapBuffers(_display, _surface);
}
@end

// ---------------------------------------------------------------------------

@interface ViewController : UIViewController
@end
@implementation ViewController
- (void)loadView { self.view = [[ANGLEView alloc] initWithFrame:UIScreen.mainScreen.bounds]; }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow* window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)opts {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [ViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
