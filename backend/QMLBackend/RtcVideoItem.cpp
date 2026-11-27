#include "RtcVideoItem.h"
#include "Backend.h"

#include <QPainter>
#include <QDebug>
#include <QMutexLocker>
#include <QRectF>
#include <QtGlobal>

#include <limits>

namespace {

inline int clampByte(int value)
{
    return qBound(0, value, 255);
}

}

RtcVideoItem::RtcVideoItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setOpaquePainting(false);
}

bool RtcVideoItem::hasFrame() const
{
    QMutexLocker locker(&m_imageMutex);
    return !m_image.isNull();
}

void RtcVideoItem::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId) {
        return;
    }
    m_roomId = roomId;
    m_loggedFirstRemoteFrame = false;
    m_loggedFirstLocalFrame = false;
    clearFrame();
}

void RtcVideoItem::setIdentity(const QString &identity)
{
    if (m_identity == identity) {
        return;
    }
    m_identity = identity;
    m_loggedFirstRemoteFrame = false;
    m_loggedFirstLocalFrame = false;
    clearFrame();
}

void RtcVideoItem::setSource(const QString &source)
{
    if (m_source == source)
        return;
    m_source = source;
    m_loggedFirstRemoteFrame = false;
    clearFrame();
}

void RtcVideoItem::setBackend(Backend *backend)
{
    if (m_backend == backend) {
        return;
    }

    if (m_frameConnection)
        disconnect(m_frameConnection);
    if (m_localFrameConnection)
        disconnect(m_localFrameConnection);

    m_backend = backend;

    if (m_backend) {
        m_frameConnection = connect(
            m_backend,
            &Backend::matrixRtcRemoteVideoFrame,
            this,
            &RtcVideoItem::receiveRemoteFrame
        );
        m_localFrameConnection = connect(
            m_backend,
            &Backend::matrixRtcLocalVideoFrame,
            this,
            &RtcVideoItem::receiveLocalFrame
        );
    }

    emit backendChanged();
}

void RtcVideoItem::receiveRemoteFrame(
    const QString &roomId,
    const QString &identity,
    const QString &source,
    int width,
    int height,
    const QByteArray &i420
)
{
    if (roomId != m_roomId || identity != m_identity || source != m_source) {
        return;
    }

    setI420Frame(i420, width, height);

    if (!m_loggedFirstRemoteFrame) {
        if (hasFrame()) {
            qInfo() << "RTC remote frame displayed by RtcVideoItem:"
                    << identity << width << "x" << height
                    << "bytes:" << i420.size();
        } else {
            qWarning() << "RTC remote frame received but rejected by RtcVideoItem:"
                       << identity << width << "x" << height
                       << "bytes:" << i420.size();
        }
        m_loggedFirstRemoteFrame = true;
    }
}

void RtcVideoItem::receiveLocalFrame(
    const QString &roomId,
    int width,
    int height,
    const QByteArray &i420
)
{
    if (m_source != QStringLiteral("local") || roomId != m_roomId)
        return;

    setI420Frame(i420, width, height);

    if (!m_loggedFirstLocalFrame) {
        qInfo() << "RTC local camera frame displayed:"
                << width << "x" << height
                << "bytes:" << i420.size()
                << "accepted:" << hasFrame();
        m_loggedFirstLocalFrame = true;
    }
}

void RtcVideoItem::setI420Frame(
    const QByteArray &data,
    int width,
    int height
)
{
    if (width < 2 || height < 2 ||
        width > 1920 || height > 1080 ||
        (width & 1) || (height & 1)) {
        return;
    }

    const qint64 ySize = qint64(width) * height;
    const qint64 chromaSize = ySize / 4;
    const qint64 expected = ySize + chromaSize * 2;

    if (expected <= 0 ||
        expected > std::numeric_limits<int>::max() ||
        data.size() != expected) {
        return;
    }

    const uchar *yPlane =
        reinterpret_cast<const uchar *>(data.constData());
    const uchar *uPlane = yPlane + ySize;
    const uchar *vPlane = uPlane + chromaSize;

    QImage next(width, height, QImage::Format_RGB32);
    if (next.isNull()) {
        return;
    }

    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(next.scanLine(y));
        const int chromaRow = (y / 2) * (width / 2);
        const int yRow = y * width;

        for (int x = 0; x < width; ++x) {
            const int yy = qMax(0, int(yPlane[yRow + x]) - 16);
            const int uv = chromaRow + (x / 2);
            const int u = int(uPlane[uv]) - 128;
            const int v = int(vPlane[uv]) - 128;

            const int c = 298 * yy;
            const int red = (c + 409 * v + 128) >> 8;
            const int green = (c - 100 * u - 208 * v + 128) >> 8;
            const int blue = (c + 516 * u + 128) >> 8;

            line[x] = qRgb(
                clampByte(red),
                clampByte(green),
                clampByte(blue)
            );
        }
    }

    {
        QMutexLocker locker(&m_imageMutex);
        m_image = next;
    }
    emit frameChanged();
    update();
}

void RtcVideoItem::clearFrame()
{
    {
        QMutexLocker locker(&m_imageMutex);
        if (m_image.isNull()) {
            return;
        }
        m_image = QImage();
    }
    emit frameChanged();
    update();
}

void RtcVideoItem::paint(QPainter *painter)
{
    QImage image;
    {
        QMutexLocker locker(&m_imageMutex);
        image = m_image;
    }

    if (image.isNull()) {
        return;
    }

    const QSizeF sourceSize = image.size();
    const QSizeF targetSize = boundingRect().size();
    if (sourceSize.isEmpty() || targetSize.isEmpty()) {
        return;
    }

    QSizeF fitted = sourceSize;
    fitted.scale(targetSize, Qt::KeepAspectRatio);

    const QRectF target(
        (targetSize.width() - fitted.width()) / 2.0,
        (targetSize.height() - fitted.height()) / 2.0,
        fitted.width(),
        fitted.height()
    );

    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, image);
}
