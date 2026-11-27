#pragma once

#include <QObject>
#include <QImage>
#include <QLibrary>
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QVector>
#include <QSize>
#include <atomic>
#include <cstdint>

struct AalCameraControlListener;
class NativeAalCamera final : public QObject
{
    Q_OBJECT

public:
    explicit NativeAalCamera(QObject *parent = nullptr);
    ~NativeAalCamera() override;

    bool start(int cameraId, int sensorOrientation, bool frontFacing,
               const QString &roomId, QString *error);
    void stop();
    bool isRunning() const { return m_running.load(); }

signals:
    void frameReceived(const QString &roomId, const QImage &image);

private:
    using MessageCallback = void (*)(void *);
    using ZoomCallback = void (*)(void *, int32_t);
    using DataCallback = void (*)(void *, uint32_t, void *);
    using PreviewFrameCallback = void (*)(void *, uint32_t, void *);

    struct Listener {
        MessageCallback onError = nullptr;
        MessageCallback onShutter = nullptr;
        MessageCallback onFocus = nullptr;
        ZoomCallback onZoom = nullptr;
        DataCallback onRawImage = nullptr;
        DataCallback onCompressedImage = nullptr;
        MessageCallback onPreviewTexture = nullptr;
        void *context = nullptr;
        PreviewFrameCallback onPreviewFrame = nullptr;
    };

    using ConnectById = void *(*)(int32_t, Listener *);
    using Disconnect = void (*)(void *);
    using DeleteControl = void (*)(void *);
    using SetCallbackMode = int (*)(void *, int);
    using SetPreviewFormat = void (*)(void *, int);
    using SetPreviewSize = void (*)(void *, int, int);
    using GetPreviewSize = void (*)(void *, int *, int *);
    using EnumeratePreviewSizes = void (*)(void *, void (*)(void *, int, int), void *);
    using SetPreviewTexture = void (*)(void *, int);
    using UpdatePreviewTexture = void (*)(void *);
    using StartPreview = void (*)(void *);
    using StopPreview = void (*)(void *);

    static void previewFrameCallback(void *data, uint32_t size, void *context);
    static void previewTextureCallback(void *context);
    static void previewSizeCallback(void *context, int width, int height);
    void receivePreviewFrame(const void *data, uint32_t size);
    bool initialisePreviewTexture(QString *error);
    void destroyPreviewTexture();
    Q_INVOKABLE void updatePreviewTexture();
    bool resolveApi(QString *error);
    QImage convertYuv420Sp(const uint8_t *data, uint32_t size) const;

    QLibrary m_library;
    QOffscreenSurface *m_previewSurface = nullptr;
    QOpenGLContext *m_previewContext = nullptr;
    unsigned int m_previewTexture = 0;
    Listener m_listener;
    void *m_control = nullptr;
    QString m_roomId;
    ConnectById m_connectById = nullptr;
    Disconnect m_disconnect = nullptr;
    DeleteControl m_deleteControl = nullptr;
    SetCallbackMode m_setCallbackMode = nullptr;
    SetPreviewFormat m_setPreviewFormat = nullptr;
    SetPreviewSize m_setPreviewSize = nullptr;
    GetPreviewSize m_getPreviewSize = nullptr;
    EnumeratePreviewSizes m_enumeratePreviewSizes = nullptr;
    SetPreviewTexture m_setPreviewTexture = nullptr;
    UpdatePreviewTexture m_updatePreviewTexture = nullptr;
    StartPreview m_startPreview = nullptr;
    StopPreview m_stopPreview = nullptr;
    QVector<QSize> m_supportedSizes;
    int m_width = 0;
    int m_height = 0;
    std::atomic_bool m_running{false};
    std::atomic_bool m_previewUpdatePending{false};
    int m_sensorOrientation = 0;
    bool m_frontFacing = false;
    QMutex m_callbackMutex;
    std::atomic_uint m_frameCount{0};
    std::atomic_int m_invalidBufferLogged{0};
};
