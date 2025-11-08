#include "label_img.h"
#include <QPainter>
#include <QImageReader>
#include <math.h>       /* fabs */
#include <algorithm>
#include <cmath>
#include <sstream>
#include <QCursor>
#include <QtGlobal>
#include <utility>

//#include <omp.h>

#include <QSet>

static double iou(const QRectF &a, const QRectF &b) {
    QRectF inter = a.intersected(b);
    if (inter.isEmpty()) return 0.0;
    const double interArea = inter.width() * inter.height();
    const double unionArea = a.width()*a.height() + b.width()*b.height() - interArea;
    return unionArea > 0.0 ? (interArea / unionArea) : 0.0;
}

// Try several anchor positions around the box; if a label rect would collide with
// previously placed labels, nudge it elsewhere (and keep it on-canvas).
static QRectF placeLabelRect(
    const QRectF &box,
    const QSizeF &textSz,
    const QSizeF &canvasSz,
    const QVector<QRectF> &alreadyPlaced
) {
    const int pad = 3;
    const QPointF anchors[] = {
        QPointF(box.left(),                   box.top()    - textSz.height() - 4), // TL above
        QPointF(box.right() - textSz.width(), box.top()    - textSz.height() - 4), // TR above
        QPointF(box.left(),                   box.bottom() + 4),                    // BL below
        QPointF(box.right() - textSz.width(), box.bottom() + 4)                    // BR below
    };
    auto fits = [&](const QRectF &bg) {
        if (bg.left()   < 0 || bg.top()    < 0 ||
            bg.right()  > canvasSz.width() ||
            bg.bottom() > canvasSz.height()) return false;
        for (const QRectF &u : alreadyPlaced)
            if (bg.intersects(u)) return false;
        return true;
    };

    for (const auto &p : anchors) {
        QRectF r(p, textSz);
        QRectF bg = r.adjusted(-pad, -pad, +pad, +pad);
        if (fits(bg)) return r;
    }
    // Fallback: step downward from TL-above
    QPointF p = anchors[0];
    for (int step = 1; step <= 12; ++step) {
        QRectF r(p + QPointF(0, step * (textSz.height() + 4)), textSz);
        QRectF bg = r.adjusted(-pad, -pad, +pad, +pad);
        if (fits(bg)) return r;
    }
    return QRectF(anchors[0], textSz); // last resort (may overlap)
}


using std::ifstream;

QColor label_img::BOX_COLORS[10] ={  Qt::green,
        Qt::darkGreen,
        Qt::blue,
        Qt::darkBlue,
        Qt::yellow,
        Qt::darkYellow,
        Qt::red,
        Qt::darkRed,
        Qt::cyan,
        Qt::darkCyan};

label_img::label_img(QWidget *parent)
    :QLabel(parent)
{
    init();
}

void label_img::mouseMoveEvent(QMouseEvent *ev)
{
    if (m_dragging && m_dragIndex >= 0) {
        QRect r = m_startAbsRect;
        QPoint delta = ev->pos() - m_dragStartPos;
        switch (m_handle) {
            case HMove:
                r.translate(delta);
                break;
            case HNW:
                r.setTopLeft(r.topLeft() + delta);
                break;
            case HNE:
                r.setTopRight(r.topRight() + delta);
                break;
            case HSW:
                r.setBottomLeft(r.bottomLeft() + delta);
                break;
            case HSE:
                r.setBottomRight(r.bottomRight() + delta);
                break;
            default: break;
        }
        r = clampToUiImage(r);
        // Convert the dragged UI rect corners into normalized relative coords
        QPointF relTopLeft = cvtAbsoluteToRelativePoint(r.topLeft());
        QPointF relBottomRight = cvtAbsoluteToRelativePoint(r.bottomRight());

        // Now create the relative QRectF
        QRectF rel = getRelativeRectFromTwoPoints(relTopLeft, relBottomRight);
        m_objBoundingBoxes[m_dragIndex].box = rel;
        emit boxesChanged();
        showImage();
        return;
    }

    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint p = ev->position().toPoint();
    #else
        const QPoint p = ev->pos();
    #endif
    setMousePosition(p.x(), p.y());

    showImage();
    emit Mouse_Moved();
}

void label_img::mousePressEvent(QMouseEvent *ev)
{
    // keep mouse position current for drawing
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint p = ev->position().toPoint();
    #else
        const QPoint p = ev->pos();
    #endif
    setMousePosition(p.x(), p.y());

    if (m_cropMode) {
        if (ev->button() == Qt::RightButton) {
            cancelCropMode();
            emit Mouse_Pressed();
            return;
        }

        if (ev->button() == Qt::LeftButton) {
            if (!m_croppingActive) {
                m_relatvie_mouse_pos_LBtnClicked_in_ui = m_relative_mouse_pos_in_ui;
                m_bLabelingStarted = true;
                m_labelingGrab = true;
                m_croppingActive = true;
                grabMouse();
                showImage();
            } else {
                if (m_labelingGrab) { releaseMouse(); m_labelingGrab = false; }
                m_bLabelingStarted = false;
                m_croppingActive = false;
                m_cropMode = false;

                QRectF relRect = getRelativeRectFromTwoPoints(m_relative_mouse_pos_in_ui,
                                                              m_relatvie_mouse_pos_LBtnClicked_in_ui);
                if (!applyCrop(relRect))
                    emit cropCanceled();
            }

            emit Mouse_Pressed();
            return;
        }
    }

    // Right-click: delete (and cancel any labeling-in-progress)
    if (ev->button() == Qt::RightButton) {
        if (m_bLabelingStarted && m_labelingGrab) { releaseMouse(); m_labelingGrab = false; }
        removeFocusedObjectBox(m_relative_mouse_pos_in_ui);
        showImage();
        emit Mouse_Pressed();
        return;
    }

    if (ev->button() == Qt::LeftButton) {

        // A) If we're in the middle of creating a new box, CLOSE IT FIRST
        //    (ignore hit-testing so overlaps don't steal the click)
        if (m_bLabelingStarted) {
            ObjectLabelingBox ob;
            ob.label = m_focusedObjectLabel;
            ob.box   = getRelativeRectFromTwoPoints(m_relative_mouse_pos_in_ui,
                                                    m_relatvie_mouse_pos_LBtnClicked_in_ui);

            bool tooSmallW = ob.box.width()  * m_inputImg.width()  < 4;
            bool tooSmallH = ob.box.height() * m_inputImg.height() < 4;
            if (!tooSmallW && !tooSmallH)
                m_objBoundingBoxes.push_back(ob);

            m_bLabelingStarted = false;
            if (m_labelingGrab) { releaseMouse(); m_labelingGrab = false; }

            emit boxesChanged();
            showImage();
            emit Mouse_Pressed();
            return;
        }

        // B) Not labeling yet. Decide: edit existing vs start a new one.
        //    - Click on a CORNER handle => edit/resize
        //    - Hold Ctrl/Alt => allow move/resize even if clicking inside
        //    - Otherwise => start a NEW annotation (even if inside another box)
        Handle h = HNone;
        int idx = hitTestBox(ev->pos(), h);

        const bool modifierEdit = (ev->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
        if (idx >= 0 && h != HNone && (h != HMove || modifierEdit)) {
            // Start edit/drag
            m_dragIndex    = idx;
            m_handle       = h;
            m_dragging     = true;
            m_dragStartPos = ev->pos();
            m_startAbsRect = toUiRect(m_objBoundingBoxes[m_dragIndex]);
            grabMouse();
            emit Mouse_Pressed();
            return;
        }

        // C) Start a NEW annotation here (allowed even inside another box)
        m_relatvie_mouse_pos_LBtnClicked_in_ui = m_relative_mouse_pos_in_ui;
        m_bLabelingStarted = true;
        m_labelingGrab = true;
        grabMouse();
        showImage();
        emit Mouse_Pressed();
        return;
    }

    emit Mouse_Pressed();
}


void label_img::mouseReleaseEvent(QMouseEvent *ev)
{
    if (m_dragging) {
        QRect r = m_startAbsRect;
        QPoint delta = ev->pos() - m_dragStartPos;
        switch (m_handle) {
            case HMove: r.translate(delta); break;
            case HNW:   r.setTopLeft(r.topLeft() + delta); break;
            case HNE:   r.setTopRight(r.topRight() + delta); break;
            case HSW:   r.setBottomLeft(r.bottomLeft() + delta); break;
            case HSE:   r.setBottomRight(r.bottomRight() + delta); break;
            default: break;
        }
        r = clampToUiImage(r);

        QPointF relTL = cvtAbsoluteToRelativePoint(r.topLeft());
        QPointF relBR = cvtAbsoluteToRelativePoint(r.bottomRight());
        m_objBoundingBoxes[m_dragIndex].box = getRelativeRectFromTwoPoints(relTL, relBR);

        m_dragging = false;
        m_dragIndex = -1;
        m_handle = HNone;
        releaseMouse();

        emit boxesChanged();
        showImage();
        return;
    }

    emit Mouse_Release();
}


void label_img::init()
{
    m_objBoundingBoxes.clear();
    m_bLabelingStarted              = false;
    m_focusedObjectLabel            = 0;
    m_cropMode                      = false;
    m_croppingActive                = false;
    m_imageDirty                    = false;
    resetView();

    m_bVisualizeClassName = true;     // or false, your preference
    m_avoidLabelOverlap   = true;
    m_showOverlapHints    = true;
    m_overlapIoUThresh    = 0.90;

    QPoint mousePosInUi = this->mapFromGlobal(QCursor::pos());
    bool mouse_is_in_image = QRect(0, 0, this->width(), this->height()).contains(mousePosInUi);

    if  (mouse_is_in_image)
    {
        setMousePosition(mousePosInUi.x(), mousePosInUi.y());
    }
    else
    {
        setMousePosition(0., 0.);
    }
}

void label_img::setMousePosition(int x, int y)
{
    if(x < 0) x = 0;
    if(y < 0) y = 0;

    if(x > this->width())   x = this->width() - 1;
    if(y > this->height())  y = this->height() - 1;

    m_relative_mouse_pos_in_ui = cvtAbsoluteToRelativePoint(QPoint(x, y));
}

void label_img::openImage(const QString &qstrImg, bool &ret)
{
    QImageReader imgReader(qstrImg);
    imgReader.setAutoTransform(true);
    QImage img = imgReader.read();

    if(img.isNull())
    {
        m_inputImg = QImage();
        ret = false;
    }
    else
    {
        ret = true;

        m_objBoundingBoxes.clear();

        m_inputImg          = img.copy();
        m_inputImg          = m_inputImg.convertToFormat(QImage::Format_RGB888);
        m_resized_inputImg  = m_inputImg.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)

                .convertToFormat(QImage::Format_RGB888);

        resetView();

        m_bLabelingStarted  = false;
        m_cropMode          = false;
        m_croppingActive    = false;
        m_imageDirty        = false;

        QPoint mousePosInUi     = this->mapFromGlobal(QCursor::pos());
        bool mouse_is_in_image  = QRect(0, 0, this->width(), this->height()).contains(mousePosInUi);

        if  (mouse_is_in_image)
        {
            setMousePosition(mousePosInUi.x(), mousePosInUi.y());
        }
        else
        {
            setMousePosition(0., 0.);
        }
    }
}

void label_img::showImage()
{
    if (m_inputImg.isNull()) return;

    const QSize canvasSz = this->size();
    if (canvasSz.isEmpty()) return;

    const QSize imgSz = m_inputImg.size();
    double fitScaleW = canvasSz.width()  / static_cast<double>(imgSz.width());
    double fitScaleH = canvasSz.height() / static_cast<double>(imgSz.height());
    double fitScale = std::min(fitScaleW, fitScaleH);
    if (!std::isfinite(fitScale) || fitScale <= 0.0)
        fitScale = 1.0;

    double scale = fitScale * m_zoomFactor;
    if (scale <= 0.0)
        scale = 1.0;

    QSize scaledSz(
        std::max(1, static_cast<int>(std::round(imgSz.width()  * scale))),
        std::max(1, static_cast<int>(std::round(imgSz.height() * scale)))
    );

    QImage scaled = m_inputImg
        .scaled(scaledSz, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB888);

    auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    m_zoomCenter.setX(clamp01(m_zoomCenter.x()));
    m_zoomCenter.setY(clamp01(m_zoomCenter.y()));

    double drawX = 0.0;
    if (scaled.width() <= canvasSz.width()) {
        drawX = (canvasSz.width() - scaled.width()) / 2.0;
    } else {
        double desiredLeft = canvasSz.width() / 2.0 - m_zoomCenter.x() * scaled.width();
        double minLeft = canvasSz.width() - scaled.width();
        double maxLeft = 0.0;
        drawX = std::clamp(desiredLeft, minLeft, maxLeft);
    }

    double drawY = 0.0;
    if (scaled.height() <= canvasSz.height()) {
        drawY = (canvasSz.height() - scaled.height()) / 2.0;
    } else {
        double desiredTop = canvasSz.height() / 2.0 - m_zoomCenter.y() * scaled.height();
        double minTop = canvasSz.height() - scaled.height();
        double maxTop = 0.0;
        drawY = std::clamp(desiredTop, minTop, maxTop);
    }

    m_imgDrawRect = QRect(
        static_cast<int>(std::round(drawX)),
        static_cast<int>(std::round(drawY)),
        scaled.width(),
        scaled.height()
    );

    // Apply gamma on the scaled image only (not on the letterbox background)
    gammaTransform(scaled);

    // Compose a full-size canvas so our overlay math stays in widget coords
    QImage canvas(canvasSz, QImage::Format_RGB888);
    canvas.fill(QColor(24, 24, 24)); // letterbox background

    QPainter painter(&canvas);
    painter.drawImage(m_imgDrawRect.topLeft(), scaled);

    // UI styling
    QFont font = painter.font();
    int fontSize = 16, xMargin = 5, yMargin = 2;
    font.setPixelSize(fontSize);
    font.setBold(true);
    painter.setFont(font);

    int penThick = 3;
    QColor crossLineColor(255, 187, 0);

    // Draw overlays in widget coords; converters below map to m_imgDrawRect
    drawCrossLine(painter, crossLineColor, penThick);
    drawFocusedObjectBox(painter, Qt::magenta, penThick);
    drawObjectBoxes(painter, penThick);
    if (m_bVisualizeClassName)
        drawObjectLabels(painter, penThick, fontSize, xMargin, yMargin);

    this->setPixmap(QPixmap::fromImage(canvas));
}


void label_img::loadLabelData(const QString& labelFilePath)
{
    std::ifstream inputFile(qPrintable(labelFilePath));
    if (inputFile.is_open())
    {
        std::string line;
        while (std::getline(inputFile, line))
        {
            if (line.empty()) continue;
            std::istringstream iss(line);

            double cls, midX, midY, width, height, conf;
            if (!(iss >> cls >> midX >> midY >> width >> height))
                continue; // malformed line

            ObjectLabelingBox objBox;
            objBox.label = static_cast<int>(cls);

            // optional confidence (6th value)
            if (iss >> conf) objBox.confidence = conf; // present
            else             objBox.confidence = 1.0;  // default

            // convert center->top-left (normalized coords)
            double leftX = midX - width / 2.0;
            double topY  = midY - height / 2.0;

            objBox.box.setX(leftX);
            objBox.box.setY(topY);
            objBox.box.setWidth(width);
            objBox.box.setHeight(height);

            m_objBoundingBoxes.push_back(objBox);
        }
    }
    emit boxesChanged();
}


void label_img::setFocusObjectLabel(int nLabel)
{
    m_focusedObjectLabel = nLabel;
}

void label_img::setFocusObjectName(QString qstrName)
{
    m_foucsedObjectName = qstrName;
}

bool label_img::isOpened()
{
    return !m_inputImg.isNull();
}

QImage label_img::crop(QRect rect)
{
    return m_inputImg.copy(rect);
}

void label_img::beginCropSelection()
{
    if (m_inputImg.isNull())
        return;

    if (m_labelingGrab) { releaseMouse(); m_labelingGrab = false; }
    m_bLabelingStarted = false;
    m_cropMode = true;
    m_croppingActive = false;
    showImage();
}

void label_img::cancelCropMode()
{
    if (!m_cropMode && !m_croppingActive)
        return;

    if (m_labelingGrab) { releaseMouse(); m_labelingGrab = false; }
    m_bLabelingStarted = false;
    m_cropMode = false;
    m_croppingActive = false;
    showImage();
    emit cropCanceled();
}

bool label_img::applyCrop(const QRectF &relRect)
{
    if (m_inputImg.isNull())
        return false;

    QRectF normalized = relRect.normalized();
    QRect cropRect = cvtRelativeToAbsoluteRectInImage(normalized);
    if (cropRect.width() < 1 || cropRect.height() < 1)
        return false;

    QRectF cropRectF(cropRect);
    const double newW = cropRectF.width();
    const double newH = cropRectF.height();
    if (newW <= 0.0 || newH <= 0.0)
        return false;

    QVector<ObjectLabelingBox> newBoxes;
    newBoxes.reserve(m_objBoundingBoxes.size());
    const double imgW = m_inputImg.width();
    const double imgH = m_inputImg.height();

    for (const auto &ob : std::as_const(m_objBoundingBoxes)) {
        QRectF absRect(
            ob.box.x()      * imgW,
            ob.box.y()      * imgH,
            ob.box.width()  * imgW,
            ob.box.height() * imgH
        );

        QRectF intersection = absRect.intersected(cropRectF);
        if (intersection.isEmpty() || intersection.width() < 1.0 || intersection.height() < 1.0)
            continue;

        intersection.translate(-cropRectF.left(), -cropRectF.top());

        ObjectLabelingBox adjusted = ob;
        adjusted.box = QRectF(
            intersection.x() / newW,
            intersection.y() / newH,
            intersection.width() / newW,
            intersection.height() / newH
        );

        auto clampUnit = [](double v) { return std::clamp(v, 0.0, 1.0); };
        adjusted.box.setX(clampUnit(adjusted.box.x()));
        adjusted.box.setY(clampUnit(adjusted.box.y()));
        adjusted.box.setWidth(clampUnit(adjusted.box.width()));
        adjusted.box.setHeight(clampUnit(adjusted.box.height()));

        newBoxes.push_back(adjusted);
    }

    m_objBoundingBoxes = newBoxes;
    m_inputImg = m_inputImg.copy(cropRect);
    m_resized_inputImg = m_inputImg;
    m_imageDirty = true;
    m_imgDrawRect = QRect();
    m_focusedIndex = -1;

    m_relative_mouse_pos_in_ui = QPointF(0.5, 0.5);
    m_relatvie_mouse_pos_LBtnClicked_in_ui = m_relative_mouse_pos_in_ui;
    resetView();

    emit boxesChanged();
    emit cropApplied();
    showImage();
    return true;
}

void label_img::wheelEvent(QWheelEvent *ev)
{
    if (!(ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
        QLabel::wheelEvent(ev);
        return;
    }

    int delta = ev->angleDelta().y();
    if (delta == 0)
        delta = ev->pixelDelta().y();

    if (delta == 0) {
        ev->ignore();
        return;
    }

    applyZoomDelta(delta, QPointF(ev->pos()));
    ev->accept();
}

bool label_img::saveCurrentImage(const QString &path)
{
    if (m_inputImg.isNull())
        return false;

    if (!m_imageDirty)
        return true;

    bool ok = m_inputImg.save(path);
    if (ok)
        m_imageDirty = false;
    return ok;
}

void label_img::drawCrossLine(QPainter& painter, QColor color, int thickWidth)
{
    if(m_relative_mouse_pos_in_ui == QPointF(0., 0.)) return;

    QPen pen;
    pen.setWidth(thickWidth);

    pen.setColor(color);
    painter.setPen(pen);

    QPoint absolutePoint = cvtRelativeToAbsolutePoint(m_relative_mouse_pos_in_ui);

    //draw cross line
    painter.drawLine(QPoint(absolutePoint.x(),0), QPoint(absolutePoint.x(), this->height() - 1));
    painter.drawLine(QPoint(0,absolutePoint.y()), QPoint(this->width() - 1, absolutePoint.y()));
}

void label_img::drawFocusedObjectBox(QPainter& painter, Qt::GlobalColor color, int thickWidth)
{
    if(m_bLabelingStarted == true)
    {
        QPen pen;
        pen.setWidth(thickWidth);

        pen.setColor(color);
        painter.setPen(pen);

        //relative coord to absolute coord

        QPoint absolutePoint1 = cvtRelativeToAbsolutePoint(m_relatvie_mouse_pos_LBtnClicked_in_ui);
        QPoint absolutePoint2 = cvtRelativeToAbsolutePoint(m_relative_mouse_pos_in_ui);

        painter.drawRect(QRect(absolutePoint1, absolutePoint2));
    }
}

void label_img::drawObjectBoxes(QPainter& painter, int thickWidth)
{
    QPen pen; pen.setWidth(thickWidth);

    // Precompute overlaps (IoU in UI coords)
    const int n = m_objBoundingBoxes.size();
    QVector<int> overlapCount(n, 0);
    if (m_showOverlapHints) {
        for (int i = 0; i < n; ++i) {
            QRectF ri = QRectF(cvtRelativeToAbsoluteRectInUi(m_objBoundingBoxes[i].box));
            for (int j = i + 1; j < n; ++j) {
                QRectF rj = QRectF(cvtRelativeToAbsoluteRectInUi(m_objBoundingBoxes[j].box));
                if (iou(ri, rj) >= m_overlapIoUThresh) {
                    overlapCount[i]++; overlapCount[j]++;
                }
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        const auto &ob = m_objBoundingBoxes[i];
        QColor c = (ob.label >= 0 && ob.label < m_drawObjectBoxColor.size())
         ? m_drawObjectBoxColor.at(ob.label)
         : QColor(255, 0, 255); // magenta fallback

        QRect rectUi = cvtRelativeToAbsoluteRectInUi(ob.box);

        pen.setColor(c);
        pen.setStyle((m_showOverlapHints && overlapCount[i] > 0) ? Qt::DashLine : Qt::SolidLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rectUi);

        // Small count badge (self + others)
        if (m_showOverlapHints && overlapCount[i] > 0) {
            const int count = overlapCount[i] + 1;
            QRect badge(rectUi.topRight() + QPoint(-22, 2), QSize(20, 16));
            painter.fillRect(badge, QColor(255, 0, 0, 180));
            painter.setPen(Qt::white);
            painter.drawText(badge, Qt::AlignCenter, QString::number(count));
        }
    }
}


void label_img::drawObjectLabels(QPainter& painter, int thickWidth, int fontPixelSize, int xMargin, int yMargin)
{
    QFontMetrics fm = painter.fontMetrics();

    const int n = m_objBoundingBoxes.size();
    QVector<QVector<int>> overlaps(n);
    if (m_showOverlapHints) {
        for (int i = 0; i < n; ++i) {
            QRectF ri = QRectF(cvtRelativeToAbsoluteRectInUi(m_objBoundingBoxes[i].box));
            for (int j = i + 1; j < n; ++j) {
                QRectF rj = QRectF(cvtRelativeToAbsoluteRectInUi(m_objBoundingBoxes[j].box));
                if (iou(ri, rj) >= m_overlapIoUThresh) {
                    overlaps[i].push_back(j);
                    overlaps[j].push_back(i);
                }
            }
        }
    }

    QVector<QRectF> placed; // label background rects already placed (to avoid collisions)

    for (int i = 0; i < n; ++i) {
        const auto &ob = m_objBoundingBoxes[i];
        QRect rectUi = cvtRelativeToAbsoluteRectInUi(ob.box);

        // Base class name (with optional confidence)
        QString base = (ob.label >= 0 && ob.label < m_objList.size())
            ? m_objList.at(ob.label)
            : QString("Class %1").arg(ob.label);

        QString text = base;
        if (i < m_confForThisImage.size())
            text = QString("%1 (%2)").arg(base).arg(m_confForThisImage[i], 0, 'f', 2);

        // If multiple classes overlap, show them together: "A • B • C"
        if (m_showOverlapHints && !overlaps[i].isEmpty()) {
            QSet<QString> uniq{base};
            for (int j : overlaps[i]) {
                int lj = m_objBoundingBoxes[j].label;
                uniq.insert((lj >= 0 && lj < m_objList.size())
                            ? m_objList.at(lj)
                            : QString("Class %1").arg(lj));
            }
            if (uniq.size() > 1)
                text = QStringList(uniq.values()).join(" • ");
        }

        // Compute label rect and nudge to avoid collisions
        QSize textSz = fm.size(Qt::TextSingleLine, text) + QSize(xMargin*2, yMargin*2);
        QRectF labelRect;
        if (m_avoidLabelOverlap) {
            labelRect = placeLabelRect(QRectF(rectUi), textSz, QSizeF(width(), height()), placed);
        } else {
            // simple fallback above TL if room, else inside TL
            QPoint tl = rectUi.topLeft() + QPoint(-thickWidth/2, 0);
            if (rectUi.top() > fontPixelSize + yMargin*2 + thickWidth + 1)
                tl.setY(rectUi.top() - (fontPixelSize + yMargin*2 + thickWidth + 1));
            labelRect = QRectF(tl, textSz);
        }

        // Draw bg + text
        QColor bg = (ob.label >= 0 && ob.label < m_drawObjectBoxColor.size())
          ? m_drawObjectBoxColor.at(ob.label)
          : QColor(255, 0, 255);

        painter.fillRect(labelRect, bg);

        QPen fg;
        fg.setColor(qGray(bg.rgb()) > 120 ? QColorConstants::Black : QColorConstants::White);
        painter.setPen(fg);
        painter.drawText(QPointF(labelRect.left()+xMargin, labelRect.top()+yMargin+fontPixelSize), text);

        placed.push_back(labelRect);
    }
}


void label_img::gammaTransform(QImage &image)
{
    uchar* bits = image.bits();

    int h = image.height();
    int w = image.width();

    //#pragma omp parallel for collapse(2)
    for(int y = 0 ; y < h; ++y)
    {
        for(int x = 0; x < w; ++x)
        {
            int index_pixel = (y*w+x)*3;

            unsigned char r = bits[index_pixel + 0];
            unsigned char g = bits[index_pixel + 1];
            unsigned char b = bits[index_pixel + 2];

            bits[index_pixel + 0] = m_gammatransform_lut[r];
            bits[index_pixel + 1] = m_gammatransform_lut[g];
            bits[index_pixel + 2] = m_gammatransform_lut[b];
        }
    }
}

void label_img::removeFocusedObjectBox(QPointF point)
{
    int     removeBoxIdx = -1;
    double  nearestBoxDistance   = 99999999999999.;

    for(int i = 0; i < m_objBoundingBoxes.size(); i++)
    {
        QRectF objBox = m_objBoundingBoxes.at(i).box;

        if(objBox.contains(point))
        {
            double distance = objBox.width() + objBox.height();
            if(distance < nearestBoxDistance)
            {
                nearestBoxDistance = distance;
                removeBoxIdx = i;
            }
        }
    }

    if(removeBoxIdx != -1)
    {
        m_objBoundingBoxes.remove(removeBoxIdx);
        emit boxesChanged();
    }

}

QRectF label_img::getRelativeRectFromTwoPoints(QPointF p1, QPointF p2)
{
    double midX    = (p1.x() + p2.x()) / 2.;
    double midY    = (p1.y() + p2.y()) / 2.;
    double width   = fabs(p1.x() - p2.x());
    double height  = fabs(p1.y() - p2.y());

    QPointF topLeftPoint(midX - width/2., midY - height/2.);
    QPointF bottomRightPoint(midX + width/2., midY + height/2.);

    return QRectF(topLeftPoint, bottomRightPoint);
}

QRect label_img::cvtRelativeToAbsoluteRectInUi(QRectF r) const
{
    return QRect(
        int(m_imgDrawRect.left() + r.x()      * m_imgDrawRect.width()  + 0.5),
        int(m_imgDrawRect.top()  + r.y()      * m_imgDrawRect.height() + 0.5),
        int(                       r.width()  * m_imgDrawRect.width()  + 0.5),
        int(                       r.height() * m_imgDrawRect.height() + 0.5)
    );
}

QRect label_img::cvtRelativeToAbsoluteRectInImage(QRectF r) const
{
    if (m_inputImg.isNull())
        return QRect();

    QRectF abs(
        r.x()      * m_inputImg.width(),
        r.y()      * m_inputImg.height(),
        r.width()  * m_inputImg.width(),
        r.height() * m_inputImg.height()
    );

    abs = abs.intersected(QRectF(0, 0, m_inputImg.width(), m_inputImg.height()));
    return abs.toAlignedRect();
}

QPoint label_img::cvtRelativeToAbsolutePoint(QPointF p) const
{
    return QPoint(
        int(m_imgDrawRect.left() + p.x() * m_imgDrawRect.width()  + 0.5),
        int(m_imgDrawRect.top()  + p.y() * m_imgDrawRect.height() + 0.5)
    );
}

QPointF label_img::cvtAbsoluteToRelativePoint(QPoint p) const
{
    if (m_imgDrawRect.width() <= 0 || m_imgDrawRect.height() <= 0)
        return QPointF(0, 0);

    double rx = (p.x() - m_imgDrawRect.left()) / double(m_imgDrawRect.width());
    double ry = (p.y() - m_imgDrawRect.top())  / double(m_imgDrawRect.height());

    rx = std::clamp(rx, 0.0, 1.0);
    ry = std::clamp(ry, 0.0, 1.0);
    return QPointF(rx, ry);
}


int label_img::hitTestBox(const QPoint &p, Handle &h) const {
    const int grab = 6; // handle size
    for (int i = 0; i < m_objBoundingBoxes.size(); ++i) {
        QRect r = toUiRect(m_objBoundingBoxes[i]);
        QRect nw(r.topLeft() - QPoint(grab, grab), QSize(grab*2, grab*2));
        QRect ne(r.topRight() - QPoint(grab, grab), QSize(grab*2, grab*2));
        QRect sw(r.bottomLeft() - QPoint(grab, grab), QSize(grab*2, grab*2));
        QRect se(r.bottomRight() - QPoint(grab, grab), QSize(grab*2, grab*2));
        if (nw.contains(p)) { h = HNW; return i; }
        if (ne.contains(p)) { h = HNE; return i; }
        if (sw.contains(p)) { h = HSW; return i; }
        if (se.contains(p)) { h = HSE; return i; }
        if (r.contains(p))  { h = HMove; return i; }
    }
    h = HNone;
    return -1;
}

QRect label_img::toUiRect(const ObjectLabelingBox &ob) const {
    QRect abs = cvtRelativeToAbsoluteRectInUi(ob.box);
    return abs;
}

QRect label_img::clampToUiImage(const QRect &r) const {
    return r.intersected(m_imgDrawRect);
}


void label_img::setContrastGamma(float gamma)
{
    for(int i=0; i < 256; i++)
    {
        int s = (int)(pow((float)i/255., gamma) * 255.);
        s = std::clamp(s, 0, 255);
        m_gammatransform_lut[i] = (unsigned char)s;
    }
    showImage();
}

void label_img::resetView()
{
    m_zoomFactor = 1.0;
    m_zoomCenter = QPointF(0.5, 0.5);
}

void label_img::applyZoomDelta(int delta, const QPointF &focusPos)
{
    if (m_inputImg.isNull())
        return;

    if (delta == 0)
        return;

    double steps = static_cast<double>(delta) / 120.0;
    double factor = std::pow(1.2, steps);
    double newZoom = std::clamp(m_zoomFactor * factor, m_minZoom, m_maxZoom);

    if (std::abs(newZoom - m_zoomFactor) < 1e-4)
        return;

    QPoint focusPoint = focusPos.toPoint();
    if (m_imgDrawRect.contains(focusPoint))
        m_zoomCenter = cvtAbsoluteToRelativePoint(focusPoint);
    else
        m_zoomCenter = QPointF(0.5, 0.5);

    m_zoomFactor = newZoom;
    showImage();
}
