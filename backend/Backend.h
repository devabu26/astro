
#pragma once

#include <QObject>
#include <QString>

class QThread;
class StorageWorker;

class Backend : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool storageReady READ storageReady NOTIFY storageReadyChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    Q_INVOKABLE QString status() const;
    Q_INVOKABLE QString prepareDataDirectory() const;
    Q_INVOKABLE bool hasExistingStorage() const;
    Q_INVOKABLE QString secretServiceStatus() const;

    Q_INVOKABLE void testSecretService();

    Q_INVOKABLE void openStorage(const QString &passphrase);
    Q_INVOKABLE void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );
    Q_INVOKABLE void logoutMatrix();
    Q_INVOKABLE void closeStorage();

    bool storageReady() const;

signals:
    void secretServiceTestFinished(const QString &result);

    void storageReadyChanged();
    void storageStatusChanged(const QString &status);
    void matrixSessionRestored(bool restored);
    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();

    void requestOpenStorage(const QString &passphrase);
    void requestLoginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );
    void requestLogoutMatrix();
    void requestCloseStorage();

private:
    bool m_storageReady = false;
    bool m_storageOperationInProgress = false;

    QThread *m_storageThread = nullptr;
    StorageWorker *m_storageWorker = nullptr;

    void setStorageReady(bool ready);
};
