#include "gl_presenter.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "platform.h"

namespace cartucho {

namespace {

const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
uniform vec2 u_uv_scale;   // part of the texture holding the picture
uniform float u_flip;      // 1.0 when the picture is stored bottom-up
uniform vec4 u_crop;       // shown part of the picture: x0, y0, x1, y1 (top-left origin)
out vec2 v_uv;
void main() {
    vec2 uv = mix(u_crop.xy, u_crop.zw, a_uv);
    if (u_flip > 0.5) uv.y = 1.0 - uv.y;
    v_uv = uv * u_uv_scale;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

// One shader for every filter; the branch is on a uniform, so it costs
// nothing measurable and keeps switching filters instant.
const char* kFragmentShader = R"(#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform int u_mode;
uniform vec2 u_src_size;   // picture size in texels
uniform vec2 u_out_size;   // picture size on screen in pixels
uniform vec2 u_uv_scale;
uniform float u_flip;
uniform vec4 u_viewport;   // where the picture is drawn: x, y, w, h (GL pixels)
out vec4 o_color;

vec2 to_texel(vec2 uv) { return uv / u_uv_scale * u_src_size; }
vec2 to_uv(vec2 texel) { return texel / u_src_size * u_uv_scale; }

// Colour of the source pixel at `cell` + offset (in screen directions: +y is
// down on screen whatever the texture's row order).
vec3 px(vec2 cell, vec2 d) {
    if (u_flip > 0.5) d.y = -d.y;
    vec2 c = clamp(cell + d, vec2(0.0), u_src_size - 1.0);
    return texture(u_tex, to_uv(c + 0.5)).rgb;
}
bool eq(vec3 a, vec3 b) { return all(lessThan(abs(a - b), vec3(0.02))); }

// Scale2x (EPX): each source pixel becomes 2x2; a corner takes a neighbour's
// colour where two neighbours meet along a diagonal edge.
vec3 scale2x(vec2 cell, vec2 sub) {
    vec3 B = px(cell, vec2(0, -1)), D = px(cell, vec2(-1, 0)), E = px(cell, vec2(0, 0));
    vec3 F = px(cell, vec2(1, 0)), H = px(cell, vec2(0, 1));
    if (eq(B, H) || eq(D, F)) return E;
    if (sub.y < 1.0) {
        if (sub.x < 1.0) return eq(D, B) ? D : E;
        return eq(B, F) ? F : E;
    }
    if (sub.x < 1.0) return eq(D, H) ? D : E;
    return eq(H, F) ? F : E;
}

// Scale3x (AdvMAME3x): 3x3 per source pixel, also filling edge midpoints.
vec3 scale3x(vec2 cell, vec2 sub) {
    vec3 A = px(cell, vec2(-1, -1)), B = px(cell, vec2(0, -1)), C = px(cell, vec2(1, -1));
    vec3 D = px(cell, vec2(-1, 0)), E = px(cell, vec2(0, 0)), F = px(cell, vec2(1, 0));
    vec3 G = px(cell, vec2(-1, 1)), H = px(cell, vec2(0, 1)), I = px(cell, vec2(1, 1));
    if (eq(B, H) || eq(D, F)) return E;
    bool db = eq(D, B), bf = eq(B, F), dh = eq(D, H), hf = eq(H, F);
    int i = int(sub.y) * 3 + int(sub.x);
    if (i == 0) return db ? D : E;
    if (i == 1) return ((db && !eq(E, C)) || (bf && !eq(E, A))) ? B : E;
    if (i == 2) return bf ? F : E;
    if (i == 3) return ((db && !eq(E, G)) || (dh && !eq(E, A))) ? D : E;
    if (i == 5) return ((bf && !eq(E, I)) || (hf && !eq(E, C))) ? F : E;
    if (i == 6) return dh ? D : E;
    if (i == 7) return ((dh && !eq(E, I)) || (hf && !eq(E, G))) ? H : E;
    if (i == 8) return hf ? F : E;
    return E;
}

// The CRT look shared by the TV filters: a sharp-bilinear sample, scanline
// beams that widen with brightness, and an RGB mask on the screen pixels.
vec3 crt_shade(vec2 texel, float mask_amount) {
    vec2 scale = max(floor(u_out_size / u_src_size), vec2(1.0));
    vec2 base = floor(texel);
    vec2 range = 0.5 - 0.5 / scale;
    vec2 centre = texel - base - 0.5;
    vec2 f = (centre - clamp(centre, -range, range)) * scale + 0.5;
    vec3 c = texture(u_tex, to_uv(base + f)).rgb;
    float strength = clamp((min(u_out_size.x / u_src_size.x,
                                u_out_size.y / u_src_size.y) - 1.5) / 2.0, 0.0, 1.0);
    float y = fract(texel.y) - 0.5;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float width = mix(0.2, 0.36, lum);
    float beam = exp(-(y * y) / (2.0 * width * width));
    c *= mix(1.0, mix(0.5, 1.32, beam), strength);
    int m = int(mod(gl_FragCoord.x, 3.0));
    vec3 mask = vec3(0.82);
    if (m == 0) mask.r = 1.18; else if (m == 1) mask.g = 1.18; else mask.b = 1.18;
    return c * mix(vec3(1.0), mask, strength * mask_amount);
}

void main() {
    vec2 texel = to_texel(v_uv);
    if (u_mode == 5 || u_mode == 6) {
        // Screen-down cell coordinates even for bottom-up (hardware) frames.
        vec2 t = texel;
        if (u_flip > 0.5) t.y = u_src_size.y - t.y;
        vec2 cell = floor(t);
        float n = u_mode == 5 ? 2.0 : 3.0;
        vec2 sub = floor(fract(t) * n);
        if (u_flip > 0.5) cell.y = u_src_size.y - 1.0 - cell.y;
        vec3 c = u_mode == 5 ? scale2x(cell, sub) : scale3x(cell, sub);
        o_color = vec4(c, 1.0);
        return;
    }
    if (u_mode == 8) {
        // Curved CRT: bends the picture like the glass of a tube TV, rounds
        // the corners, adds a soft glow around bright parts and a faint
        // reflection on the glass.
        vec2 cp = (gl_FragCoord.xy - u_viewport.xy) / u_viewport.zw;
        vec2 d = cp * 2.0 - 1.0;
        d *= 1.0 + vec2(0.045, 0.06) * (d.yx * d.yx);
        vec2 cp2 = d * 0.5 + 0.5;
        // Rounded, slightly soft edge of the tube.
        vec2 e = abs(d) - (1.0 - 0.07);
        float corner = length(max(e, 0.0)) - 0.07;
        float px_ = 2.0 / min(u_viewport.z, u_viewport.w);
        float inside = 1.0 - smoothstep(-px_, px_, corner);
        if (inside <= 0.0) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
        vec2 duv = vec2(dFdx(v_uv.x) * u_viewport.z, dFdy(v_uv.y) * u_viewport.w);
        vec2 uv = v_uv + (cp2 - cp) * duv;
        vec2 t = to_texel(uv);
        vec3 c = crt_shade(t, 0.75);
        // Glow: a wide blur of the picture, added where it is bright.
        vec3 glow = vec3(0.0);
        for (int i = -2; i <= 2; i++) {
            glow += texture(u_tex, to_uv(t + vec2(float(i) * 1.5, 0.0))).rgb;
            glow += texture(u_tex, to_uv(t + vec2(0.0, float(i) * 1.2))).rgb;
        }
        glow /= 10.0;
        c += glow * glow * 0.22;
        // Darker towards the edges, plus a soft highlight top-left.
        c *= 1.0 - 0.28 * dot(d * d, vec2(0.5));
        vec2 h = d - vec2(-0.55, 0.6);
        c += vec3(0.05) * exp(-dot(h, h) * 4.0);
        o_color = vec4(clamp(c * 1.08 * inside, 0.0, 1.0), 1.0);
        return;
    }
    if (u_mode == 9) {
        // Composite TV: the luma a little soft, the colour much softer and
        // bleeding sideways, light scanlines.
        const mat3 to_yiq = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
        const mat3 to_rgb = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);
        vec3 y = vec3(0.0);
        float wy = 0.0, wc = 0.0;
        vec2 iq = vec2(0.0);
        for (int i = -4; i <= 4; i++) {
            float fi = float(i);
            vec3 s = to_yiq * texture(u_tex, to_uv(texel + vec2(fi * 0.5, 0.0))).rgb;
            float ly = exp(-fi * fi / 3.0), lc = exp(-fi * fi / 18.0);
            y.x += s.x * ly; wy += ly;
            iq += s.yz * lc; wc += lc;
        }
        vec3 c = to_rgb * vec3(y.x / wy, iq / wc * 1.12);
        float strength = clamp((min(u_out_size.x / u_src_size.x,
                                    u_out_size.y / u_src_size.y) - 1.5) / 2.0, 0.0, 1.0);
        float sy = fract(texel.y) - 0.5;
        c *= mix(1.0, 0.82 + 0.3 * exp(-sy * sy / 0.08), strength);
        vec2 n = texel / u_src_size * 2.0 - 1.0;
        c *= 1.0 - 0.15 * dot(n * n, vec2(0.5));
        o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
        return;
    }
    if (u_mode == 7) {
        // CRT TV: sharp-bilinear sample, bright scanline beams, an RGB
        // aperture-grille mask on the real screen pixels and darker corners.
        vec2 scale = max(floor(u_out_size / u_src_size), vec2(1.0));
        vec2 base = floor(texel);
        vec2 range = 0.5 - 0.5 / scale;
        vec2 centre = texel - base - 0.5;
        vec2 f = (centre - clamp(centre, -range, range)) * scale + 0.5;
        vec3 c = texture(u_tex, to_uv(base + f)).rgb;
        float strength = clamp((min(u_out_size.x / u_src_size.x,
                                    u_out_size.y / u_src_size.y) - 1.5) / 2.0, 0.0, 1.0);
        float y = fract(texel.y) - 0.5;
        float lum = dot(c, vec3(0.299, 0.587, 0.114));
        float width = mix(0.2, 0.36, lum);
        float beam = exp(-(y * y) / (2.0 * width * width));
        c *= mix(1.0, mix(0.5, 1.32, beam), strength);
        int m = int(mod(gl_FragCoord.x, 3.0));
        vec3 mask = vec3(0.82);
        if (m == 0) mask.r = 1.18; else if (m == 1) mask.g = 1.18; else mask.b = 1.18;
        c *= mix(vec3(1.0), mask, strength * 0.85);
        vec2 n = texel / u_src_size * 2.0 - 1.0;
        c *= 1.0 - 0.22 * dot(n * n, vec2(0.5));
        o_color = vec4(clamp(c * 1.06, 0.0, 1.0), 1.0);
        return;
    }
    if (u_mode == 2) {
        // Sharp bilinear: nearest inside each source pixel, a one-screen-
        // pixel blend at its edges. Crisp at any scale, no shimmering.
        vec2 scale = max(floor(u_out_size / u_src_size), vec2(1.0));
        vec2 base = floor(texel);
        vec2 frac_ = texel - base;
        vec2 range = 0.5 - 0.5 / scale;
        vec2 centre = frac_ - 0.5;
        vec2 f = (centre - clamp(centre, -range, range)) * scale + 0.5;
        o_color = vec4(texture(u_tex, to_uv(base + f)).rgb, 1.0);
        return;
    }
    vec3 c = texture(u_tex, v_uv).rgb;
    if (u_mode == 3) {
        // LCD: thin dark gaps between pixels, like the handheld screen, fading
        // out when the picture is too small for them to read as a grid.
        vec2 p = fract(texel);
        vec2 edge = smoothstep(0.0, 0.18, p) * smoothstep(1.0, 0.82, p);
        float strength = clamp((min(u_out_size.x / u_src_size.x,
                                    u_out_size.y / u_src_size.y) - 1.5) / 2.0, 0.0, 1.0);
        float grid = mix(1.0, 0.72 + 0.28 * edge.x * edge.y, strength);
        c = c * grid * 1.08;
    } else if (u_mode == 4) {
        // Scanlines: brightest at each line's centre, with a little bloom so
        // the picture keeps its overall brightness.
        float y = fract(texel.y) - 0.5;
        float lum = dot(c, vec3(0.299, 0.587, 0.114));
        float width = mix(0.18, 0.32, lum);
        float beam = exp(-(y * y) / (2.0 * width * width));
        c = c * mix(0.45, 1.25, beam);
    }
    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        PlatformLog(3, (std::string("Shader compile failed: ") + log).c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

GlPresenter::~GlPresenter() {
    // GL objects die with the context; nothing safe to do here without one.
}

bool GlPresenter::Init() {
    // A new context: names from an old one mean nothing any more.
    program_ = vao_ = vbo_ = sw_texture_ = 0;
    hw_fbo_ = hw_color_ = hw_depth_ = 0;
    sw_w_ = sw_h_ = hw_w_ = hw_h_ = 0;
    uploaded_serial_ = 0;

    const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs)
        return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        PlatformLog(3, "Display shader failed to link");
        return false;
    }
    u_mode_ = glGetUniformLocation(program_, "u_mode");
    u_src_size_ = glGetUniformLocation(program_, "u_src_size");
    u_out_size_ = glGetUniformLocation(program_, "u_out_size");
    u_uv_scale_ = glGetUniformLocation(program_, "u_uv_scale");
    u_flip_ = glGetUniformLocation(program_, "u_flip");  // vertex and fragment
    u_crop_ = glGetUniformLocation(program_, "u_crop");
    u_viewport_ = glGetUniformLocation(program_, "u_viewport");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);

    // Full-screen quad as a triangle strip: x, y, u, v. Texture row 0 is
    // the top of the picture, so v grows downwards.
    static const float quad[] = {
        -1.f, -1.f, 0.f, 1.f,  //
        1.f,  -1.f, 1.f, 1.f,  //
        -1.f, 1.f,  0.f, 0.f,  //
        1.f,  1.f,  1.f, 0.f,  //
    };
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenTextures(1, &sw_texture_);
    return true;
}

bool GlPresenter::CreateHwFramebuffer(unsigned width, unsigned height, bool depth, bool stencil) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (hw_fbo_ && width == hw_w_ && height == hw_h_)
        return true;
    if (hw_fbo_) {
        glDeleteFramebuffers(1, &hw_fbo_);
        glDeleteTextures(1, &hw_color_);
        if (hw_depth_)
            glDeleteRenderbuffers(1, &hw_depth_);
        hw_fbo_ = hw_color_ = hw_depth_ = 0;
    }
    hw_w_ = width;
    hw_h_ = height;

    glGenTextures(1, &hw_color_);
    glBindTexture(GL_TEXTURE_2D, hw_color_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &hw_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, hw_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hw_color_, 0);
    if (depth || stencil) {
        glGenRenderbuffers(1, &hw_depth_);
        glBindRenderbuffer(GL_RENDERBUFFER, hw_depth_);
        glRenderbufferStorage(GL_RENDERBUFFER, stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                              width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, hw_depth_);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        PlatformLog(3, "Hardware framebuffer is incomplete");
        return false;
    }
    return true;
}

bool GlPresenter::ReadHwFrame(unsigned width, unsigned height, bool bottom_up,
                              std::vector<uint8_t>* rgba) {
    if (!hw_fbo_ || width == 0 || height == 0 || width > hw_w_ || height > hw_h_)
        return false;
    rgba->resize(static_cast<size_t>(width) * height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, hw_fbo_);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (bottom_up) {
        // GL rows start at the bottom; images start at the top.
        const size_t row = static_cast<size_t>(width) * 4;
        std::vector<uint8_t> tmp(row);
        for (unsigned y = 0; y < height / 2; y++) {
            uint8_t* a = rgba->data() + y * row;
            uint8_t* b = rgba->data() + (height - 1 - y) * row;
            std::copy(a, a + row, tmp.data());
            std::copy(b, b + row, a);
            std::copy(tmp.data(), tmp.data() + row, b);
        }
    }
    return glGetError() == GL_NO_ERROR;
}

void GlPresenter::UploadSoftware(const VideoFrame& frame) {
    if (frame.serial == uploaded_serial_ || frame.rgba.empty())
        return;
    uploaded_serial_ = frame.serial;
    // Cores and drivers may leave unpack state behind; reset what matters.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glBindTexture(GL_TEXTURE_2D, sw_texture_);
    if (frame.width != sw_w_ || frame.height != sw_h_) {
        sw_w_ = frame.width;
        sw_h_ = frame.height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw_w_, sw_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     frame.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw_w_, sw_h_, GL_RGBA, GL_UNSIGNED_BYTE,
                        frame.rgba.data());
    }
}

Rect GlPresenter::FitRect(float aspect, unsigned src_w, unsigned src_h, int sw, int sh) const {
    const int avail_h = std::max(1, static_cast<int>(std::lround(sh * (1.0f - bottom_reserve_))));
    Rect r;
    if (integer_scale_ && src_w && src_h) {
        // Largest whole multiple of the source that fits (width corrected
        // for the display aspect), falling back to a plain fit if even 1x
        // does not.
        const float px_aspect = aspect / (static_cast<float>(src_w) / src_h);
        const int scale = std::min(static_cast<int>(sw / (src_w * px_aspect)),
                                   static_cast<int>(avail_h / src_h));
        if (scale >= 1) {
            r.h = src_h * scale;
            r.w = static_cast<int>(std::lround(r.h * aspect));
        }
    }
    if (r.w == 0) {
        if (sw / static_cast<float>(avail_h) > aspect) {
            r.h = avail_h;
            r.w = static_cast<int>(std::lround(avail_h * aspect));
        } else {
            r.w = sw;
            r.h = static_cast<int>(std::lround(sw / aspect));
        }
    }
    r.x = (sw - r.w) / 2;
    r.y = vertical_align_ == 1 ? 0 : (avail_h - r.h) / 2;
    return r;
}

FrameSource GlPresenter::Prepare(const CoreHost& host) {
    const VideoFrame& frame = host.frame();
    FrameSource s;
    if (!program_ || frame.width == 0 || frame.height == 0)
        return s;
    s.frame_w = frame.width;
    s.frame_h = frame.height;
    s.aspect = host.aspect();
    if (frame.hw) {
        if (!hw_color_)
            return FrameSource{};
        s.texture = hw_color_;
        s.uv_w = static_cast<float>(frame.width) / hw_w_;
        s.uv_h = static_cast<float>(frame.height) / hw_h_;
        s.flip = host.hw_render().bottom_left_origin;
    } else {
        UploadSoftware(frame);
        s.texture = sw_texture_;
    }
    return s;
}

void GlPresenter::Draw(const CoreHost& host, int surface_width, int surface_height) {
    DrawSource(Prepare(host), surface_width, surface_height);
}

void GlPresenter::DrawSource(const FrameSource& source, int surface_width, int surface_height) {
    // The core may have left anything bound; take back what we use.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, surface_width, surface_height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!program_ || !source.valid() || !show_picture_ || crop_.w() <= 0.f || crop_.h() <= 0.f) {
        viewport_ = Rect{};
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, source.texture);
    const bool linear = filter_ == Filter::kSmooth || filter_ == Filter::kSharpSmooth ||
                        filter_ == Filter::kCrt || filter_ == Filter::kCrtCurved || filter_ == Filter::kComposite;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);

    // The shown part keeps its own proportions.
    const float aspect = source.aspect * crop_.w() / crop_.h();
    const unsigned crop_w = static_cast<unsigned>(source.frame_w * crop_.w() + 0.5f);
    const unsigned crop_h = static_cast<unsigned>(source.frame_h * crop_.h() + 0.5f);
    viewport_ = FitRect(aspect, crop_w, crop_h, surface_width, surface_height);
    glViewport(viewport_.x, surface_height - viewport_.y - viewport_.h, viewport_.w, viewport_.h);

    glUseProgram(program_);
    glUniform1i(u_mode_, static_cast<int>(filter_));
    glUniform2f(u_src_size_, static_cast<float>(source.frame_w), static_cast<float>(source.frame_h));
    // Filters compare screen pixels to picture texels: scale the output
    // size up to what the whole picture would take at this zoom.
    glUniform2f(u_out_size_, viewport_.w / crop_.w(), viewport_.h / crop_.h());
    glUniform2f(u_uv_scale_, source.uv_w, source.uv_h);
    glUniform1f(u_flip_, source.flip ? 1.f : 0.f);
    glUniform4f(u_crop_, crop_.x0, crop_.y0, crop_.x1, crop_.y1);
    glUniform4f(u_viewport_, static_cast<float>(viewport_.x), static_cast<float>(surface_height - viewport_.y - viewport_.h),
                static_cast<float>(viewport_.w), static_cast<float>(viewport_.h));
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

}  // namespace cartucho
