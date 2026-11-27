
#include "StorageWorker.h"

#include <QMutexLocker>
#include <thread>
#include <exception>
#include <QDebug>
#include <QAbstractVideoSurface>
#include <QAbstractVideoBuffer>
#include <QVideoFrame>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QVideoProbe>
#include <QStringList>
#include <QImage>
#include <QVideoSurfaceFormat>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFunctions>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <algorithm>
#include <cmath>

#include <QByteArray>
#include <QAtomicInt>
#include <QMetaObject>
#include <QThread>
#include <QVector>
#include <QSharedPointer>
#include <limits>
#include <unistd.h>

#include <QDebug>

namespace {

#if defined(__aarch64__) || defined(__ARM_NEON)
static inline void rgbaToYRowNeon(
    const uchar *source,
    uchar *destination,
    int width
)
{
    const uint8x8_t coeffR = vdup_n_u8(66);
    const uint8x8_t coeffG = vdup_n_u8(129);
    const uint8x8_t coeffB = vdup_n_u8(25);

    int x = 0;
    for (; x + 16 <= width; x += 16) {
        const uint8x16x4_t rgba = vld4q_u8(source + x * 4);

        uint16x8_t yLow = vmull_u8(vget_low_u8(rgba.val[0]), coeffR);
        yLow = vmlal_u8(yLow, vget_low_u8(rgba.val[1]), coeffG);
        yLow = vmlal_u8(yLow, vget_low_u8(rgba.val[2]), coeffB);
        yLow = vaddq_u16(yLow, vdupq_n_u16(128));
        yLow = vshrq_n_u16(yLow, 8);
        yLow = vaddq_u16(yLow, vdupq_n_u16(16));

        uint16x8_t yHigh = vmull_u8(vget_high_u8(rgba.val[0]), coeffR);
        yHigh = vmlal_u8(yHigh, vget_high_u8(rgba.val[1]), coeffG);
        yHigh = vmlal_u8(yHigh, vget_high_u8(rgba.val[2]), coeffB);
        yHigh = vaddq_u16(yHigh, vdupq_n_u16(128));
        yHigh = vshrq_n_u16(yHigh, 8);
        yHigh = vaddq_u16(yHigh, vdupq_n_u16(16));

        const uint8x8_t low = vqmovun_s16(vreinterpretq_s16_u16(yLow));
        const uint8x8_t high = vqmovun_s16(vreinterpretq_s16_u16(yHigh));
        vst1q_u8(destination + x, vcombine_u8(low, high));
    }

    for (; x < width; ++x) {
        const int r = source[x * 4];
        const int g = source[x * 4 + 1];
        const int b = source[x * 4 + 2];
        destination[x] = static_cast<uchar>(
            qBound(0, ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 255)
        );
    }
}

static void rgbaToChromaRowsNeon(
    const uchar *row0,
    const uchar *row1,
    uchar *uOut,
    uchar *vOut,
    int width
)
{
    const int vectorWidth = width & ~15;
    int x = 0;
    for (; x < vectorWidth; x += 16) {
        const uint8x16x4_t top = vld4q_u8(row0 + x * 4);
        const uint8x16x4_t bottom = vld4q_u8(row1 + x * 4);

        const uint16x8_t rTop = vcombine_u16(
            vpaddl_u8(vget_low_u8(top.val[0])),
            vpaddl_u8(vget_high_u8(top.val[0]))
        );
        const uint16x8_t gTop = vcombine_u16(
            vpaddl_u8(vget_low_u8(top.val[1])),
            vpaddl_u8(vget_high_u8(top.val[1]))
        );
        const uint16x8_t bTop = vcombine_u16(
            vpaddl_u8(vget_low_u8(top.val[2])),
            vpaddl_u8(vget_high_u8(top.val[2]))
        );
        const uint16x8_t rBottom = vcombine_u16(
            vpaddl_u8(vget_low_u8(bottom.val[0])),
            vpaddl_u8(vget_high_u8(bottom.val[0]))
        );
        const uint16x8_t gBottom = vcombine_u16(
            vpaddl_u8(vget_low_u8(bottom.val[1])),
            vpaddl_u8(vget_high_u8(bottom.val[1]))
        );
        const uint16x8_t bBottom = vcombine_u16(
            vpaddl_u8(vget_low_u8(bottom.val[2])),
            vpaddl_u8(vget_high_u8(bottom.val[2]))
        );

        const uint16x8_t rounding = vdupq_n_u16(2);
        const int16x8_t r = vreinterpretq_s16_u16(vshrq_n_u16(
            vaddq_u16(vaddq_u16(rTop, rBottom), rounding), 2));
        const int16x8_t g = vreinterpretq_s16_u16(vshrq_n_u16(
            vaddq_u16(vaddq_u16(gTop, gBottom), rounding), 2));
        const int16x8_t b = vreinterpretq_s16_u16(vshrq_n_u16(
            vaddq_u16(vaddq_u16(bTop, bBottom), rounding), 2));

        int16x8_t u = vmulq_n_s16(r, -38);
        u = vmlaq_n_s16(u, g, -74);
        u = vmlaq_n_s16(u, b, 112);
        u = vaddq_s16(u, vdupq_n_s16(128));
        u = vshrq_n_s16(u, 8);
        u = vaddq_s16(u, vdupq_n_s16(128));

        int16x8_t v = vmulq_n_s16(r, 112);
        v = vmlaq_n_s16(v, g, -94);
        v = vmlaq_n_s16(v, b, -18);
        v = vaddq_s16(v, vdupq_n_s16(128));
        v = vshrq_n_s16(v, 8);
        v = vaddq_s16(v, vdupq_n_s16(128));

        const int outputOffset = x / 2;
        vst1_u8(uOut + outputOffset, vqmovun_s16(u));
        vst1_u8(vOut + outputOffset, vqmovun_s16(v));
    }

    for (; x < width; x += 2) {
        const uchar *p00 = row0 + x * 4;
        const uchar *p01 = p00 + 4;
        const uchar *p10 = row1 + x * 4;
        const uchar *p11 = p10 + 4;
        const int r = (p00[0] + p01[0] + p10[0] + p11[0] + 2) / 4;
        const int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) / 4;
        const int b = (p00[2] + p01[2] + p10[2] + p11[2] + 2) / 4;
        const int index = x / 2;
        uOut[index] = static_cast<uchar>(qBound(
            0, ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 255));
        vOut[index] = static_cast<uchar>(qBound(
            0, ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 255));
    }
}

#endif

bool transformI420Frame(
    const QByteArray &source,
    int sourceWidth,
    int sourceHeight,
    int targetWidth,
    int targetHeight,
    bool rotate270,
    QByteArray &output
)
{
    if (sourceWidth < 2 || sourceHeight < 2 ||
        targetWidth < 2 || targetHeight < 2 ||
        (sourceWidth & 1) || (sourceHeight & 1) ||
        (targetWidth & 1) || (targetHeight & 1) ||
        source.size() != sourceWidth * sourceHeight * 3 / 2) {
        return false;
    }

    if (!rotate270 &&
        sourceWidth == targetWidth &&
        sourceHeight == targetHeight) {
        if (output.size() != source.size())
            output.resize(source.size());
        std::memcpy(output.data(), source.constData(),
                    static_cast<size_t>(source.size()));
        return true;
    }

    const int rotatedWidth = rotate270 ? sourceHeight : sourceWidth;
    const int rotatedHeight = rotate270 ? sourceWidth : sourceHeight;

    int cropWidth = rotatedWidth;
    int cropHeight = rotatedHeight;
    int cropX = 0;
    int cropY = 0;

    // Match VideoOutput.PreserveAspectCrop: fill the target and crop the
    // excess from the centre rather than stretching the camera image.
    if (static_cast<qint64>(rotatedWidth) * targetHeight >
        static_cast<qint64>(rotatedHeight) * targetWidth) {
        cropWidth = (rotatedHeight * targetWidth / targetHeight) & ~1;
        cropX = ((rotatedWidth - cropWidth) / 2) & ~1;
    } else {
        cropHeight = (rotatedWidth * targetHeight / targetWidth) & ~1;
        cropY = ((rotatedHeight - cropHeight) / 2) & ~1;
    }

    const int sourceYSize = sourceWidth * sourceHeight;
    const int sourceChromaSize = sourceYSize / 4;
    const int targetYSize = targetWidth * targetHeight;
    const int targetChromaSize = targetYSize / 4;

    const int outputSize = targetYSize + targetChromaSize * 2;
    // Reuse the surface-owned destination while dimensions stay constant.
    // The pending-frame gate prevents another conversion until the queued
    // submission has finished consuming its implicitly-shared snapshot.
    if (output.size() != outputSize)
        output.resize(outputSize);

    const uchar *sourceY =
        reinterpret_cast<const uchar *>(source.constData());
    const uchar *sourceU = sourceY + sourceYSize;
    const uchar *sourceV = sourceU + sourceChromaSize;
    uchar *targetY = reinterpret_cast<uchar *>(output.data());
    uchar *targetU = targetY + targetYSize;
    uchar *targetV = targetU + targetChromaSize;

    // Fuse rotation, centre crop and nearest-neighbour scaling into a
    // single source-to-destination pass per I420 plane. The previous
    // implementation allocated and filled a complete rotated temporary
    // plane before reading it again to crop/scale. Direct coordinate
    // mapping produces the same pixel selection without those allocations
    // or the extra full-frame memory pass.
    auto transformPlane = [rotate270](
        const uchar *input,
        int inputWidth,
        int inputHeight,
        uchar *destination,
        int outputWidth,
        int outputHeight,
        int left,
        int top,
        int cropPlaneWidth,
        int cropPlaneHeight
    ) {
        for (int y = 0; y < outputHeight; ++y) {
            const int rotatedY = top +
                std::min(cropPlaneHeight - 1,
                         y * cropPlaneHeight / outputHeight);
            uchar *destinationRow =
                destination + y * outputWidth;

            for (int x = 0; x < outputWidth; ++x) {
                const int rotatedX = left +
                    std::min(cropPlaneWidth - 1,
                             x * cropPlaneWidth / outputWidth);

                const int sourceX = rotate270
                    ? inputWidth - 1 - rotatedY
                    : rotatedX;
                const int sourceY = rotate270
                    ? rotatedX
                    : rotatedY;

                // Keep the mapped coordinates bounded even when unusual
                // aspect ratios produce an edge-aligned crop.
                const int boundedX =
                    std::min(inputWidth - 1, std::max(0, sourceX));
                const int boundedY =
                    std::min(inputHeight - 1, std::max(0, sourceY));

                destinationRow[x] =
                    input[boundedY * inputWidth + boundedX];
            }
        }
    };

    transformPlane(
        sourceY, sourceWidth, sourceHeight,
        targetY, targetWidth, targetHeight,
        cropX, cropY, cropWidth, cropHeight
    );
    transformPlane(
        sourceU, sourceWidth / 2, sourceHeight / 2,
        targetU, targetWidth / 2, targetHeight / 2,
        cropX / 2, cropY / 2, cropWidth / 2, cropHeight / 2
    );
    transformPlane(
        sourceV, sourceWidth / 2, sourceHeight / 2,
        targetV, targetWidth / 2, targetHeight / 2,
        cropX / 2, cropY / 2, cropWidth / 2, cropHeight / 2
    );

    return true;
}


bool mirRgbaFrameToI420AndCopy(
    const uchar *source, int sourceStride, int width, int height,
    QByteArray &rgbaSnapshot,
    uchar *yOut, int strideY, uchar *uOut, int strideU, uchar *vOut, int strideV
)
{
    if (!source || width < 2 || height < 2 || (width & 1) || (height & 1) ||
        sourceStride < width * 4 || !yOut || !uOut || !vOut ||
        strideY < width || strideU < width / 2 || strideV < width / 2)
        return false;

    const int frameBytes = width * height * 4;
    if (rgbaSnapshot.size() != frameBytes)
        rgbaSnapshot.resize(frameBytes);
    uchar *snapshot = reinterpret_cast<uchar *>(rgbaSnapshot.data());

    auto clamp = [](int value) -> uchar {
        return static_cast<uchar>(qBound(0, value, 255));
    };

    for (int row = 0; row < height; ++row) {
        const uchar *sourceRow = source + (height - 1 - row) * sourceStride;
        std::memcpy(snapshot + row * width * 4, sourceRow,
                    static_cast<size_t>(width * 4));
#if defined(__aarch64__) || defined(__ARM_NEON)
        rgbaToYRowNeon(sourceRow, yOut + row * strideY, width);
#else
        for (int col = 0; col < width; ++col) {
            const uchar *pixel = sourceRow + col * 4;
            yOut[row * strideY + col] = clamp(
                ((66 * pixel[0] + 129 * pixel[1] + 25 * pixel[2] + 128) >> 8) + 16);
        }
#endif
    }

    for (int row = 0; row < height; row += 2) {
        const uchar *row0 = source + (height - 1 - row) * sourceStride;
        const uchar *row1 = source + (height - 2 - row) * sourceStride;
        uchar *uRow = uOut + (row / 2) * strideU;
        uchar *vRow = vOut + (row / 2) * strideV;
#if defined(__aarch64__) || defined(__ARM_NEON)
        rgbaToChromaRowsNeon(row0, row1, uRow, vRow, width);
#else
        for (int col = 0; col < width; col += 2) {
            const uchar *p00 = row0 + col * 4, *p01 = p00 + 4;
            const uchar *p10 = row1 + col * 4, *p11 = p10 + 4;
            const int r = (p00[0] + p01[0] + p10[0] + p11[0] + 2) / 4;
            const int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) / 4;
            const int b = (p00[2] + p01[2] + p10[2] + p11[2] + 2) / 4;
            const int i = col / 2;
            uRow[i] = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vRow[i] = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
#endif
    }
    return true;
}

}

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

void StorageWorker::fetchMatrixAccountProfile()
{
    if (!m_matrixClient) {
        emit matrixAccountProfileUpdated(
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    char *error = nullptr;
    char *json = astro_matrix_get_account_profile_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        const QString message = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not retrieve Matrix account profile");

        if (error)
            astro_string_free(error);

        emit matrixAccountProfileUpdated(QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error)
        astro_string_free(error);

    emit matrixAccountProfileUpdated(result, QString());
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

void StorageWorker::pollRtcRemoteAudioFrames()
{
    if (!m_matrixClient)
        return;

    for (int frameIndex = 0; frameIndex < 32; ++frameIndex) {
        char *frameError = nullptr;
        AstroRtcAudioFrame *frame = astro_matrix_take_rtc_audio_frame(
            m_matrixClient,
            &frameError
        );

        if (!frame) {
            if (frameError) {
                qWarning() << "ASTRO: Remote audio frame polling failed:"
                           << QString::fromUtf8(frameError);
                astro_string_free(frameError);
            }
            break;
        }

        const QString roomId = frame->room_id
            ? QString::fromUtf8(frame->room_id) : QString();
        const QString identity = frame->identity
            ? QString::fromUtf8(frame->identity) : QString();

        const bool validFrame =
            frame->data && frame->len > 0 &&
            frame->len <= 192000 &&
            frame->sample_rate == 48000 &&
            (frame->channels == 1 || frame->channels == 2) &&
            (frame->len % (frame->channels * sizeof(qint16))) == 0;

        if (!validFrame) {
            qWarning() << "ASTRO: Rejected malformed or unsupported remote audio frame"
                       << "room:" << roomId
                       << "identity:" << identity
                       << "sampleRate:" << frame->sample_rate
                       << "channels:" << frame->channels
                       << "bytes:" << static_cast<qulonglong>(frame->len);
            astro_matrix_rtc_audio_frame_free(frame);
            continue;
        }

        if (!m_rtcRemoteAudioOutput) {
            const QAudioDeviceInfo outputDevice =
                QAudioDeviceInfo::defaultOutputDevice();
            if (outputDevice.isNull()) {
                qWarning() << "ASTRO RTC AUDIO: no default audio output device";
                astro_matrix_rtc_audio_frame_free(frame);
                continue;
            }

            QAudioFormat outputFormat;
            outputFormat.setCodec(QStringLiteral("audio/pcm"));
            outputFormat.setSampleRate(48000);
            outputFormat.setSampleSize(16);
            outputFormat.setByteOrder(QAudioFormat::LittleEndian);
            outputFormat.setSampleType(QAudioFormat::SignedInt);
            outputFormat.setChannelCount(2);

            if (!outputDevice.isFormatSupported(outputFormat)) {
                outputFormat.setChannelCount(1);
            }
            if (!outputDevice.isFormatSupported(outputFormat)) {
                outputFormat = outputDevice.nearestFormat(outputFormat);
            }
            if (outputFormat.codec() != QStringLiteral("audio/pcm") ||
                outputFormat.sampleSize() != 16 ||
                outputFormat.sampleType() != QAudioFormat::SignedInt ||
                outputFormat.byteOrder() != QAudioFormat::LittleEndian ||
                (outputFormat.channelCount() != 1 &&
                 outputFormat.channelCount() != 2) ||
                outputFormat.sampleRate() <= 0 ||
                !outputDevice.isFormatSupported(outputFormat)) {
                qWarning() << "ASTRO RTC AUDIO: no supported signed 16-bit PCM output format"
                           << "preferred:" << outputDevice.preferredFormat().sampleRate()
                           << outputDevice.preferredFormat().channelCount()
                           << outputDevice.preferredFormat().sampleSize();
                astro_matrix_rtc_audio_frame_free(frame);
                continue;
            }

            m_rtcRemoteAudioOutput = new QAudioOutput(
                outputDevice, outputFormat, this
            );
            m_rtcRemoteAudioOutput->setNotifyInterval(20);
            m_rtcRemoteAudioDevice = m_rtcRemoteAudioOutput->start();
            if (!m_rtcRemoteAudioDevice) {
                qWarning() << "ASTRO RTC AUDIO: could not start output:"
                           << m_rtcRemoteAudioOutput->error();
                m_rtcRemoteAudioOutput->deleteLater();
                m_rtcRemoteAudioOutput = nullptr;
                astro_matrix_rtc_audio_frame_free(frame);
                continue;
            }
            qInfo() << "ASTRO RTC AUDIO: remote playback started"
                    << "device:" << outputDevice.deviceName()
                    << "sampleRate:" << outputFormat.sampleRate()
                    << "channels:" << outputFormat.channelCount();
        }

        const int inputSamples = static_cast<int>(
            frame->len / sizeof(qint16)
        );
        QVector<qint16> alignedInput(inputSamples);
        std::memcpy(alignedInput.data(), frame->data, frame->len);
        const qint16 *input = alignedInput.constData();
        const int inputChannels = static_cast<int>(frame->channels);
        const int outputChannels =
            m_rtcRemoteAudioOutput->format().channelCount();

        const int inputSamplesPerChannel = inputSamples / inputChannels;
        const int outputRate = m_rtcRemoteAudioOutput->format().sampleRate();
        const int outputSamplesPerChannel = qMax(
            1,
            qRound(static_cast<double>(inputSamplesPerChannel) *
                   outputRate / static_cast<double>(frame->sample_rate))
        );

        QByteArray pcm;
        pcm.resize(
            outputSamplesPerChannel * outputChannels *
            static_cast<int>(sizeof(qint16))
        );
        qint16 *output = reinterpret_cast<qint16 *>(pcm.data());

        auto inputSample = [input, inputChannels, inputSamplesPerChannel](
            int sample, int channel
        ) -> qint16 {
            sample = qBound(0, sample, inputSamplesPerChannel - 1);
            if (inputChannels == 1)
                return input[sample];
            return input[sample * inputChannels + channel];
        };

        for (int sample = 0; sample < outputSamplesPerChannel; ++sample) {
            const double sourcePosition =
                static_cast<double>(sample) * frame->sample_rate / outputRate;
            const int sourceIndex = static_cast<int>(sourcePosition);
            const double fraction = sourcePosition - sourceIndex;
            auto interpolate = [&](int channel) -> qint16 {
                const qint16 first = inputSample(sourceIndex, channel);
                const qint16 second = inputSample(sourceIndex + 1, channel);
                const double value = first +
                    (static_cast<double>(second) - first) * fraction;
                return static_cast<qint16>(qBound(
                    -32768, static_cast<int>(qRound(value)), 32767
                ));
            };

            const qint16 left = interpolate(0);
            const qint16 right = inputChannels == 2
                ? interpolate(1) : left;
            if (outputChannels == 1) {
                output[sample] = static_cast<qint16>(
                    (static_cast<int>(left) + static_cast<int>(right)) / 2
                );
            } else {
                output[sample * 2] = left;
                output[sample * 2 + 1] = right;
            }
        }

        if (!m_loggedFirstRemoteRtcAudioFrame) {
            qInfo() << "ASTRO RTC AUDIO: first decoded frame reached Qt output"
                    << "room:" << roomId
                    << "identity:" << identity
                    << "inputChannels:" << inputChannels
                    << "outputChannels:" << outputChannels
                    << "bytes:" << pcm.size();
            m_loggedFirstRemoteRtcAudioFrame = true;
        }

        // Keep at most one second of pending PCM. If the audio device stalls,
        // discard the oldest samples instead of building unbounded latency.
        const int maxPendingBytes = outputRate * outputChannels * 2;
        if (m_rtcRemoteAudioPending.size() + pcm.size() > maxPendingBytes) {
            const int excess = m_rtcRemoteAudioPending.size() + pcm.size()
                - maxPendingBytes;
            const int frameBytes = outputChannels * static_cast<int>(sizeof(qint16));
            const int discard = ((excess + frameBytes - 1) / frameBytes) * frameBytes;
            m_rtcRemoteAudioPending.remove(
                0, qMin(discard, m_rtcRemoteAudioPending.size())
            );
        }
        m_rtcRemoteAudioPending.append(pcm);
        astro_matrix_rtc_audio_frame_free(frame);
    }

    if (!m_rtcRemoteAudioOutput || !m_rtcRemoteAudioDevice ||
        m_rtcRemoteAudioPending.isEmpty()) {
        return;
    }

    if (m_rtcRemoteAudioOutput->state() == QAudio::StoppedState &&
        m_rtcRemoteAudioOutput->error() != QAudio::NoError) {
        qWarning() << "ASTRO RTC AUDIO: output stopped with error"
                   << m_rtcRemoteAudioOutput->error();
        m_rtcRemoteAudioOutput->stop();
        m_rtcRemoteAudioDevice = m_rtcRemoteAudioOutput->start();
        if (!m_rtcRemoteAudioDevice)
            return;
    }

    const int frameBytes =
        m_rtcRemoteAudioOutput->format().channelCount() *
        static_cast<int>(sizeof(qint16));
    const int available = qMax(0, m_rtcRemoteAudioOutput->bytesFree());
    const int writable = qMin(
        available,
        m_rtcRemoteAudioPending.size()
    ) / frameBytes * frameBytes;
    if (writable <= 0)
        return;

    const qint64 written = m_rtcRemoteAudioDevice->write(
        m_rtcRemoteAudioPending.constData(), writable
    );
    if (written > 0) {
        m_rtcRemoteAudioPending.remove(0, static_cast<int>(written));
    } else if (written < 0) {
        qWarning() << "ASTRO RTC AUDIO: PCM write failed:"
                   << m_rtcRemoteAudioDevice->errorString();
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

        if (frame->room_id && frame->identity && frame->source && frame->data &&
            frame->len <= static_cast<uintptr_t>(std::numeric_limits<int>::max())) {
            const QString roomId = QString::fromUtf8(frame->room_id);
            const QString identity = QString::fromUtf8(frame->identity);
            const QString source = QString::fromUtf8(frame->source);
            const QByteArray i420(
                reinterpret_cast<const char *>(frame->data),
                static_cast<int>(frame->len)
            );

            if (!m_loggedFirstRemoteRtcFrame) {
                qInfo() << "RTC remote frame reached StorageWorker:"
                        << roomId << identity
                        << static_cast<int>(frame->width) << "x"
                        << static_cast<int>(frame->height)
                        << "bytes:" << i420.size();
                m_loggedFirstRemoteRtcFrame = true;
            }

            emit matrixRtcRemoteVideoFrame(
                roomId,
                identity,
                source,
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


void StorageWorker::declineRtcCall(const QString &roomId, const QString &eventId)
{
    if (!m_matrixClient || roomId.isEmpty() || eventId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID or call event ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();
    char *error = nullptr;
    const int32_t result = astro_matrix_decline_rtc_call(
        m_matrixClient, roomIdBytes.constData(),
        eventIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not decline call");

    emit matrixRtcActionFinished(roomId, errorMessage);
}


void StorageWorker::acceptRtcCall(const QString &roomId)
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
    const int32_t result = astro_matrix_accept_rtc_call(
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not accept call");

    emit matrixRtcActionFinished(roomId, errorMessage);
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

void StorageWorker::setRtcCameraOpenGLContext(
    QOpenGLContext *context,
    QOffscreenSurface *surface
)
{
    m_rtcCameraOpenGLContext = context;
    m_rtcCameraOffscreenSurface = surface;
}

bool StorageWorker::makeRtcCameraOpenGLContextCurrent(
    QOpenGLContext **previousContext,
    QSurface **previousSurface
)
{
    if (previousContext)
        *previousContext = QOpenGLContext::currentContext();
    if (previousSurface) {
        QOpenGLContext *previous =
            previousContext ? *previousContext : QOpenGLContext::currentContext();
        *previousSurface = previous ? previous->surface() : nullptr;
    }

    if (!m_rtcCameraOpenGLContext ||
        !m_rtcCameraOffscreenSurface ||
        !m_rtcCameraOpenGLContext->isValid() ||
        !m_rtcCameraOffscreenSurface->isValid()) {
        return false;
    }

    if (QOpenGLContext::currentContext() == m_rtcCameraOpenGLContext)
        return true;

    QOpenGLContext *previous =
        previousContext ? *previousContext : QOpenGLContext::currentContext();
    if (previous)
        previous->doneCurrent();

    if (m_rtcCameraOpenGLContext->makeCurrent(m_rtcCameraOffscreenSurface))
        return true;

    // If the shared context cannot be activated, leave the caller's context
    // in the same state it had before the attempted switch.
    if (previous && previousSurface && *previousSurface &&
        previous->isValid()) {
        previous->makeCurrent(*previousSurface);
    }
    return false;
}

void StorageWorker::releaseRtcCameraOpenGLContext(
    QOpenGLContext *previousContext,
    QSurface *previousSurface
)
{
    // If the shared context was already current on entry, it belongs to the
    // caller and must remain current when this operation finishes.
    if (previousContext == m_rtcCameraOpenGLContext)
        return;

    if (m_rtcCameraOpenGLContext &&
        QOpenGLContext::currentContext() == m_rtcCameraOpenGLContext) {
        m_rtcCameraOpenGLContext->doneCurrent();
    }

    // Restore the context/surface pair that was current when readback began.
    // A null previous context intentionally leaves no context current.
    if (previousContext && previousSurface &&
        previousContext->isValid() &&
        previousContext->thread() == QThread::currentThread()) {
        previousContext->makeCurrent(previousSurface);
    }
}

// ASTRO_RTC_VIDEO_STAGE3
class RtcCameraVideoSurface : public QAbstractVideoSurface
{
public:
    RtcCameraVideoSurface(
        StorageWorker *worker,
        const QString &roomId,
        int targetWidth,
        int targetHeight,
        QObject *parent = nullptr
    )
        : QAbstractVideoSurface(parent),
          m_worker(worker),
          m_roomId(roomId),
          m_targetWidth(targetWidth),
          m_targetHeight(targetHeight),
          m_pending(QSharedPointer<QAtomicInt>::create(0))
    {
    }

    QList<QVideoFrame::PixelFormat> supportedPixelFormats(
        QAbstractVideoBuffer::HandleType type =
            QAbstractVideoBuffer::NoHandle
    ) 
    
    const override
    {
        qInfo() << "RTC supportedPixelFormats requested handle:"
        << static_cast<int>(type);
        
        if (type == QAbstractVideoBuffer::GLTextureHandle) {
            if (m_allowGlTextureFrames.loadAcquire() == 0) {
                qInfo() << "RTC camera withholding GL formats during CPU-frame probe";
                return {};
            }
            // The Ubuntu Touch camera backend supplies RGB32 frames backed
            // by GL_TEXTURE_2D. RtcCameraVideoSurface::present() reads these
            // back synchronously through the worker's shared context before
            // the camera releases/reuses the texture.
            qInfo() << "RTC supportedPixelFormats enabling GL texture readback";
            return { QVideoFrame::Format_RGB32 };
        }

        if (type != QAbstractVideoBuffer::NoHandle) {
            qInfo() << "RTC camera rejecting unsupported surface handle:"
                    << static_cast<int>(type);
            return {};
        }

        qInfo() << "RTC supportedPixelFormats returning CPU-backed formats";
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

    bool hasAcceptedFormat() const
    {
        return m_surfaceStarted.loadAcquire() != 0;
    }

    void setAllowGlTextureFrames(bool allow)
    {
        m_allowGlTextureFrames.storeRelease(allow ? 1 : 0);
    }

    bool start(const QVideoSurfaceFormat &format) override
    {
        const bool glTextureFormat =
            format.handleType() == QAbstractVideoBuffer::GLTextureHandle;

        if (glTextureFormat) {
            if (m_allowGlTextureFrames.loadAcquire() == 0) {
                m_surfaceStarted.storeRelease(0);
                qInfo() << "RTC camera declining GL texture format during CPU-frame probe"
                        << format.pixelFormat() << format.frameSize();
                return false;
            }
            // Do not accept a GL surface unless the worker's shared context
            // can actually be made current. present() uses this context for
            // synchronous texture-to-CPU readback.
            QOpenGLContext *previousContext = nullptr;
            QSurface *previousSurface = nullptr;
            const bool contextReady =
                m_worker && m_worker->makeRtcCameraOpenGLContextCurrent(
                    &previousContext, &previousSurface
                );
            if (contextReady) {
                m_worker->releaseRtcCameraOpenGLContext(
                    previousContext, previousSurface
                );
            }

            if (!contextReady ||
                format.pixelFormat() != QVideoFrame::Format_RGB32) {
                m_surfaceStarted.storeRelease(0);
                qWarning() << "RTC camera refusing unusable GL surface format:"
                           << format.pixelFormat()
                           << format.frameSize()
                           << "contextReady:" << contextReady;
                return false;
            }

            qInfo() << "RTC camera accepting GL texture surface for readback"
                    << format.pixelFormat()
                    << format.frameSize();
        } else if (format.handleType() != QAbstractVideoBuffer::NoHandle) {
            m_surfaceStarted.storeRelease(0);
            qWarning() << "RTC camera refusing unsupported surface format:"
                       << format.pixelFormat()
                       << format.frameSize()
                       << "handle:" << static_cast<int>(format.handleType());
            return false;
        }

        const bool started = QAbstractVideoSurface::start(format);
        m_surfaceStarted.storeRelease(started ? 1 : 0);
        qInfo() << "RTC camera negotiated format:"
                << format.pixelFormat()
                << format.frameSize()
                << "surface handle:" << format.handleType()
                << "surface started:" << started
                << "surface error code:" << static_cast<int>(error());
        return started;
    }

    void stop() override
    {
        QAbstractVideoSurface::stop();
        m_surfaceStarted.storeRelease(0);
    }

    bool present(const QVideoFrame &frame) override
    {
        if (!frame.isValid()) {
            if (m_loggedInvalidFrame.testAndSetOrdered(0, 1))
                qWarning() << "RTC camera received an invalid QVideoFrame";
            return false;
        }

        const int width = frame.width();
        const int height = frame.height();
        const QVideoFrame::PixelFormat format = frame.pixelFormat();
        const bool glTextureFrame =
            frame.handleType() == QAbstractVideoBuffer::GLTextureHandle;

        // Keep native GL-camera diagnostics separate from the foreground QML
        // camera: both paths intentionally share this surface instance.
        if (glTextureFrame) {
            if (m_worker)
                m_worker->recordRtcNativeGlFrame();
            const int glFrameNumber = m_glFrameCount.fetchAndAddRelaxed(1) + 1;
            if (!m_glFrameDiagnosticTimer.isValid())
                m_glFrameDiagnosticTimer.start();
            if (glFrameNumber <= 3 || glFrameNumber % 30 == 0) {
                const qint64 elapsedMs = m_glFrameDiagnosticTimer.elapsed();
                qInfo() << "ASTRO_RTC_CAMERA: native GL frame received"
                        << "frames:" << glFrameNumber
                        << "elapsedMs:" << elapsedMs
                        << "averageFps:" << (elapsedMs > 0
                            ? (glFrameNumber * 1000.0 / elapsedMs) : 0.0)
                        << "size:" << width << "x" << height
                        << "format:" << format
                        << "pending:" << m_pending->loadAcquire()
                        << "thread:" << QThread::currentThreadId();
            }
        }

        const int frameNumber = m_loggedFrameCount.fetchAndAddRelaxed(1) + 1;
        if (!m_frameDiagnosticTimer.isValid())
            m_frameDiagnosticTimer.start();
        if (frameNumber % 60 == 0) {
            const qint64 elapsedMs = m_frameDiagnosticTimer.elapsed();
            qInfo() << "ASTRO_RTC_CAMERA: shared frame throughput"
                    << "frames:" << frameNumber
                    << "elapsedMs:" << elapsedMs
                    << "averageFps:" << (elapsedMs > 0
                        ? (frameNumber * 1000.0 / elapsedMs) : 0.0)
                    << "source:" << width << "x" << height
                    << "format:" << format
                    << "thread:" << QThread::currentThreadId();
        }

        if (frameNumber <= 10) {
            qInfo() << "RTC camera frame:"
                    << frameNumber
                    << width << "x" << height
                    << "format:" << format
                    << "handle:" << frame.handleType()
                    << "texture:" << frame.handle()
                    << "planes:" << frame.planeCount();
        }

        if (width < 2 || height < 2 ||
            width > 1920 || height > 1080 ||
            (width & 1) || (height & 1)) {
            if (m_loggedInvalidDimensions.testAndSetOrdered(0, 1))
                qWarning() << "RTC camera rejected frame dimensions:"
                           << width << "x" << height
                           << "format:" << format;
            return false;
        }

        // A previous frame is still being converted/submitted on the worker.
        // Drop this capture before mapping CPU buffers or reading back a GL
        // texture; the camera preview itself continues rendering normally.
        if (m_pending->loadAcquire() != 0)
            return true;

        QVideoFrame mapped(frame);
        bool glContextCurrent = false;
        QOpenGLContext *previousGlContext = nullptr;
        QSurface *previousGlSurface = nullptr;

        if (glTextureFrame) {
            // QtUbuntu's camera buffer owns the GL/EGL details. Let its
            // QAbstractVideoBuffer::map() implementation perform the
            // platform-specific GL texture readback instead of assuming
            // frame.handle() contains a usable GLuint.
            glContextCurrent =
                m_worker && m_worker->makeRtcCameraOpenGLContextCurrent(
                    &previousGlContext, &previousGlSurface
                );
            const bool mapOk = glContextCurrent &&
                mapped.map(QAbstractVideoBuffer::ReadOnly);

            if (!mapOk) {
                if (glContextCurrent)
                    m_worker->releaseRtcCameraOpenGLContext(
                        previousGlContext, previousGlSurface
                    );
                if (m_loggedMapFailure.testAndSetOrdered(0, 1)) {
                    qWarning() << "RTC camera could not map GL-backed frame:"
                               << width << "x" << height
                               << "format:" << format
                               << "handle:" << frame.handle()
                               << "planes:" << frame.planeCount()
                               << "worker GL context current:" << glContextCurrent;
                }
                // Keep the surface active so a later camera frame can recover.
                return true;
            }

            // Keep the shared context current until mapped.unmap() below.
            // QtUbuntu's GL-backed buffer may need that context to release
            // the mapped EGL/GL resources; releasing it immediately after
            // map() leaves the unmap operation running without its context.
            if (m_loggedMapFailure.testAndSetOrdered(0, 1)) {
                qInfo() << "RTC camera mapped GL-backed frame:"
                        << "planes:" << mapped.planeCount()
                        << "bytes:" << mapped.mappedBytes()
                        << "stride:" << mapped.bytesPerLine(0)
                        << "keeping GL context current through unmap";
            }
        } else if (!mapped.map(QAbstractVideoBuffer::ReadOnly)) {
            if (m_loggedMapFailure.testAndSetOrdered(0, 1))
                qWarning() << "RTC camera could not map first frame:"
                           << width << "x" << height
                           << "format:" << format
                           << "handle:" << frame.handleType()
                           << "planes:" << frame.planeCount();
            return false;
        }

        if (!m_pending->testAndSetOrdered(0, 1)) {
            if (mapped.isMapped())
                mapped.unmap();
            if (glContextCurrent)
                m_worker->releaseRtcCameraOpenGLContext(
                previousGlContext, previousGlSurface
            );
            return true;
        }

        const int ySize = width * height;
        const int chromaSize = ySize / 4;
        const int i420Size = ySize + chromaSize * 2;
        if (m_i420Input.size() != i420Size)
            m_i420Input.resize(i420Size);
        uchar *yOut = reinterpret_cast<uchar *>(m_i420Input.data());
        uchar *uOut = yOut + ySize;
        uchar *vOut = uOut + chromaSize;
        bool converted = false;
        const int sourcePlaneCount = mapped.planeCount();
        QImage::Format fallbackImageFormat = QImage::Format_Invalid;
        int fallbackStride = 0;
        bool fallbackHasBits = false;

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
                        std::memcpy(uOut + row * (width / 2),
                               uIn + row * uStride, width / 2);
                        std::memcpy(vOut + row * (width / 2),
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
                        std::memcpy(yOut + row * width, yIn + row * yStride, width);

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
        } else {
            const QImage::Format imageFormat =
                QVideoFrame::imageFormatFromPixelFormat(format);
            fallbackImageFormat = imageFormat;
            fallbackStride = mapped.bytesPerLine(0);
            fallbackHasBits = mapped.bits(0) != nullptr;
            if (imageFormat != QImage::Format_Invalid &&
                fallbackHasBits &&
                fallbackStride > 0) {
                const QImage source(
                    mapped.bits(0),
                    width,
                    height,
                    mapped.bytesPerLine(0),
                    imageFormat
                );
                // RGBA8888 has a defined byte order on every CPU architecture.
                // Read channels explicitly instead of relying on native-endian QRgb
                // storage when converting the QML-captured image to I420.
                const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);

                if (m_loggedRgbFormat.testAndSetOrdered(0, 1)) {
                    qInfo() << "RTC camera RGB conversion formats:"
                            << "frame:" << format
                            << "source QImage:" << source.format()
                            << "RGBA output:" << rgba.format()
                            << "size:" << width << "x" << height;
                }

                if (!rgba.isNull()) {
                    auto clamp = [](int value) -> uchar {
                        return static_cast<uchar>(qBound(0, value, 255));
                    };

                    for (int row = 0; row < height; row += 2) {
                        const uchar *row0 = rgba.constScanLine(row);
                        const uchar *row1 = rgba.constScanLine(row + 1);

                        for (int col = 0; col < width; col += 2) {
                            const uchar *pixels[4] = {
                                row0 + col * 4,
                                row0 + (col + 1) * 4,
                                row1 + col * 4,
                                row1 + (col + 1) * 4
                            };

                            int rSum = 0;
                            int gSum = 0;
                            int bSum = 0;

                            for (int p = 0; p < 4; ++p) {
                                const int r = pixels[p][0];
                                const int g = pixels[p][1];
                                const int b = pixels[p][2];
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

                            // Trial correction: the published camera image has a
                            // pronounced blue cast on remote clients. Reverse the
                            // chroma assignment to compensate for a possible RGB
                            // channel-order mismatch in the Ubuntu Touch capture path.
                            vOut[chromaIndex] = clamp(
                                ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128
                            );
                            uOut[chromaIndex] = clamp(
                                ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128
                            );
                        }
                    }
                    converted = true;
                }
            }
        }

        if (mapped.isMapped())
            mapped.unmap();
        if (glContextCurrent) {
            m_worker->releaseRtcCameraOpenGLContext(
                previousGlContext, previousGlSurface
            );
            const int restoreCount =
                m_glContextRestoreCount.fetchAndAddRelaxed(1) + 1;
            if (restoreCount <= 3) {
                qInfo() << "ASTRO_RTC_CAMERA: restored caller GL context after readback"
                        << "previousContext:" << previousGlContext
                        << "restoredContext:" << QOpenGLContext::currentContext()
                        << "thread:" << QThread::currentThreadId();
            }
        }

        if (!converted) {
            if (m_loggedUnsupportedFormat.testAndSetOrdered(0, 1)) {
                qWarning() << "RTC camera frame format is not convertible:"
                           << format
                           << "planes:" << sourcePlaneCount
                           << "image format:" << fallbackImageFormat
                           << "stride:" << fallbackStride
                           << "has bits:" << fallbackHasBits;
            }
            m_pending->storeRelease(0);
            return false;
        }

        const bool rotate270 =
            (width > height && m_targetWidth < m_targetHeight) ||
            (width < height && m_targetWidth > m_targetHeight);
        if (!transformI420Frame(
                m_i420Input,
                width,
                height,
                m_targetWidth,
                m_targetHeight,
                rotate270,
                m_i420Output)) {
            m_pending->storeRelease(0);
            return false;
        }

        // Track native GL conversion independently from QML frames. Log
        // only a small sample so diagnostics do not add per-frame overhead.
        if (glTextureFrame) {
            const int convertedFrame =
                m_nativeI420ConversionCount.fetchAndAddRelaxed(1) + 1;
            if (!m_nativeI420ConversionTimer.isValid())
                m_nativeI420ConversionTimer.start();
            if (convertedFrame <= 3 || convertedFrame % 30 == 0) {
                const qint64 elapsedMs =
                    m_nativeI420ConversionTimer.elapsed();
                qInfo() << "ASTRO_RTC_CAMERA: native I420 conversion succeeded"
                        << "frames:" << convertedFrame
                        << "elapsedMs:" << elapsedMs
                        << "averageFps:" << (elapsedMs > 0
                            ? convertedFrame * 1000.0 / elapsedMs : 0.0)
                        << "source:" << width << "x" << height
                        << "format:" << format
                        << "output:" << m_targetWidth << "x" << m_targetHeight
                        << "bytes:" << m_i420Output.size()
                        << "rotated270:" << rotate270;
            }
        }

        // QByteArray's implicit sharing gives the queued worker a stable
        // snapshot. The pending gate prevents this surface from writing the
        // reusable storage again until pushRtcVideoFrame() has returned.
        const QByteArray outputFrame = m_i420Output;

        if (m_loggedFirstFrame.testAndSetOrdered(0, 1)) {
            qInfo() << "RTC camera submitted first I420 frame:"
                    << m_targetWidth << "x" << m_targetHeight
                    << "source:" << width << "x" << height
                    << "format:" << format
                    << "rotated 270:" << rotate270;
        }

        StorageWorker *worker = m_worker;
        const QString roomId = m_roomId;
        const auto pending = m_pending;
        const int outputWidth = m_targetWidth;
        const int outputHeight = m_targetHeight;

        const bool queued = QMetaObject::invokeMethod(
            worker,
            [worker, pending, roomId, outputFrame, outputWidth, outputHeight,
             nativeCameraFrame = glTextureFrame]() {
                worker->pushRtcVideoFrame(
                    roomId, outputWidth, outputHeight, outputFrame,
                    nativeCameraFrame, true
                );
                pending->storeRelease(0);
            },
            Qt::QueuedConnection
        );

        if (!queued) {
            if (m_loggedQueueFailure.testAndSetOrdered(0, 1))
                qWarning() << "RTC camera could not queue converted frame";
            m_pending->storeRelease(0);
        }

        return true;
    }

private:
    StorageWorker *m_worker = nullptr;
    QString m_roomId;
    int m_targetWidth = 360;
    int m_targetHeight = 480;
    QSharedPointer<QAtomicInt> m_pending;
    // Reuse conversion storage across camera frames. These buffers are only
    // touched while m_pending is clear, and the queued submission owns a
    // shared snapshot of the transformed output until FFI submission ends.
    QByteArray m_i420Input;
    QByteArray m_i420Output;
    QAtomicInt m_surfaceStarted{0};
    QAtomicInt m_allowGlTextureFrames{1};
    QAtomicInt m_loggedFrameCount{0};
    QElapsedTimer m_frameDiagnosticTimer;
    QAtomicInt m_glFrameCount{0};
    QAtomicInt m_glContextRestoreCount{0};
    QElapsedTimer m_glFrameDiagnosticTimer;
    QAtomicInt m_nativeI420ConversionCount{0};
    QElapsedTimer m_nativeI420ConversionTimer;
    QAtomicInt m_loggedInvalidFrame{0};
    QAtomicInt m_loggedInvalidDimensions{0};
    QAtomicInt m_loggedMapFailure{0};
    QAtomicInt m_loggedFirstFrame{0};
    QAtomicInt m_loggedRgbFormat{0};
    QAtomicInt m_loggedUnsupportedFormat{0};
    QAtomicInt m_loggedQueueFailure{0};
};

void StorageWorker::startRtcVideo(
    const QString &roomId,
    int width,
    int height
)
{
    // Do not destroy a camera while the platform backend is still loading,
    // capturing, or asynchronously unloading it. Defer track setup until the
    // shared camera-release completion path confirms UnloadedStatus.
    if (m_rtcCamera || m_rtcBackgroundCameraStopPending) {
        m_rtcStartVideoAfterBackgroundUnload = true;
        m_rtcStartVideoAfterBackgroundUnloadRoomId = roomId;
        m_rtcStartVideoAfterBackgroundUnloadWidth = width;
        m_rtcStartVideoAfterBackgroundUnloadHeight = height;
        m_rtcSwitchCameraAfterBackgroundUnload = false;
        m_rtcSwitchCameraAfterBackgroundUnloadRoomId.clear();
        m_rtcSwitchCameraAfterBackgroundUnloadDeviceId.clear();
        m_rtcBackgroundCameraRestartAfterStop = false;
        m_rtcBackgroundCameraRestartRoomId.clear();
        m_rtcBackgroundCameraRestartDeviceId.clear();
        qInfo() << "ASTRO_BACKGROUND_CAMERA: deferring video-track start until camera unload"
                << "room:" << roomId
                << "cameraStatus:" << (m_rtcCamera
                    ? static_cast<int>(m_rtcCamera->status()) : -1)
                << "unloadPending:" << m_rtcBackgroundCameraStopPending;
        // Stop the camera using the room that owns its current track. The
        // requested new room is retained separately for the deferred start.
        stopRtcBackgroundCamera(m_rtcVideoRoomId);
        return;
    }

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
        // Foreground QML frames and background native-camera frames share this
        // conversion surface and are normalized to the published track size.
        m_rtcVideoWidth = width;
        m_rtcVideoHeight = height;
        m_rtcVideoSurface = new RtcCameraVideoSurface(
            this, roomId, width, height, this
        );
        m_rtcVideoRoomId = roomId;
        qInfo() << "RTC camera CPU frame bridge ready for Qt Quick VideoOutput";
    }

    if (result >= 0 && !errorMessage.isEmpty()) {
        stopRtcCamera();
        char *stopError = nullptr;
        astro_matrix_stop_rtc_video(
            m_matrixClient, roomIdBytes.constData(), &stopError
        );
        if (stopError)
            astro_string_free(stopError);
    }

    emit matrixRtcVideoActionFinished(roomId, errorMessage);
}


void StorageWorker::startRtcBackgroundCamera(
    const QString &roomId,
    const QString &deviceId
)
{
    QString errorMessage;
    QString selectedDeviceId;
    bool recoverExistingCamera = false;

    qInfo() << "ASTRO_BACKGROUND_CAMERA: native start requested"
            << "room:" << roomId
            << "deviceId:" << deviceId
            << "workerThread:" << QThread::currentThreadId()
            << "workerThreadObject:" << thread()
            << "matrixSession:" << (m_matrixClient != nullptr)
            << "videoSurface:" << m_rtcVideoSurface
            << "surfaceRoom:" << m_rtcVideoRoomId
            << "surfaceActive:" << (m_rtcVideoSurface && m_rtcVideoSurface->isActive());

    {
        QMutexLocker locker(&m_rtcCameraFrameMutex);

        if (!m_matrixClient || roomId.isEmpty()) {
            errorMessage = !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Room ID is empty");
        } else if (!m_rtcVideoSurface || m_rtcVideoRoomId != roomId) {
            errorMessage = QStringLiteral(
                "The LiveKit camera track is not ready"
            );
        } else if (m_rtcBackgroundCameraStartPending) {
            // An app-state transition can issue duplicate start requests.
            return;
        } else if (m_rtcBackgroundCameraStopPending) {
            // The camera is still unloading. Remember the newest background
            // request and start it only after UnloadedStatus is confirmed.
            m_rtcBackgroundCameraRestartAfterStop = true;
            m_rtcBackgroundCameraRestartRoomId = roomId;
            m_rtcBackgroundCameraRestartDeviceId = deviceId;
            qInfo() << "RTC background camera restart deferred until unload completes";
            return;
        } else if (m_rtcCamera) {
            if (m_rtcCamera->status() == QCamera::ActiveStatus &&
                m_rtcVideoRoomId == roomId &&
                m_rtcVideoSurface &&
                m_rtcVideoSurface->isActive()) {
                qInfo() << "ASTRO_BACKGROUND_CAMERA: start request found"
                        << "an already active camera; keeping current capture"
                        << "room:" << roomId
                        << "generation:" << m_rtcCameraGeneration;
                return;
            }

            // Retain the newest foreground request and retry release of a
            // camera object left behind by a stalled asynchronous unload.
            m_rtcBackgroundCameraRestartAfterStop = true;
            m_rtcBackgroundCameraRestartRoomId = roomId;
            m_rtcBackgroundCameraRestartDeviceId = deviceId;
            recoverExistingCamera = true;
            qWarning() << "ASTRO_BACKGROUND_CAMERA: existing camera requires"
                       << "release before restart"
                       << "status:" << static_cast<int>(m_rtcCamera->status())
                       << "state:" << static_cast<int>(m_rtcCamera->state())
                       << "unloadPending:" << m_rtcBackgroundCameraStopPending;
        } else {

            QCameraInfo selected;
            const QList<QCameraInfo> cameras =
                QCameraInfo::availableCameras();

            qInfo() << "ASTRO_BACKGROUND_CAMERA: camera enumeration"
                    << "count:" << cameras.size()
                    << "requestedDevice:" << deviceId;
            for (const QCameraInfo &camera : cameras) {
                qInfo() << "ASTRO_BACKGROUND_CAMERA: available device"
                        << camera.deviceName()
                        << camera.description()
                        << "position:" << static_cast<int>(camera.position())
                        << "orientation:" << camera.orientation();
                if (!deviceId.isEmpty() &&
                    camera.deviceName() == deviceId) {
                    selected = camera;
                    break;
                }
            }

            if (selected.deviceName().isEmpty())
                selected = QCameraInfo::defaultCamera();

            if (selected.deviceName().isEmpty()) {
                errorMessage = QStringLiteral(
                    "No native camera device is available"
                );
            } else {
                selectedDeviceId = selected.deviceName();

                qInfo() << "ASTRO_BACKGROUND_CAMERA: constructing native QCamera"
                        << "device:" << selectedDeviceId
                        << "description:" << selected.description()
                        << "workerThread:" << QThread::currentThreadId()
                        << "parent:" << this;

                m_rtcCamera = new QCamera(selected, this);
                const quint64 cameraGeneration = ++m_rtcCameraGeneration;
                const QPointer<QCamera> cameraGuard(m_rtcCamera);

                connect(
                    m_rtcCamera,
                    &QCamera::stateChanged,
                    this,
                    [this, roomId, cameraGuard, cameraGeneration](QCamera::State state) {
                        if (cameraGuard && m_rtcCamera == cameraGuard.data() &&
                            m_rtcCameraGeneration == cameraGeneration) {
                            qInfo() << "ASTRO_BACKGROUND_CAMERA: native state changed"
                                    << "room:" << roomId
                                    << "state:" << static_cast<int>(state)
                                    << "status:" << static_cast<int>(m_rtcCamera->status())
                                    << "error:" << static_cast<int>(m_rtcCamera->error())
                                    << "errorString:" << m_rtcCamera->errorString()
                                    << "thread:" << QThread::currentThreadId();
                        }
                    }
                );
                connect(
                    m_rtcCamera,
                    &QCamera::statusChanged,
                    this,
                    [this, roomId, cameraGuard, cameraGeneration](QCamera::Status status) {
                        if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                            m_rtcCameraGeneration != cameraGeneration)
                            return;
                        qInfo() << "ASTRO_BACKGROUND_CAMERA: native status changed"
                                << "room:" << roomId
                                << "status:" << static_cast<int>(status)
                                << "state:" << (m_rtcCamera
                                    ? static_cast<int>(m_rtcCamera->state()) : -1)
                                << "error:" << (m_rtcCamera
                                    ? static_cast<int>(m_rtcCamera->error()) : -1)
                                << "errorString:" << (m_rtcCamera
                                    ? m_rtcCamera->errorString() : QString())
                                << "thread:" << QThread::currentThreadId();
                    }
                );
                connect(
                    m_rtcCamera,
                    QOverload<QCamera::Error>::of(&QCamera::error),
                    this,
                    [this, roomId, cameraGuard, cameraGeneration](QCamera::Error error) {
                        if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                            m_rtcCameraGeneration != cameraGeneration)
                            return;
                        qWarning() << "ASTRO_BACKGROUND_CAMERA: native camera error signal"
                                   << "room:" << roomId
                                   << "error:" << static_cast<int>(error)
                                   << "status:" << (m_rtcCamera
                                       ? static_cast<int>(m_rtcCamera->status()) : -1)
                                   << "state:" << (m_rtcCamera
                                       ? static_cast<int>(m_rtcCamera->state()) : -1)
                                   << "errorString:" << (m_rtcCamera
                                       ? m_rtcCamera->errorString() : QString())
                                   << "thread:" << QThread::currentThreadId();
                    }
                );

                qInfo() << "ASTRO_BACKGROUND_CAMERA: native QCamera constructed"
                        << "camera:" << m_rtcCamera
                        << "status:" << static_cast<int>(m_rtcCamera->status())
                        << "state:" << static_cast<int>(m_rtcCamera->state())
                        << "available:" << m_rtcCamera->isAvailable()
                        << "error:" << static_cast<int>(m_rtcCamera->error())
                        << "errorString:" << m_rtcCamera->errorString();

                qInfo() << "ASTRO_BACKGROUND_CAMERA: setting native camera capture mode"
                        << "mode:" << static_cast<int>(QCamera::CaptureVideo)
                        << "camera:" << m_rtcCamera;
                m_rtcCamera->setCaptureMode(QCamera::CaptureVideo);
                qInfo() << "ASTRO_BACKGROUND_CAMERA: native capture mode configured"
                        << "status:" << static_cast<int>(m_rtcCamera->status())
                        << "state:" << static_cast<int>(m_rtcCamera->state())
                        << "available:" << m_rtcCamera->isAvailable()
                        << "error:" << static_cast<int>(m_rtcCamera->error())
                        << "errorString:" << m_rtcCamera->errorString();

                // Load first so the camera backend can report its real
                // viewfinder capabilities. Starting with the backend's
                // default format can select a GL-backed surface that this
                // CPU-only RTC surface deliberately refuses.
                m_rtcBackgroundCameraStartPending = true;
                m_rtcBackgroundCameraPendingRoomId = roomId;
                m_rtcBackgroundCameraPendingDeviceId = selectedDeviceId;

                connect(
                    m_rtcCamera,
                    &QCamera::statusChanged,
                    this,
                    [this, roomId, cameraGuard, cameraGeneration](QCamera::Status status) {
                        if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                            m_rtcCameraGeneration != cameraGeneration ||
                            !m_rtcBackgroundCameraStartPending ||
                            m_rtcBackgroundCameraPendingRoomId != roomId)
                            return;

                        qInfo() << "RTC background camera status:"
                                << static_cast<int>(status)
                                << "available:" << m_rtcCamera->isAvailable();

                        if (status == QCamera::LoadedStatus) {
                            if (!m_rtcVideoSurface ||
                                m_rtcVideoRoomId != roomId) {
                                const QString message = QStringLiteral(
                                    "The RTC video surface is no longer available"
                                );
                                m_rtcBackgroundCameraStartPending = false;
                                m_rtcBackgroundCameraPendingRoomId.clear();
                                m_rtcBackgroundCameraPendingDeviceId.clear();
                                emit matrixRtcBackgroundCameraActionFinished(
                                    roomId, false, QString(), message
                                );
                                stopRtcBackgroundCamera(roomId);
                                return;
                            }

                            const QList<QVideoFrame::PixelFormat> surfaceFormats =
                                m_rtcVideoSurface->supportedPixelFormats(
                                    QAbstractVideoBuffer::NoHandle
                                );
                            const QList<QCameraViewfinderSettings> cameraSettings =
                                m_rtcCamera->supportedViewfinderSettings();

                            // Prefer formats already handled efficiently by
                            // RtcCameraVideoSurface, then the packed RGB
                            // format observed from the working foreground
                            // camera. The camera's own settings list is
                            // already ordered by backend preference.
                            const QList<QVideoFrame::PixelFormat> formatPriority = {
                                QVideoFrame::Format_YUV420P,
                                QVideoFrame::Format_NV12,
                                QVideoFrame::Format_YV12,
                                QVideoFrame::Format_NV21,
                                QVideoFrame::Format_YUYV,
                                QVideoFrame::Format_UYVY,
                                QVideoFrame::Format_ARGB32_Premultiplied,
                                QVideoFrame::Format_ARGB32,
                                QVideoFrame::Format_RGB32,
                                QVideoFrame::Format_RGB24
                            };

                            QCameraViewfinderSettings selectedSettings;
                            bool foundCompatibleSettings = false;

                            qInfo() << "ASTRO_BACKGROUND_CAMERA: viewfinder capabilities"
                                    << "cameraSettings:" << cameraSettings.size()
                                    << "surfaceCpuFormats:" << surfaceFormats;

                            // This Ubuntu Touch backend exposes no
                            // supported viewfinder settings and selects an
                            // RGB32 GL texture. Negotiate that known-supported
                            // surface before starting QCamera; do not probe
                            // CPU formats or try to replace the surface after
                            // the camera has entered ActiveStatus.
                            if (cameraSettings.isEmpty()) {
                                m_rtcVideoSurface->setAllowGlTextureFrames(true);

                                qInfo() << "ASTRO_BACKGROUND_CAMERA: camera backend"
                                        << "exposes no viewfinder settings; enabling"
                                        << "GL texture readback before attaching surface"
                                        << "device:"
                                        << m_rtcBackgroundCameraPendingDeviceId;

                                m_rtcCamera->setViewfinder(m_rtcVideoSurface);

                                qInfo() << "ASTRO_BACKGROUND_CAMERA: GL viewfinder"
                                        << "attached before camera start"
                                        << "surfaceAccepted:"
                                        << m_rtcVideoSurface->hasAcceptedFormat()
                                        << "surfaceActive:"
                                        << m_rtcVideoSurface->isActive();

                                m_rtcNativeGlFrameCount.storeRelease(0);
                                m_rtcNativeGlWatchdogPreviousCount = 0;
                                m_rtcNativeGlWatchdogTicks = 0;
                                if (m_rtcBackgroundCameraWatchdogTimer)
                                    m_rtcBackgroundCameraWatchdogTimer->start();
                                qInfo() << "ASTRO_BACKGROUND_CAMERA: native camera watchdog started"
                                        << "intervalMs:"
                                        << (m_rtcBackgroundCameraWatchdogTimer
                                            ? m_rtcBackgroundCameraWatchdogTimer->interval() : -1);

                                m_rtcCamera->start();
                                return;
                            }

                            for (QVideoFrame::PixelFormat preferredFormat : formatPriority) {
                                if (!surfaceFormats.contains(preferredFormat))
                                    continue;

                                for (const QCameraViewfinderSettings &candidate : cameraSettings) {
                                    if (candidate.pixelFormat() != preferredFormat)
                                        continue;

                                    const QSize resolution = candidate.resolution();
                                    if (!resolution.isEmpty() &&
                                        ((resolution.width() & 1) ||
                                         (resolution.height() & 1))) {
                                        continue;
                                    }

                                    selectedSettings = candidate;
                                    foundCompatibleSettings = true;
                                    break;
                                }

                                if (foundCompatibleSettings)
                                    break;
                            }

                            if (!foundCompatibleSettings) {
                                QStringList cameraFormats;
                                for (const QCameraViewfinderSettings &candidate : cameraSettings) {
                                    cameraFormats.append(
                                        QStringLiteral("%1x%2:%3@%4-%5")
                                            .arg(candidate.resolution().width())
                                            .arg(candidate.resolution().height())
                                            .arg(static_cast<int>(candidate.pixelFormat()))
                                            .arg(candidate.minimumFrameRate())
                                            .arg(candidate.maximumFrameRate())
                                    );
                                }

                                const QString message = QStringLiteral(
                                    "The native camera has no viewfinder format compatible with the CPU video surface"
                                );
                                qWarning() << "ASTRO_BACKGROUND_CAMERA: no compatible viewfinder format"
                                           << "cameraFormats:" << cameraFormats
                                           << "surfaceFormats:" << surfaceFormats;

                                m_rtcBackgroundCameraStartPending = false;
                                m_rtcBackgroundCameraPendingRoomId.clear();
                                m_rtcBackgroundCameraPendingDeviceId.clear();
                                emit matrixRtcBackgroundCameraActionFinished(
                                    roomId, false, QString(), message
                                );
                                stopRtcBackgroundCamera(roomId);
                                return;
                            }

                            m_rtcCamera->setViewfinderSettings(selectedSettings);
                            m_rtcCamera->setViewfinder(m_rtcVideoSurface);

                            qInfo() << "ASTRO_BACKGROUND_CAMERA: selected CPU viewfinder format"
                                    << "pixelFormat:"
                                    << static_cast<int>(selectedSettings.pixelFormat())
                                    << "resolution:" << selectedSettings.resolution()
                                    << "minimumFps:" << selectedSettings.minimumFrameRate()
                                    << "maximumFps:" << selectedSettings.maximumFrameRate()
                                    << "device:" << m_rtcBackgroundCameraPendingDeviceId;

                            qInfo() << "ASTRO_BACKGROUND_CAMERA: starting camera with negotiated viewfinder settings"
                                    << "device:" << m_rtcBackgroundCameraPendingDeviceId
                                    << "thread:" << QThread::currentThreadId();
                            m_rtcCamera->start();
                            return;
                        }

                        if (status == QCamera::ActiveStatus) {
                            const bool surfaceAccepted =
                                m_rtcVideoSurface &&
                                m_rtcVideoSurface->hasAcceptedFormat();
                            const QString activeDevice =
                                m_rtcBackgroundCameraPendingDeviceId;

                            qInfo() << "RTC native camera reached ActiveStatus;"
                                    << "viewfinder format accepted:" << surfaceAccepted
                                    << "surface active:"
                                    << (m_rtcVideoSurface &&
                                        m_rtcVideoSurface->isActive());

                            if (!surfaceAccepted) {
                                const QString message = QStringLiteral(
                                    "The camera became active, but its GL viewfinder surface was not accepted"
                                );
                                qWarning() << "ASTRO_BACKGROUND_CAMERA: native GL viewfinder"
                                           << "was not accepted before ActiveStatus"
                                           << "device:" << activeDevice;
                                m_rtcBackgroundCameraStartPending = false;
                                m_rtcBackgroundCameraPendingRoomId.clear();
                                m_rtcBackgroundCameraPendingDeviceId.clear();
                                emit matrixRtcBackgroundCameraActionFinished(
                                    roomId, false, QString(), message
                                );
                                stopRtcBackgroundCamera(roomId);
                                return;
                            }

                            m_rtcBackgroundCameraStartPending = false;
                            m_rtcBackgroundCameraPendingRoomId.clear();
                            m_rtcBackgroundCameraPendingDeviceId.clear();

                            qInfo() << "RTC native background camera is active:"
                                    << activeDevice;
                            emit matrixRtcBackgroundCameraActionFinished(
                                roomId, true, activeDevice, QString()
                            );
                        }
                    },
                    Qt::AutoConnection
                );

                qInfo() << "ASTRO_BACKGROUND_CAMERA: native camera status handler connected"
                        << "camera:" << m_rtcCamera
                        << "thread:" << QThread::currentThreadId();

                connect(
                    m_rtcCamera,
                    QOverload<QCamera::Error>::of(&QCamera::error),
                    this,
                    [this, roomId, cameraGuard, cameraGeneration](QCamera::Error cameraError) {
                        if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                            m_rtcCameraGeneration != cameraGeneration ||
                            cameraError == QCamera::NoError)
                            return;

                        const QString message = m_rtcCamera->errorString();
                        qWarning() << "RTC background camera error:"
                                   << static_cast<int>(cameraError)
                                   << message;

                        m_rtcBackgroundCameraStartPending = false;
                        m_rtcBackgroundCameraPendingRoomId.clear();
                        m_rtcBackgroundCameraPendingDeviceId.clear();

                        bool unloadAlreadyPending = false;
                        {
                            QMutexLocker cleanupLocker(&m_rtcCameraFrameMutex);
                            unloadAlreadyPending =
                                m_rtcBackgroundCameraStopPending;
                        }

                        // Do not delete a camera from its error callback.
                        // The backend may still be unwinding a capture or
                        // unload operation. Route cleanup through the same
                        // UnloadedStatus completion path as normal teardown.
                        emit matrixRtcBackgroundCameraActionFinished(
                            roomId, false, QString(), message
                        );
                        if (!unloadAlreadyPending)
                            stopRtcBackgroundCamera(roomId);
                    },
                    Qt::QueuedConnection
                );

                qInfo() << "ASTRO_BACKGROUND_CAMERA: native camera error handler connected"
                        << "camera:" << m_rtcCamera
                        << "thread:" << QThread::currentThreadId();

                qInfo() << "ASTRO_BACKGROUND_CAMERA: loading native camera before format negotiation"
                        << "statusBefore:" << static_cast<int>(m_rtcCamera->status())
                        << "device:" << selectedDeviceId
                        << "thread:" << QThread::currentThreadId();
                m_rtcCamera->load();
                qInfo() << "ASTRO_BACKGROUND_CAMERA: asynchronous load state"
                        << "status:" << static_cast<int>(m_rtcCamera->status())
                        << "state:" << static_cast<int>(m_rtcCamera->state())
                        << "generation:" << cameraGeneration;
                qInfo() << "ASTRO_BACKGROUND_CAMERA: QCamera::load returned"
                        << "statusAfter:" << static_cast<int>(m_rtcCamera->status())
                        << "stateAfter:" << static_cast<int>(m_rtcCamera->state())
                        << "errorAfter:" << static_cast<int>(m_rtcCamera->error())
                        << "errorString:" << m_rtcCamera->errorString();

                // All startup completion is handled by the queued
                // statusChanged callback above. Keeping a single completion
                // path avoids reporting success before the viewfinder has
                // accepted a CPU-backed format, and avoids duplicate signals
                // when a backend changes status synchronously inside start().

                // Device availability only means that a service exists. It
                // does not mean that capture has started. Wait for ActiveStatus
                // and fail explicitly if the backend never activates.
                QTimer::singleShot(8000, this, [this, roomId, cameraGuard, cameraGeneration]() {
                    if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                        m_rtcCameraGeneration != cameraGeneration ||
                        !m_rtcBackgroundCameraStartPending ||
                        m_rtcBackgroundCameraPendingRoomId != roomId)
                        return;

                    const QString message = m_rtcCamera
                        ? m_rtcCamera->errorString()
                        : QStringLiteral("The native camera disappeared");
                    m_rtcBackgroundCameraStartPending = false;
                    m_rtcBackgroundCameraPendingRoomId.clear();
                    m_rtcBackgroundCameraPendingDeviceId.clear();

                    emit matrixRtcBackgroundCameraActionFinished(
                        roomId,
                        false,
                        QString(),
                        message.isEmpty()
                            ? QStringLiteral(
                                "The native camera did not become active within 8 seconds"
                              )
                            : message
                    );

                    // Timeout is not proof that the device has closed.
                    // Request unload and wait for UnloadedStatus instead of
                    // deleting QCamera while its backend may still be active.
                    stopRtcBackgroundCamera(roomId);
                });

                qInfo() << "RTC native background camera start requested; awaiting ActiveStatus:"
                        << selected.description() << selectedDeviceId;
            }
        }
    }

    if (recoverExistingCamera) {
        stopRtcBackgroundCamera(roomId);
        return;
    }

    if (!errorMessage.isEmpty()) {
        qWarning() << "ASTRO_BACKGROUND_CAMERA: native start rejected"
                   << "room:" << roomId
                   << "error:" << errorMessage
                   << "workerThread:" << QThread::currentThreadId();
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(), errorMessage
        );
    }
}

void StorageWorker::stopRtcBackgroundCamera(const QString &roomId)
{
    qInfo() << "ASTRO_BACKGROUND_CAMERA: native stop request received"
            << "room:" << roomId
            << "workerThread:" << QThread::currentThreadId()
            << "camera:" << m_rtcCamera
            << "cameraStatus:" << (m_rtcCamera
                ? static_cast<int>(m_rtcCamera->status()) : -1)
            << "cameraState:" << (m_rtcCamera
                ? static_cast<int>(m_rtcCamera->state()) : -1);

    QCamera *cameraToUnload = nullptr;
    bool alreadyStopped = false;

    {
        QMutexLocker locker(&m_rtcCameraFrameMutex);

        // Reject stale requests before touching a queued foreground restart.
        if (m_rtcCamera && !roomId.isEmpty() && roomId != m_rtcVideoRoomId) {
            qInfo() << "ASTRO_BACKGROUND_CAMERA: ignoring stale stop request"
                    << "room:" << roomId << "activeRoom:" << m_rtcVideoRoomId;
            return;
        }

        if (m_rtcBackgroundCameraStartPending &&
            (roomId.isEmpty() ||
             roomId == m_rtcBackgroundCameraPendingRoomId)) {
            m_rtcBackgroundCameraStartPending = false;
            m_rtcBackgroundCameraPendingRoomId.clear();
            m_rtcBackgroundCameraPendingDeviceId.clear();
        }

        if (m_rtcBackgroundCameraStopPending) {
            // Teardown takes precedence, but duplicate lifecycle stops must
            // not erase a foreground restart queued during this unload.
            if (m_rtcStopVideoAfterBackgroundUnload ||
                m_rtcStopAudioAfterBackgroundUnload) {
                m_rtcBackgroundCameraRestartAfterStop = false;
                m_rtcBackgroundCameraRestartRoomId.clear();
                m_rtcBackgroundCameraRestartDeviceId.clear();
            }
            qInfo() << "ASTRO_BACKGROUND_CAMERA: unload already pending;"
                    << "preserving restart intent:"
                    << m_rtcBackgroundCameraRestartAfterStop;
            return;
        }

        if (m_rtcStopVideoAfterBackgroundUnload ||
            m_rtcStopAudioAfterBackgroundUnload) {
            m_rtcBackgroundCameraRestartAfterStop = false;
            m_rtcBackgroundCameraRestartRoomId.clear();
            m_rtcBackgroundCameraRestartDeviceId.clear();
        }

        if (!m_rtcCamera) {
            alreadyStopped = true;
        } else {
            m_rtcBackgroundCameraStopPending = true;
            m_rtcBackgroundCameraStopRoomId =
                roomId.isEmpty() ? m_rtcVideoRoomId : roomId;
            m_rtcBackgroundCameraUnloadAttempts = 0;
            cameraToUnload = m_rtcCamera;
        }
    }

    if (alreadyStopped) {
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(), QString()
        );
        return;
    }

    if (!cameraToUnload)
        return;

    const quint64 cameraGeneration = m_rtcCameraGeneration;
    const QPointer<QCamera> cameraGuard(cameraToUnload);

    // QCamera::stop() only returns to LoadedState and keeps the device open.
    // unload() closes the device asynchronously. Keep the camera and its
    // viewfinder alive until the backend confirms UnloadedStatus.
    connect(
        cameraToUnload,
        &QCamera::statusChanged,
        this,
        [this, cameraGuard, cameraGeneration](QCamera::Status status) {
            if (!cameraGuard || cameraGuard.data() != m_rtcCamera ||
                cameraGeneration != m_rtcCameraGeneration ||
                !m_rtcBackgroundCameraStopPending ||
                status != QCamera::UnloadedStatus) {
                return;
            }
            finishRtcBackgroundCameraStop();
        },
        Qt::QueuedConnection
    );

    qInfo() << "RTC native background camera unloading:"
            << m_rtcBackgroundCameraStopRoomId
            << "current status:" << static_cast<int>(cameraToUnload->status());

    if (!cameraGuard || cameraGuard.data() != m_rtcCamera ||
        cameraGeneration != m_rtcCameraGeneration)
        return;
    cameraGuard->unload();

    // Some backends complete unload synchronously and may not emit another
    // statusChanged notification after the call returns.
    if (cameraGuard && cameraGuard.data() == m_rtcCamera &&
        cameraGeneration == m_rtcCameraGeneration &&
        cameraGuard->status() == QCamera::UnloadedStatus) {
        finishRtcBackgroundCameraStop();
    } else {
        QTimer::singleShot(4000, this, [this, cameraGuard, cameraGeneration]() {
            checkRtcBackgroundCameraUnload(cameraGuard, cameraGeneration);
        });
    }
}

void StorageWorker::checkRtcBackgroundCameraUnload(
    const QPointer<QCamera> &cameraGuard,
    quint64 cameraGeneration
)
{
    if (!cameraGuard || cameraGuard.data() != m_rtcCamera ||
        cameraGeneration != m_rtcCameraGeneration ||
        !m_rtcBackgroundCameraStopPending)
        return;

    if (cameraGuard->status() == QCamera::UnloadedStatus) {
        finishRtcBackgroundCameraStop();
        return;
    }

    if (m_rtcBackgroundCameraUnloadAttempts >= 3) {
        const QString failedRoom = m_rtcBackgroundCameraStopRoomId;
        const QString message = QStringLiteral(
            "The camera service did not release the camera after 3 unload retries"
        );
        qWarning() << "ASTRO_BACKGROUND_CAMERA: unload retries exhausted;"
                   << "retaining camera object for a later recovery attempt"
                   << "room:" << failedRoom
                   << "status:" << static_cast<int>(cameraGuard->status())
                   << "state:" << static_cast<int>(cameraGuard->state())
                   << "error:" << static_cast<int>(cameraGuard->error())
                   << "errorString:" << cameraGuard->errorString();
        m_rtcBackgroundCameraStopPending = false;
        m_rtcBackgroundCameraStopRoomId.clear();
        m_rtcBackgroundCameraUnloadAttempts = 0;
        emit matrixRtcBackgroundCameraActionFinished(
            failedRoom, false, QString(), message
        );
        return;
    }

    ++m_rtcBackgroundCameraUnloadAttempts;
    qWarning() << "ASTRO_BACKGROUND_CAMERA: retrying camera unload"
               << "attempt:" << m_rtcBackgroundCameraUnloadAttempts
               << "status:" << static_cast<int>(cameraGuard->status())
               << "state:" << static_cast<int>(cameraGuard->state());
    cameraGuard->stop();
    cameraGuard->unload();

    if (cameraGuard->status() == QCamera::UnloadedStatus) {
        finishRtcBackgroundCameraStop();
        return;
    }
    QTimer::singleShot(4000, this, [this, cameraGuard, cameraGeneration]() {
        checkRtcBackgroundCameraUnload(cameraGuard, cameraGeneration);
    });
}

void StorageWorker::finishRtcBackgroundCameraStop()
{
    QString stoppedRoomId;
    QString restartRoomId;
    QString restartDeviceId;
    QString stopVideoRoomId;
    QString stopAudioRoomId;
    bool stopVideoAfterUnload = false;
    bool stopAudioAfterUnload = false;
    bool startVideoAfterUnload = false;
    QString startVideoRoomId;
    int startVideoWidth = 0;
    int startVideoHeight = 0;
    bool switchCameraAfterUnload = false;
    QString switchCameraRoomId;
    QString switchCameraDeviceId;
    bool restart = false;

    {
        QMutexLocker locker(&m_rtcCameraFrameMutex);

        if (!m_rtcBackgroundCameraStopPending ||
            !m_rtcCamera ||
            m_rtcCamera->status() != QCamera::UnloadedStatus) {
            return;
        }

        stoppedRoomId = m_rtcBackgroundCameraStopRoomId;
        if (m_rtcBackgroundCameraWatchdogTimer)
            m_rtcBackgroundCameraWatchdogTimer->stop();
        delete m_rtcCameraVideoProbe;
        m_rtcCameraVideoProbe = nullptr;
        m_rtcCamera->setViewfinder(
            static_cast<QAbstractVideoSurface *>(nullptr)
        );
        delete m_rtcCamera;
        m_rtcCamera = nullptr;

        m_rtcBackgroundCameraStopPending = false;
        m_rtcBackgroundCameraStopRoomId.clear();
        m_rtcBackgroundCameraUnloadAttempts = 0;

        stopVideoAfterUnload = m_rtcStopVideoAfterBackgroundUnload;
        stopVideoRoomId = m_rtcStopVideoAfterBackgroundUnloadRoomId;
        m_rtcStopVideoAfterBackgroundUnload = false;
        m_rtcStopVideoAfterBackgroundUnloadRoomId.clear();

        stopAudioAfterUnload = m_rtcStopAudioAfterBackgroundUnload;
        stopAudioRoomId = m_rtcStopAudioAfterBackgroundUnloadRoomId;
        m_rtcStopAudioAfterBackgroundUnload = false;
        m_rtcStopAudioAfterBackgroundUnloadRoomId.clear();

        startVideoAfterUnload = m_rtcStartVideoAfterBackgroundUnload;
        startVideoRoomId = m_rtcStartVideoAfterBackgroundUnloadRoomId;
        startVideoWidth = m_rtcStartVideoAfterBackgroundUnloadWidth;
        startVideoHeight = m_rtcStartVideoAfterBackgroundUnloadHeight;
        m_rtcStartVideoAfterBackgroundUnload = false;
        m_rtcStartVideoAfterBackgroundUnloadRoomId.clear();
        m_rtcStartVideoAfterBackgroundUnloadWidth = 0;
        m_rtcStartVideoAfterBackgroundUnloadHeight = 0;

        switchCameraAfterUnload = m_rtcSwitchCameraAfterBackgroundUnload;
        switchCameraRoomId = m_rtcSwitchCameraAfterBackgroundUnloadRoomId;
        switchCameraDeviceId = m_rtcSwitchCameraAfterBackgroundUnloadDeviceId;
        m_rtcSwitchCameraAfterBackgroundUnload = false;
        m_rtcSwitchCameraAfterBackgroundUnloadRoomId.clear();
        m_rtcSwitchCameraAfterBackgroundUnloadDeviceId.clear();

        // Explicit call teardown always wins over a deferred camera action.
        if (stopVideoAfterUnload || stopAudioAfterUnload) {
            startVideoAfterUnload = false;
            switchCameraAfterUnload = false;
        }

        restart = m_rtcBackgroundCameraRestartAfterStop &&
                  !stopVideoAfterUnload &&
                  !stopAudioAfterUnload &&
                  !startVideoAfterUnload &&
                  !switchCameraAfterUnload;
        restartRoomId = m_rtcBackgroundCameraRestartRoomId;
        restartDeviceId = m_rtcBackgroundCameraRestartDeviceId;
        m_rtcBackgroundCameraRestartAfterStop = false;
        m_rtcBackgroundCameraRestartRoomId.clear();
        m_rtcBackgroundCameraRestartDeviceId.clear();

        qInfo() << "RTC native background camera fully unloaded:"
                << stoppedRoomId;
    }

    // Report release only after the camera backend has closed the device.
    emit matrixRtcBackgroundCameraActionFinished(
        stoppedRoomId, false, QString(), QString()
    );

    if (stopVideoAfterUnload || stopAudioAfterUnload) {
        QMetaObject::invokeMethod(
            this,
            [this, stopVideoAfterUnload, stopVideoRoomId,
             stopAudioAfterUnload, stopAudioRoomId]() {
                if (stopVideoAfterUnload)
                    stopRtcVideo(stopVideoRoomId);
                if (stopAudioAfterUnload)
                    stopRtcAudio(stopAudioRoomId);
            },
            Qt::QueuedConnection
        );
        return;
    }

    if (startVideoAfterUnload && !startVideoRoomId.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, startVideoRoomId, startVideoWidth, startVideoHeight]() {
                startRtcVideo(startVideoRoomId, startVideoWidth, startVideoHeight);
            },
            Qt::QueuedConnection
        );
        return;
    }

    if (switchCameraAfterUnload &&
        !switchCameraRoomId.isEmpty() &&
        !switchCameraDeviceId.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, switchCameraRoomId, switchCameraDeviceId]() {
                finishRtcCameraSwitch(switchCameraRoomId, switchCameraDeviceId);
            },
            Qt::QueuedConnection
        );
        return;
    }

    if (restart && !restartRoomId.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, restartRoomId, restartDeviceId]() {
                startRtcBackgroundCamera(restartRoomId, restartDeviceId);
            },
            Qt::QueuedConnection
        );
    }
}

void StorageWorker::stopRtcVideo(const QString &roomId)
{
    // An explicit stop supersedes any queued start or device switch.
    m_rtcStartVideoAfterBackgroundUnload = false;
    m_rtcStartVideoAfterBackgroundUnloadRoomId.clear();
    m_rtcStartVideoAfterBackgroundUnloadWidth = 0;
    m_rtcStartVideoAfterBackgroundUnloadHeight = 0;
    m_rtcSwitchCameraAfterBackgroundUnload = false;
    m_rtcSwitchCameraAfterBackgroundUnloadRoomId.clear();
    m_rtcSwitchCameraAfterBackgroundUnloadDeviceId.clear();

    // Native QCamera unload is asynchronous. Do not let stopRtcCamera()
    // delete the QCamera while its backend still owns the capture device.
    if (m_rtcCamera &&
        (roomId.isEmpty() || roomId == m_rtcVideoRoomId)) {
        m_rtcStopVideoAfterBackgroundUnload = true;
        m_rtcStopVideoAfterBackgroundUnloadRoomId = roomId;
        stopRtcBackgroundCamera(roomId);
        return;
    }

    stopRtcCamera();
    m_rtcVideoWidth = 0;
    m_rtcVideoHeight = 0;
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
        m_matrixClient, roomIdBytes.constData(), &error
    );

    QString errorMessage;
    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }
    if (result < 0 && errorMessage.isEmpty())
        errorMessage = QStringLiteral("Could not stop LiveKit video");

    emit matrixRtcVideoActionFinished(roomId, errorMessage);
}


void StorageWorker::processRtcCameraFrame(const QVideoFrame &frame)
{
    // RtcCameraVideoFilter calls this directly from the Qt Quick render
    // thread, where the camera's GL texture context is current. Serialize
    // against stopRtcCamera() so the surface cannot be deleted mid-frame.
    QMutexLocker locker(&m_rtcCameraFrameMutex);
    if (m_rtcVideoSurface && frame.isValid())
        m_rtcVideoSurface->present(frame);
}

void StorageWorker::processRtcCameraImage(
    const QString &roomId,
    const QImage &image
)
{
    if (image.isNull() || roomId.isEmpty())
        return;

    static QAtomicInt loggedCameraImage{0};
    if (loggedCameraImage.testAndSetOrdered(0, 1)) {
        const QColor sample = image.pixelColor(image.width() / 2, image.height() / 2);
        qInfo() << "RTC camera QML image:"
                << "format:" << image.format()
                << "size:" << image.size()
                << "centre RGBA:"
                << sample.red() << sample.green()
                << sample.blue() << sample.alpha();
    }

    QMutexLocker locker(&m_rtcCameraFrameMutex);
    if (!m_rtcVideoSurface || roomId != m_rtcVideoRoomId)
        return;

    // QML's VideoOutput grab supplies a CPU-backed QImage. This avoids
    // trying to attach Ubuntu Touch's camera GL texture to a 2D framebuffer.
    const QVideoFrame frame(image);
    if (frame.isValid())
        m_rtcVideoSurface->present(frame);
}

void StorageWorker::switchRtcCamera(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty() ||
        !m_rtcCamera || !m_rtcVideoSurface ||
        m_rtcVideoRoomId != roomId) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : !m_rtcCamera
                    ? QStringLiteral("Camera is not running")
                    : QStringLiteral("Room ID is empty or camera belongs to another room")
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

    const QString currentDevice = QCameraInfo(*m_rtcCamera).deviceName();
    const QCamera::Position currentPosition =
        QCameraInfo(*m_rtcCamera).position();

    QCameraInfo selected;
    for (const QCameraInfo &camera : cameras) {
        if (camera.deviceName() != currentDevice &&
            camera.position() != QCamera::UnspecifiedPosition &&
            currentPosition != QCamera::UnspecifiedPosition &&
            camera.position() != currentPosition) {
            selected = camera;
            break;
        }
    }

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

    // Retain the video surface and selected device, but release the old
    // physical camera asynchronously before constructing its replacement.
    m_rtcSwitchCameraAfterBackgroundUnload = true;
    m_rtcSwitchCameraAfterBackgroundUnloadRoomId = roomId;
    m_rtcSwitchCameraAfterBackgroundUnloadDeviceId = selected.deviceName();
    m_rtcStartVideoAfterBackgroundUnload = false;
    m_rtcStartVideoAfterBackgroundUnloadRoomId.clear();
    m_rtcStartVideoAfterBackgroundUnloadWidth = 0;
    m_rtcStartVideoAfterBackgroundUnloadHeight = 0;
    m_rtcBackgroundCameraRestartAfterStop = false;
    m_rtcBackgroundCameraRestartRoomId.clear();
    m_rtcBackgroundCameraRestartDeviceId.clear();

    qInfo() << "ASTRO_BACKGROUND_CAMERA: deferring camera switch until unload"
            << "room:" << roomId
            << "from:" << currentDevice
            << "to:" << selected.deviceName()
            << "status:" << static_cast<int>(m_rtcCamera->status());
    stopRtcBackgroundCamera(roomId);
}

void StorageWorker::finishRtcCameraSwitch(
    const QString &roomId,
    const QString &deviceId
)
{
    if (!m_matrixClient || roomId.isEmpty() ||
        !m_rtcVideoSurface || m_rtcVideoRoomId != roomId ||
        m_rtcCamera || m_rtcBackgroundCameraStopPending) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Camera could not be switched after releasing the previous device")
        );
        return;
    }

    QCameraInfo selected;
    for (const QCameraInfo &camera : QCameraInfo::availableCameras()) {
        if (camera.deviceName() == deviceId) {
            selected = camera;
            break;
        }
    }
    if (selected.deviceName().isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("The selected camera is no longer available")
        );
        return;
    }

    if (m_rtcCameraOpenGLContext &&
        !makeRtcCameraOpenGLContextCurrent()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Could not activate the camera OpenGL context")
        );
        return;
    }

    m_rtcCamera = new QCamera(selected, this);
    const quint64 cameraGeneration = ++m_rtcCameraGeneration;
    const QPointer<QCamera> cameraGuard(m_rtcCamera);
    connect(
        m_rtcCamera, &QCamera::statusChanged, this,
        [this, cameraGuard, cameraGeneration](QCamera::Status status) {
            if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                m_rtcCameraGeneration != cameraGeneration)
                return;
            qInfo() << "ASTRO_BACKGROUND_CAMERA: switched camera status"
                    << static_cast<int>(status);
        }
    );
    connect(
        m_rtcCamera,
        QOverload<QCamera::Error>::of(&QCamera::error),
        this,
        [this, cameraGuard, cameraGeneration](QCamera::Error cameraError) {
            if (!cameraGuard || m_rtcCamera != cameraGuard.data() ||
                m_rtcCameraGeneration != cameraGeneration ||
                cameraError == QCamera::NoError)
                return;
            qWarning() << "ASTRO_BACKGROUND_CAMERA: switched camera error"
                       << static_cast<int>(cameraError)
                       << cameraGuard->errorString();
            stopRtcBackgroundCamera(m_rtcVideoRoomId);
        }
    );

    m_rtcCamera->setCaptureMode(QCamera::CaptureVideo);
    m_rtcCamera->setViewfinder(m_rtcVideoSurface);
    m_rtcCamera->start();

    const QString switchError = m_rtcCamera->isAvailable()
        ? QString()
        : QStringLiteral("The selected camera is unavailable");
    if (!switchError.isEmpty()) {
        stopRtcBackgroundCamera(roomId);
    } else {
        qInfo() << "RTC switched camera to:"
                << selected.description()
                << selected.deviceName()
                << "position:" << selected.position();
    }
    emit matrixRtcVideoActionFinished(roomId, switchError);
}

void StorageWorker::pushRtcVideoFrame(
    const QString &roomId,
    int width,
    int height,
    const QByteArray &i420,
    bool nativeCameraFrame,
    bool localCameraFrame
)
{
    if (!m_matrixClient || roomId.isEmpty())
        return;

    if (localCameraFrame)
        emit matrixRtcLocalVideoFrame(roomId, width, height, i420);

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

    static QAtomicInt loggedCameraSubmissionError{0};
    if (error) {
        if (loggedCameraSubmissionError.testAndSetOrdered(0, 1)) {
            qWarning() << "RTC video frame submission failed:"
                       << QString::fromUtf8(error)
                       << "frame:" << width << "x" << height;
        }
        astro_string_free(error);
    } else if (result < 0 &&
               loggedCameraSubmissionError.testAndSetOrdered(0, 1)) {
        qWarning() << "RTC video frame submission failed"
                   << "frame:" << width << "x" << height;
    } else if (nativeCameraFrame) {
        static QAtomicInt nativeSuccessfulSubmissions{0};
        const int submitted =
            nativeSuccessfulSubmissions.fetchAndAddRelaxed(1) + 1;
        if (submitted <= 3 || submitted % 30 == 0) {
            qInfo() << "ASTRO_RTC_CAMERA: native LiveKit frame submission succeeded"
                    << "frames:" << submitted
                    << "size:" << width << "x" << height
                    << "i420Bytes:" << i420.size()
                    << "result:" << result
                    << "room:" << roomId;
        }
    }
}

void StorageWorker::startRtcScreenShare(
    const QString &roomId,
    int width,
    int height,
    bool includeDeviceAudio
)
{
    qInfo() << "ASTRO_SCREEN_SHARE: worker request received"
            << "matrixClient:" << (m_matrixClient != nullptr)
            << "roomIdEmpty:" << roomId.isEmpty()
            << "size:" << width << "x" << height;
    if (!m_rtcScreenShareStopPendingRoomId.isEmpty() ||
        !m_rtcMirReleasePendingRoomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            QStringLiteral("Screen-share teardown is still in progress")
        );
        return;
    }
    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(roomId,
            !m_matrixClient ? QStringLiteral("No active Matrix session")
                            : QStringLiteral("Room ID is empty"));
        return;
    }
    if (m_rtcScreenShareTimer) {
        emit matrixRtcScreenShareActionFinished(
            roomId, QStringLiteral("Screen sharing is already running"));
        return;
    }
    if (m_rtcScreenShareConnection || m_rtcScreenCast ||
        m_rtcScreenShareStream) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            QStringLiteral("The previous Mir screen-share session is retained to avoid the UI freeze; restart Astro before sharing again")
        );
        return;
    }

    // Each accepted start owns a unique generation. Any delayed callbacks
    // from a previous attempt become harmless, even when the same room starts
    // another share before an old timeout would otherwise fire.
    const quint64 generation = ++m_rtcScreenShareGeneration;
    qInfo() << "ASTRO_SCREEN_SHARE: starting capture session"
            << "generation:" << generation
            << "room:" << roomId;
    // width/height are retained as a caller hint for API compatibility.
    // The actual capture and LiveKit dimensions are selected from Mir below.
    const QByteArray roomIdBytes = roomId.toUtf8();
    auto failStart = [this, &roomId](const QString &message) {
        stopRtcScreenShareCapture(roomId);
        beginRtcScreenShareUnpublish(roomId, message);
    };

    if (includeDeviceAudio) {
        QString audioError;
        if (!startRtcScreenShareAudioCapture(roomId, &audioError)) {
            failStart(audioError.isEmpty()
                ? QStringLiteral("Could not start device audio capture")
                : audioError);
            return;
        }
    }

    // Lomiri exposes the app-facing Mir socket under the user's runtime
    // directory. The system socket at /run/mir_socket is not accessible to
    // confined applications.
    const QByteArray mirSocketPath = QStringLiteral("/run/user/%1/mir_socket")
        .arg(static_cast<qulonglong>(getuid())).toLocal8Bit();
    QThread *currentThread = QThread::currentThread();
    qInfo() << "ASTRO_SCREEN_SHARE: connecting to Mir"
            << "socket:" << mirSocketPath
            << "threadName:" << currentThread->objectName()
            << "threadId:" << quintptr(QThread::currentThreadId());
    // Give each screen-share session its own Mir connection. Disconnecting
    // this connection on stop lets Mir tear down the screencast without
    // touching any other Astro or Lomiri client connection.
    m_rtcScreenShareConnection = mir_connect_sync(
        mirSocketPath.constData(), "Astro Screen Share");
    const bool mirConnectionValid =
        m_rtcScreenShareConnection &&
        mir_connection_is_valid(m_rtcScreenShareConnection);
    qInfo() << "ASTRO_SCREEN_SHARE: Mir connect returned"
            << "connection:" << static_cast<const void *>(m_rtcScreenShareConnection)
            << "valid:" << mirConnectionValid;
    if (!mirConnectionValid) {
        const QString message = m_rtcScreenShareConnection
            ? QString::fromLocal8Bit(mir_connection_get_error_message(m_rtcScreenShareConnection))
            : QStringLiteral("Could not connect to the Mir display server");
        qWarning() << "ASTRO_SCREEN_SHARE: Mir connection failed"
                   << "error:" << message;
        failStart(message.isEmpty()
            ? QStringLiteral("Could not connect to the Mir display server") : message);
        return;
    }

    MirDisplayConfig *displayConfig =
        mir_connection_create_display_configuration(m_rtcScreenShareConnection);
    if (!displayConfig || mir_display_config_get_num_outputs(displayConfig) == 0) {
        if (displayConfig) mir_display_config_release(displayConfig);
        failStart(QStringLiteral("Mir did not report an active display"));
        return;
    }
    const MirOutput *output = mir_display_config_get_output(displayConfig, 0);
    const MirOutputMode *mode = output ? mir_output_get_current_mode(output) : nullptr;
    if (!output || !mode) {
        mir_display_config_release(displayConfig);
        failStart(QStringLiteral("Could not determine the active display size"));
        return;
    }
    const MirRectangle captureRegion{
        mir_output_get_position_x(output),
        mir_output_get_position_y(output),
        static_cast<unsigned int>(mir_output_mode_get_width(mode)),
        static_cast<unsigned int>(mir_output_mode_get_height(mode))
    };
    mir_display_config_release(displayConfig);

    const int displayWidth = static_cast<int>(captureRegion.width);
    const int displayHeight = static_cast<int>(captureRegion.height);
    if (displayWidth < 2 || displayHeight < 2 ||
        (displayWidth & 1) || (displayHeight & 1)) {
        failStart(QStringLiteral("Mir reported unsupported display dimensions"));
        return;
    }

    // Keep text readable while reducing the CPU-bound conversion workload:
    // cap the long edge at 1280 pixels and the frame at roughly 0.9 megapixels.
    const double displayPixels =
        static_cast<double>(displayWidth) * displayHeight;
    const double scale = std::min(
        1.0,
        std::min(
            1280.0 / std::max(displayWidth, displayHeight),
            std::sqrt(921600.0 / displayPixels)
        )
    );
    width = std::max(2, static_cast<int>(displayWidth * scale) & ~1);
    height = std::max(2, static_cast<int>(displayHeight * scale) & ~1);

    qInfo() << "ASTRO_SCREEN_SHARE: selected capture resolution"
            << width << "x" << height
            << "from display" << displayWidth << "x" << displayHeight
            << "caller hint" << "480x854";

    qInfo() << "ASTRO_SCREEN_SHARE: publishing LiveKit screen-share track";
    char *publishError = nullptr;
    const int32_t publishResult = astro_matrix_start_rtc_screenshare(
        m_matrixClient, roomIdBytes.constData(),
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        includeDeviceAudio ? 1 : 0, &publishError);
    QString publishErrorMessage;
    if (publishError) {
        publishErrorMessage = QString::fromUtf8(publishError);
        astro_string_free(publishError);
    }
    if (publishResult < 0) {
        if (publishErrorMessage.isEmpty())
            publishErrorMessage = QStringLiteral("Could not publish LiveKit screen-share track");
        failStart(publishErrorMessage);
        return;
    }

    const MirPixelFormat pixelFormat = mir_pixel_format_abgr_8888;
    MirScreencastSpec *spec = mir_create_screencast_spec(m_rtcScreenShareConnection);
    if (!spec) {
        failStart(QStringLiteral("Could not create a Mir screencast specification"));
        return;
    }
    mir_screencast_spec_set_width(spec, static_cast<unsigned int>(width));
    mir_screencast_spec_set_height(spec, static_cast<unsigned int>(height));
    mir_screencast_spec_set_pixel_format(spec, pixelFormat);
    mir_screencast_spec_set_capture_region(spec, &captureRegion);
    // Keep Mir's server-allocated screencast buffers. The CPU-only buffers
    // from mir_connection_allocate_buffer_sync() cannot be used as GL targets.
    m_rtcScreenCast = mir_screencast_create_sync(spec);
    mir_screencast_spec_release(spec);
    if (!mir_screencast_is_valid(m_rtcScreenCast)) {
        const QString message = m_rtcScreenCast
            ? QString::fromLocal8Bit(mir_screencast_get_error_message(m_rtcScreenCast))
            : QStringLiteral("Mir rejected the screen-capture request");
        failStart(message.isEmpty()
            ? QStringLiteral("Mir rejected the screen-capture request") : message);
        return;
    }

    // Use the screencast's server-allocated buffer stream, as mirscreencast
    // does. These buffers support the compositor's GL render-target path.
    m_rtcScreenShareStream = mir_screencast_get_buffer_stream(m_rtcScreenCast);
    if (!m_rtcScreenShareStream ||
        !mir_buffer_stream_is_valid(m_rtcScreenShareStream)) {
        const QString message = m_rtcScreenShareStream
            ? QString::fromLocal8Bit(
                mir_buffer_stream_get_error_message(m_rtcScreenShareStream))
            : QStringLiteral("Mir did not provide a screencast buffer stream");
        failStart(message.isEmpty()
            ? QStringLiteral("Could not obtain a Mir screencast buffer stream")
            : message);
        return;
    }
    qInfo() << "ASTRO_SCREEN_SHARE: Mir screencast buffer stream ready";

    m_rtcScreenShareRoomId = roomId;
    m_rtcScreenShareWidth = width;
    m_rtcScreenShareHeight = height;
    m_rtcScreenShareLastFrame.clear();
    m_rtcScreenShareLastFrameSent.invalidate();
    m_rtcScreenShareStartPending = true;
    m_rtcScreenSharePerfWindow.start();
    m_rtcScreenSharePerfFrames = 0;
    m_rtcScreenSharePerfMapUs = 0;
    m_rtcScreenSharePerfCompareUs = 0;
    m_rtcScreenSharePerfCopyConvertUs = 0;
    m_rtcScreenSharePerfSubmitUs = 0;
    m_rtcScreenShareTimer = new QTimer(this);
    m_rtcScreenShareTimer->setInterval(33);
    connect(m_rtcScreenShareTimer, &QTimer::timeout,
            this, [this, generation]() {
        if (generation != m_rtcScreenShareGeneration)
            return;
        captureRtcScreenShareFrame();
    });
    m_rtcScreenShareTimer->start();
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != m_rtcScreenShareGeneration)
            return;
        captureRtcScreenShareFrame();
    });
    QTimer::singleShot(5000, this, [this, roomId, generation]() {
        if (generation != m_rtcScreenShareGeneration ||
            !m_rtcScreenShareStartPending ||
            m_rtcScreenShareRoomId != roomId)
            return;

        qWarning() << "ASTRO_SCREEN_SHARE: capture startup timed out"
                   << "generation:" << generation
                   << "room:" << roomId;
        stopRtcScreenShareCapture();
        beginRtcScreenShareUnpublish(
            roomId,
            QStringLiteral("Mir connected, but no screen frame was delivered within 5 seconds")
        );
    });
    qInfo() << "In-process Mir screen capture started:" << width << "x" << height;
}

void StorageWorker::captureRtcScreenShareFrame()
{
    if (!m_rtcScreenShareConnection || !m_rtcScreenCast ||
        !m_rtcScreenShareStream || m_rtcScreenShareRoomId.isEmpty())
        return;

    QElapsedTimer stageClock;
    stageClock.start();

    const QString roomId = m_rtcScreenShareRoomId;
    const int width = m_rtcScreenShareWidth;
    const int height = m_rtcScreenShareHeight;

    MirGraphicsRegion region{};
    const bool mapped =
        mir_buffer_stream_get_graphics_region(m_rtcScreenShareStream, &region);
    const qint64 mapUs = stageClock.nsecsElapsed() / 1000;
    stageClock.restart();
    const bool validRegion = mapped && region.vaddr &&
        region.width >= static_cast<unsigned int>(width) &&
        region.height >= static_cast<unsigned int>(height) &&
        region.stride >= width * 4;
    if (!validRegion) {
        if (mapped)
            mir_buffer_stream_swap_buffers_sync(m_rtcScreenShareStream);
        stopRtcScreenShareCapture();
        beginRtcScreenShareUnpublish(
            roomId,
            QStringLiteral("Could not access the Mir screencast buffer stream")
        );
        return;
    }

    // Mir's buffer-stream screencast is vertically inverted. Compare its
    // rows directly against the previous top-down frame before allocating or
    // copying a full RGBA image; static screens are common on mobile calls.
    const int frameBytes = width * height * 4;
    bool unchanged = m_rtcScreenShareLastFrame.size() == frameBytes &&
        m_rtcScreenShareLastFrameSent.isValid() &&
        m_rtcScreenShareLastFrameSent.elapsed() < 1000;
    if (unchanged) {
        for (int row = 0; row < height; ++row) {
            const int sourceRow = height - 1 - row;
            if (std::memcmp(
                    m_rtcScreenShareLastFrame.constData() + row * width * 4,
                    region.vaddr + sourceRow * region.stride,
                    static_cast<size_t>(width * 4)) != 0) {
                unchanged = false;
                break;
            }
        }
    }
    const qint64 compareUs = stageClock.nsecsElapsed() / 1000;
    if (unchanged) {
        mir_buffer_stream_swap_buffers_sync(m_rtcScreenShareStream);
        return;
    }
    stageClock.restart();

    const QByteArray roomBytes = roomId.toUtf8();
    AstroRtcI420Frame *i420Frame = nullptr;
    uint8_t *planeY = nullptr, *planeU = nullptr, *planeV = nullptr;
    uint32_t strideY = 0, strideU = 0, strideV = 0;
    char *frameError = nullptr;
    const int32_t createResult = astro_matrix_create_rtc_screenshare_frame(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        &i420Frame, &planeY, &strideY, &planeU, &strideU,
        &planeV, &strideV, &frameError);
    QString failure;
    if (frameError) {
        failure = QString::fromUtf8(frameError);
        astro_string_free(frameError);
        frameError = nullptr;
    } else if (createResult < 0 || !i420Frame || !planeY || !planeU || !planeV) {
        failure = QStringLiteral("Could not allocate a LiveKit I420 screen frame");
    }
    if (!failure.isEmpty()) {
        if (i420Frame)
            astro_matrix_free_rtc_screenshare_frame(i420Frame);
        mir_buffer_stream_swap_buffers_sync(m_rtcScreenShareStream);
        stopRtcScreenShareCapture();
        beginRtcScreenShareUnpublish(roomId, failure);
        return;
    }

    stageClock.restart();
    const bool converted = mirRgbaFrameToI420AndCopy(
        reinterpret_cast<const uchar *>(region.vaddr),
        static_cast<int>(region.stride), width, height,
        m_rtcScreenShareLastFrame,
        planeY, static_cast<int>(strideY),
        planeU, static_cast<int>(strideU),
        planeV, static_cast<int>(strideV));
    mir_buffer_stream_swap_buffers_sync(m_rtcScreenShareStream);
    const qint64 copyConvertUs = stageClock.nsecsElapsed() / 1000;
    if (!converted) {
        astro_matrix_free_rtc_screenshare_frame(i420Frame);
        stopRtcScreenShareCapture();
        beginRtcScreenShareUnpublish(
            roomId, QStringLiteral("Could not convert the Mir screen frame to I420"));
        return;
    }

    stageClock.restart();
    const int32_t pushResult = astro_matrix_submit_rtc_screenshare_frame(
        m_matrixClient, roomBytes.constData(), i420Frame, &frameError);
    QString submitFailure;
    if (frameError) {
        submitFailure = QString::fromUtf8(frameError);
        astro_string_free(frameError);
    } else if (pushResult < 0) {
        submitFailure = QStringLiteral("LiveKit rejected the screen-share frame");
    }
    if (!submitFailure.isEmpty()) {
        stopRtcScreenShareCapture();
        beginRtcScreenShareUnpublish(roomId, submitFailure);
        return;
    }

    const qint64 submitUs = stageClock.nsecsElapsed() / 1000;
    ++m_rtcScreenSharePerfFrames;
    m_rtcScreenSharePerfMapUs += mapUs;
    m_rtcScreenSharePerfCompareUs += compareUs;
    m_rtcScreenSharePerfCopyConvertUs += copyConvertUs;
    m_rtcScreenSharePerfSubmitUs += submitUs;

    if (m_rtcScreenSharePerfWindow.elapsed() >= 5000) {
        const double count = static_cast<double>(m_rtcScreenSharePerfFrames);
        if (count > 0.0) {
            qInfo() << "ASTRO_SCREEN_SHARE_PERF:"
                    << "resolution:" << width << "x" << height
                    << "changedFrames:" << m_rtcScreenSharePerfFrames
                    << "avgMapMs:" << (m_rtcScreenSharePerfMapUs / count / 1000.0)
                    << "avgCompareMs:" << (m_rtcScreenSharePerfCompareUs / count / 1000.0)
                    << "avgCopyConvertMs:" << (m_rtcScreenSharePerfCopyConvertUs / count / 1000.0)
                    << "avgLiveKitSubmitMs:" << (m_rtcScreenSharePerfSubmitUs / count / 1000.0);
        }
        m_rtcScreenSharePerfFrames = 0;
        m_rtcScreenSharePerfMapUs = 0;
        m_rtcScreenSharePerfCompareUs = 0;
        m_rtcScreenSharePerfCopyConvertUs = 0;
        m_rtcScreenSharePerfSubmitUs = 0;
        m_rtcScreenSharePerfWindow.restart();
    }

    m_rtcScreenShareLastFrameSent.restart();

    if (m_rtcScreenShareStartPending) {
        m_rtcScreenShareStartPending = false;
        emit matrixRtcScreenShareActionFinished(roomId, QString());
    }
}

void StorageWorker::stopRtcScreenShare(const QString &roomId)
{
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: worker received request"
            << "thread:" << quintptr(QThread::currentThreadId())
            << "captureActive:" << (m_rtcScreenShareTimer != nullptr)
            << "mirCastActive:" << (m_rtcScreenCast != nullptr);

    if (!m_rtcScreenShareStopPendingRoomId.isEmpty() ||
        !m_rtcMirReleasePendingRoomId.isEmpty()) {
        const QString pendingRoom = !m_rtcMirReleasePendingRoomId.isEmpty()
            ? m_rtcMirReleasePendingRoomId
            : m_rtcScreenShareStopPendingRoomId;
        if (pendingRoom == roomId)
            return;
        emit matrixRtcScreenShareActionFinished(
            roomId,
            QStringLiteral("Another screen-share teardown is already running")
        );
        return;
    }

    if (m_rtcScreenShareTimer && m_rtcScreenShareRoomId != roomId) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            QStringLiteral("Screen sharing is active in another room")
        );
        return;
    }

    if (!m_matrixClient || roomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            !m_matrixClient ? QStringLiteral("No active Matrix session")
                            : QStringLiteral("Room ID is empty")
        );
        return;
    }

    // Invalidate queued capture/timeout callbacks before stopping producers.
    ++m_rtcScreenShareGeneration;
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: invalidated capture generation"
            << "generation:" << m_rtcScreenShareGeneration;

    // Stop frame/audio producers first. Record each synchronous stage so a
    // device-side stall can be attributed to audio shutdown, timer shutdown,
    // or the later LiveKit FFI request.
    QElapsedTimer stopStageTimer;
    stopStageTimer.start();
    if (m_rtcScreenShareAudioInput) {
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: stopping device audio input";
        m_rtcScreenShareAudioInput->stop();
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: device audio input stopped"
                << "elapsedMs:" << stopStageTimer.restart();
        m_rtcScreenShareAudioInput->deleteLater();
        m_rtcScreenShareAudioInput = nullptr;
    } else {
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: no device audio input to stop";
    }
    m_rtcScreenShareAudioDevice = nullptr;
    m_rtcScreenShareAudioBuffer.clear();
    m_rtcScreenShareAudioRoomId.clear();

    if (m_rtcScreenShareTimer) {
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: stopping capture timer";
        m_rtcScreenShareTimer->stop();
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: capture timer stopped"
                << "elapsedMs:" << stopStageTimer.restart();
        m_rtcScreenShareTimer->deleteLater();
        m_rtcScreenShareTimer = nullptr;
    }

    releaseRtcScreenShareMirConnection(roomId);
    m_rtcScreenShareStream = nullptr;
    m_rtcScreenShareRoomId.clear();
    m_rtcScreenShareWidth = 0;
    m_rtcScreenShareHeight = 0;
    m_rtcScreenShareLastFrame.clear();
    m_rtcScreenShareLastFrameSent.invalidate();
    m_rtcScreenShareStartPending = false;

    // Mir teardown runs on a dedicated connection in a background thread.
    // The public C API only provides synchronous screencast destruction, so
    // this diagnostic test disconnects the whole session instead.
    beginRtcScreenShareUnpublish(roomId, QString());
}

void StorageWorker::beginRtcScreenShareUnpublish(
    const QString &roomId,
    const QString &completionError
)
{
    if (roomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(
            roomId, QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (!m_rtcScreenShareStopPendingRoomId.isEmpty()) {
        if (m_rtcScreenShareStopPendingRoomId == roomId) {
            qInfo() << "ASTRO_SCREEN_SHARE_STOP: duplicate unpublish ignored"
                    << "room:" << roomId;
            return;
        }
        emit matrixRtcScreenShareActionFinished(
            roomId,
            QStringLiteral("Another screen-share teardown is already running")
        );
        return;
    }

    m_rtcScreenShareStopPendingRoomId = roomId;
    m_rtcScreenShareStopCompletionError = completionError;
    m_rtcScreenShareUnpublishStarted = true;
    m_rtcScreenShareUnpublishFinished = false;

    if (!m_matrixClient) {
        if (m_rtcScreenShareStopCompletionError.isEmpty())
            m_rtcScreenShareStopCompletionError =
                QStringLiteral("No active Matrix session");
        m_rtcScreenShareUnpublishFinished = true;
        pollRtcScreenShareStop();
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;
    QElapsedTimer unpublishRequestTimer;
    unpublishRequestTimer.start();
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: requesting LiveKit unpublish"
            << "room:" << roomId;
    const int32_t result = astro_matrix_stop_rtc_screenshare(
        m_matrixClient, roomIdBytes.constData(), &error
    );
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: LiveKit unpublish request returned"
            << "result:" << result
            << "elapsedMs:" << unpublishRequestTimer.elapsed();

    if (error) {
        const QString ffiError = QString::fromUtf8(error);
        astro_string_free(error);
        if (!ffiError.isEmpty()) {
            if (!m_rtcScreenShareStopCompletionError.isEmpty())
                m_rtcScreenShareStopCompletionError += QStringLiteral("; ");
            m_rtcScreenShareStopCompletionError += ffiError;
        }
    }

    if (result <= 0) {
        if (result < 0 && m_rtcScreenShareStopCompletionError.isEmpty())
            m_rtcScreenShareStopCompletionError =
                QStringLiteral("Could not start LiveKit screen-share teardown");
        m_rtcScreenShareUnpublishFinished = true;
        pollRtcScreenShareStop();
        return;
    }

    if (m_rtcScreenShareStopPollTimer)
        m_rtcScreenShareStopPollTimer->start();
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: LiveKit unpublish running asynchronously"
            << "room:" << roomId;
}

void StorageWorker::pollRtcScreenShareStop()
{
    // Keep the stop operation pending until both independent resources have
    // actually finished: LiveKit must unpublish its tracks and the dedicated
    // Mir connection must disconnect before another session can start.
    if (!m_rtcMirReleasePendingRoomId.isEmpty() &&
        m_rtcMirReleaseComplete &&
        m_rtcMirReleaseComplete->load(std::memory_order_acquire)) {
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: dedicated Mir connection disconnected"
                << "room:" << m_rtcMirReleasePendingRoomId;
        m_rtcMirReleasePendingRoomId.clear();
        m_rtcMirReleasePendingError.clear();
        m_rtcMirReleaseComplete.reset();
    }

    if (m_rtcScreenShareStopPendingRoomId.isEmpty()) {
        if (m_rtcMirReleasePendingRoomId.isEmpty() &&
            m_rtcScreenShareStopPollTimer) {
            m_rtcScreenShareStopPollTimer->stop();
        }
        return;
    }

    const QString roomId = m_rtcScreenShareStopPendingRoomId;
    if (!m_rtcScreenShareUnpublishFinished) {
        if (!m_matrixClient) {
            if (!m_rtcScreenShareStopCompletionError.isEmpty())
                m_rtcScreenShareStopCompletionError += QStringLiteral("; ");
            m_rtcScreenShareStopCompletionError +=
                QStringLiteral("Matrix session closed during screen-share teardown");
            m_rtcScreenShareUnpublishFinished = true;
        } else {
            const QByteArray roomIdBytes = roomId.toUtf8();
            char *error = nullptr;
            const int32_t result = astro_matrix_poll_rtc_screenshare_stop(
                m_matrixClient, roomIdBytes.constData(), &error
            );

            if (error) {
                const QString ffiError = QString::fromUtf8(error);
                astro_string_free(error);
                if (!ffiError.isEmpty()) {
                    if (!m_rtcScreenShareStopCompletionError.isEmpty())
                        m_rtcScreenShareStopCompletionError += QStringLiteral("; ");
                    m_rtcScreenShareStopCompletionError += ffiError;
                }
            }

            if (result > 0)
                return;

            if (result < 0 &&
                m_rtcScreenShareStopCompletionError.isEmpty()) {
                m_rtcScreenShareStopCompletionError =
                    QStringLiteral("Could not finish LiveKit screen-share teardown");
            }
            m_rtcScreenShareUnpublishFinished = true;
            qInfo() << "ASTRO_SCREEN_SHARE_STOP: LiveKit unpublish completed"
                    << "result:" << result
                    << "error:" << m_rtcScreenShareStopCompletionError;
        }
    }

    if (!m_rtcScreenShareUnpublishFinished ||
        !m_rtcMirReleasePendingRoomId.isEmpty()) {
        return;
    }

    const QString completionError = m_rtcScreenShareStopCompletionError;
    m_rtcScreenShareStopPendingRoomId.clear();
    m_rtcScreenShareStopCompletionError.clear();
    m_rtcScreenShareUnpublishStarted = false;
    m_rtcScreenShareUnpublishFinished = false;

    qInfo() << "ASTRO_SCREEN_SHARE_STOP: full teardown completed"
            << "room:" << roomId
            << "error:" << completionError;
    emit matrixRtcScreenShareActionFinished(roomId, completionError);

    if (m_rtcScreenShareStopPollTimer)
        m_rtcScreenShareStopPollTimer->stop();
}
bool StorageWorker::startRtcScreenShareAudioCapture(
    const QString &roomId,
    QString *error
)
{
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec(QStringLiteral("audio/pcm"));
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    const QList<QAudioDeviceInfo> devices =
        QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    QAudioDeviceInfo monitor;
    for (const QAudioDeviceInfo &device : devices) {
        const QString name = device.deviceName();
        qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: input device:" << name;
        if (!name.contains(QStringLiteral("monitor"), Qt::CaseInsensitive) ||
            !device.isFormatSupported(format))
            continue;

        monitor = device;
        if (name.contains(QStringLiteral("primary_output.monitor"),
                          Qt::CaseInsensitive))
            break;
    }

    if (monitor.isNull()) {
        if (error) {
            *error = QStringLiteral(
                "Qt could not find a 48 kHz stereo PulseAudio playback monitor input");
        }
        return false;
    }

    if (!monitor.isFormatSupported(format)) {
        if (error) {
            *error = QStringLiteral(
                "The playback monitor does not support 48 kHz stereo 16-bit PCM");
        }
        return false;
    }

    m_rtcScreenShareAudioInput = new QAudioInput(monitor, format, this);
    m_rtcScreenShareAudioInput->setBufferSize(3840);
    m_rtcScreenShareAudioRoomId = roomId;
    m_rtcScreenShareAudioBuffer.clear();
    m_rtcScreenShareAudioDiagnosticBytes = 0;
    m_rtcScreenShareAudioDiagnosticFrames = 0;
    m_rtcScreenShareAudioDiagnosticSquares = 0.0L;
    m_rtcScreenShareAudioDiagnosticSamples = 0;
    m_rtcScreenShareAudioDiagnosticsTimer.start();
    m_rtcScreenShareAudioInput->setNotifyInterval(1000);
    m_rtcScreenShareAudioDevice = m_rtcScreenShareAudioInput->start();

    // start() can successfully open the monitor while it is idle because
    // no playback samples are available yet. IdleState is not an open failure;
    // readyRead will fire when playback data becomes available.
    if (!m_rtcScreenShareAudioDevice ||
        m_rtcScreenShareAudioInput->error() != QAudio::NoError ||
        m_rtcScreenShareAudioInput->state() == QAudio::StoppedState) {
        qWarning() << "ASTRO_SCREEN_SHARE_AUDIO: QAudioInput failed to open"
                   << "device:" << monitor.deviceName()
                   << "error:" << static_cast<int>(m_rtcScreenShareAudioInput->error())
                   << "state:" << static_cast<int>(m_rtcScreenShareAudioInput->state())
                   << "deviceReturned:" << (m_rtcScreenShareAudioDevice != nullptr);
        if (error) {
            *error = QStringLiteral("PulseAudio playback monitor could not be opened: %1")
                .arg(monitor.deviceName());
        }
        if (m_rtcScreenShareAudioInput) {
            m_rtcScreenShareAudioInput->stop();
            m_rtcScreenShareAudioInput->deleteLater();
            m_rtcScreenShareAudioInput = nullptr;
        }
        m_rtcScreenShareAudioDevice = nullptr;
        m_rtcScreenShareAudioRoomId.clear();
        return false;
    }

    connect(
        m_rtcScreenShareAudioDevice,
        &QIODevice::readyRead,
        this,
        &StorageWorker::captureRtcScreenShareAudio
    );
    connect(
        m_rtcScreenShareAudioInput,
        &QAudioInput::notify,
        this,
        [this]() {
            if (!m_rtcScreenShareAudioInput)
                return;
            qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: input notify"
                    << "state:" << static_cast<int>(m_rtcScreenShareAudioInput->state())
                    << "error:" << static_cast<int>(m_rtcScreenShareAudioInput->error())
                    << "processedUs:" << m_rtcScreenShareAudioInput->processedUSecs()
                    << "bytesReady:" << (m_rtcScreenShareAudioDevice
                        ? m_rtcScreenShareAudioDevice->bytesAvailable() : -1);
        }
    );
    connect(
        m_rtcScreenShareAudioInput,
        &QAudioInput::stateChanged,
        this,
        [this](QAudio::State state) {
            qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: input state changed"
                    << "state:" << static_cast<int>(state)
                    << "error:" << (m_rtcScreenShareAudioInput
                        ? static_cast<int>(m_rtcScreenShareAudioInput->error()) : -1);
        }
    );

    qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: capture opened"
            << "device:" << monitor.deviceName()
            << "state:" << static_cast<int>(m_rtcScreenShareAudioInput->state())
            << "error:" << static_cast<int>(m_rtcScreenShareAudioInput->error())
            << "bufferSize:" << m_rtcScreenShareAudioInput->bufferSize()
            << "processedUs:" << m_rtcScreenShareAudioInput->processedUSecs();
    qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: capturing playback monitor:"
            << monitor.deviceName();
    return true;
}

void StorageWorker::captureRtcScreenShareAudio()
{
    if (!m_rtcScreenShareAudioDevice || !m_matrixClient ||
        m_rtcScreenShareAudioRoomId.isEmpty())
        return;

    const QByteArray incoming = m_rtcScreenShareAudioDevice->readAll();
    m_rtcScreenShareAudioDiagnosticBytes += static_cast<quint64>(incoming.size());
    m_rtcScreenShareAudioBuffer.append(incoming);

    // 10 ms at 48 kHz stereo, signed 16-bit little-endian PCM.
    constexpr int sampleCount = 960;
    constexpr int frameBytes = sampleCount * static_cast<int>(sizeof(qint16));
    while (m_rtcScreenShareAudioBuffer.size() >= frameBytes) {
        QVector<qint16> samples(sampleCount);
        std::memcpy(
            samples.data(),
            m_rtcScreenShareAudioBuffer.constData(),
            static_cast<size_t>(frameBytes)
        );
        m_rtcScreenShareAudioBuffer.remove(0, frameBytes);
        long double frameSquares = 0.0L;
        for (int i = 0; i < sampleCount; ++i) {
            const long double sample = samples[i];
            frameSquares += sample * sample;
        }
        m_rtcScreenShareAudioDiagnosticSquares += frameSquares;
        m_rtcScreenShareAudioDiagnosticSamples += sampleCount;

        const QByteArray roomBytes = m_rtcScreenShareAudioRoomId.toUtf8();
        char *error = nullptr;
        const int32_t result = astro_matrix_push_rtc_screenshare_audio_frame(
            m_matrixClient,
            roomBytes.constData(),
            reinterpret_cast<const int16_t *>(samples.constData()),
            static_cast<uint64_t>(samples.size()),
            &error
        );

        if (error || result < 0) {
            const QString message = error
                ? QString::fromUtf8(error)
                : QStringLiteral("LiveKit rejected a screen-share audio frame");
            if (error)
                astro_string_free(error);
            qWarning() << "ASTRO_SCREEN_SHARE_AUDIO: capture stopped:" << message;
            if (m_rtcScreenShareAudioInput) {
                m_rtcScreenShareAudioInput->stop();
                m_rtcScreenShareAudioInput->deleteLater();
                m_rtcScreenShareAudioInput = nullptr;
            }
            m_rtcScreenShareAudioDevice = nullptr;
            m_rtcScreenShareAudioBuffer.clear();
            m_rtcScreenShareAudioRoomId.clear();
            return;
        }
        ++m_rtcScreenShareAudioDiagnosticFrames;
    }

    if (m_rtcScreenShareAudioDiagnosticsTimer.isValid() &&
        m_rtcScreenShareAudioDiagnosticsTimer.elapsed() >= 1000) {
        const double rms = m_rtcScreenShareAudioDiagnosticSamples > 0
            ? std::sqrt(static_cast<double>(
                m_rtcScreenShareAudioDiagnosticSquares /
                m_rtcScreenShareAudioDiagnosticSamples))
            : 0.0;
        qInfo() << "ASTRO_SCREEN_SHARE_AUDIO: capture stats"
                << "inputBytes:" << m_rtcScreenShareAudioDiagnosticBytes
                << "submittedFrames:" << m_rtcScreenShareAudioDiagnosticFrames
                << "rms:" << rms
                << "bufferedBytes:" << m_rtcScreenShareAudioBuffer.size()
                << "inputState:" << (m_rtcScreenShareAudioInput
                    ? static_cast<int>(m_rtcScreenShareAudioInput->state()) : -1)
                << "inputError:" << (m_rtcScreenShareAudioInput
                    ? static_cast<int>(m_rtcScreenShareAudioInput->error()) : -1);
        m_rtcScreenShareAudioDiagnosticBytes = 0;
        m_rtcScreenShareAudioDiagnosticFrames = 0;
        m_rtcScreenShareAudioDiagnosticSquares = 0.0L;
        m_rtcScreenShareAudioDiagnosticSamples = 0;
        m_rtcScreenShareAudioDiagnosticsTimer.restart();
    }
}


void StorageWorker::releaseRtcScreenShareMirConnection(const QString &roomId)
{
    Q_UNUSED(roomId);

    // DIAGNOSTIC ONLY: retain the entire Mir session (connection, screencast,
    // and buffer stream) until Astro exits. Do not call either
    // mir_screencast_release_sync() or mir_connection_release() here.
    //
    // The earlier successful stop experiment skipped screencast destruction,
    // but the current implementation subsequently began closing the dedicated
    // connection asynchronously. This test removes both teardown operations
    // to determine whether Mir session destruction is what stalls presentation.
    //
    // This intentionally leaks the Mir session. Test one screen-share
    // start/stop per app launch, then restart Astro before testing again.
    if (m_rtcScreenShareConnection || m_rtcScreenCast) {
        qWarning() << "ASTRO_SCREEN_SHARE_STOP: DIAGNOSTIC - retaining Mir session"
                   << "connection:" << static_cast<const void *>(m_rtcScreenShareConnection)
                   << "screencast:" << static_cast<const void *>(m_rtcScreenCast)
                   << "until process exit; do not repeat screen-share in this run";
    }
}

void StorageWorker::stopRtcScreenShareCapture(const QString &releaseRoomId)
{
    // Also invalidate the startup timeout and any already-queued frame tick.
    ++m_rtcScreenShareGeneration;
    qInfo() << "ASTRO_SCREEN_SHARE_STOP: invalidated capture generation"
            << "generation:" << m_rtcScreenShareGeneration;

    if (m_rtcScreenShareAudioInput) {
        m_rtcScreenShareAudioInput->stop();
        m_rtcScreenShareAudioInput->deleteLater();
        m_rtcScreenShareAudioInput = nullptr;
    }
    m_rtcScreenShareAudioDevice = nullptr;
    m_rtcScreenShareAudioBuffer.clear();
    m_rtcScreenShareAudioRoomId.clear();

    if (m_rtcScreenShareTimer) {
        m_rtcScreenShareTimer->stop();
        m_rtcScreenShareTimer->deleteLater();
        m_rtcScreenShareTimer = nullptr;
        qInfo() << "ASTRO_SCREEN_SHARE_STOP: capture timer stopped";
    }
    const QString roomId = !releaseRoomId.isEmpty()
        ? releaseRoomId : m_rtcScreenShareRoomId;
    releaseRtcScreenShareMirConnection(roomId);
    m_rtcScreenShareRoomId.clear();
    m_rtcScreenShareWidth = 0;
    m_rtcScreenShareHeight = 0;
    m_rtcScreenShareLastFrame.clear();
    m_rtcScreenShareLastFrameSent.invalidate();
    m_rtcScreenShareStartPending = false;
}

void StorageWorker::stopRtcAudio(const QString &roomId)
{
    // Ending the call supersedes any queued camera start or device switch.
    m_rtcStartVideoAfterBackgroundUnload = false;
    m_rtcStartVideoAfterBackgroundUnloadRoomId.clear();
    m_rtcStartVideoAfterBackgroundUnloadWidth = 0;
    m_rtcStartVideoAfterBackgroundUnloadHeight = 0;
    m_rtcSwitchCameraAfterBackgroundUnload = false;
    m_rtcSwitchCameraAfterBackgroundUnloadRoomId.clear();
    m_rtcSwitchCameraAfterBackgroundUnloadDeviceId.clear();

    // A call can be ended while the background QCamera is still unloading
    // after the app returns to the foreground. Defer the audio teardown until
    // QCamera reports UnloadedStatus, just as we do for video teardown.
    if (m_rtcCamera &&
        (roomId.isEmpty() || roomId == m_rtcVideoRoomId)) {
        m_rtcStopAudioAfterBackgroundUnload = true;
        m_rtcStopAudioAfterBackgroundUnloadRoomId = roomId;
        stopRtcBackgroundCamera(roomId);
        return;
    }

    if ((m_rtcScreenShareTimer &&
         m_rtcScreenShareRoomId == roomId) ||
        (m_rtcScreenShareAudioInput &&
         m_rtcScreenShareAudioRoomId == roomId)) {
        stopRtcScreenShareCapture();
    }
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

    if (result >= 0 && errorMessage.isEmpty()) {
        // Drop buffered samples from the call that just ended so they cannot
        // leak into the next call's playback.
        m_rtcRemoteAudioPending.clear();
        if (m_rtcRemoteAudioOutput)
            m_rtcRemoteAudioOutput->reset();

        const QJsonObject disconnectedEvent{
            { QStringLiteral("room_id"), roomId },
            { QStringLiteral("type"), QStringLiteral("disconnected") }
        };
        const QJsonObject payload{
            { QStringLiteral("events"), QJsonArray{disconnectedEvent} }
        };
        emit matrixRtcAudioEventsUpdated(
            QString::fromUtf8(
                QJsonDocument(payload).toJson(QJsonDocument::Compact)
            )
        );
    }

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
    QMutexLocker frameLocker(&m_rtcCameraFrameMutex);
    ++m_rtcCameraGeneration;

    delete m_rtcCameraVideoProbe;
    m_rtcCameraVideoProbe = nullptr;

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
    m_rtcVideoRoomId.clear();

    if (m_rtcCamera) {
        delete m_rtcCamera;
        m_rtcCamera = nullptr;
    }

    releaseRtcCameraOpenGLContext();
}
