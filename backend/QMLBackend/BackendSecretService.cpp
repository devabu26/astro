

#include "Backend.h"
#include "StorageWorker/StorageWorker.h"
#include "notifications/NotificationManager.h"

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