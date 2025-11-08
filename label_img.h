#ifndef LABEL_IMG_H
#define LABEL_IMG_H

#include <QObject>
#include <QLabel>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QGestureEvent>
#include <QPinchGesture>
#include <iostream>
#include <fstream>

#include <QColor>
#include <QVector>
#include <QStringList>
#include <QRect>
#include <QRectF>
#include <QPoint>
#include <QPointF>

class QPainter;

struct ObjectLabelingBox
{
    int     label;
    QRectF  box;
    double  confidence = 1.0;

};

class label_img : public QLabel
{
    Q_OBJECT

public:
    label_img(QWidget *parent = nullptr);

    void mouseMoveEvent(QMouseEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    bool event(QEvent *ev) override;


    QVector<QColor> m_drawObjectBoxColor;
    QStringList     m_objList;
	
    enum EditMode { None, RedefineFocused };

    int m_uiX;
    int m_uiY;

    int m_imgX;
    int m_imgY;

    bool m_bLabelingStarted;
    bool m_bVisualizeClassName;

    static  QColor BOX_COLORS[10];

    QVector<ObjectLabelingBox> m_objBoundingBoxes;
    QVector<double> m_confForThisImage; // one-time confidences for current image

    // highlight stacked boxes + avoid label collisions
    bool   m_avoidLabelOverlap  = true;   // nudge label texts to free space
    bool   m_showOverlapHints   = true;   // dashed outline + count badge
    double m_overlapIoUThresh   = 0.90;   // "near-duplicate" threshold


    void init();
    void openImage(const QString &, bool& ret);
    void showImage();

    void loadLabelData(const QString &);

    void setFocusObjectLabel(int);
    void setFocusObjectName(QString);
    void setContrastGamma(float);

    bool isOpened();
    QImage crop(QRect);

    void beginCropSelection();
    void cancelCropMode();
    bool applyCrop(const QRectF &relRect);
    bool saveCurrentImage(const QString &path);
    bool hasPendingImageChanges() const { return m_imageDirty; }

    QRectF  getRelativeRectFromTwoPoints(QPointF, QPointF);

    QRect   cvtRelativeToAbsoluteRectInUi(QRectF) const;
    QRect   cvtRelativeToAbsoluteRectInImage(QRectF) const;
    QPoint  cvtRelativeToAbsolutePoint(QPointF) const;
    QPointF cvtAbsoluteToRelativePoint(QPoint) const;

signals:
    void boxesChanged();
    void Mouse_Moved();
    void Mouse_Pressed();
    void Mouse_Release();
    void cropApplied();
    void cropCanceled();

private:
    int             m_focusedObjectLabel;
    QString         m_foucsedObjectName;
    QRect m_imgDrawRect;  // centered rect (inside the label) where the image is painted

    double m_aspectRatioWidth;
    double m_aspectRatioHeight;

    QImage m_inputImg;
    QImage m_resized_inputImg;

    QPointF m_relative_mouse_pos_in_ui;
    QPointF m_relatvie_mouse_pos_LBtnClicked_in_ui;

    EditMode m_editMode = None;
    int m_focusedIndex = -1; // index of currently focused bbox
    bool m_firstClickForRedefine = true;
    QPoint m_tempFirstCorner;

    unsigned char m_gammatransform_lut[256];
    QVector<QRgb> colorTable;

    void setMousePosition(int, int);

    void drawCrossLine(QPainter&, QColor, int thickWidth = 3);
    void drawFocusedObjectBox(QPainter&, Qt::GlobalColor, int thickWidth = 3);
    void drawObjectBoxes(QPainter&, int thickWidth = 3);
    void drawObjectLabels(QPainter&, int thickWidth = 3, int fontPixelSize = 14, int xMargin = 5, int yMargin = 2);
    void gammaTransform(QImage& image);
    void removeFocusedObjectBox(QPointF);

    enum Handle { HNone, HMove, HNW, HNE, HSW, HSE };
    bool m_dragging = false;
    int  m_dragIndex = -1;
    Handle m_handle = HNone;
    QPoint m_dragStartPos;
    QRect  m_startAbsRect; // absolute pixels in UI coords at drag start

    bool m_labelingGrab = false;  // true while creating a new box (between first & second click)

    bool m_cropMode = false;
    bool m_croppingActive = false;
    bool m_imageDirty = false;
    double m_zoomFactor = 1.0;

    bool handleGestureEvent(QGestureEvent *ev);
    void applyZoomDelta(double scaleDelta);

    int hitTestBox(const QPoint &p, Handle &h) const;
    QRect toUiRect(const ObjectLabelingBox &ob) const;
    QRect clampToUiImage(const QRect &r) const;
};

#endif // LABEL_IMG_H
