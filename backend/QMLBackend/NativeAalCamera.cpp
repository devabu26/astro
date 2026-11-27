#include "NativeAalCamera.h"

#include <QDebug>
#include <QColor>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLFunctions>
#include <QSize>
#include <QSurfaceFormat>
#include <QTimer>
#include <QThread>
#include <QTransform>
#include <algorithm>
#include <cstring>

namespace {
constexpr int AalPreviewCallbackEnabled = 1;
constexpr int AalPreviewCallbackDisabled = 0;
constexpr int AalPixelFormatYuv420Sp = 1;
constexpr unsigned int GlTextureExternalOes = 0x8D65;
}

NativeAalCamera::NativeAalCamera(QObject *parent)
    : QObject(parent),
      m_library(QStringLiteral("/lib/aarch64-linux-gnu/libcamera.so.1"))
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
    RESOLVE(m_setPreviewSize, "android_camera_set_preview_size")
    RESOLVE(m_getPreviewSize, "android_camera_get_preview_size")
    RESOLVE(m_enumeratePreviewSizes, "android_camera_enumerate_supported_preview_sizes")
    RESOLVE(m_setPreviewTexture, "android_camera_set_preview_texture")
    RESOLVE(m_updatePreviewTexture, "android_camera_update_preview_texture")
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

QStringList NativeAalCamera::supportedPreviewResolutions(int cameraId)
{
    if (!isRunning()) {
        QString error;
        if (!resolveApi(&error))
            return {};

        m_supportedSizes.clear();
        Listener listener{};
        void *control = m_connectById(cameraId, &listener);
        if (!control)
            return {};

        m_enumeratePreviewSizes(
            control, &NativeAalCamera::previewSizeCallback, this
        );
        m_disconnect(control);
        m_deleteControl(control);
    }

    QList<QSize> portraitSizes;
    for (const QSize &nativeSize : m_supportedSizes) {
        if (nativeSize.width() < 2 || nativeSize.height() < 2 ||
            (nativeSize.width() & 1) || (nativeSize.height() & 1))
            continue;
        const QSize displaySize = nativeSize.width() > nativeSize.height()
            ? QSize(nativeSize.height(), nativeSize.width())
            : nativeSize;
        if (!portraitSizes.contains(displaySize))
            portraitSizes.append(displaySize);
    }
    std::sort(portraitSizes.begin(), portraitSizes.end(),
              [](const QSize &a, const QSize &b) {
                  return qint64(a.width()) * a.height() <
                         qint64(b.width()) * b.height();
              });

    QStringList result;
    for (const QSize &size : portraitSizes)
        result.append(QStringLiteral("%1x%2").arg(size.width()).arg(size.height()));
    return result;
}

bool NativeAalCamera::initialisePreviewTexture(QString *error)
{
    // libhybris requires update_preview_texture() to run on the same thread
    // that owns the EGL context used to create the external-OES texture.
    // Keep that context on this QObject's thread; texture callbacks from the
    // camera service are marshalled here with a queued invocation.
    m_previewSurface = new QOffscreenSurface;
    m_previewSurface->setFormat(QSurfaceFormat::defaultFormat());
    m_previewSurface->create();
    if (!m_previewSurface->isValid()) {
        if (error)
            *error = QStringLiteral("Could not create the native camera offscreen GL surface");
        destroyPreviewTexture();
        return false;
    }

    m_previewContext = new QOpenGLContext;
    m_previewContext->setFormat(m_previewSurface->format());
    if (!m_previewContext->create() ||
        !m_previewContext->makeCurrent(m_previewSurface)) {
        if (error)
            *error = QStringLiteral("Could not create or activate the native camera GL context");
        destroyPreviewTexture();
        return false;
    }

    QOpenGLFunctions *gl = m_previewContext->functions();
    gl->initializeOpenGLFunctions();
    gl->glGenTextures(1, &m_previewTexture);
    gl->glBindTexture(GlTextureExternalOes, m_previewTexture);
    gl->glTexParameteri(GlTextureExternalOes, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GlTextureExternalOes, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GlTextureExternalOes, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GlTextureExternalOes, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GlTextureExternalOes, 0);

    const GLenum glError = gl->glGetError();
    if (!m_previewTexture || glError != GL_NO_ERROR) {
        if (error)
            *error = QStringLiteral("Could not allocate the native camera external texture (GL error 0x%1)")
                         .arg(glError, 0, 16);
        m_previewContext->doneCurrent();
        destroyPreviewTexture();
        return false;
    }

    m_setPreviewTexture(m_control, static_cast<int>(m_previewTexture));
    m_previewContext->doneCurrent();

    qInfo() << "ASTRO_RTC_AAL: native preview texture configured"
            << "texture:" << m_previewTexture
            << "contextThread:" << QThread::currentThread();
    return true;
}

void NativeAalCamera::destroyPreviewTexture()
{
    if (m_previewContext && m_previewSurface &&
        m_previewContext->makeCurrent(m_previewSurface)) {
        if (m_previewTexture) {
            QOpenGLFunctions *gl = m_previewContext->functions();
            gl->initializeOpenGLFunctions();
            gl->glDeleteTextures(1, &m_previewTexture);
        }
        m_previewContext->doneCurrent();
    }

    m_previewTexture = 0;
    delete m_previewContext;
    m_previewContext = nullptr;
    delete m_previewSurface;
    m_previewSurface = nullptr;
}

bool NativeAalCamera::start(int cameraId, int sensorOrientation,
                            int outputWidth, int outputHeight,
                            const QString &roomId, QString *error)
{
    stop();
    if (!resolveApi(error))
        return false;

    if (roomId.isEmpty()) {
        if (error)
            *error = QStringLiteral("Room ID is empty");
        return false;
    }

    m_roomId = roomId;
    m_sensorOrientation = ((sensorOrientation % 360) + 360) % 360;
    m_previewUpdatePending.store(false);
    m_listener = {};
    m_listener.context = this;
    m_listener.onPreviewTexture = &NativeAalCamera::previewTextureCallback;
    m_listener.onPreviewFrame = &NativeAalCamera::previewFrameCallback;

    m_control = m_connectById(cameraId, &m_listener);
    if (!m_control) {
        if (error)
            *error = QStringLiteral("libhybris could not connect to camera %1").arg(cameraId);
        return false;
    }

    if (!initialisePreviewTexture(error)) {
        stop();
        return false;
    }

    // The CPU callback supplies frames for LiveKit. The external-OES preview
    // target is also required by the Android camera stack to start its preview.
    m_enumeratePreviewSizes(m_control, &NativeAalCamera::previewSizeCallback, this);
    QSize selectedSize;
    if (outputWidth > 0 && outputHeight > 0) {
        // The advertised call resolutions are portrait-oriented; camera HAL
        // preview sizes are commonly landscape-oriented.
        const QSize requestedNative(outputHeight, outputWidth);
        for (const QSize &size : m_supportedSizes) {
            if (size == requestedNative || size == QSize(outputWidth, outputHeight)) {
                selectedSize = size;
                break;
            }
        }
    }
    if (selectedSize.isEmpty()) {
        for (const QSize &size : m_supportedSizes) {
            if (size.width() <= 640 && size.height() <= 480 &&
                (selectedSize.isEmpty() ||
                 qint64(size.width()) * size.height() >
                     qint64(selectedSize.width()) * selectedSize.height())) {
                selectedSize = size;
            }
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
    // Validate the negotiated CPU buffer layout when the first callback arrives.
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
    m_invalidBufferLogged.store(0);
    m_running.store(true);
    m_startPreview(m_control);

    qInfo() << "ASTRO_RTC_AAL: native camera preview started"
            << "cameraId:" << cameraId
            << "size:" << m_width << "x" << m_height
            << "format: YUV420SP"
            << "sensorOrientation:" << m_sensorOrientation
            << "frontFacing:" << m_frontFacing
            << "texture:" << m_previewTexture
            << "callback: enabled";

    QTimer::singleShot(3000, this, [this, roomId, cameraId]() {
        if (m_running.load() && m_roomId == roomId &&
            m_frameCount.load() == 0) {
            qWarning() << "ASTRO_RTC_AAL: no CPU preview callbacks received"
                       << "after 3 seconds"
                       << "cameraId:" << cameraId
                       << "size:" << m_width << "x" << m_height;
        }
    });
    return true;
}

void NativeAalCamera::stop()
{
    m_running.store(false);
    m_previewUpdatePending.store(false);

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

    // Wait for a CPU callback already in flight before releasing the GL
    // texture/context and clearing callback-owned state.
    QMutexLocker locker(&m_callbackMutex);
    m_roomId.clear();
    m_width = 0;
    m_height = 0;
    m_supportedSizes.clear();
    locker.unlock();
    destroyPreviewTexture();
}

void NativeAalCamera::previewTextureCallback(void *context)
{
    auto *self = static_cast<NativeAalCamera *>(context);
    if (!self || !self->m_running.load())
        return;

    // libhybris may invoke this callback on any camera-service thread. Its
    // API explicitly requires update_preview_texture() on the GL-context
    // setup thread, so never make the context current in this callback.
    bool expected = false;
    if (!self->m_previewUpdatePending.compare_exchange_strong(expected, true))
        return;

    if (!QMetaObject::invokeMethod(
            self, "updatePreviewTexture", Qt::QueuedConnection)) {
        self->m_previewUpdatePending.store(false);
    }
}

void NativeAalCamera::updatePreviewTexture()
{
    if (!m_running.load() || !m_control ||
        !m_previewContext || !m_previewSurface) {
        m_previewUpdatePending.store(false);
        return;
    }

    if (!m_previewContext->makeCurrent(m_previewSurface)) {
        qWarning() << "ASTRO_RTC_AAL: could not activate preview GL context for texture update";
        m_previewUpdatePending.store(false);
        return;
    }

    m_updatePreviewTexture(m_control);
    m_previewContext->doneCurrent();
    m_previewUpdatePending.store(false);
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
    if (!data || m_width < 2 || m_height < 2)
        return {};

    const qint64 tightYSize = qint64(m_width) * m_height;
    const qint64 tightRequired = tightYSize + tightYSize / 2;
    int rowStride = m_width;

    // Android camera HALs may align YUV rows. If the callback size indicates
    // a uniform padded stride, account for it instead of interpreting padding
    // as pixels. The normal tightly packed NV21/YUV420SP case stays unchanged.
    if (qint64(size) != tightRequired) {
        const qint64 inferredStride =
            (qint64(size) * 2) / (qint64(m_height) * 3);
        if (inferredStride >= m_width &&
            inferredStride <= m_width + 256 &&
            (inferredStride & 1) == 0 &&
            inferredStride * m_height * 3 / 2 <= size) {
            rowStride = static_cast<int>(inferredStride);
        } else {
            return {};
        }
    }

    const qint64 ySize = qint64(rowStride) * m_height;
    const qint64 required = ySize + ySize / 2;
    if (required > size)
        return {};

    QImage image(m_width, m_height, QImage::Format_RGB32);
    if (image.isNull())
        return {};

    const uint8_t *yPlane = data;
    const uint8_t *vuPlane = data + ySize;
    for (int y = 0; y < m_height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int yRow = y * rowStride;
        const int chromaRow = (y / 2) * rowStride;
        for (int x = 0; x < m_width; ++x) {
            const int yy = qMax(0, int(yPlane[yRow + x]) - 16);
            const int chroma = chromaRow + (x & ~1);
            // YUV420SP preview callback data is NV21: V then U.
            // The ordering is the same for front- and rear-facing cameras.
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
    // Android camera preview callbacks contain sensor-oriented bytes; the
    // legacy display-orientation API does not rotate callback buffers. The
    // downstream I420 path rotates landscape frames clockwise by 90 degrees
    // to match Astro's portrait track, so compensate relative to that fixed
    // rotation using the selected camera's sensor orientation.
    const int additionalRotation = (m_sensorOrientation - 90 + 360) % 360;
    if (additionalRotation != 0) {
        image = image.transformed(
            QTransform().rotate(additionalRotation),
            Qt::FastTransformation
        );
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
    if (image.isNull()) {
        int expected = 0;
        if (m_invalidBufferLogged.compare_exchange_strong(expected, 1)) {
            qWarning() << "ASTRO_RTC_AAL: rejected native YUV preview buffer"
                       << "bytes:" << size
                       << "expectedTightBytes:"
                       << (qint64(m_width) * m_height * 3 / 2)
                       << "size:" << m_width << "x" << m_height;
        }
        return;
    }

    const unsigned count = m_frameCount.fetch_add(1) + 1;
    if (count <= 3 || count % 30 == 0) {
        const QColor centre = image.pixelColor(image.width() / 2, image.height() / 2);
        qInfo() << "ASTRO_RTC_AAL: CPU preview frame"
                << "frames:" << count
                << "size:" << image.size()
                << "centre RGB:" << centre.red() << centre.green() << centre.blue();
    }
    emit frameReceived(m_roomId, image);
}
