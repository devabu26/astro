
import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend
    property bool existingStorage: false
    property string storageResult: "Enter your storage passphrase to continue."
    property bool showDeleteConfirmation: false
    property bool deletingData: false

    // Opens or creates encrypted Astro storage.
    function openAstroStorage() {
        var passphrase = unlockPassphrase.text

        if (!root.existingStorage &&
            passphrase !== confirmPassphrase.text) {
            root.storageResult = "The passphrases do not match."
            return
        }

        unlockPassphrase.text = ""
        confirmPassphrase.text = ""

        root.storageResult = "Opening encrypted storage..."

        backend.openStorage(passphrase)
        passphrase = ""
    }

    // Signs in to the Matrix account.
    function signInToMatrix() {
        if (homeserverField.text.trim().length === 0 ||
            matrixUsername.text.trim().length === 0 ||
            matrixPassword.text.length === 0) {
            return
        }

        var homeserver = homeserverField.text.trim()
        var username = matrixUsername.text.trim()
        var password = matrixPassword.text

        matrixPassword.text = ""
        root.storageResult = "Signing in to Matrix..."

        backend.loginMatrix(
            homeserver,
            username,
            password
        )

        password = ""
    }

    // Requests deletion after the user confirms.
    function deleteLocalData() {
        if (!root.backend || root.deletingData)
            return

        root.showDeleteConfirmation = false
        root.deletingData = true
        root.storageResult = "Deleting Astro local data..."

        root.backend.deleteAllData()
    }

    // Handle completion and errors from the existing backend.
    Connections {
        target: root.backend

        onDataDeleted: {
            root.deletingData = false
            root.existingStorage = false

            unlockPassphrase.text = ""
            confirmPassphrase.text = ""
            homeserverField.text = "https://matrix.org"
            matrixUsername.text = ""
            matrixPassword.text = ""

            root.storageResult =
                "All local data deleted. Create new encrypted storage to continue."

            console.log("ASTRO: All local data deleted.")
        }

            onDataDeletionFinished: {
            root.deletingData = false

            if (!deleted) {
                root.storageResult = deletionError.length > 0
                    ? deletionError
                    : "Could not delete Astro local data."

                if (logoutAttempted && !logoutSucceeded) {
                    root.storageResult +=
                        " Matrix logout could not be confirmed."
                }

                console.log(
                    "ASTRO: Local deletion failed:",
                    deletionError
                )
                return
            }

            if (!storageWasUnlocked) {
                root.storageResult =
                    "Local data deleted. Encrypted storage was locked, " +
                    "so Astro could not verify or log out the saved Matrix session. " +
                    "Revoke this device from another Matrix session if necessary."

            } else if (logoutAttempted && logoutSucceeded) {
                root.storageResult =
                    "All local data deleted. This Astro device was also logged out of Matrix."

            } else if (logoutAttempted) {
                root.storageResult =
                    "All local data deleted, but Matrix logout could not be confirmed. " +
                    "You may need to revoke this device from another Matrix session."

                if (logoutError.length > 0)
                    console.log("ASTRO: Matrix logout warning:", logoutError)

            } else {
                root.storageResult =
                    "All local data deleted. No active Matrix session was found."
            }

            console.log("ASTRO: Local data deletion finished.")
        }

        onStorageStatusChanged: {
                if (root.deletingData)
                    return

                console.log("ASTRO: Storage status:", status)

                var lowerStatus = status.toLowerCase()

                if (lowerStatus.indexOf("incorrect passphrase") !== -1 ||
                    lowerStatus.indexOf("could not decrypt") !== -1) {

                    root.storageResult =
                        "Unable to unlock Astro. Your storage passphrase may be incorrect. Please try again."

                } else if (lowerStatus.indexOf("could not open") !== -1 ||
                        lowerStatus.indexOf("storage") !== -1 &&
                        lowerStatus.indexOf("failed") !== -1) {

                    root.storageResult =
                        "Unable to open encrypted storage. Please check your passphrase and try again."

                } else {
                    root.storageResult = status
                }
            }
    }

    // Encrypted storage panel.
    Rectangle {
        anchors.fill: parent
        color: "#101820"
        visible: !backend.storageReady

        Flickable {
            anchors.fill: parent
            contentWidth: width
            contentHeight: unlockContent.height + units.gu(3)
            clip: true

            Column {
                id: unlockContent

                width: parent.width
                spacing: units.gu(2)

                Item {
                    width: parent.width
                    height: units.gu(7)
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "✦"
                    color: "#80b9e8"
                    font.pixelSize: units.gu(8)
                }

                Text {
                    width: parent.width
                    text: root.existingStorage
                          ? "Welcome back"
                          : "Welcome to Astro"

                    color: "white"
                    font.pixelSize: units.gu(3)
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    width: parent.width - units.gu(6)
                    anchors.horizontalCenter: parent.horizontalCenter

                    text: root.existingStorage
                          ? "Unlock your encrypted Astro storage to continue."
                          : "Create encrypted storage to protect your Matrix account data."

                    color: "#9aabba"
                    font.pixelSize: units.gu(1.8)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }

                Item {
                    width: parent.width
                    height: units.gu(2)
                }

                Column {
                    width: Math.min(
                        parent.width - units.gu(6),
                        units.gu(42)
                    )

                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: units.gu(1.5)

                    Text {
                        text: "STORAGE PASSPHRASE"
                        color: "#91a4b5"
                        font.bold: true
                        font.pixelSize: units.gu(1.5)
                    }

                    TextField {
                        id: unlockPassphrase

                        width: parent.width
                        placeholderText: "Enter passphrase"
                        echoMode: TextInput.Password

                        onAccepted: {
                            if (root.existingStorage) {
                                root.openAstroStorage()
                            } else {
                                confirmPassphrase.forceActiveFocus()
                            }
                        }
                    }

                    Text {
                        visible: !root.existingStorage

                        text: "CONFIRM PASSPHRASE"
                        color: "#91a4b5"
                        font.bold: true
                        font.pixelSize: units.gu(1.5)
                    }

                    TextField {
                        id: confirmPassphrase

                        width: parent.width
                        visible: !root.existingStorage
                        placeholderText: "Confirm passphrase"
                        echoMode: TextInput.Password

                        onAccepted: root.openAstroStorage()
                    }

                    Button {
                        width: parent.width

                        text: root.existingStorage
                              ? "Unlock Astro"
                              : "Create Encrypted Storage"

                        enabled: !root.deletingData &&
                                 unlockPassphrase.text.length > 0 &&
                                 (root.existingStorage ||
                                  confirmPassphrase.text.length > 0)

                        onClicked: root.openAstroStorage()
                    }

                    Text {
                        width: parent.width

                        text: root.storageResult
                        color: root.storageResult.indexOf("successfully") !== -1
                               ? "#75c995"
                               : "#91a4b5"

                        font.pixelSize: units.gu(1.6)
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        width: parent.width
                        height: units.gu(5)
                        radius: units.gu(1)
                        color: "transparent"

                        Text {
                            anchors.centerIn: parent

                            text: "Clear local data"
                            color: "#e57373"
                            font.pixelSize: units.gu(1.7)
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !root.deletingData

                            onClicked: {
                                root.showDeleteConfirmation = true
                            }
                        }
                    }
                }
            }
        }
    }

    // Matrix sign-in panel.
    Rectangle {
        anchors.fill: parent
        color: "#101820"
        visible: backend.storageReady

        Column {
            width: Math.min(
                parent.width - units.gu(6),
                units.gu(42)
            )

            anchors.centerIn: parent
            spacing: units.gu(2)

            Text {
                width: parent.width
                text: "Connect to Matrix"
                color: "white"
                font.pixelSize: units.gu(3)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width
                text: "Sign in with your Matrix account."
                color: "#9aabba"
                font.pixelSize: units.gu(1.8)
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                text: "HOMESERVER"
                color: "#91a4b5"
                font.bold: true
                font.pixelSize: units.gu(1.5)
            }

            TextField {
                id: homeserverField

                width: parent.width
                text: "https://matrix.org"
                placeholderText: "https://matrix.org"

                onAccepted: matrixUsername.forceActiveFocus()
            }

            Text {
                text: "USERNAME"
                color: "#91a4b5"
                font.bold: true
                font.pixelSize: units.gu(1.5)
            }

            TextField {
                id: matrixUsername

                width: parent.width
                placeholderText: "Matrix username"

                onAccepted: matrixPassword.forceActiveFocus()
            }

            Text {
                text: "PASSWORD"
                color: "#91a4b5"
                font.bold: true
                font.pixelSize: units.gu(1.5)
            }

            TextField {
                id: matrixPassword

                width: parent.width
                placeholderText: "Matrix password"
                echoMode: TextInput.Password

                onAccepted: root.signInToMatrix()
            }

            Button {
                width: parent.width
                text: "Sign in"

                enabled: !root.deletingData &&
                         homeserverField.text.trim().length > 0 &&
                         matrixUsername.text.trim().length > 0 &&
                         matrixPassword.text.length > 0

                onClicked: root.signInToMatrix()
            }

            Text {
                width: parent.width
                text: root.storageResult
                color: root.storageResult.indexOf("successful") !== -1
                       ? "#75c995"
                       : "#91a4b5"

                font.pixelSize: units.gu(1.6)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Rectangle {
                width: parent.width
                height: units.gu(5)
                radius: units.gu(1)
                color: "transparent"

                Text {
                    anchors.centerIn: parent

                    text: "Clear local data"
                    color: "#e57373"
                    font.pixelSize: units.gu(1.7)
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !root.deletingData

                    onClicked: {
                        root.showDeleteConfirmation = true
                    }
                }
            }
        }
    }

    // Delete confirmation overlay.
    Rectangle {
        anchors.fill: parent

        visible: root.showDeleteConfirmation
        color: "#99000000"
        z: 100

        MouseArea {
            anchors.fill: parent
        }

        Rectangle {
            width: Math.min(
                parent.width - units.gu(6),
                units.gu(45)
            )

            height: units.gu(46)

            anchors.centerIn: parent

            radius: units.gu(2)
            color: "#1b2935"
            border.color: "#e57373"
            border.width: 1

            Column {
                anchors {
                    fill: parent
                    margins: units.gu(3)
                }

                spacing: units.gu(2)

                Text {
                    width: parent.width

                    text: "Delete all Astro data?"
                    color: "white"
                    font.pixelSize: units.gu(2.5)
                    font.bold: true

                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                Text {
                    width: parent.width

                    text: "This will permanently delete your encrypted Astro storage, Matrix session, local messages, cached media, downloaded files and other application data.\n\nYou will need to create new encrypted storage and sign in again.\n\nThis action cannot be undone."

                    color: "#c4ced6"
                    font.pixelSize: units.gu(1.7)

                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                Item {
                    width: parent.width
                    height: units.gu(1)
                }

                Button {
                    width: parent.width
                    text: "Cancel"

                    enabled: !root.deletingData

                    onClicked: {
                        root.showDeleteConfirmation = false
                    }
                }

                Button {
                    width: parent.width
                    text: root.deletingData
                          ? "Deleting..."
                          : "Delete Everything"

                    color: LomiriColors.red
                    enabled: !root.deletingData

                    onClicked: root.deleteLocalData()
                }
            }
        }
    }
}
