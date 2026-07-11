#include "GlScreen.h"
#include <cstring>

static const char* kVert = R"(#version 330 core
layout(location=0) in vec2 pos;
layout(location=1) in vec2 uv;
out vec2 vUv;
void main() { vUv = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* kFrag = R"(#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D tex;
void main() { fragColor = texture(tex, vUv); }
)";

GlScreen::GlScreen(QWidget* parent) : QOpenGLWidget(parent) {
    std::memset(frame_, 0, sizeof(frame_));
    setMinimumSize(512, 384);
}

GlScreen::~GlScreen() {
    if (context()) {
        makeCurrent();
        if (texture_) glDeleteTextures(1, &texture_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        doneCurrent();
    }
}

void GlScreen::setFrame(const uint32_t* pixels) {
    std::memcpy(frame_, pixels, sizeof(frame_));
    frameDirty_ = true;
    update();
}

void GlScreen::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.f, 0.f, 0.f, 1.f);

    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    program_.link();

    // Fullscreen quad: pos.xy + uv. (v flipped so row 0 is at the top.)
    const float verts[] = {
        //  x     y     u    v
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
         1.f,  1.f, 1.f, 0.f,
        -1.f, -1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 0.f,
    };
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, frame_);
}

void GlScreen::resizeGL(int w, int h) {
    // Keep a 4:3 aspect ratio, letterboxed inside the widget.
    float dpr = devicePixelRatioF();
    int pw = int(w * dpr), ph = int(h * dpr);
    float targetAspect = 4.f / 3.f;
    int vw = pw, vh = int(pw / targetAspect);
    if (vh > ph) { vh = ph; vw = int(ph * targetAspect); }
    glViewport((pw - vw) / 2, (ph - vh) / 2, vw, vh);
}

void GlScreen::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    if (frameDirty_) {
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, frame_);
        frameDirty_ = false;
    }
    program_.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    program_.setUniformValue("tex", 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    program_.release();
}
