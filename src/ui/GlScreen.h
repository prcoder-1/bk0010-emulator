#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <cstdint>
#include "Screen.h"

// Displays the BK-0010 512x256 RGBA framebuffer via OpenGL. Пиксель БК не квадратный:
// кадр ложился на телевизионное поле 4:3, поэтому по умолчанию один пиксель цветного
// режима рисуется блоком 4x3 точек хоста (тексель буфера — 2x3, весь кадр — 1024x768).
// Масштаб всегда целый, остаток виджета остаётся чёрными полями.
class GlScreen : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit GlScreen(QWidget* parent = nullptr);
    ~GlScreen() override;

    // Copy a new frame (512x256, 0xAARRGGBB) to be shown on next paint.
    void setFrame(const uint32_t* pixels);

    // Неквадратные пиксели БК (блок 4x3) вместо квадратных.
    void setPixelAspect34(bool on);
    bool pixelAspect34() const { return aspect34_; }

    // Размер, при котором масштаб получается ровно кратным (k = 1 для 3:4, k = 2 для
    // квадратных пикселей — иначе картинка была бы вдвое мельче привычной).
    QSize preferredSize() const;
    QSize sizeHint() const override { return preferredSize(); }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void applyViewport();

    static constexpr int W = bk::Screen::TEX_W, H = bk::Screen::TEX_H;
    static constexpr int UNIT_X = 2, UNIT_Y = 3;              // точек хоста на тексель
    static constexpr int BASE_W = W * UNIT_X, BASE_H = H * UNIT_Y;   // 1024x768
    uint32_t frame_[W * H];
    bool frameDirty_ = true;
    bool aspect34_ = true;
    GLuint texture_ = 0;
    GLuint vbo_ = 0, vao_ = 0;
    QOpenGLShaderProgram program_;
};
