
#include "StorageWorker.h"

#include <QMutexLocker>
#include <QDebug>
#include <QAbstractVideoSurface>
#include <QAbstractVideoBuffer>
#include <QVideoFrame>
#include <QCameraInfo>

#include <QByteArray>
#include <QAtomicInt>
#include <QMetaObject>
#include <QSharedPointer>
#include <limits>

#include <QDebug>

// =============================================================
// Matrix room and Space retrieval
// =============================================================

void StorageWorker::fetchMatrixRooms()
{
    if (!m_matrixClient) {
        emit matrixRoomsFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Return cached rooms first; Backend schedules network sync afterward.
    char *error = nullptr;

    char *json = astro_matrix_get_cached_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve cached Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixRoomsFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, false);
}

void StorageWorker::fetchMatrixDevices()
{
    if (!m_matrixClient) {
        emit matrixDevicesUpdated(
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    char *error = nullptr;

    char *json = astro_matrix_get_devices_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        const QString message = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not retrieve registered Matrix devices");

        if (error) {
            astro_string_free(error);
        }

        emit matrixDevicesUpdated(QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixDevicesUpdated(result, QString());
}

void StorageWorker::syncMatrixRooms()
{
    if (!m_matrixClient) {
        emit matrixRoomsFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Network sync is deliberately separate from cached room retrieval.
    char *error = nullptr;

    char *json = astro_matrix_get_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not refresh Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixRoomsFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, true);
}

void StorageWorker::resumeMatrixSync()
{
    if (!m_matrixClient) {
        emit matrixSyncResumeFinished(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    char *error = nullptr;
    const int32_t result = astro_matrix_resume_sync(
        m_matrixClient,
        &error
    );

    QString message;
    if (error) {
        message = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result == 0 && message.isEmpty()) {
        message = QStringLiteral("Could not resume Matrix syncing");
    }

    emit matrixSyncResumeFinished(message);
}

// =============================================================
// Room timeline retrieval
// =============================================================

void StorageWorker::fetchRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_get_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve room timeline");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Polling begins once a room timeline has been loaded.
    if (m_timelinePollTimer && !m_timelinePollTimer->isActive()) {
        m_timelinePollTimer->start();
    }

    emit matrixTimelineUpdated(roomId, result, QString());
}

void StorageWorker::refreshRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_refresh_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not refresh room timeline");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    if (m_timelinePollTimer && !m_timelinePollTimer->isActive()) {
        m_timelinePollTimer->start();
    }

    emit matrixTimelineUpdated(roomId, result, QString());
}

void StorageWorker::paginateRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixOlderTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixOlderTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_paginate_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve older room messages");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixOlderTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixOlderTimelineUpdated(roomId, result, QString());
}

// =============================================================
// Live Matrix timeline updates
// =============================================================

// =============================================================
// Live Matrix timeline and MatrixRTC membership updates
// =============================================================

void StorageWorker::pollRoomTimelineUpdates()
{
    if (!m_matrixClient) {
        if (m_timelinePollTimer) {
            m_timelinePollTimer->stop();
        }
        return;
    }

    // ---------------------------------------------------------
    // MatrixRTC membership updates
    // ---------------------------------------------------------

    char *rtcError = nullptr;

    char *rtcJson = astro_matrix_poll_rtc_memberships_json(
        m_matrixClient,
        &rtcError
    );

    if (!rtcJson) {
        QString message =
            QStringLiteral("Could not retrieve MatrixRTC membership updates");

        if (rtcError) {
            message = QString::fromUtf8(rtcError);
            astro_string_free(rtcError);
        }

        qWarning() << "ASTRO: MatrixRTC membership polling failed:" << message;
    } else {
        const QString rtcResult = QString::fromUtf8(rtcJson);

        astro_string_free(rtcJson);

        if (rtcError) {
            astro_string_free(rtcError);
        }

        // Avoid emitting an empty update on every timer tick.
        if (rtcResult != QStringLiteral("{\"updates\":[]}")) {
            emit matrixRtcMembershipsUpdated(rtcResult);
        }
    }

    // ---------------------------------------------------------
    // LiveKit audio events
    // ---------------------------------------------------------

    char *rtcAudioError = nullptr;
    char *rtcAudioJson = astro_matrix_poll_rtc_audio_events_json(
        m_matrixClient,
        &rtcAudioError
    );

    if (!rtcAudioJson) {
        QString message =
            QStringLiteral("Could not retrieve LiveKit audio events");

        if (rtcAudioError) {
            message = QString::fromUtf8(rtcAudioError);
            astro_string_free(rtcAudioError);
        }

        qWarning() << "ASTRO: LiveKit audio event polling failed:" << message;
    } else {
        const QString rtcAudioResult = QString::fromUtf8(rtcAudioJson);
        astro_string_free(rtcAudioJson);

        if (rtcAudioError) {
            astro_string_free(rtcAudioError);
        }

        if (rtcAudioResult != QStringLiteral("{\"events\":[]}")) {
            emit matrixRtcAudioEventsUpdated(rtcAudioResult);
        }
    }

    // ---------------------------------------------------------
    // LiveKit remote video frames
    // ---------------------------------------------------------

    for (int frameIndex = 0; frameIndex < 12; ++frameIndex) {
        char *frameError = nullptr;
        AstroRtcVideoFrame *frame = astro_matrix_take_rtc_video_frame(
            m_matrixClient,
            &frameError
        );

        if (!frame) {
            if (frameError) {
                qWarning() << "ASTRO: Remote video frame polling failed:"
                           << QString::fromUtf8(frameError);
                astro_string_free(frameError);
            }
            break;
        }

        if (frameError) {
            astro_string_free(frameError);
        }

        if (frame->room_id && frame->identity && frame->data &&
            frame->len <= static_cast<uintptr_t>(std::numeric_limits<int>::max())) {
            const QString roomId = QString::fromUtf8(frame->room_id);
            const QString identity = QString::fromUtf8(frame->identity);
            const QByteArray i420(
                reinterpret_cast<const char *>(frame->data),
                static_cast<int>(frame->len)
            );

            emit matrixRtcRemoteVideoFrame(
                roomId,
                identity,
                static_cast<int>(frame->width),
                static_cast<int>(frame->height),
                i420
            );
        }

        astro_matrix_rtc_video_frame_free(frame);
    }

    // ---------------------------------------------------------
    // Live Matrix timeline updates
    // ---------------------------------------------------------

    char *error = nullptr;

    char *json = astro_matrix_poll_room_timeline_updates_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve live timeline updates");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        qWarning() << "ASTRO: Live timeline polling failed:" << message;
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Suppress empty results rather than emitting them every timer tick.
    if (result != QStringLiteral("{\"updates\":[]}")) {
        emit matrixLiveTimelineUpdated(result);
    }
}

// =============================================================
// Read receipts
// =============================================================

void StorageWorker::markRoomAsRead(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    const int result = astro_matrix_mark_room_as_read(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (result < 0) {
        QString message = QStringLiteral("Could not mark room as read");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        qWarning() << "ASTRO: Read receipt failed for"
                   << roomId << ":" << message;
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (result == 1) {
        qDebug() << "ASTRO: Read receipt sent for" << roomId;
    }
}

// =============================================================
// Matrix message sending and reactions
// =============================================================

void StorageWorker::sendRoomMessage(
    const QString &roomId,
    const QString &message
)
{
    if (!m_matrixClient) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (message.trimmed().isEmpty()) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Message must not be empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray messageBytes = message.toUtf8();

    char *error = nullptr;

    char *eventId = astro_matrix_send_room_message(
        m_matrixClient,
        roomIdBytes.constData(),
        messageBytes.constData(),
        &error
    );

    if (!eventId) {
        QString errorMessage =
            QStringLiteral("Could not send Matrix message");

        if (error) {
            errorMessage = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixMessageSent(roomId, QString(), errorMessage);
        return;
    }

    const QString result = QString::fromUtf8(eventId);
    astro_string_free(eventId);

    if (error) {
        astro_string_free(error);
    }

    emit matrixMessageSent(roomId, result, QString());
}


void StorageWorker::joinRtcRoom(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_join_rtc_room(
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not join call presence");

    emit matrixRtcActionFinished(roomId, errorMessage);
}

void StorageWorker::leaveRtcRoom(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_leave_rtc_room(
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not leave call presence");

    emit matrixRtcActionFinished(roomId, errorMessage);
}


void StorageWorker::startRtcAudio(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_start_rtc_audio(
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not start LiveKit audio");

    emit matrixRtcAudioActionFinished(roomId, errorMessage);
}

// ASTRO_RTC_VIDEO_STAGE3
class RtcCameraVideoSurface : public QAbstractVideoSurface
{
public:
    RtcCameraVideoSurface(
        StorageWorker *worker,
        const QString &roomId,
        QObject *parent = nullptr
    )
        : QAbstractVideoSurface(parent),
          m_worker(worker),
          m_roomId(roomId),
          m_pending(QSharedPointer<QAtomicInt>::create(0))
    {
    }

    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
        QAbstractVideoBuffer::HandleType type =
            QAbstractVideoBuffer::NoHandle
    ) const override
    {
        if (type != QAbstractVideoBuffer::NoHandle)
            return {};

        return {
            QVideoFrame::Format_RGB32,
            QVideoFrame::Format_ARGB32
        };
    }

    bool present(const QVideoFrame &frame) override
    {
        if (!frame.isValid())
            return false;

        if (frame.pixelFormat() != QVideoFrame::Format_RGB32 &&
            frame.pixelFormat() != QVideoFrame::Format_ARGB32)
            return false;

        const int width = frame.width();
        const int height = frame.height();

        if (width < 2 || height < 2 ||
            width > 1920 || height > 1080 ||
            (width & 1) || (height & 1))
            return false;

        QVideoFrame mapped(frame);

        if (!mapped.map(QAbstractVideoBuffer::ReadOnly))
            return false;

        const int stride = mapped.bytesPerLine();

        if (stride < width * 4) {
            mapped.unmap();
            return false;
        }

        // Keep at most one queued or active submission.
        if (!m_pending->testAndSetOrdered(0, 1)) {
            mapped.unmap();
            return true;
        }

        const int ySize = width * height;
        QByteArray i420(ySize + ySize / 2, Qt::Uninitialized);

        uchar *yPlane = reinterpret_cast<uchar *>(i420.data());
        uchar *uPlane = yPlane + ySize;
        uchar *vPlane = uPlane + ySize / 4;

        auto clamp = [](int value) -> uchar {
            return static_cast<uchar>(qBound(0, value, 255));
        };

        for (int y = 0; y < height; y += 2) {
            const QRgb *row0 =
                reinterpret_cast<const QRgb *>(
                    mapped.bits() + y * stride
                );

            const QRgb *row1 =
                reinterpret_cast<const QRgb *>(
                    mapped.bits() + (y + 1) * stride
                );

            for (int x = 0; x < width; x += 2) {
                const QRgb pixels[4] = {
                    row0[x], row0[x + 1],
                    row1[x], row1[x + 1]
                };

                int rSum = 0;
                int gSum = 0;
                int bSum = 0;

                for (int p = 0; p < 4; ++p) {
                    const int r = qRed(pixels[p]);
                    const int g = qGreen(pixels[p]);
                    const int b = qBlue(pixels[p]);

                    rSum += r;
                    gSum += g;
                    bSum += b;

                    const int index =
                        (y + p / 2) * width + x + p % 2;

                    yPlane[index] = clamp(
                        ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
                    );
                }

                const int r = (rSum + 2) / 4;
                const int g = (gSum + 2) / 4;
                const int b = (bSum + 2) / 4;

                const int ci =
                    (y / 2) * (width / 2) + x / 2;

                uPlane[ci] = clamp(
                    ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
                );

                vPlane[ci] = clamp(
                    ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
                );
            }
        }

        mapped.unmap();

        if (m_loggedFirstFrame.testAndSetOrdered(0, 1)) {
            qInfo() << "RTC camera converted first I420 frame:"
                    << width << "x" << height;
        }

        StorageWorker *worker = m_worker;
        const QString roomId = m_roomId;
        const auto pending = m_pending;

        const bool queued = QMetaObject::invokeMethod(
            worker,
            [worker, pending, roomId, i420, width, height]() {
                worker->pushRtcVideoFrame(
                    roomId, width, height, i420
                );

                pending->storeRelease(0);
            },
            Qt::QueuedConnection
        );

        if (!queued)
            m_pending->storeRelease(0);

        return true;
    }

private:
    StorageWorker *m_worker = nullptr;
    QString m_roomId;
    QSharedPointer<QAtomicInt> m_pending;
    QAtomicInt m_loggedFirstFrame{0};
};

void StorageWorker::startRtcVideo(
    const QString &roomId,
    int width,
    int height
)
{
    stopRtcCamera();
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (width < 2 || height < 2) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Invalid camera dimensions")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    const int32_t result = astro_matrix_start_rtc_video(
        m_matrixClient,
        roomIdBytes.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result < 0 && errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Could not start LiveKit video");
    }

    if (result >= 0 && errorMessage.isEmpty()) {
        qInfo() << "RTC available cameras:";
       const auto cameras = QCameraInfo::availableCameras();
       for (const QCameraInfo &camera : cameras) {
           qInfo() << " -" << camera.description()
                   << "device:" << camera.deviceName();
       }

       const QCameraInfo defaultCamera = QCameraInfo::defaultCamera();
       qInfo() << "RTC default camera:"
               << defaultCamera.description()
               << "device:" << defaultCamera.deviceName();

       m_rtcCamera = new QCamera(this);

       connect(m_rtcCamera, &QCamera::statusChanged, this,
               [](QCamera::Status status) {
           qInfo() << "RTC camera status:" << static_cast<int>(status);
       });

       connect(m_rtcCamera,
               QOverload<QCamera::Error>::of(&QCamera::error),
               this, [this](QCamera::Error error) {
           qWarning() << "RTC camera error:"
                      << static_cast<int>(error)
                      << m_rtcCamera->errorString();
       });

        if (!m_rtcCamera->isAvailable()) {
            errorMessage = QStringLiteral(
                "No available camera device or camera permission denied"
            );
        } else {
            // Configure the camera for video before attaching the probe.
            m_rtcCamera->setCaptureMode(QCamera::CaptureVideo);

            m_rtcVideoSurface = new RtcCameraVideoSurface(
                this, roomId, this
            );
            m_rtcCamera->setViewfinder(m_rtcVideoSurface);
            m_rtcCamera->start();
        }

        if (!errorMessage.isEmpty()) {
            stopRtcCamera();

            char *stopError = nullptr;
            astro_matrix_stop_rtc_video(
                m_matrixClient,
                roomIdBytes.constData(),
                &stopError
            );

            if (stopError) {
                astro_string_free(stopError);
            }
        }
    }

    emit matrixRtcVideoActionFinished(roomId, errorMessage);
}

void StorageWorker::pushRtcVideoFrame(
    const QString &roomId,
    int width,
    int height,
    const QByteArray &i420
)
{
    if (!m_matrixClient || roomId.isEmpty())
        return;

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    const int32_t result = astro_matrix_push_rtc_video_frame(
        m_matrixClient,
        roomIdBytes.constData(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        reinterpret_cast<const uint8_t *>(i420.constData()),
        static_cast<uint64_t>(i420.size()),
        &error
    );

    if (error) {
        qWarning() << "RTC video frame submission failed:"
                   << QString::fromUtf8(error);
        astro_string_free(error);
    } else if (result < 0) {
        qWarning() << "RTC video frame submission failed";
    }
}

void StorageWorker::stopRtcVideo(const QString &roomId)
{
    stopRtcCamera();
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    const int32_t result = astro_matrix_stop_rtc_video(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result < 0 && errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Could not stop LiveKit video");
    }

    emit matrixRtcVideoActionFinished(roomId, errorMessage);
}

void StorageWorker::stopRtcAudio(const QString &roomId)
{
    stopRtcCamera();
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_stop_rtc_audio(
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not stop LiveKit audio");

    emit matrixRtcAudioActionFinished(roomId, errorMessage);
}

void StorageWorker::setRtcAudioMuted(const QString &roomId, bool muted)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_set_rtc_audio_muted(
        m_matrixClient, roomIdBytes.constData(), muted ? 1 : 0, &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not change LiveKit microphone mute state");

    emit matrixRtcAudioActionFinished(roomId, errorMessage);
}

void StorageWorker::toggleRoomReaction(
    const QString &roomId,
    const QString &eventId,
    const QString &reactionKey
)
{
    if (!m_matrixClient) {
        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty() || eventId.isEmpty() || reactionKey.isEmpty()) {
        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false,
            QStringLiteral("Room ID, event ID and reaction key are required")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();
    const QByteArray reactionKeyBytes = reactionKey.toUtf8();

    char *error = nullptr;

    const int32_t result = astro_matrix_toggle_room_reaction(
        m_matrixClient,
        roomIdBytes.constData(),
        eventIdBytes.constData(),
        reactionKeyBytes.constData(),
        &error
    );

    QString errorMessage;

    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result < 0) {
        if (errorMessage.isEmpty()) {
            errorMessage = QStringLiteral("Could not toggle Matrix reaction");
        }

        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false, errorMessage
        );
        return;
    }

    emit matrixReactionToggled(
        roomId, eventId, reactionKey, result == 1, QString()
    );
}


// =============================================================
// Matrix reply, edit and redaction
// =============================================================

void StorageWorker::sendRoomReply(
    const QString &roomId,
    const QString &eventId,
    const QString &message
)
{
    const QString action = QStringLiteral("reply");

    if (!m_matrixClient || roomId.isEmpty() ||
        eventId.isEmpty() || message.trimmed().isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, action,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID, event ID and message are required")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();
    const QByteArray messageBytes = message.toUtf8();

    char *error = nullptr;
    const int32_t result = astro_matrix_send_room_reply(
        m_matrixClient,
        roomIdBytes.constData(),
        eventIdBytes.constData(),
        messageBytes.constData(),
        &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result != 1 && errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Could not send Matrix reply");
    }

    emit matrixMessageActionFinished(
        roomId, eventId, action, errorMessage
    );
}


void StorageWorker::editRoomMessage(
    const QString &roomId,
    const QString &eventId,
    const QString &message
)
{
    const QString action = QStringLiteral("edit");

    if (!m_matrixClient || roomId.isEmpty() ||
        eventId.isEmpty() || message.trimmed().isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, action,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID, event ID and message are required")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();
    const QByteArray messageBytes = message.toUtf8();

    char *error = nullptr;
    const int32_t result = astro_matrix_edit_room_message(
        m_matrixClient,
        roomIdBytes.constData(),
        eventIdBytes.constData(),
        messageBytes.constData(),
        &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result != 1 && errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Could not edit Matrix message");
    }

    emit matrixMessageActionFinished(
        roomId, eventId, action, errorMessage
    );
}


void StorageWorker::redactRoomEvent(
    const QString &roomId,
    const QString &eventId
)
{
    const QString action = QStringLiteral("delete");

    if (!m_matrixClient || roomId.isEmpty() || eventId.isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, action,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID and event ID are required")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();

    char *error = nullptr;
    const int32_t result = astro_matrix_redact_room_event(
        m_matrixClient,
        roomIdBytes.constData(),
        eventIdBytes.constData(),
        &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result != 1 && errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Could not delete Matrix message");
    }

    emit matrixMessageActionFinished(
        roomId, eventId, action, errorMessage
    );
}


// =============================================================
// RTC camera lifecycle
// =============================================================

void StorageWorker::stopRtcCamera()
{
    if (m_rtcCamera) {
        m_rtcCamera->stop();
    }

    if (m_rtcCamera) {
        m_rtcCamera->setViewfinder(
            static_cast<QAbstractVideoSurface *>(nullptr)
        );
    }

    if (m_rtcVideoSurface) {
        delete m_rtcVideoSurface;
        m_rtcVideoSurface = nullptr;
    }

    if (m_rtcCamera) {
        delete m_rtcCamera;
        m_rtcCamera = nullptr;
    }
}
