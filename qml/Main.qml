
import QtQuick 2.7
import Lomiri.Components 1.3
import Astro.Backend 1.0

MainView {
    id: root
    objectName: "mainView"
    applicationName: "astro.madabu"

    width: units.gu(45)
    height: units.gu(75)

    property string storageResult:
        "Enter your storage passphrase to continue."

    property bool existingStorage: astroBackend.hasExistingStorage()
    property bool matrixAuthenticated: false
    property bool showingSettings: false

    Backend {
        id: astroBackend
    }

    Connections {
        target: astroBackend

        onStorageStatusChanged: {
            root.storageResult = status
            root.existingStorage = astroBackend.hasExistingStorage()

            chatPage.storageResult = status

            if (!astroBackend.storageReady) {
                root.matrixAuthenticated = false
            }
        }

        onMatrixSessionRestored: {
            root.matrixAuthenticated = restored
        }

        onMatrixLoginSucceeded: {
            console.log("ASTRO: Matrix login success signal received")
            console.log("ASTRO: storageReady =", astroBackend.storageReady)

            root.matrixAuthenticated = true

            console.log("ASTRO: matrixAuthenticated =", root.matrixAuthenticated)
        }

        onMatrixLogoutSucceeded: {
            root.matrixAuthenticated = false
            root.showingSettings = false
        }

    }

    Page {
        anchors.fill: parent

        header: Item {
            height: 0
            visible: false
        }

        LoginPage {
            id: loginPage
            anchors.fill: parent

            backend: astroBackend
            existingStorage: root.existingStorage
            storageResult: root.storageResult

            visible: !root.matrixAuthenticated
        }

        SettingsPage {
            id: settingsPage
            anchors.fill: parent

            backend: astroBackend
            visible: astroBackend.storageReady &&
                     root.matrixAuthenticated &&
                     root.showingSettings

            onBackRequested: {
                root.showingSettings = false
            }
        }

        ChatPage {
            id: chatPage
            anchors.fill: parent

            mainRoot: root
            backend: astroBackend
            visible: astroBackend.storageReady &&
                    root.matrixAuthenticated &&
                    !root.showingSettings
        }
    }
}
