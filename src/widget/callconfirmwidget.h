#ifndef CALLCONFIRMWIDGET_H
#define CALLCONFIRMWIDGET_H

#include <QWidget>
#include <QRect>
#include <QPolygon>
#include <QBrush>

class QPaintEvent;
class QShowEvent;

/// This is a widget with dialog buttons to accept/reject a call
/// It tracks the position of another widget called the anchor
/// and looks like a bubble at the bottom of that widget.
class CallConfirmWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CallConfirmWidget(const QWidget *Anchor);

signals:
    void accepted();
    void rejected();

protected:
    virtual void paintEvent(QPaintEvent* event) override;
    virtual void showEvent(QShowEvent * event) override;

protected slots:
    void reposition(); ///< Recalculate our positions to track the anchor

private:
    const QWidget* anchor; ///< The widget we're going to be tracking

    QRect mainRect;
    QPolygon spikePoly;
    QBrush brush;

    const int rectW, rectH;
    const int spikeW, spikeH;
    const int roundedFactor; ///< By how much are the corners of the main rect rounded
    const qreal rectRatio; ///< Used to correct the rounding factors on non-square rects
};

#endif // CALLCONFIRMWIDGET_H
