#include "NativeAalCamera.h"

#include <QDebug>
#include <QMutexLocker>
#include <QSize>
#include <algorithm>
#include <cstring>

namespace {
constexpr int AalPreviewCallbackEnabled = 1;
constexpr int AalPreviewCallbackDisabled = 0;
constexpr int AalPixelFormatYuv420Sp = 1;
}

NativeAalCamera::NativeAalCamera(QObject *parent)
    : QObject(parent),
      m_library(QStringLiteral("camera_compat_layer"))
{
}

NativeAalCamera::~NativeAalCamera()
{
    stop();
}

bool NativeAalCamera::resolveApi(QString *error)
{
    if (!m_library.isLoaded() && !m_library.load()) {
        if (error)
            *error = QStringLiteral("Could not load libcamera_compat_layer: %1")
                         .arg(m_library.errorString());
        return false;
    }

#define RESOLVE(member, symbol) \
    member = reinterpret_cast<decltype(member)>(m_library.resolve(symbol)); \
    if (!member) { \
        if (error) *error = QStringLiteral("Camera API symbol is missing: %1").arg(QStringLiteral(symbol)); \
        return false; \
    }

    RESOLVE(m_connectById, "android_camera_connect_by_id")
    RESOLVE(m_disconnect, "android_camera_disconnect")
    RESOLVE(m_deleteControl, "android_camera_delete")
    RESOLVE(m_setCallbackMode, "android_camera_set_preview_callback_mode")
    RESOLVE(m_setPreviewFormat, "android_camera_set_preview_format")
    RESOLVE(m_getPreviewFormat, "android_camera_get_preview_format")
    RESOLVE(m_setPreviewSize, "android_camera_set_preview_size")
    RESOLVE(m_getPreviewSize, "android_camera_get_preview_size")
    RESOLVE(m_enumeratePreviewSizes, "android_camera_enumerate_supported_preview_sizes")
    RESOLVE(m_startPreview, "android_camera_start_preview")
    RESOLVE(m_stopPreview, "android_camera_stop_preview")

#undef RESOLVE
    return true;
}

void NativeAalCamera::previewSizeCallback(void *context, int width, int height)
{
    auto *self = static_cast<NativeAalCamera *>(context);
    if (self && width > 0 && height > 0)
        self->m_supportedSizes.append(QSize(width, height));
}

bool NativeAalCamera::start(int cameraId, QString *error)
{
    stop();
    if (!resolveApi(error))
        return false;

    m_listener = {};
    m_listener.context = this;
    m_listener.onPreviewFrame = &NativeAalCamera::previewFrameCallback;

    m_control = m_connectById(cameraId, &m_listener);
    if (!m_control) {
        if (error)
            *error = QStringLiteral("libhybris could not connect to camera %1").arg(cameraId);
        return false;
    }

    // The preview callback delivers CPU-backed Android camera buffers. This
    // avoids the QtUbuntu plugin's GL-texture-only viewfinder path entirely.
    m_enumeratePreviewSizes(m_control, &NativeAalCamera::previewSizeCallback, this);
    QSize selectedSize;
    for (const QSize &size : m_supportedSizes) {
        if (size.width() <= 640 && size.height() <= 480 &&
            (selectedSize.isEmpty() ||
             qint64(size.width()) * size.height() >
                 qint64(selectedSize.width()) * selectedSize.height())) {
            selectedSize = size;
        }
    }
    if (selectedSize.isEmpty()) {
        for (const QSize &size : m_supportedSizes) {
            if (size.width() <= 1280 && size.height() <= 720 &&
                (selectedSize.isEmpty() ||
                 qint64(size.width()) * size.height() >
                     qint64(selectedSize.width()) * selectedSize.height())) {
                selectedSize = size;
            }
        }
    }
    if (!selectedSize.isEmpty())
        m_setPreviewSize(m_control, selectedSize.width(), selectedSize.height());

    m_setPreviewFormat(m_control, AalPixelFormatYuv420Sp);
    int pixelFormat = -1;
    m_getPreviewFormat(m_control, &pixelFormat);
    if (pixelFormat != AalPixelFormatYuv420Sp) {
        if (error)
            *error = QStringLiteral("Camera did not accept YUV420SP preview format (reported %1)")
                         .arg(pixelFormat);
        stop();
        return false;
    }

    m_getPreviewSize(m_control, &m_width, &m_height);
    if (m_width < 2 || m_height < 2 ||
        (m_width & 1) || (m_height & 1)) {
        if (error)
            *error = QStringLiteral("Camera returned an invalid preview size: %1x%2")
                         .arg(m_width).arg(m_height);
        stop();
        return false;
    }

    if (m_setCallbackMode(m_control, AalPreviewCallbackEnabled) != 0) {
        if (error)
            *error = QStringLiteral("Could not enable native camera preview callbacks");
        stop();
        return false;
    }

    m_frameCount.store(0);
    m_running.store(true);
    m_startPreview(m_control);

    qInfo() << "ASTRO_RTC_AAL: native camera preview started"
            << "cameraId:" << cameraId
            << "size:" << m_width << "x" << m_height
            << "format: YUV420SP"
            << "callback: enabled";
    return true;
}

void NativeAalCamera::stop()
{
    QMutexLocker locker(&m_callbackMutex);
    m_running.store(false);

    if (m_control) {
        if (m_setCallbackMode)
            m_setCallbackMode(m_control, AalPreviewCallbackDisabled);
        if (m_stopPreview)
            m_stopPreview(m_control);
        if (m_disconnect)
            m_disconnect(m_control);
        if (m_deleteControl)
            m_deleteControl(m_control);
        m_control = nullptr;
    }

    m_width = 0;
    m_height = 0;
    m_supportedSizes.clear();
    if (m_library.isLoaded())
        m_library.unload();
}

void NativeAalCamera::previewFrameCallback(
    void *data, uint32_t size, void *context
)
{
    auto *self = static_cast<NativeAalCamera *>(context);
    if (self)
        self->receivePreviewFrame(data, size);
}

QImage NativeAalCamera::convertYuv420Sp(
    const uint8_t *data, uint32_t size
) const
{
    const qint64 ySize = qint64(m_width) * m_height;
    const qint64 required = ySize + ySize / 2;
    if (!data || required <= 0 || required > size)
        return {};

    QImage image(m_width, m_height, QImage::Format_RGB32);
    if (image.isNull())
        return {};

    const uint8_t *yPlane = data;
    const uint8_t *vuPlane = data + ySize;
    for (int y = 0; y < m_height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int yRow = y * m_width;
        const int chromaRow = (y / 2) * m_width;
        for (int x = 0; x < m_width; ++x) {
            const int yy = qMax(0, int(yPlane[yRow + x]) - 16);
            const int chroma = chromaRow + (x & ~1);
            const int v = int(vuPlane[chroma]) - 128;
            const int u = int(vuPlane[chroma + 1]) - 128;
            const int c = 298 * yy;
            line[x] = qRgb(
                qBound(0, (c + 409 * v + 128) >> 8, 255),
                qBound(0, (c - 100 * u - 208 * v + 128) >> 8, 255),
                qBound(0, (c + 516 * u + 128) >> 8, 255)
            );
        }
    }
    return image;
}

void NativeAalCamera::receivePreviewFrame(const void *data, uint32_t size)
{
    QMutexLocker locker(&m_callbackMutex);
    if (!m_running.load() || !data)
        return;

    QImage image = convertYuv420Sp(
        static_cast<const uint8_t *>(data), size
    );
    if (image.isNull())
        return;

    const unsigned count = m_frameCount.fetch_add(1) + 1;
    if (count <= 3 || count % 30 == 0) {
        const QColor centre = image.pixelColor(image.width() / 2, image.height() / 2);
        qInfo() << "ASTRO_RTC_AAL: CPU preview frame"
                << "frames:" << count
                << "size:" << image.size()
                << "centre RGB:" << centre.red() << centre.green() << centre.blue();
    }
    emit frameReceived(image);
}
