#include "RtcCameraVideoFilter.h"

#include "Backend.h"

#include <QVideoFrame>
#include <QVideoFilterRunnable>
#include <QVideoSurfaceFormat>
#include <QMutexLocker>

class RtcCameraVideoFilterRunnable final : public QVideoFilterRunnable
{
public:
    explicit RtcCameraVideoFilterRunnable(RtcCameraVideoFilter *filter)
        : m_filter(filter)
    {
    }

    QVideoFrame run(
        QVideoFrame *input,
        const QVideoSurfaceFormat &surfaceFormat,
        RunFlags flags
    ) override
    {
        Q_UNUSED(surfaceFormat);
        Q_UNUSED(flags);

        if (!input || !input->isValid() || !m_filter)
            return input ? *input : QVideoFrame();

        Backend *backend = qobject_cast<Backend *>(m_filter->backend());
        const QString roomId = m_filter->roomId();

        if (backend && !roomId.isEmpty())
            backend->processRtcCameraFrame(roomId, *input);

        // Observe the frame without replacing it; VideoOutput renders it.
        return *input;
    }

private:
    QPointer<RtcCameraVideoFilter> m_filter;
};

RtcCameraVideoFilter::RtcCameraVideoFilter(QObject *parent)
    : QAbstractVideoFilter(parent)
{
}

QObject *RtcCameraVideoFilter::backend() const
{
    QMutexLocker locker(&m_mutex);
    return m_backend.data();
}

void RtcCameraVideoFilter::setBackend(QObject *backend)
{
    QMutexLocker locker(&m_mutex);
    m_backend = backend;
}

QString RtcCameraVideoFilter::roomId() const
{
    QMutexLocker locker(&m_mutex);
    return m_roomId;
}

void RtcCameraVideoFilter::setRoomId(const QString &roomId)
{
    QMutexLocker locker(&m_mutex);
    m_roomId = roomId;
}

QVideoFilterRunnable *RtcCameraVideoFilter::createFilterRunnable()
{
    return new RtcCameraVideoFilterRunnable(this);
}
