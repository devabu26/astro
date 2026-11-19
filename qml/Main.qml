
import QtQuick 2.7
import Lomiri.Components 1.3
import Astro.Backend 1.0
import "Components"

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

            verificationDialog.closeDialog()
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

            verificationDialog.closeDialog()
            chatPage.stopRoomLoading()
        }

        // -----------------------------------------------------
        // Matrix rooms retrieved
        // -----------------------------------------------------

        onMatrixRoomsRetrieved: {
            console.log(
                "ASTRO: Matrix rooms received"
            )

            // loadRooms() stops the animation only after
            // the final network synchronisation response.
            chatPage.loadRooms(json, finished)
        }

        // -----------------------------------------------------
        // Matrix device verification events
        // -----------------------------------------------------

        onMatrixLiveTimelineUpdated: {
            var data

            try {
                data = JSON.parse(json)
            } catch (error) {
                console.warn(
                    "ASTRO: Could not parse live timeline JSON:",
                    error
                )
                return
            }

            var events = data.verification_requests || []

            for (var i = 0; i < events.length; i++) {
                var event = events[i]

                // SAS state updates use the same array as
                // incoming verification requests.
                if (event.type === "verification_state") {
                    if (verificationDialog.visible &&
                        verificationDialog.userId === event.user_id &&
                        verificationDialog.flowId === event.flow_id) {

                        verificationDialog.updateVerificationState(
                            event.state,
                            event.emojis || []
                        )
                    }

                    continue
                }

                // Incoming verification request.
                if (event.user_id && event.flow_id) {
                    if (!verificationDialog.visible) {
                        verificationDialog.showRequest(
                            event.user_id,
                            event.flow_id,
                            event.room_id || ""
                        )
                    } else if (
                        verificationDialog.userId !== event.user_id ||
                        verificationDialog.flowId !== event.flow_id
                    ) {
                        console.warn(
                            "ASTRO: Another verification request arrived while a dialog is active:",
                            event.user_id,
                            event.flow_id
                        )
                    }
                }
            }
        }

        // -----------------------------------------------------
        // Matrix verification action results
        // -----------------------------------------------------


        onMatrixVerificationActionFinished: {
            if (!verificationDialog.visible ||
                verificationDialog.userId !== userId ||
                verificationDialog.flowId !== flowId) {
                return
            }

            if (error !== "") {
                verificationDialog.showError(error)
                return
            }

            if (action === "cancel") {
                verificationDialog.updateVerificationState(
                    "cancelled",
                    []
                )
            } else if (action === "mismatch") {
                verificationDialog.updateVerificationState(
                    "mismatched",
                    []
                )
            } else {
                verificationDialog.busy = false
            }
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

    // =========================================================
    // Matrix device verification dialog
    // =========================================================

    DeviceVerificationDialog {
        id: verificationDialog

        anchors.fill: parent
        z: 1000

        onAcceptRequested: {
            astroBackend.acceptVerificationRequest(userId, flowId)
        }

        onCancelRequested: {
            astroBackend.cancelVerificationRequest(userId, flowId)
        }

        onConfirmRequested: {
            astroBackend.confirmVerification(userId, flowId)
        }

        onMismatchRequested: {
            astroBackend.mismatchVerification(userId, flowId)
        }
    }
}
