

#include "Backend.h"
#include "StorageWorker.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QUuid>
#include <QMap>
#include <QVariantMap>
#include <QList>
#include <QThread>
#include <QMetaObject>
#include <functional>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusArgument>
#include <QDBusMetaType>

// =============================================================
// Secret Service data structure
// D-Bus signature: (oayays)
// =============================================================

struct SecretServiceSecret
{
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

Q_DECLARE_METATYPE(SecretServiceSecret)

// Serialize SecretServiceSecret for D-Bus.
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const SecretServiceSecret &secret
)
{
    argument.beginStructure();

    argument << secret.session
             << secret.parameters
             << secret.value
             << secret.contentType;

    argument.endStructure();

    return argument;
}

// Deserialize SecretServiceSecret from D-Bus.
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    SecretServiceSecret &secret
)
{
    argument.beginStructure();

    argument >> secret.session
             >> secret.parameters
             >> secret.value
             >> secret.contentType;

    argument.endStructure();

    return argument;
}

// =============================================================
// Backend construction and worker-thread setup
// =============================================================

Backend::Backend(QObject *parent)
    : QObject(parent)
{
    QCoreApplication::setApplicationName(
        QStringLiteral("astro.madabu")
    );

    // ---------------------------------------------------------
    // Create the dedicated storage thread and worker.
    // ---------------------------------------------------------

    m_storageThread = new QThread(this);
    m_storageWorker = new StorageWorker();

    m_storageWorker->moveToThread(m_storageThread);

    connect(
        m_storageThread,
        &QThread::finished,
        m_storageWorker,
        &QObject::deleteLater
    );

    // ---------------------------------------------------------
    // Encrypted storage request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestOpenStorage,
        m_storageWorker,
        &StorageWorker::openStorage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestCloseStorage,
        m_storageWorker,
        &StorageWorker::closeStorage,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix account request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestLoginMatrix,
        m_storageWorker,
        &StorageWorker::loginMatrix,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestLogoutMatrix,
        m_storageWorker,
        &StorageWorker::logoutMatrix,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix room and Space request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestFetchMatrixRooms,
        m_storageWorker,
        &StorageWorker::fetchMatrixRooms,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    connect(
       this,
       &Backend::requestSyncMatrixRooms,
       m_storageWorker,
       &StorageWorker::syncMatrixRooms,
       Qt::QueuedConnection
   );

   // Matrix room timeline request connection.
   connect(
        m_storageWorker,
        &StorageWorker::matrixLiveTimelineUpdated,
        this,
        &Backend::matrixLiveTimelineUpdated
   );

   connect(
       this,
       &Backend::requestFetchRoomTimeline,
       m_storageWorker,
       &StorageWorker::fetchRoomTimeline,
       Qt::QueuedConnection
   );

   connect(
        this,
        &Backend::requestPaginateRoomTimeline,
        m_storageWorker,
        &StorageWorker::paginateRoomTimeline,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestMarkRoomAsRead,
        m_storageWorker,
        &StorageWorker::markRoomAsRead,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSendRoomMessage,
        m_storageWorker,
        &StorageWorker::sendRoomMessage,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix device verification request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestAcceptVerificationRequest,
        m_storageWorker,
        &StorageWorker::acceptVerificationRequest,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestCancelVerificationRequest,
        m_storageWorker,
        &StorageWorker::cancelVerificationRequest,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestConfirmVerification,
        m_storageWorker,
        &StorageWorker::confirmVerification,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestMismatchVerification,
        m_storageWorker,
        &StorageWorker::mismatchVerification,
        Qt::QueuedConnection
    );

    // Forward verification action results back to QML.
    connect(
        m_storageWorker,
        &StorageWorker::matrixVerificationActionFinished,
        this,
        &Backend::matrixVerificationActionFinished,
        Qt::QueuedConnection
    );


   // Forward room timeline results to QML.
   connect(
       m_storageWorker,
       &StorageWorker::matrixTimelineUpdated,
       this,
       [this](
           const QString &roomId,
           const QString &json,
           const QString &error
       )
       {
           emit matrixTimelineRetrieved(roomId, json, error);
       }
   );

   connect(
        m_storageWorker,
        &StorageWorker::matrixOlderTimelineUpdated,
        this,
        [this](
            const QString &roomId,
            const QString &json,
            const QString &error
        )
        {
            emit matrixOlderTimelineRetrieved(roomId, json, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixMessageSent,
        this,
        [this](
            const QString &roomId,
            const QString &eventId,
            const QString &error
        )
        {
            emit matrixMessageSent(roomId, eventId, error);
        }
    );


   // Matrix media request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestFetchRoomAvatar,
        m_storageWorker,
        &StorageWorker::fetchRoomAvatar,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchSenderAvatar,
        m_storageWorker,
        &StorageWorker::fetchSenderAvatar,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageImage,
        m_storageWorker,
        &StorageWorker::fetchMessageImage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageThumbnail,
        m_storageWorker,
        &StorageWorker::fetchMessageThumbnail,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageVideo,
        m_storageWorker,
        &StorageWorker::fetchMessageVideo,
        Qt::QueuedConnection
    );

    // =========================================================
    // Storage worker response connections
    // =========================================================

    // ---------------------------------------------------------
    // Successful storage opening
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageOpened,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            setStorageReady(true);

            emit storageStatusChanged(
                QStringLiteral(
                    "Encrypted storage opened successfully"
                )
            );
        }
    );

    // ---------------------------------------------------------
    // Successful storage closing
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageClosed,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            setStorageReady(false);

            emit storageStatusChanged(
                QStringLiteral("Encrypted storage closed")
            );
        }
    );

    // ---------------------------------------------------------
    // Storage errors
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageFailed,
        this,
        [this](const QString &error)
        {
            m_storageOperationInProgress = false;

            emit storageStatusChanged(error);
        }
    );

    // ---------------------------------------------------------
    // Matrix session restoration
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::sessionRestored,
        this,
        [this](bool restored)
        {
            emit matrixSessionRestored(restored);

            emit storageStatusChanged(
                restored
                    ? QStringLiteral("Matrix session restored")
                    : QStringLiteral(
                        "Storage unlocked. Please sign in to Matrix."
                    )
            );
        }
    );

    // =========================================================
    // Matrix account response connections
    // =========================================================

    // ---------------------------------------------------------
    // Matrix login
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::matrixLoginSucceeded,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            emit matrixLoginSucceeded();

            emit storageStatusChanged(
                QStringLiteral("Matrix login successful")
            );
        }
    );

    // ---------------------------------------------------------
    // Matrix logout
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::matrixLogoutSucceeded,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            emit matrixLogoutSucceeded();

            emit storageStatusChanged(
                QStringLiteral("Logged out of Matrix")
            );
        }
    );

    // =========================================================
   connect(
        m_storageWorker,
        &StorageWorker::dataDeletionFinished,
        this,
        [this](
            bool deleted,
            bool storageWasUnlocked,
            bool logoutAttempted,
            bool logoutSucceeded,
            const QString &logoutError,
            const QString &deletionError
        )
        {
            m_storageOperationInProgress = false;

            if (deleted) {
                setStorageReady(false);
                emit dataDeleted();
            }

            emit dataDeletionFinished(
                deleted,
                storageWasUnlocked,
                logoutAttempted,
                logoutSucceeded,
                logoutError,
                deletionError
            );
        }
    );

    // Matrix room and Space response connections
    // =========================================================

    connect(
    m_storageWorker,
    &StorageWorker::matrixRoomsUpdated,
   this,
   [this](const QString &json, bool finished)
   {
       // Forward cached or freshly synchronised rooms to QML.
       // QML can queue avatar requests while handling the cached list.
       emit matrixRoomsRetrieved(json, finished);

       // Schedule network sync only after cached rooms reach QML.
       if (!finished) {
           emit requestSyncMatrixRooms();
           return;
       }

       m_storageOperationInProgress = false;

       emit storageStatusChanged(
           QStringLiteral("Matrix rooms retrieved")
       );
   }
);

   // =========================================================
   // Matrix media and room avatar response connections
   // =========================================================

   // ---------------------------------------------------------
   // Successfully retrieved room avatar
   // ---------------------------------------------------------

   connect(
        m_storageWorker,
        &StorageWorker::roomAvatarRetrieved,
        this,
        [this](
            const QString &roomId,
            const QString &dataUrl
        )
        {
            emit roomAvatarRetrieved(roomId, dataUrl);
        }
    );

    // ---------------------------------------------------------
    // Room avatar retrieval failure
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::roomAvatarFailed,
        this,
        [this](
            const QString &roomId,
            const QString &error
        )
        {
            emit roomAvatarFailed(roomId, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::senderAvatarRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit senderAvatarRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::senderAvatarFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit senderAvatarFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit messageImageRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageImageFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageThumbnailRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit messageThumbnailRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageThumbnailFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageThumbnailFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageVideoRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &fileUrl
        )
        {
            emit messageVideoRetrieved(requestKey, fileUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageVideoFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageVideoFailed(requestKey, error);
        }
    );

    // ---------------------------------------------------------
    // Start the worker thread after all connections exist.
    // ---------------------------------------------------------

    m_storageThread->start();

    // ---------------------------------------------------------
    // Report the application data directory.
    // Do not create it here.
    // ---------------------------------------------------------

    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        qWarning()
            << "Astro: Could not determine application data directory";
    } else {
        qInfo()
            << "Astro: Application data directory:"
            << path;
    }
}

// =============================================================
// Backend destruction
// =============================================================

Backend::~Backend()
{
    if (m_storageThread && m_storageThread->isRunning()) {

        // Close Rust-side resources on the worker thread.
        // BlockingQueuedConnection ensures this finishes before
        // the worker thread is stopped.

        if (m_storageWorker) {
            QMetaObject::invokeMethod(
                m_storageWorker,
                "closeStorage",
                Qt::BlockingQueuedConnection
            );
        }

        m_storageThread->quit();
        m_storageThread->wait();
    }
}

// =============================================================
// General application status
// =============================================================

QString Backend::status() const
{
    const char *rustStatus = astro_get_status();

    if (!rustStatus) {
        return QStringLiteral(
            "Astro Rust backend returned no status!"
        );
    }

    return QString::fromUtf8(rustStatus);
}

// =============================================================
// Application data directory
// =============================================================

QString Backend::prepareDataDirectory() const
{
    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        return QString();
    }

    QDir directory;

    if (!directory.mkpath(path)) {
        return QString();
    }

    const QFileInfo info(path);

    if (!info.isDir() || !info.isWritable()) {
        return QString();
    }

    return path;
}

// =============================================================
// Existing encrypted storage detection
// =============================================================

bool Backend::hasExistingStorage() const
{
    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        return false;
    }

    const QDir directory(path);

    if (!directory.exists()) {
        return false;
    }

    return
        QFileInfo::exists(
            directory.filePath("matrix-sdk-state.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-event-cache.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-media.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-crypto.sqlite3")
        );
}

// =============================================================
// Secret Service status diagnostic
// =============================================================

QString Backend::secretServiceStatus() const
{
    const QDBusConnection connection =
        QDBusConnection::sessionBus();

    if (!connection.isConnected()) {
        return QStringLiteral(
            "Session D-Bus: not connected"
        );
    }

    QDBusConnectionInterface *interface =
        connection.interface();

    if (!interface) {
        return QStringLiteral(
            "Session D-Bus: interface unavailable"
        );
    }

    const QDBusReply<bool> reply =
        interface->isServiceRegistered(
            QStringLiteral("org.freedesktop.secrets")
        );

    if (!reply.isValid()) {
        return QStringLiteral(
            "Secret Service check failed: "
        ) + reply.error().message();
    }

    if (reply.value()) {
        return QStringLiteral(
            "Secret Service: accessible"
        );
    }

    return QStringLiteral(
        "Secret Service: not registered"
    );
}

// =============================================================
// Secret Service round-trip diagnostic
// =============================================================

void Backend::testSecretService()
{
    const QDBusConnection connection =
        QDBusConnection::sessionBus();

    if (!connection.isConnected()) {
        emit secretServiceTestFinished(
            QStringLiteral(
                "Keyring test failed: session D-Bus unavailable"
            )
        );

        return;
    }

    qDBusRegisterMetaType<SecretServiceSecret>();

    // Unique diagnostic value.
    // This is not a real credential.

    const QString testId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QByteArray testValue =
        (
            QStringLiteral("Astro-Keyring-Test-") + testId
        ).toUtf8();

    const QString service =
        QStringLiteral("org.freedesktop.secrets");

    const QString servicePath =
        QStringLiteral("/org/freedesktop/secrets");

    const QString serviceInterface =
        QStringLiteral("org.freedesktop.Secret.Service");

    const QString collectionPath =
        QStringLiteral("/org/freedesktop/secrets/collection/login");

    // ---------------------------------------------------------
    // Asynchronous D-Bus helper
    // ---------------------------------------------------------

    // Send an asynchronous D-Bus call and deliver its reply
    // through a callback. Nothing here blocks the UI thread.

    auto callAsync =
        [this, connection, service](
            const QString &path,
            const QString &interfaceName,
            const QString &method,
            const QList<QVariant> &arguments,
            std::function<void(QDBusPendingCallWatcher *)> callback
        )
    {
        QDBusMessage message =
            QDBusMessage::createMethodCall(
                service,
                path,
                interfaceName,
                method
            );

        message.setArguments(arguments);

        auto *watcher =
            new QDBusPendingCallWatcher(
                connection.asyncCall(message),
                this
            );

        connect(
            watcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [watcher, callback]()
            {
                callback(watcher);
                watcher->deleteLater();
            }
        );
    };

    // =========================================================
    // Step 1: Open a Secret Service session
    // =========================================================

    callAsync(
        servicePath,
        serviceInterface,
        QStringLiteral("OpenSession"),
        {
            QStringLiteral("plain"),
            QVariant::fromValue(
                QDBusVariant(QVariant(QString()))
            )
        },
        [this, callAsync, collectionPath, testId, testValue]
        (QDBusPendingCallWatcher *watcher)
        {
            QDBusPendingReply<
                QDBusVariant,
                QDBusObjectPath
            > reply = *watcher;

            if (reply.isError()) {
                emit secretServiceTestFinished(
                    QStringLiteral("Keyring session failed: ")
                    + reply.error().message()
                );

                return;
            }

            const QDBusObjectPath sessionPath =
                reply.argumentAt<1>();

            // =================================================
            // Step 2: Prepare the temporary item
            // =================================================

            QMap<QString, QString> attributes;

            attributes.insert(
                QStringLiteral("application"),
                QStringLiteral("astro.madabu")
            );

            attributes.insert(
                QStringLiteral("purpose"),
                QStringLiteral("round-trip-test")
            );

            attributes.insert(
                QStringLiteral("test-id"),
                testId
            );

            QVariantMap properties;

            properties.insert(
                QStringLiteral(
                    "org.freedesktop.Secret.Item.Label"
                ),
                QStringLiteral("Astro Keyring diagnostic")
            );

            properties.insert(
                QStringLiteral(
                    "org.freedesktop.Secret.Item.Attributes"
                ),
                QVariant::fromValue(attributes)
            );

            SecretServiceSecret secret;

            secret.session = sessionPath;
            secret.parameters = QByteArray();
            secret.value = testValue;
            secret.contentType = QStringLiteral("text/plain");

            // =================================================
            // Step 3: Create the temporary item
            // =================================================

            callAsync(
                collectionPath,
                QStringLiteral(
                    "org.freedesktop.Secret.Collection"
                ),
                QStringLiteral("CreateItem"),
                {
                    QVariant::fromValue(properties),
                    QVariant::fromValue(secret),
                    QVariant(false)
                },
                [this, callAsync, sessionPath, testValue]
                (QDBusPendingCallWatcher *createWatcher)
                {
                    QDBusPendingReply<
                        QDBusObjectPath,
                        QDBusObjectPath
                    > createReply = *createWatcher;

                    if (createReply.isError()) {
                        emit secretServiceTestFinished(
                            QStringLiteral("Keyring store failed: ")
                            + createReply.error().message()
                        );

                        return;
                    }

                    const QDBusObjectPath itemPath =
                        createReply.argumentAt<0>();

                    const QDBusObjectPath promptPath =
                        createReply.argumentAt<1>();

                    if (promptPath.path() != QStringLiteral("/")) {
                        emit secretServiceTestFinished(
                            QStringLiteral(
                                "Keyring requires an interactive prompt; "
                                "test stopped safely"
                            )
                        );

                        return;
                    }

                    if (itemPath.path() == QStringLiteral("/")) {
                        emit secretServiceTestFinished(
                            QStringLiteral(
                                "Keyring did not return a valid item"
                            )
                        );

                        return;
                    }

                    // =========================================
                    // Step 4: Retrieve the stored value
                    // =========================================

                    callAsync(
                        itemPath.path(),
                        QStringLiteral(
                            "org.freedesktop.Secret.Item"
                        ),
                        QStringLiteral("GetSecret"),
                        {
                            QVariant::fromValue(sessionPath)
                        },
                        [this, callAsync, itemPath, testValue]
                        (QDBusPendingCallWatcher *getWatcher)
                        {
                            QDBusPendingReply<SecretServiceSecret>
                                getReply = *getWatcher;

                            const bool retrieved =
                                !getReply.isError()
                                && getReply.value().value == testValue;

                            const QString retrievalError =
                                getReply.isError()
                                ? getReply.error().message()
                                : QString();

                            // =================================
                            // Step 5: Delete the temporary item
                            // =================================

                            callAsync(
                                itemPath.path(),
                                QStringLiteral(
                                    "org.freedesktop.Secret.Item"
                                ),
                                QStringLiteral("Delete"),
                                {},
                                [this, retrieved, retrievalError]
                                (
                                    QDBusPendingCallWatcher *deleteWatcher
                                )
                                {
                                    QDBusPendingReply<QDBusObjectPath>
                                        deleteReply = *deleteWatcher;

                                    if (deleteReply.isError()) {
                                        emit secretServiceTestFinished(
                                            QStringLiteral(
                                                "Keyring cleanup failed: "
                                            )
                                            + deleteReply.error().message()
                                        );

                                        return;
                                    }

                                    if (
                                        deleteReply.value().path()
                                        != QStringLiteral("/")
                                    ) {
                                        emit secretServiceTestFinished(
                                            QStringLiteral(
                                                "Keyring cleanup requires "
                                                "an interactive prompt"
                                            )
                                        );

                                        return;
                                    }

                                    if (!retrievalError.isEmpty()) {
                                        emit secretServiceTestFinished(
                                            QStringLiteral(
                                                "Keyring retrieval failed: "
                                            )
                                            + retrievalError
                                            + QStringLiteral(
                                                ". Temporary item deleted."
                                            )
                                        );

                                        return;
                                    }

                                    if (retrieved) {
                                        emit secretServiceTestFinished(
                                            QStringLiteral(
                                                "Keyring round-trip successful; "
                                                "temporary item deleted."
                                            )
                                        );
                                    } else {
                                        emit secretServiceTestFinished(
                                            QStringLiteral(
                                                "Keyring verification failed; "
                                                "temporary item deleted."
                                            )
                                        );
                                    }
                                }
                            );
                        }
                    );
                }
            );
        }
    );
}

// =============================================================
// Storage state
// =============================================================

bool Backend::storageReady() const
{
    return m_storageReady;
}

void Backend::setStorageReady(bool ready)
{
    if (m_storageReady == ready) {
        return;
    }

    m_storageReady = ready;

    emit storageReadyChanged();
}

// =============================================================
// Open encrypted storage
// =============================================================

void Backend::openStorage(const QString &passphrase)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "A storage operation is already in progress"
            )
        );

        return;
    }

    if (m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral(
                "Encrypted storage is already open"
            )
        );

        return;
    }

    if (passphrase.isEmpty()) {
        emit storageStatusChanged(
            QStringLiteral(
                "Please enter a storage passphrase"
            )
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Opening encrypted storage...")
    );

    emit requestOpenStorage(passphrase);
}

// =============================================================
// Matrix login
// =============================================================

void Backend::loginMatrix(
    const QString &homeserver,
    const QString &username,
    const QString &password
)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Connecting to Matrix...")
    );

    emit requestLoginMatrix(
        homeserver,
        username,
        password
    );
}

// =============================================================
// Matrix room and Space retrieval
// =============================================================

void Backend::refreshMatrixRooms()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Retrieving Matrix rooms...")
    );

    emit requestFetchMatrixRooms();
}

// =============================================================
// Matrix media and room avatar requests
// =============================================================

void Backend::requestRoomAvatar(
    const QString &roomId,
    const QString &mxcUri
)
{
    if (!m_storageReady) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Encrypted storage is not open")
        );

        return;
    }

    if (roomId.isEmpty() || mxcUri.isEmpty()) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Room ID or avatar URI is empty")
        );

        return;
    }

    // Request a 96 x 96 thumbnail.
    // The request is queued on the existing worker thread.
    // Avatar retrieval does not block normal storage operations.

    emit requestFetchRoomAvatar(
        roomId,
        mxcUri,
        96,
        96
    );
}

void Backend::requestSenderAvatar(
    const QString &requestKey,
    const QString &mxcUri
)
{
    if (!m_storageReady) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );

        return;
    }

    if (requestKey.isEmpty() || mxcUri.isEmpty()) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Request key or avatar URI is empty")
        );

        return;
    }

    // Request a 96 x 96 thumbnail.
    // The request is queued on the existing worker thread.
    // Avatar retrieval does not block normal storage operations.

    emit requestFetchSenderAvatar(
        requestKey,
        mxcUri,
        96,
        96
    );
}

void Backend::requestMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson
)
{
    if (!m_storageReady) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    emit requestFetchMessageImage(requestKey, mediaSourceJson);
}



void Backend::requestMessageThumbnail(
    const QString &requestKey,
    const QString &mediaSourceJson,
    quint32 width,
    quint32 height
)
{
    if (!m_storageReady) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    if (width == 0 || height == 0 || width > 2048 || height > 2048) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Thumbnail dimensions are invalid")
        );
        return;
    }

    emit requestFetchMessageThumbnail(
        requestKey,
        mediaSourceJson,
        width,
        height
    );
}

void Backend::requestMessageVideo(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType
)
{
    if (!m_storageReady) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    emit requestFetchMessageVideo(
        requestKey,
        mediaSourceJson,
        mimeType
    );
}

// =============================================================
// Matrix logout
// =============================================================

// Request a room's initial message timeline.
void Backend::requestRoomTimeline(const QString &roomId)
{
    if (!m_storageReady) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestFetchRoomTimeline(roomId);
}

// Request another batch of older messages for an existing room timeline.
void Backend::requestOlderRoomTimeline(const QString &roomId)
{
    if (!m_storageReady) {
        emit matrixOlderTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixOlderTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestPaginateRoomTimeline(roomId);
}

void Backend::markRoomAsRead(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        return;
    }

    emit requestMarkRoomAsRead(roomId);
}

void Backend::sendRoomMessage(
    const QString &roomId,
    const QString &message
)
{
    if (!m_storageReady) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
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

    emit requestSendRoomMessage(roomId, message);
}


void Backend::acceptVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("accept"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestAcceptVerificationRequest(userId, flowId);
}

void Backend::cancelVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("cancel"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestCancelVerificationRequest(userId, flowId);
}

void Backend::confirmVerification(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("confirm"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestConfirmVerification(userId, flowId);
}

void Backend::mismatchVerification(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("mismatch"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestMismatchVerification(userId, flowId);
}

void Backend::logoutMatrix()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Logging out of Matrix...")
    );

    emit requestLogoutMatrix();
}

// =============================================================
// Close encrypted storage
// =============================================================

void Backend::closeStorage()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "A storage operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Encrypted storage is not open")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Closing encrypted storage...")
    );

    emit requestCloseStorage();
}

// =============================================================
// Delete all Astro application data
// =============================================================

void Backend::deleteAllData()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("Another operation is already in progress")
        );
        return;
    }

    const QString path = prepareDataDirectory();

    if (path.isEmpty()) {
        emit storageStatusChanged(
            QStringLiteral("Could not determine Astro data directory")
        );
        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Deleting Astro local data...")
    );

    // Run logout, client cleanup and deletion on the storage thread.
    QMetaObject::invokeMethod(
        m_storageWorker,
        [this, path]()
        {
            m_storageWorker->deleteAllData(path);
        },
        Qt::QueuedConnection
    );
}
