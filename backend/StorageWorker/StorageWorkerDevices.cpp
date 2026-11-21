
#include "StorageWorker.h"

#include <QByteArray>

// Run a verification action through the common Rust FFI signature.
// Rust-allocated error strings are released before returning.
void StorageWorker::runVerificationAction(
    const QString &userId,
    const QString &flowId,
    const QString &actionName,
    VerificationAction action
)
{
    if (!m_matrixClient) {
        emit matrixVerificationActionFinished(
            userId,
            flowId,
            actionName,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId,
            flowId,
            actionName,
            QStringLiteral("User ID or verification flow ID is empty")
        );
        return;
    }

    const QByteArray userIdBytes = userId.toUtf8();
    const QByteArray flowIdBytes = flowId.toUtf8();

    char *error = nullptr;

    const int32_t result = action(
        m_matrixClient,
        userIdBytes.constData(),
        flowIdBytes.constData(),
        &error
    );

    QString errorMessage;

    if (result != 1) {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Matrix verification action failed");
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixVerificationActionFinished(
        userId,
        flowId,
        actionName,
        errorMessage
    );
}

void StorageWorker::acceptVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId,
        flowId,
        QStringLiteral("accept"),
        astro_matrix_accept_verification_request
    );
}

void StorageWorker::cancelVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId,
        flowId,
        QStringLiteral("cancel"),
        astro_matrix_cancel_verification_request
    );
}

void StorageWorker::confirmVerification(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId,
        flowId,
        QStringLiteral("confirm"),
        astro_matrix_confirm_verification
    );
}

void StorageWorker::mismatchVerification(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId,
        flowId,
        QStringLiteral("mismatch"),
        astro_matrix_mismatch_verification
    );
}

void StorageWorker::requestDeviceVerification(
    const QString &deviceId
)
{
    if (!m_matrixClient || deviceId.isEmpty()) {
        emit matrixDeviceVerificationFinished(
            deviceId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Device ID is empty")
        );
        return;
    }

    const QByteArray deviceIdBytes = deviceId.toUtf8();
    char *error = nullptr;

    const int32_t result = astro_matrix_request_device_verification(
        m_matrixClient,
        deviceIdBytes.constData(),
        &error
    );

    QString errorMessage;

    if (result != 1) {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not start device verification");
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixDeviceVerificationFinished(deviceId, errorMessage);
}

void StorageWorker::renameMatrixDevice(
    const QString &deviceId,
    const QString &displayName
)
{
    if (!m_matrixClient ||
        deviceId.isEmpty() ||
        displayName.trimmed().isEmpty()) {

        emit matrixDeviceRenameFinished(
            deviceId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : deviceId.isEmpty()
                    ? QStringLiteral("Device ID is empty")
                    : QStringLiteral("Device name cannot be empty")
        );
        return;
    }

    const QByteArray deviceIdBytes = deviceId.toUtf8();
    const QByteArray displayNameBytes = displayName.trimmed().toUtf8();

    char *error = nullptr;

    const int32_t result = astro_matrix_rename_device(
        m_matrixClient,
        deviceIdBytes.constData(),
        displayNameBytes.constData(),
        &error
    );

    QString errorMessage;

    if (result != 1) {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not rename Matrix device");
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixDeviceRenameFinished(deviceId, errorMessage);
}

void StorageWorker::revokeMatrixDevice(
    const QString &deviceId,
    const QString &password
)
{
    if (!m_matrixClient || deviceId.isEmpty() || password.isEmpty()) {
        emit matrixDeviceRevocationFinished(
            deviceId,
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : deviceId.isEmpty()
                    ? QStringLiteral("Device ID is empty")
                    : QStringLiteral("Account password is required")
        );
        return;
    }

    const QByteArray deviceIdBytes = deviceId.toUtf8();

    // Keep the temporary password bytes mutable so they can be wiped
    // as soon as the synchronous FFI call has finished using them.
    QByteArray passwordBytes = password.toUtf8();

    char *error = nullptr;

    const int32_t result = astro_matrix_revoke_device(
        m_matrixClient,
        deviceIdBytes.constData(),
        passwordBytes.constData(),
        &error
    );

    passwordBytes.fill('\0');

    QString errorMessage;

    if (result != 1) {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not sign out Matrix device");
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixDeviceRevocationFinished(deviceId, errorMessage);
}

void StorageWorker::checkMatrixDeviceManagementUrl(
    const QString &deviceId
)
{
    if (!m_matrixClient || deviceId.isEmpty()) {
        emit matrixDeviceManagementUrlReady(
            deviceId,
            QString(),
            !m_matrixClient
                ? QStringLiteral("No active Matrix session")
                : QStringLiteral("Device ID is empty")
        );
        return;
    }

    const QByteArray deviceIdBytes = deviceId.toUtf8();

    char *url = nullptr;
    char *error = nullptr;

    const int32_t result = astro_matrix_device_management_url(
        m_matrixClient,
        deviceIdBytes.constData(),
        &url,
        &error
    );

    QString urlString;
    QString errorMessage;

    if (result == 1 && url) {
        urlString = QString::fromUtf8(url);
    } else {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not retrieve device management URL");
    }

    if (url) {
        astro_string_free(url);
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixDeviceManagementUrlReady(
        deviceId,
        urlString,
        errorMessage
    );
}
