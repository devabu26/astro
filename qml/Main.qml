
import QtQuick 2.7
import QtMultimedia 5.12
import Qt.labs.settings 1.0
import Lomiri.Components 1.3
import Astro.Backend 1.0
import "Components"
import "Settings"

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
    property bool applicationWasBackgrounded: false
    property bool matrixRoomsReady: false
    property bool showingSettings: false
    property bool showingCallPage: false
    property bool incomingCall: false
    property string callRoomId: ""
    property string callRoomName: ""
    property string pendingNotificationRoomId: ""
    property string pendingNotificationEventId: ""
    property string lastNotificationUrl: ""
    property double lastNotificationUrlAt: 0

    function openCallPage(roomId, roomName, startImmediately) {
        if (!roomId)
            return

        root.callRoomId = roomId
        root.callRoomName = roomName || ""
        root.showingCallPage = true

        if (startImmediately) {
            // Let CallPage receive its room, audio and membership bindings
            // before asking it to join and start the LiveKit audio session.
            Qt.callLater(function() {
                if (root.showingCallPage && root.callRoomId === roomId)
                    callPage.startCall()
            })
        }
    }
    Connections {
        target: Qt.application

        onStateChanged: {
            console.info(
                "ASTRO_BACKGROUND_CAMERA: application state changed",
                Qt.application.state,
                "callVisible:", root.showingCallPage,
                "authenticated:", root.matrixAuthenticated
            )

            if (Qt.application.state === Qt.ApplicationActive) {
                if (root.matrixAuthenticated
                    && root.applicationWasBackgrounded) {
                    astroBackend.resumeMatrixSync()
                }

                root.applicationWasBackgrounded = false
            } else {
                if (root.matrixAuthenticated)
                    root.applicationWasBackgrounded = true
            }
        }
    }

    Settings {
        id: notificationPreferences
        category: "Notifications"
        property string privacyLevel: "sender_room"
        property bool pushNotificationsEnabled: true
        property bool bannerNotificationsEnabled: true

        onBannerNotificationsEnabledChanged: {
            if (!bannerNotificationsEnabled && notificationBanner)
                notificationBanner.clearNotifications()
        }
    }

    function handlePushNotification(payload) {
        if (!notificationPreferences.bannerNotificationsEnabled)
            return

        try {
            var envelope = JSON.parse(payload)
            var data = envelope.message || envelope

            if (typeof data === "string")
                data = JSON.parse(data)

            var notification = envelope.notification || {}
            var card = notification.card || {}

            notificationBanner.showNotification(
                card.summary || data.sender || "New message",
                card.body || data.body || "You have a new message",
                data.room_id || envelope.room_id || "",
                data.event_id || envelope.event_id || "",
                data.room_name || envelope.room_name || ""
            )
        } catch (error) {
            console.warn(
                "ASTRO: Could not parse push notification:",
                error
            )
        }
    }

    function handleNotificationUrl(argument) {
        console.warn("ASTRO PUSH TAP: argument:", String(argument))
        console.warn("ASTRO PUSH TAP: argument type:", typeof argument)

        var url = String(argument).trim()

        // Lomiri can deliver the same URI through the desktop launch
        // arguments and UriHandler during a cold start. Ignore only the
        // immediate duplicate; a later deliberate tap must still work.
        var now = new Date().getTime()
        if (url === root.lastNotificationUrl
            && now - root.lastNotificationUrlAt < 1500) {
            console.warn("ASTRO PUSH TAP: ignoring duplicate launch URI")
            return
        }
        root.lastNotificationUrl = url
        root.lastNotificationUrlAt = now

        if (url.indexOf("astro://open") !== 0) {
            console.warn("ASTRO PUSH TAP: URL prefix not recognised")
            return
        }

        var queryStart = url.indexOf("?")
        if (queryStart < 0)
            return

        var query = url.substring(queryStart + 1).split("#")[0]
        var parameters = query.split("&")
        var values = {}

        for (var i = 0; i < parameters.length; i++) {
            var pair = parameters[i].split("=")
            if (pair.length < 2)
                continue

            try {
                values[decodeURIComponent(pair[0])] =
                    decodeURIComponent(pair.slice(1).join("="))
            } catch (error) {
                console.warn("ASTRO: Invalid notification URL parameter")
            }
        }

        if (!values.room_id)
            return

        pendingNotificationRoomId = values.room_id
        pendingNotificationEventId = values.event_id || ""

        root.openPendingNotificationIfReady()
    }

    function handleLaunchArguments() {
        var args = Qt.application.arguments || []

        for (var i = 0; i < args.length; i++)
            root.handleNotificationUrl(args[i])
    }

    function openPendingNotificationIfReady() {
        if (!matrixAuthenticated
            || !matrixRoomsReady
            || !pendingNotificationRoomId)
            return

        var roomId = pendingNotificationRoomId
        var eventId = pendingNotificationEventId

        pendingNotificationRoomId = ""
        pendingNotificationEventId = ""

        console.warn("ASTRO PUSH TAP: opening room:", roomId)
        console.warn("ASTRO PUSH TAP: event:", eventId)

        // A notification must always land in the chat UI, even if
        // Astro was previously left on Settings or an active call page.
        root.showingSettings = false
        // Do not tear down or hide an active call just because a
        // message notification was tapped.
        chatPage.openNotification(roomId, eventId)
    }

    // =========================================================
    // Backend instance
    // =========================================================

    Backend {
        id: astroBackend
    }

    Component.onCompleted: {
        astroBackend.setPushPrivacyLevel(
            notificationPreferences.privacyLevel
        )
        astroBackend.setPushNotificationsEnabled(
            notificationPreferences.pushNotificationsEnabled
        )
        root.handleLaunchArguments()
    }

    Connections {
        target: UriHandler

        onOpened: {
            for (var i = 0; i < uris.length; i++)
                root.handleNotificationUrl(uris[i])
        }
    }

    // Register Astro with the Ubuntu Touch push service.
    NotificationPushClient {
        onPushTokenAvailable: {
            astroBackend.setPushToken(token)
        }

        onNotificationReceived: {
            root.handlePushNotification(payload)
        }
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

            // A failed network room sync has no final rooms response.
            // Stop the loading animation while retaining cached rooms.
            if (status.indexOf("Matrix room sync failed:") === 0) {
                chatPage.stopRoomLoading()
            }

            if (!astroBackend.storageReady) {
                root.matrixAuthenticated = false
                root.matrixRoomsReady = false
                chatPage.stopRoomLoading()
            }
        }

        // -----------------------------------------------------
        // Matrix session restoration
        // -----------------------------------------------------

        onMatrixSessionRestored: {
            root.matrixAuthenticated = restored
            root.matrixRoomsReady = false

            if (restored) {
                console.log(
                    "ASTRO: Session restored; fetching Matrix rooms"
                )

                if (astroBackend.refreshMatrixRooms()) {
                    chatPage.startRoomLoading()
                }
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
            root.matrixRoomsReady = false

            console.log(
                "ASTRO: matrixAuthenticated =",
                root.matrixAuthenticated
            )

            console.log(
                "ASTRO: Login complete; fetching Matrix rooms"
            )

            if (astroBackend.refreshMatrixRooms()) {
                chatPage.startRoomLoading()
            }
        }

        // -----------------------------------------------------
        // Matrix logout
        // -----------------------------------------------------

        onMatrixLogoutSucceeded: {
            root.applicationWasBackgrounded = false
            root.matrixAuthenticated = false
            root.matrixRoomsReady = false
            root.showingSettings = false
            root.showingCallPage = false
            root.callRoomId = ""
            root.callRoomName = ""

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

            root.applicationWasBackgrounded = false
            root.matrixAuthenticated = false
            root.matrixRoomsReady = false
            root.showingSettings = false
            root.showingCallPage = false
            root.callRoomId = ""
            root.callRoomName = ""
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
            root.matrixRoomsReady = true
            root.openPendingNotificationIfReady()
        }

        // -----------------------------------------------------
        onNotificationReceived: {
            if (!notificationPreferences.bannerNotificationsEnabled)
                return

            notificationBanner.showNotification(
                sender,
                body,
                roomId,
                eventId,
                chatPage.roomNameForId(roomId)
            )
        }

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

            // Incoming MatrixRTC rings are separate from ordinary message
            // notifications. Only surface them while Astro is foregrounded.
            var incomingCalls = data.incoming_calls || []
            if (Qt.application.state === Qt.ApplicationActive) {
                for (var callIndex = 0; callIndex < incomingCalls.length; callIndex++) {
                    var incomingCall = incomingCalls[callIndex]

                    // Do not cover an active call with a duplicate ring
                    // generated when another participant joins the room.
                    if (root.showingCallPage
                        && root.callRoomId === incomingCall.room_id) {
                        continue
                    }

                    incomingCallBanner.showIncomingCall(
                        incomingCall.sender,
                        incomingCall.room_id,
                        incomingCall.event_id,
                        incomingCall.call_intent
                    )
                }
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

                    if (event.state === "done")
                        settingsPage.refreshDevices()

                    continue
                }

                // Incoming or outgoing verification request.
                if (event.user_id && event.flow_id) {
                    if (!verificationDialog.visible) {
                        if (event.is_outgoing === true) {
                            verificationDialog.showOutgoingRequest(
                                event.user_id,
                                event.flow_id
                            )
                        } else {
                            verificationDialog.showRequest(
                                event.user_id,
                                event.flow_id,
                                event.room_id || ""
                            )
                        }
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
            notificationPreferences: notificationPreferences

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
                     !root.showingSettings &&
                     !root.showingCallPage
            z: root.showingCallPage ? 0 : 1
        }

        // =====================================================
        // Dedicated voice call page
        // =====================================================

        CallPage {
            id: callPage

            anchors.fill: parent

            backend: astroBackend
            roomId: root.callRoomId
            roomName: root.callRoomName
            directCall: chatPage.selectedRoomKind === "dm"
            incomingCall: root.incomingCall
            audioState: chatPage.rtcAudioStatesByRoom[root.callRoomId] || ({})
            membershipState: chatPage.rtcCallStatesByRoom[root.callRoomId] || ({})
            memberships: chatPage.rtcMembershipsByRoom[root.callRoomId] || []

            // Keep CallPage instantiated and rendered while it is behind
            // ChatPage. Its VideoOutput is also the source for the outgoing
            // camera-frame grabToImage() pipeline; making the whole page
            // invisible stops that pipeline even though MatrixRTC remains
            // connected. ChatPage is stacked above it when the call UI is
            // dismissed, so the camera keeps publishing without showing the
            // call interface.
            visible: astroBackend.storageReady &&
                     root.matrixAuthenticated
            enabled: root.showingCallPage
            z: root.showingCallPage ? 1 : 0

            onBackRequested: {
                root.showingCallPage = false
                root.incomingCall = false
            }
        }
    }

    // =========================================================
    // Matrix device verification dialog
    // =========================================================

    SoundEffect {
        id: incomingCallAlert
        source: astroBackend.rtcRingbackToneUrl()
        loops: SoundEffect.Infinite
        volume: 1.0
        property bool alertRequested: false

        onStatusChanged: {
            if (alertRequested && status === SoundEffect.Ready) {
                play()
                alertRequested = false
            }
        }
    }

    IncomingCallBanner {
        id: incomingCallBanner
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: units.gu(2)
        width: parent.width - units.gu(4)
        z: 2100

        onAlertRequested: {
            incomingCallAlert.alertRequested = true
            if (incomingCallAlert.status === SoundEffect.Ready) {
                incomingCallAlert.play()
                incomingCallAlert.alertRequested = false
            }
        }

        onVisibleChanged: {
            if (!visible)
                incomingCallAlert.stop()
        }

        onAcceptRequested: {
            incomingCallAlert.stop()
            root.showingSettings = false
            root.incomingCall = true
            root.openCallPage(
                roomId,
                chatPage.roomNameForId(roomId),
                true
            )
        }

        onDeclineRequested: {
            incomingCallAlert.stop()
            astroBackend.declineRtcCall(roomId, eventId)
        }
    }

    NotificationBanner {
        id: notificationBanner
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: incomingCallBanner.visible
            ? incomingCallBanner.height + units.gu(3)
            : units.gu(2)
        width: parent.width - units.gu(4)
        z: 2000

        onOpenRequested: {
            chatPage.openNotification(roomId, eventId)
        }
    }

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
