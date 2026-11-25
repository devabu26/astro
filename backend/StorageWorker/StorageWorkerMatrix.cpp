
#include "StorageWorker.h"

#include <QMutexLocker>
#include <QDebug>
#include <QAbstractVideoSurface>
#include <QAbstractVideoBuffer>
#include <QVideoFrame>
#include <QCameraInfo>
#include <QImage>
#include <QVideoSurfaceFormat>
#include <cstring>

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

void StorageWorker::pollRtcRemoteVideoFrames()
{
    if (!m_matrixClient) {
        return;
    }

    // The Rust queue retains only the latest frame per subscribed camera.
    // Drain a bounded number so several participants can update each tick.
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

        // Camera backends on Ubuntu Touch commonly negotiate YUV420P,
        // YV12 or NV12 rather than the RGB formats used by desktop cameras.
        return {
            QVideoFrame::Format_RGB32,
            QVideoFrame::Format_ARGB32,
            QVideoFrame::Format_ARGB32_Premultiplied,
            QVideoFrame::Format_RGB24,
            QVideoFrame::Format_YUV420P,
            QVideoFrame::Format_YV12,
            QVideoFrame::Format_NV12,
            QVideoFrame::Format_NV21,
            QVideoFrame::Format_YUYV,
            QVideoFrame::Format_UYVY
        };
    }

    bool start(const QVideoSurfaceFormat &format) override
    {
        qInfo() << "RTC camera negotiated format:"
                << format.pixelFormat()
                << format.frameSize();
        return QAbstractVideoSurface::start(format);
    }

    bool present(const QVideoFrame &frame) override
    {
        if (!frame.isValid())
            return false;

        const int width = frame.width();
        const int height = frame.height();
        const QVideoFrame::PixelFormat format = frame.pixelFormat();

        if (width < 2 || height < 2 ||
            width > 1920 || height > 1080 ||
            (width & 1) || (height & 1))
            return false;

        QVideoFrame mapped(frame);
        if (!mapped.map(QAbstractVideoBuffer::ReadOnly))
            return false;

        if (!m_pending->testAndSetOrdered(0, 1)) {
            mapped.unmap();
            return true;
        }

        const int ySize = width * height;
        const int chromaSize = ySize / 4;
        QByteArray i420(ySize + chromaSize * 2, Qt::Uninitialized);
        uchar *yOut = reinterpret_cast<uchar *>(i420.data());
        uchar *uOut = yOut + ySize;
        uchar *vOut = uOut + chromaSize;
        bool converted = false;
        const int sourcePlaneCount = mapped.planeCount();

        if (format == QVideoFrame::Format_YUV420P ||
            format == QVideoFrame::Format_YV12) {
            if (mapped.planeCount() >= 3) {
                const int yStride = mapped.bytesPerLine(0);
                const int cStride1 = mapped.bytesPerLine(1);
                const int cStride2 = mapped.bytesPerLine(2);
                const uchar *yIn = mapped.bits(0);
                const uchar *plane1 = mapped.bits(1);
                const uchar *plane2 = mapped.bits(2);

                if (yIn && plane1 && plane2 &&
                    yStride >= width &&
                    cStride1 >= width / 2 &&
                    cStride2 >= width / 2) {
                    for (int row = 0; row < height; ++row)
                        std::memcpy(yOut + row * width, yIn + row * yStride, width);

                    const uchar *uIn =
                        format == QVideoFrame::Format_YUV420P
                            ? plane1 : plane2;
                    const uchar *vIn =
                        format == QVideoFrame::Format_YUV420P
                            ? plane2 : plane1;
                    const int uStride =
                        format == QVideoFrame::Format_YUV420P
                            ? cStride1 : cStride2;
                    const int vStride =
                        format == QVideoFrame::Format_YUV420P
                            ? cStride2 : cStride1;

                    for (int row = 0; row < height / 2; ++row) {
                        memcpy(uOut + row * (width / 2),
                               uIn + row * uStride, width / 2);
                        memcpy(vOut + row * (width / 2),
                               vIn + row * vStride, width / 2);
                    }
                    converted = true;
                }
            }
        } else if (format == QVideoFrame::Format_NV12 ||
                   format == QVideoFrame::Format_NV21) {
            if (mapped.planeCount() >= 2) {
                const int yStride = mapped.bytesPerLine(0);
                const int uvStride = mapped.bytesPerLine(1);
                const uchar *yIn = mapped.bits(0);
                const uchar *uvIn = mapped.bits(1);

                if (yIn && uvIn &&
                    yStride >= width &&
                    uvStride >= width) {
                    for (int row = 0; row < height; ++row)
                        memcpy(yOut + row * width, yIn + row * yStride, width);

                    for (int row = 0; row < height / 2; ++row) {
                        const uchar *src = uvIn + row * uvStride;
                        for (int col = 0; col < width / 2; ++col) {
                            const int offset = col * 2;
                            if (format == QVideoFrame::Format_NV12) {
                                uOut[row * (width / 2) + col] = src[offset];
                                vOut[row * (width / 2) + col] = src[offset + 1];
                            } else {
                                vOut[row * (width / 2) + col] = src[offset];
                                uOut[row * (width / 2) + col] = src[offset + 1];
                            }
                        }
                    }
                    converted = true;
                }
            }
        } else if (format == QVideoFrame::Format_YUYV ||
                   format == QVideoFrame::Format_UYVY) {
            const int stride = mapped.bytesPerLine(0);
            const uchar *packed = mapped.bits(0);
            if (packed && stride >= width * 2) {
                for (int row = 0; row < height; row += 2) {
                    const uchar *src0 = packed + row * stride;
                    const uchar *src1 = packed + (row + 1) * stride;
                    for (int col = 0; col < width; col += 2) {
                        const int offset = col * 2;
                        int y00, y01, y10, y11;
                        int u0, v0, u1, v1;
                        if (format == QVideoFrame::Format_YUYV) {
                            y00 = src0[offset];
                            u0 = src0[offset + 1];
                            y01 = src0[offset + 2];
                            v0 = src0[offset + 3];
                            y10 = src1[offset];
                            u1 = src1[offset + 1];
                            y11 = src1[offset + 2];
                            v1 = src1[offset + 3];
                        } else {
                            u0 = src0[offset];
                            y00 = src0[offset + 1];
                            v0 = src0[offset + 2];
                            y01 = src0[offset + 3];
                            u1 = src1[offset];
                            y10 = src1[offset + 1];
                            v1 = src1[offset + 2];
                            y11 = src1[offset + 3];
                        }
                        yOut[row * width + col] = static_cast<uchar>(y00);
                        yOut[row * width + col + 1] = static_cast<uchar>(y01);
                        yOut[(row + 1) * width + col] = static_cast<uchar>(y10);
                        yOut[(row + 1) * width + col + 1] = static_cast<uchar>(y11);
                        const int chromaIndex =
                            (row / 2) * (width / 2) + col / 2;
                        uOut[chromaIndex] = static_cast<uchar>((u0 + u1 + 1) / 2);
                        vOut[chromaIndex] = static_cast<uchar>((v0 + v1 + 1) / 2);
                    }
                }
                converted = true;
            }

            const QImage::Format imageFormat =
                QVideoFrame::imageFormatFromPixelFormat(format);
            if (imageFormat != QImage::Format_Invalid &&
                mapped.bits(0) &&
                mapped.bytesPerLine(0) > 0) {
                const QImage source(
                    mapped.bits(0),
                    width,
                    height,
                    mapped.bytesPerLine(0),
                    imageFormat
                );
                const QImage rgb = source.convertToFormat(QImage::Format_RGB32);

                if (!rgb.isNull()) {
                    auto clamp = [](int value) -> uchar {
                        return static_cast<uchar>(qBound(0, value, 255));
                    };

                    for (int row = 0; row < height; row += 2) {
                        const QRgb *row0 =
                            reinterpret_cast<const QRgb *>(rgb.constScanLine(row));
                        const QRgb *row1 =
                            reinterpret_cast<const QRgb *>(rgb.constScanLine(row + 1));

                        for (int col = 0; col < width; col += 2) {
                            const QRgb pixels[4] = {
                                row0[col], row0[col + 1],
                                row1[col], row1[col + 1]
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
                                    (row + p / 2) * width + col + p % 2;
                                yOut[index] = clamp(
                                    ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16
                                );
                            }

                            const int r = (rSum + 2) / 4;
                            const int g = (gSum + 2) / 4;
                            const int b = (bSum + 2) / 4;
                            const int chromaIndex =
                                (row / 2) * (width / 2) + col / 2;

                            uOut[chromaIndex] = clamp(
                                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
                            );
                            vOut[chromaIndex] = clamp(
                                ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
                            );
                        }
                    }
                    converted = true;
                }
            }
        }

        mapped.unmap();

        if (!converted) {
            if (m_loggedUnsupportedFormat.testAndSetOrdered(0, 1)) {
                qWarning() << "RTC camera frame format is not convertible:"
                           << format
                           << "planes:" << sourcePlaneCount;
            }
            m_pending->storeRelease(0);
            return false;
        }

        if (m_loggedFirstFrame.testAndSetOrdered(0, 1)) {
            qInfo() << "RTC camera submitted first I420 frame:"
                    << width << "x" << height
                    << "source format:" << format;
        }

        StorageWorker *worker = m_worker;
        const QString roomId = m_roomId;
        const auto pending = m_pending;

        const bool queued = QMetaObject::invokeMethod(
            worker,
            [worker, pending, roomId, i420, width, height]() {
                worker->pushRtcVideoFrame(roomId, width, height, i420);
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
    QAtomicInt m_loggedUnsupportedFormat{0};
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
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not start LiveKit video");

    if (result >= 0 && errorMessage.isEmpty()) {
        const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
        qInfo() << "RTC available cameras:" << cameras.size();
        for (const QCameraInfo &camera : cameras) {
            qInfo() << " -" << camera.description()
                    << "device:" << camera.deviceName()
                    << "position:" << camera.position();
        }

        QCameraInfo selected = QCameraInfo::defaultCamera();
        if (selected.deviceName().isEmpty() && !cameras.isEmpty())
            selected = cameras.first();

        if (selected.deviceName().isEmpty()) {
            errorMessage = QStringLiteral("No camera device is available");
        } else {
            m_rtcCamera = new QCamera(selected, this);
            connect(
                m_rtcCamera, &QCamera::statusChanged, this,
                [](QCamera::Status status) {
                    qInfo() << "RTC camera status:" << static_cast<int>(status);
                }
            );
            connect(
                m_rtcCamera,
                QOverload<QCamera::Error>::of(&QCamera::error),
                this,
                [this](QCamera::Error cameraError) {
                    qWarning() << "RTC camera error:"
                               << static_cast<int>(cameraError)
                               << m_rtcCamera->errorString();
                }
            );

            m_rtcCamera->setCaptureMode(QCamera::CaptureVideo);
            m_rtcVideoSurface = new RtcCameraVideoSurface(
                this, roomId, this
            );
            m_rtcCamera->setViewfinder(m_rtcVideoSurface);
            m_rtcCamera->start();

            if (!m_rtcCamera->isAvailable())
                errorMessage = QStringLiteral("Selected camera is unavailable");
        }

        if (!errorMessage.isEmpty()) {
            stopRtcCamera();
            char *stopError = nullptr;
            astro_matrix_stop_rtc_video(
                m_matrixClient, roomIdBytes.constData(), &stopError
            );
            if (stopError)
                astro_string_free(stopError);
        }
    }

    emit matrixRtcVideoActionFinished(roomId, errorMessage);
}

void StorageWorker::switchRtcCamera(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty() ||
        !m_rtcCamera || !m_rtcVideoSurface) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : !m_rtcCamera
                    ? QStringLiteral("Camera is not running")
                    : QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.size() < 2) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Only one camera is available")
        );
        return;
    }

    const QString currentDevice = QCameraInfo(m_rtcCamera).deviceName();
    const QCamera::Position currentPosition =
        QCameraInfo(m_rtcCamera).position();

    QCameraInfo selected;
    // Prefer the opposite physical facing (front <-> back).
    for (const QCameraInfo &camera : cameras) {
        if (camera.deviceName() != currentDevice &&
            camera.position() != QCamera::UnspecifiedPosition &&
            currentPosition != QCamera::UnspecifiedPosition &&
            camera.position() != currentPosition) {
            selected = camera;
            break;
        }
    }

    // Fall back to the next enumerated device when facing metadata is absent.
    if (selected.deviceName().isEmpty()) {
        for (int i = 0; i < cameras.size(); ++i) {
            if (cameras.at(i).deviceName() == currentDevice) {
                selected = cameras.at((i + 1) % cameras.size());
                break;
            }
        }
    }

    if (selected.deviceName().isEmpty() ||
        selected.deviceName() == currentDevice) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Could not find another camera device")
        );
        return;
    }

    stopRtcCamera();
    m_rtcCamera = new QCamera(selected, this);
    connect(
        m_rtcCamera, &QCamera::statusChanged, this,
        [](QCamera::Status status) {
            qInfo() << "RTC camera status:" << static_cast<int>(status);
        }
    );
    connect(
        m_rtcCamera,
        QOverload<QCamera::Error>::of(&QCamera::error),
        this,
        [this](QCamera::Error cameraError) {
            qWarning() << "RTC camera error:"
                       << static_cast<int>(cameraError)
                       << m_rtcCamera->errorString();
        }
    );

    m_rtcCamera->setCaptureMode(QCamera::CaptureVideo);
    m_rtcVideoSurface = new RtcCameraVideoSurface(this, roomId, this);
    m_rtcCamera->setViewfinder(m_rtcVideoSurface);
    m_rtcCamera->start();

    const QString switchError = m_rtcCamera->isAvailable()
        ? QString()
        : QStringLiteral("The selected camera is unavailable");
    if (!switchError.isEmpty())
        stopRtcCamera();

    qInfo() << "RTC switched camera to:"
            << selected.description()
            << selected.deviceName()
            << "position:" << selected.position();
    emit matrixRtcVideoActionFinished(roomId, switchError);
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
