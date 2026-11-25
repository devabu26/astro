#pragma once

#include <QByteArray>
#include <QImage>
#include <QQuickPaintedItem>

class QPainter;

class RtcVideoItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)

public:
    explicit RtcVideoItem(QQuickItem *parent = nullptr);

    bool hasFrame() const { return !m_image.isNull(); }

    Q_INVOKABLE void setI420Frame(
        const QByteArray &data,
        int width,
        int height
    );
    Q_INVOKABLE void clearFrame();

    void paint(QPainter *painter) override;

signals:
    void frameChanged();

private:
    QImage m_image;
};
