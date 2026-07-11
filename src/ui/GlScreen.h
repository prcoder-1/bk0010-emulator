#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <cstdint>

// Displays the BK-0010 512x256 RGBA framebuffer via OpenGL, scaled to the
// widget with a 4:3 aspect ratio (as on the original TV output).
class GlScreen : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit GlScreen(QWidget* parent = nullptr);
    ~GlScreen() override;

    // Copy a new frame (512x256, 0xAARRGGBB) to be shown on next paint.
    void setFrame(const uint32_t* pixels);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    static constexpr int W = 512, H = 256;
    uint32_t frame_[W * H];
    bool frameDirty_ = true;
    GLuint texture_ = 0;
    GLuint vbo_ = 0, vao_ = 0;
    QOpenGLShaderProgram program_;
};
