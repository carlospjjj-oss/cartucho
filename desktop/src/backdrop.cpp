#include "backdrop.h"

#include <glad/gles2.h>

#include <algorithm>

namespace cartucho {

namespace {
const char* kVs = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;
void main() { v_uv = a_pos * 0.5 + 0.5; v_uv.y = 1.0 - v_uv.y; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";
const char* kFs = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform vec3 u_tint;
uniform float u_time;
uniform vec2 u_size;
out vec4 o;
float blob(vec2 p, vec2 c, float r) {
    vec2 d = (p - c) * u_size / max(u_size.x, u_size.y);
    return clamp(1.0 - length(d) / r, 0.0, 1.0);
}
void main() {
    vec3 dark = vec3(11.0, 10.0, 20.0) / 255.0;
    float g = clamp(1.0 - (v_uv.x * 0.4 + v_uv.y) / 1.4, 0.0, 1.0);
    vec3 c = mix(dark, mix(dark, u_tint, 0.28), g);
    float t = u_time;
    float a = blob(v_uv, vec2(0.25 + 0.12 * sin(t * 0.11), 0.35 + 0.10 * cos(t * 0.07)), 0.55);
    float b = blob(v_uv, vec2(0.78 + 0.10 * cos(t * 0.09), 0.25 + 0.12 * sin(t * 0.13)), 0.45);
    float d = blob(v_uv, vec2(0.55 + 0.15 * sin(t * 0.05 + 1.0), 0.85 + 0.08 * cos(t * 0.1)), 0.6);
    c += u_tint * (a * a) * 0.25 + mix(u_tint, vec3(1.0), 0.15) * (b * b) * 0.25 + u_tint * (d * d) * 0.18;
    o = vec4(c, 1.0);
}
)";
unsigned Compile(unsigned type, const char* src) {
    const unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}
}  // namespace

Backdrop::~Backdrop() {
    if (program_) glDeleteProgram(program_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Backdrop::SetTint(uint32_t rgb, float now) {
    if (rgb == to_) return;
    from_ = Current(now);
    to_ = rgb;
    since_ = now;
}

uint32_t Backdrop::Current(float now) const {
    const float k = std::clamp((now - since_) / 0.6f, 0.0f, 1.0f);
    const float e = 1 - (1 - k) * (1 - k);
    auto ch = [&](int s) { return static_cast<uint32_t>(((from_ >> s) & 255) * (1 - e) + ((to_ >> s) & 255) * e) << s; };
    return ch(16) | ch(8) | ch(0);
}

void Backdrop::Draw(int w, int h, uint32_t tint, float seconds) {
    if (!program_) {
        program_ = glCreateProgram();
        const unsigned vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        glDeleteShader(vs);
        glDeleteShader(fs);
        u_tint_ = glGetUniformLocation(program_, "u_tint");
        u_time_ = glGetUniformLocation(program_, "u_time");
        u_size_ = glGetUniformLocation(program_, "u_size");
        const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glUseProgram(program_);
    glUniform3f(u_tint_, ((tint >> 16) & 255) / 255.f, ((tint >> 8) & 255) / 255.f, (tint & 255) / 255.f);
    glUniform1f(u_time_, seconds);
    glUniform2f(u_size_, static_cast<float>(w), static_cast<float>(h));
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace cartucho
