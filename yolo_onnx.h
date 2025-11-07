#pragma once
#include <QString>
#include <QImage>
#include <vector>
#include <utility>

struct YoloDet {
    int cls;
    float conf;
    // relative [0..1] left, top, width, height (YOLO txt-style)
    float x, y, w, h;
};

class YoloOnnx {
public:
    YoloOnnx(const QString& onnxPath, int inputW=640, int inputH=640, float confTh=0.25f, float iouTh=0.45f);
    bool isReady() const;
    std::vector<YoloDet> infer(const QImage& imgRGBAorRGB);

private:
    void* session_=nullptr; // ORT session opaque ptr
    void* env_=nullptr;     // ORT env
    int inW_, inH_;
    float conf_, iou_;
};
