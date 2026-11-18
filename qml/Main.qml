
import QtQuick 2.7
import Lomiri.Components 1.3
import Astro.Backend 1.0

MainView {
    id: root
    objectName: "mainView"
    applicationName: "astro.madabu"

    width: units.gu(45)
    height: units.gu(75)

    // =========================================================
    // Application properties
    // =========================================================

    property string storageResult:
        "Enter your storage passphrase to continue."

    property bool existingStorage: astroBackend.hasExistingStorage()
    property bool matrixAuthenticated: false
    property bool showingSettings: false

    // =========================================================
    // Backend instance
    // =========================================================

    Backend {
        id: astroBackend
    }

    // =========================================================
    // Backend signals
    // =========================================================

    Connections {
        target: astroBackend

        // -----------------------------------------------------
        // Storage status
        // -----------------------------------------------------

        onStorageStatusChanged: {
            root.storageResult = status
            root.existingStorage = astroBackend.hasExistingStorage()

            chatPage.storageResult = status

            if (!astroBackend.storageReady) {
                root.matrixAuthenticated = false
                chatPage.stopRoomLoading()
            }
        }

        // -----------------------------------------------------
        // Matrix session restoration
        // -----------------------------------------------------

        onMatrixSessionRestored: {
            root.matrixAuthenticated = restored

            if (restored) {
                console.log(
                    "ASTRO: Session restored; fetching Matrix rooms"
                )

                // Start the diamond animation before requesting rooms.
                chatPage.startRoomLoading()

                astroBackend.refreshMatrixRooms()
            }
        }

        // -----------------------------------------------------
        // Matrix login
        // -----------------------------------------------------

        onMatrixLoginSucceeded: {
            console.log(
                "ASTRO: Matrix login success signal received"
            )

            console.log(
                "ASTRO: storageReady =",
                astroBackend.storageReady
            )

            root.matrixAuthenticated = true

            console.log(
                "ASTRO: matrixAuthenticated =",
                root.matrixAuthenticated
            )

            console.log(
                "ASTRO: Login complete; fetching Matrix rooms"
            )

            // Start the diamond animation before requesting rooms.
            chatPage.startRoomLoading()

            astroBackend.refreshMatrixRooms()
        }

        // -----------------------------------------------------
        // Matrix logout
        // -----------------------------------------------------

        onMatrixLogoutSucceeded: {
            root.matrixAuthenticated = false
            root.showingSettings = false

            chatPage.stopRoomLoading()
        }

        // -----------------------------------------------------
        // Local data deletion
        // -----------------------------------------------------

        onDataDeleted: {
            console.log(
                "ASTRO: All local data deleted"
            )

            root.matrixAuthenticated = false
            root.showingSettings = false
            root.existingStorage = false

            chatPage.stopRoomLoading()
        }

        // -----------------------------------------------------
        // Matrix rooms retrieved
        // -----------------------------------------------------

        onMatrixRoomsRetrieved: {
            console.log(
                "ASTRO: Matrix rooms received"
            )

            // loadRooms() stops the animation after validating
            // the returned JSON and updating the room model.
            chatPage.loadRooms(json)
        }
    }

    // =========================================================
    // Main application page
    // =========================================================

    Page {
        anchors.fill: parent

        header: Item {
            height: 0
            visible: false
        }

        // =====================================================
        // Login page
        // =====================================================

        LoginPage {
            id: loginPage

            anchors.fill: parent

            backend: astroBackend
            existingStorage: root.existingStorage
            storageResult: root.storageResult

            visible: !root.matrixAuthenticated
        }

        // =====================================================
        // Settings page
        // =====================================================

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

        // =====================================================
        // Chat page
        // =====================================================

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
