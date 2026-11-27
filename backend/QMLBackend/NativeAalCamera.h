#pragma once

#include <QObject>
#include <QImage>
#include <QLibrary>
#include <QMutex>
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>

struct AalCameraControlListener;
class NativeAalCamera final : public QObject
{
    Q_OBJECT

public:
    explicit NativeAalCamera(QObject *parent = nullptr);
    ~NativeAalCamera() override;

    bool start(int cameraId, QString *error);
    void stop();
    bool isRunning() const { return m_running.load(); }

signals:
    void frameReceived(const QImage &image);

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
    using GetPreviewFormat = void (*)(void *, int *);
    using SetPreviewSize = void (*)(void *, int, int);
    using GetPreviewSize = void (*)(void *, int *, int *);
    using EnumeratePreviewSizes = void (*)(void *, void (*)(void *, int, int), void *);
    using StartPreview = void (*)(void *);
    using StopPreview = void (*)(void *);

    static void previewFrameCallback(void *data, uint32_t size, void *context);
    static void previewSizeCallback(void *context, int width, int height);
    void receivePreviewFrame(const void *data, uint32_t size);
    bool resolveApi(QString *error);
    QImage convertYuv420Sp(const uint8_t *data, uint32_t size) const;

    QLibrary m_library;
    Listener m_listener;
    void *m_control = nullptr;
    ConnectById m_connectById = nullptr;
    Disconnect m_disconnect = nullptr;
    DeleteControl m_deleteControl = nullptr;
    SetCallbackMode m_setCallbackMode = nullptr;
    SetPreviewFormat m_setPreviewFormat = nullptr;
    GetPreviewFormat m_getPreviewFormat = nullptr;
    SetPreviewSize m_setPreviewSize = nullptr;
    GetPreviewSize m_getPreviewSize = nullptr;
    EnumeratePreviewSizes m_enumeratePreviewSizes = nullptr;
    StartPreview m_startPreview = nullptr;
    StopPreview m_stopPreview = nullptr;
    QVector<QSize> m_supportedSizes;
    int m_width = 0;
    int m_height = 0;
    std::atomic_bool m_running{false};
    QMutex m_callbackMutex;
    std::atomic_uint m_frameCount{0};
};
