
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
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusArgument>
#include <QDBusMetaType>

// Secret Service structure: (oayays)
struct SecretServiceSecret
{
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

Q_DECLARE_METATYPE(SecretServiceSecret)

QDBusArgument &operator<<(QDBusArgument &argument,
                          const SecretServiceSecret &secret)
{
    argument.beginStructure();
    argument << secret.session
             << secret.parameters
             << secret.value
             << secret.contentType;
    argument.endStructure();

    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                SecretServiceSecret &secret)
{
    argument.beginStructure();
    argument >> secret.session
             >> secret.parameters
             >> secret.value
             >> secret.contentType;
    argument.endStructure();

    return argument;
}


// ---------------------------------------------------------------------
// Backend construction and storage worker setup
// ---------------------------------------------------------------------

Backend::Backend(QObject *parent)
    : QObject(parent)
{
    QCoreApplication::setApplicationName(
        QStringLiteral("astro.madabu")
    );

    // Create the dedicated storage thread and worker.
    m_storageThread = new QThread(this);
    m_storageWorker = new StorageWorker();

    m_storageWorker->moveToThread(m_storageThread);

    connect(
        m_storageThread,
        &QThread::finished,
        m_storageWorker,
        &QObject::deleteLater
    );

    // Send storage requests to the worker thread.
    connect(
        this,
        &Backend::requestOpenStorage,
        m_storageWorker,
        &StorageWorker::openStorage,
        Qt::QueuedConnection
    );

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

    connect(
        this,
        &Backend::requestCloseStorage,
        m_storageWorker,
        &StorageWorker::closeStorage,
        Qt::QueuedConnection
    );

    // Handle successful storage opening.
    connect(
        m_storageWorker,
        &StorageWorker::storageOpened,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;
            setStorageReady(true);

            emit storageStatusChanged(
                QStringLiteral("Encrypted storage opened successfully")
            );
        }
    );

    // Handle successful storage closing.
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

    // Handle storage errors.
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
                    : QStringLiteral("Storage unlocked. Please sign in to Matrix.")
            );
        }
    );

    //Login
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

    //Logout
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

    m_storageThread->start();

    // Prepare the application data directory.
    const QString path = prepareDataDirectory();

    if (path.isEmpty()) {
        qWarning() << "Astro: Could not prepare application data directory";
    } else {
        qInfo() << "Astro: Application data directory:" << path;
    }
}


// ---------------------------------------------------------------------
// Backend destruction
// ---------------------------------------------------------------------

Backend::~Backend()
{
    if (m_storageThread && m_storageThread->isRunning()) {

        // Close Rust storage on the worker thread.
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


// ---------------------------------------------------------------------
// Existing Rust status
// ---------------------------------------------------------------------

QString Backend::status() const
{
    const char *rustStatus = astro_get_status();

    if (!rustStatus) {
        return QStringLiteral("Astro Rust backend returned no status!");
    }

    return QString::fromUtf8(rustStatus);
}


// ---------------------------------------------------------------------
// Application data directory
// ---------------------------------------------------------------------

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
bool Backend::hasExistingStorage() const
{
    const QString path = prepareDataDirectory();

    if (path.isEmpty()) {
        return false;
    }

    const QDir directory(path);

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

// ---------------------------------------------------------------------
// Existing Secret Service status diagnostic
// ---------------------------------------------------------------------

QString Backend::secretServiceStatus() const
{
    const QDBusConnection connection =
        QDBusConnection::sessionBus();

    if (!connection.isConnected()) {
        return QStringLiteral("Session D-Bus: not connected");
    }

    QDBusConnectionInterface *interface =
        connection.interface();

    if (!interface) {
        return QStringLiteral("Session D-Bus: interface unavailable");
    }

    const QDBusReply<bool> reply =
        interface->isServiceRegistered(
            QStringLiteral("org.freedesktop.secrets")
        );

    if (!reply.isValid()) {
        return QStringLiteral("Secret Service check failed: ")
            + reply.error().message();
    }

    if (reply.value()) {
        return QStringLiteral("Secret Service: accessible");
    }

    return QStringLiteral("Secret Service: not registered");
}


// ---------------------------------------------------------------------
// Existing Secret Service round-trip diagnostic
// ---------------------------------------------------------------------

void Backend::testSecretService()
{
    const QDBusConnection connection =
        QDBusConnection::sessionBus();

    if (!connection.isConnected()) {
        emit secretServiceTestFinished(
            QStringLiteral("Keyring test failed: session D-Bus unavailable")
        );
        return;
    }

    qDBusRegisterMetaType<SecretServiceSecret>();

    // Unique diagnostic value. This is not a real credential.
    const QString testId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QByteArray testValue =
        (QStringLiteral("Astro-Keyring-Test-") + testId).toUtf8();

    const QString service =
        QStringLiteral("org.freedesktop.secrets");

    const QString servicePath =
        QStringLiteral("/org/freedesktop/secrets");

    const QString serviceInterface =
        QStringLiteral("org.freedesktop.Secret.Service");

    const QString collectionPath =
        QStringLiteral("/org/freedesktop/secrets/collection/login");

    // Send an asynchronous D-Bus call and deliver its reply
    // through a callback. Nothing here blocks the UI thread.
    auto callAsync =
        [this, connection, service](
            const QString &path,
            const QString &interfaceName,
            const QString &method,
            const QList<QVariant> &arguments,
            std::function<void(QDBusPendingCallWatcher *)> callback)
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

    // Step 1: open a Secret Service session.
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
            QDBusPendingReply<QDBusVariant, QDBusObjectPath> reply =
                *watcher;

            if (reply.isError()) {
                emit secretServiceTestFinished(
                    QStringLiteral("Keyring session failed: ")
                    + reply.error().message()
                );
                return;
            }

            const QDBusObjectPath sessionPath =
                reply.argumentAt<1>();

            // Step 2: prepare the temporary item.
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
                QStringLiteral("org.freedesktop.Secret.Item.Label"),
                QStringLiteral("Astro Keyring diagnostic")
            );

            properties.insert(
                QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                QVariant::fromValue(attributes)
            );

            SecretServiceSecret secret;

            secret.session = sessionPath;
            secret.parameters = QByteArray();
            secret.value = testValue;
            secret.contentType = QStringLiteral("text/plain");

            // Step 3: create the temporary item.
            callAsync(
                collectionPath,
                QStringLiteral("org.freedesktop.Secret.Collection"),
                QStringLiteral("CreateItem"),
                {
                    QVariant::fromValue(properties),
                    QVariant::fromValue(secret),
                    QVariant(false)
                },
                [this, callAsync, sessionPath, testValue]
                (QDBusPendingCallWatcher *createWatcher)
                {
                    QDBusPendingReply<QDBusObjectPath, QDBusObjectPath>
                        createReply = *createWatcher;

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

                    // Step 4: retrieve the stored value.
                    callAsync(
                        itemPath.path(),
                        QStringLiteral("org.freedesktop.Secret.Item"),
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

                            // Step 5: delete the temporary item.
                            callAsync(
                                itemPath.path(),
                                QStringLiteral(
                                    "org.freedesktop.Secret.Item"
                                ),
                                QStringLiteral("Delete"),
                                {},
                                [this, retrieved, retrievalError]
                                (QDBusPendingCallWatcher *deleteWatcher)
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

                                    if (deleteReply.value().path()
                                        != QStringLiteral("/")) {
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


// ---------------------------------------------------------------------
// Storage state
// ---------------------------------------------------------------------

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


// ---------------------------------------------------------------------
// Open encrypted storage
// ---------------------------------------------------------------------

void Backend::openStorage(const QString &passphrase)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("A storage operation is already in progress")
        );
        return;
    }

    if (m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Encrypted storage is already open")
        );
        return;
    }

    if (passphrase.isEmpty()) {
        emit storageStatusChanged(
            QStringLiteral("Please enter a storage passphrase")
        );
        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Opening encrypted storage...")
    );

    emit requestOpenStorage(passphrase);
}


//Login To Matrix Server
void Backend::loginMatrix(
    const QString &homeserver,
    const QString &username,
    const QString &password
)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("Another operation is already in progress")
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

//Logout Of Matrix Account
void Backend::logoutMatrix()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("Another operation is already in progress")
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

// ---------------------------------------------------------------------
// Close encrypted storage
// ---------------------------------------------------------------------

void Backend::closeStorage()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("A storage operation is already in progress")
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
