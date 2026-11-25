#pragma once

#include "Backend.h"

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QMetaObject>
#include <QQuickPaintedItem>
#include <QString>

class QPainter;

class RtcVideoItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(Backend *backend READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId)
    Q_PROPERTY(QString identity READ identity WRITE setIdentity)
    Q_PROPERTY(QString source READ source WRITE setSource)

public:
    explicit RtcVideoItem(QQuickItem *parent = nullptr);

    bool hasFrame() const;
    Backend *backend() const { return m_backend; }
    QString roomId() const { return m_roomId; }
    QString identity() const { return m_identity; }
    QString source() const { return m_source; }

    void setBackend(Backend *backend);
    void setRoomId(const QString &roomId);
    void setIdentity(const QString &identity);
    void setSource(const QString &source);

    Q_INVOKABLE void setI420Frame(
        const QByteArray &data,
        int width,
        int height
    );
    Q_INVOKABLE void clearFrame();

    void paint(QPainter *painter) override;

signals:
    void frameChanged();
    void backendChanged();

private slots:
    void receiveRemoteFrame(
        const QString &roomId,
        const QString &identity,
        const QString &source,
        int width,
        int height,
        const QByteArray &i420
    );

private:
    Backend *m_backend = nullptr;
    QString m_roomId;
    QString m_identity;
    QString m_source = QStringLiteral("camera");
    QMetaObject::Connection m_frameConnection;
    mutable QMutex m_imageMutex;
    QImage m_image;
    bool m_loggedFirstRemoteFrame = false;
};
