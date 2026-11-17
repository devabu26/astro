
import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend
    property bool existingStorage: false
    property string storageResult: "Enter your storage passphrase to continue."

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

    // Encrypted storage panel
    Rectangle {
        anchors.fill: parent
        color: "#101820"
        visible: !backend.storageReady

        Flickable {
            anchors.fill: parent
            contentWidth: width
            contentHeight: unlockContent.height
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

                        enabled: unlockPassphrase.text.length > 0 &&
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
                }
            }
        }
    }

    // Matrix sign-in panel
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

                enabled: homeserverField.text.trim().length > 0 &&
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
        }
    }
}
