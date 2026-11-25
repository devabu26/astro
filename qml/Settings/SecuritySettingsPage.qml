import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend

    property bool dangerZoneExpanded: false

    readonly property color pageColor: "#101820"
    readonly property color panelColor: "#1b2935"
    readonly property color raisedColor: "#223342"
    readonly property color accentColor: "#a78bfa"
    readonly property color primaryText: "#f4f6f8"
    readonly property color secondaryText: "#a8b7c5"
    readonly property color primaryButtonColor: "#8069c4"
    readonly property color secondaryButtonColor: "#385267"
    readonly property color verificationButtonColor: "#286b65"
    readonly property color dangerButtonColor: "#854653"
    readonly property color cancelButtonColor: "#344451"

    implicitHeight: securityContent.height

    property var matrixDevices: []
    property bool matrixDevicesLoading: false
    property string matrixDevicesError: ""
    property string deviceActionMessage: ""

    property string devicePendingRevocationId: ""
    property string devicePendingRevocationName: ""
    property bool deviceRevocationInProgress: false
    property string deviceRevocationError: ""

    function refreshDevices() {
        if (!backend)
            return

        matrixDevicesLoading = true
        matrixDevicesError = ""
        deviceActionMessage = ""
        backend.refreshMatrixDevices()
    }

    function beginDeviceRevocation(device) {
        if (!device || device.is_current || !backend)
            return

        devicePendingRevocationId = device.device_id
        devicePendingRevocationName =
            device.display_name || device.device_id

        deviceRevocationError = ""
        deviceRevocationPassword.text = ""
        deviceRevocationInProgress = true

        deviceRevocationOverlay.visible = false
        deviceActionMessage = "Checking homeserver device-management support…"

        backend.checkMatrixDeviceManagementUrl(
            devicePendingRevocationId
        )
    }

    function submitDeviceRevocation() {
        if (!backend ||
                devicePendingRevocationId === "" ||
                deviceRevocationPassword.text.length === 0 ||
                deviceRevocationInProgress)
            return

        deviceRevocationError = ""
        deviceRevocationInProgress = true

        var password = deviceRevocationPassword.text
        deviceRevocationPassword.text = ""

        backend.revokeMatrixDevice(
            devicePendingRevocationId,
            password
        )
    }

    function deviceVerificationStatus(device) {
        if (device.is_verified === true)
            return "Verified"

        if (device.is_verified === false)
            return "Not verified"

        return "Verification unavailable"
    }

    function deviceVerificationColour(device) {
        if (device.is_verified === true)
            return "#70d69b"

        if (device.is_verified === false)
            return "#e5b96f"

        return root.secondaryText
    }

    function formatDeviceLastSeen(timestamp) {
        if (timestamp === null ||
                typeof timestamp === "undefined")
            return "Last seen: Unknown"

        var date = new Date(Number(timestamp))

        if (isNaN(date.getTime()))
            return "Last seen: Unknown"

        function pad(value) {
            return value < 10 ? "0" + value : value
        }

        return "Last seen: " +
            date.getFullYear() + "-" +
            pad(date.getMonth() + 1) + "-" +
            pad(date.getDate()) + " " +
            pad(date.getHours()) + ":" +
            pad(date.getMinutes())
    }


    property string devicePendingRenameId: ""
    property string devicePendingRenameName: ""
    property bool deviceRenameInProgress: false
    property string deviceRenameError: ""

    function beginDeviceRename(device) {
        if (!device)
            return

        devicePendingRenameId = device.device_id
        devicePendingRenameName = device.display_name || ""
        deviceRenameError = ""
        deviceRenameInProgress = false
        deviceRenameOverlay.visible = true
        deviceRenameInput.text = devicePendingRenameName
        deviceRenameInput.forceActiveFocus()
    }

    function submitDeviceRename() {
        if (!backend ||
                devicePendingRenameId === "" ||
                deviceRenameInput.text.trim().length === 0 ||
                deviceRenameInProgress)
            return

        deviceRenameError = ""
        deviceRenameInProgress = true

        backend.renameMatrixDevice(
            devicePendingRenameId,
            deviceRenameInput.text.trim()
        )
    }


    Component.onCompleted: refreshDevices()

    Connections {
        target: root.backend

        onMatrixDeviceManagementUrlReady: {
            if (deviceId !== root.devicePendingRevocationId)
                return

            root.deviceRevocationInProgress = false
            root.deviceActionMessage = ""

            if (error !== "") {
                root.deviceActionMessage = error
                root.devicePendingRevocationId = ""
                root.devicePendingRevocationName = ""
                return
            }

            if (url !== "") {
                root.devicePendingRevocationId = ""
                root.devicePendingRevocationName = ""

                if (!Qt.openUrlExternally(url)) {
                    root.deviceActionMessage =
                        "Could not open the homeserver account-management page."
                } else {
                    root.deviceActionMessage =
                        "Opened the homeserver device-management page."
                }

                return
            }

            // Legacy homeserver: use the existing password/UIAA dialog.
            root.deviceRevocationOverlay.visible = true
            deviceRevocationPassword.forceActiveFocus()
        }

        onMatrixDevicesRetrieved: {
            root.matrixDevicesLoading = false

            if (error !== "") {
                root.matrixDevicesError = error
                root.matrixDevices = []
                return
            }

            try {
                var result = JSON.parse(json || "{}")
                root.matrixDevices = result.devices || []
                root.matrixDevicesError = ""
            } catch (parseError) {
                root.matrixDevices = []
                root.matrixDevicesError =
                    "Could not read the registered device list."
                console.warn(
                    "ASTRO: Could not parse Matrix devices:",
                    parseError
                )
            }
        }

        onMatrixDeviceVerificationFinished: {
            if (error !== "") {
                root.deviceActionMessage = error
            } else {
                root.deviceActionMessage =
                    "Verification request sent. Waiting for the other device."
            }
        }

        onMatrixDeviceRenameFinished: {
            root.deviceRenameInProgress = false

            if (error !== "") {
                root.deviceRenameError = error
                return
            }

            root.deviceRenameOverlay.visible = false
            root.devicePendingRenameId = ""
            root.devicePendingRenameName = ""
            root.deviceRenameError = ""
            root.deviceActionMessage = "Device name updated."
            root.refreshDevices()
        }

        onMatrixDeviceRevocationFinished: {
            root.deviceRevocationInProgress = false

            if (error !== "") {
                root.deviceRevocationError = error
                return
            }

            root.deviceRevocationOverlay.visible = false
            root.devicePendingRevocationId = ""
            root.devicePendingRevocationName = ""
            root.deviceRevocationError = ""
            root.deviceActionMessage = "Device signed out successfully."
            root.refreshDevices()
        }
    }

    Column {
        id: securityContent
        width: parent.width
        spacing: units.gu(1)

                        // PRIVACY AND SECURITY
                        Column {
                            width: parent.width
                            spacing: units.gu(1)

                            Text {
                                text: "BACKGROUND ACTIVITY"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Rectangle {
                                width: parent.width
                                height: backgroundActivityInfo.height +
                                        units.gu(3)
                                radius: units.gu(1.5)
                                color: root.panelColor

                                Column {
                                    id: backgroundActivityInfo
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: units.gu(2)
                                    spacing: units.gu(1)

                                    Text {
                                        width: parent.width
                                        text: "Allow Astro to run in the background"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(1.8)
                                        font.bold: true
                                        wrapMode: Text.WordWrap
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Prevent Ubuntu Touch from suspending Astro when you switch apps or turn off the screen. This may increase battery and data usage."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.35)
                                        wrapMode: Text.WordWrap
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Background permission is managed by Ubuntu Touch. Check its current state in UT Tweak Tool."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.4)
                                        wrapMode: Text.WordWrap
                                    }

                                    Text {
                                        id: utTweaksLink
                                        text: "Open UT Tweak Tool"
                                        color: "#8ecbff"
                                        font.pixelSize: units.gu(1.45)
                                        font.underline: true

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: Qt.openUrlExternally(
                                                "https://open-store.io/app/ut-tweak-tool.sverzegnassi"
                                            )
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(9)
                                radius: units.gu(1.5)
                                color: root.panelColor

                                Column {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: units.gu(0.5)

                                    Text {
                                        text: "Encrypted Astro storage"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(1.8)
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Lock your local storage to secure your Astro data."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.4)
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Button {
                                width: parent.width
                                text: "Lock Astro"
                                color: root.primaryButtonColor

                                onClicked: {
                                    if (root.backend)
                                        root.backend.closeStorage()
                                }
                            }

                            Item {
                                width: 1
                                height: units.gu(1)
                            }

                            Text {
                                text: "MATRIX SECURITY"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Text {
                                width: parent.width
                                text: "REGISTERED DEVICES"
                                color: root.primaryText
                                font.pixelSize: units.gu(1.8)
                                font.bold: true
                            }

                            Text {
                                width: parent.width
                                text: "Sessions registered to your Matrix account. Last-seen information does not indicate whether a device is currently online."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.4)
                                wrapMode: Text.WordWrap
                            }

                            Button {
                                width: parent.width
                                text: root.matrixDevicesLoading
                                      ? "Refreshing devices…"
                                      : "Refresh registered devices"
                                color: root.secondaryButtonColor
                                enabled: !root.matrixDevicesLoading

                                onClicked: root.refreshDevices()
                            }

                            Text {
                                width: parent.width
                                visible: root.matrixDevicesLoading
                                text: "Retrieving registered devices…"
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.4)
                            }

                            Text {
                                width: parent.width
                                visible: root.matrixDevicesError !== ""
                                text: root.matrixDevicesError
                                color: "#ff8585"
                                font.pixelSize: units.gu(1.4)
                                wrapMode: Text.WordWrap
                            }

                            Text {
                                width: parent.width
                                visible: !root.matrixDevicesLoading &&
                                         root.matrixDevicesError === "" &&
                                         root.matrixDevices.length === 0
                                text: "No registered devices were returned."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.4)
                            }

                            Repeater {
                                model: root.matrixDevices

                                delegate: Rectangle {
                                    width: parent.width
                                    height: deviceInfo.height +
                                            units.gu(3)
                                    radius: units.gu(1.5)
                                    color: root.panelColor

                                    Column {
                                        id: deviceInfo

                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: units.gu(1.5)
                                        spacing: units.gu(0.7)

                                        Text {
                                            width: parent.width
                                            text: modelData.display_name ||
                                                  modelData.device_id
                                            color: root.primaryText
                                            font.pixelSize: units.gu(1.8)
                                            font.bold: true
                                            wrapMode: Text.Wrap
                                        }

                                        Text {
                                            width: parent.width
                                            text: modelData.device_id
                                            color: root.secondaryText
                                            font.pixelSize: units.gu(1.3)
                                            wrapMode: Text.Wrap
                                        }

                                        Text {
                                            width: parent.width
                                            text: modelData.is_current
                                                  ? "Current Astro session"
                                                  : "Registered session"
                                            color: root.accentColor
                                            font.pixelSize: units.gu(1.35)
                                        }

                                        Text {
                                            width: parent.width
                                            text: root.deviceVerificationStatus(
                                                      modelData)
                                            color: root.deviceVerificationColour(
                                                       modelData)
                                            font.pixelSize: units.gu(1.45)
                                            font.bold: true
                                        }

                                        Text {
                                            width: parent.width
                                            text: root.formatDeviceLastSeen(
                                                      modelData.last_seen_ts)
                                            color: root.secondaryText
                                            font.pixelSize: units.gu(1.3)
                                        }

                                        Button {
                                            width: parent.width
                                            text: "Edit device name"
                                            color: root.secondaryButtonColor

                                            onClicked:
                                                root.beginDeviceRename(modelData)
                                        }

                                        Button {
                                            width: parent.width

                                            visible: !modelData.is_current &&
                                                     modelData.is_verified === false

                                            text: "Verify this device"
                                            color: root.verificationButtonColor

                                            onClicked: {
                                                root.deviceActionMessage = ""

                                                if (root.backend)
                                                    root.backend.requestDeviceVerification(
                                                        modelData.device_id)
                                            }
                                        }

                                        Button {
                                            width: parent.width
                                            visible: !modelData.is_current
                                            text: "Sign out device"
                                            color: root.dangerButtonColor

                                            onClicked:
                                                root.beginDeviceRevocation(modelData)
                                        }
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                visible: root.deviceActionMessage !== ""
                                text: root.deviceActionMessage
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                wrapMode: Text.WordWrap
                            }

                            Item {
                                width: 1
                                height: units.gu(2)
                            }

                            Button {
                                width: parent.width
                                text: root.dangerZoneExpanded
                                      ? "DANGER ZONE     ▲"
                                      : "DANGER ZONE     ▼"
                                color: root.dangerButtonColor

                                onClicked:
                                    root.dangerZoneExpanded =
                                        !root.dangerZoneExpanded
                            }

                            Column {
                                width: parent.width
                                visible: root.dangerZoneExpanded
                                spacing: units.gu(1.5)

                                Button {
                                    width: parent.width
                                    text: "Delete Everything"
                                    color: "#000000"

                                    onClicked: resetOverlay.visible = true
                                }

                                Text {
                                    width: parent.width
                                    text: "Permanently deletes encrypted Astro storage, your Matrix session and local Matrix data."
                                    color: root.secondaryText
                                    font.pixelSize: units.gu(1.35)
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
    }

    // Delete confirmation overlay
    Rectangle {
        id: resetOverlay

        anchors.fill: parent
        color: "#000000"
        opacity: 0.78
        visible: false
        z: 100

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        id: resetConfirmation

        width: Math.min(parent.width - units.gu(6), units.gu(58))
        height: units.gu(40)
        anchors.centerIn: parent
        radius: units.gu(2)
        color: root.panelColor
        border.color: "#596a78"
        border.width: 1
        visible: resetOverlay.visible
        z: 101

        Column {
            anchors.fill: parent
            anchors.margins: units.gu(3)
            spacing: units.gu(2)

            Text {
                width: parent.width
                text: "Delete all Astro data?"
                color: root.primaryText
                font.pixelSize: units.gu(2.5)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: "This will permanently delete your encrypted Astro storage, Matrix session and local Matrix data. You will need to sign in again.\n\nThis action cannot be undone."
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
                color: root.cancelButtonColor

                onClicked: resetOverlay.visible = false
            }

            Button {
                width: parent.width
                text: "Delete Everything"
                color: root.dangerButtonColor

                onClicked: {
                    resetOverlay.visible = false

                    if (root.backend)
                        root.backend.deleteAllData()
                }
            }
        }
    }

    // Matrix device revocation confirmation
    Rectangle {
        id: deviceRevocationOverlay

        anchors.fill: parent
        color: "#000000"
        opacity: 0.78
        visible: false
        z: 110

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        id: deviceRevocationConfirmation

        width: Math.min(parent.width - units.gu(6), units.gu(58))
        height: units.gu(39)
        anchors.centerIn: parent
        radius: units.gu(2)
        color: root.panelColor
        border.color: "#596a78"
        border.width: 1
        visible: deviceRevocationOverlay.visible
        z: 111

        Column {
            anchors.fill: parent
            anchors.margins: units.gu(3)
            spacing: units.gu(2)

            Text {
                width: parent.width
                text: "Sign out this device?"
                color: root.primaryText
                font.pixelSize: units.gu(2.4)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: "This will sign out " +
                      root.devicePendingRevocationName +
                      " from your Matrix account. The device will need to sign in again."
                color: "#c4ced6"
                font.pixelSize: units.gu(1.6)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: "MATRIX ACCOUNT PASSWORD"
                color: root.secondaryText
                font.pixelSize: units.gu(1.35)
                font.bold: true
            }

            TextField {
                id: deviceRevocationPassword

                width: parent.width
                placeholderText: "Enter your Matrix password"
                echoMode: TextInput.Password
                enabled: !root.deviceRevocationInProgress

                onAccepted: root.submitDeviceRevocation()
            }

            Text {
                width: parent.width
                visible: root.deviceRevocationError !== ""
                text: root.deviceRevocationError
                color: "#ff8585"
                font.pixelSize: units.gu(1.4)
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                text: "Cancel"
                color: root.cancelButtonColor
                enabled: !root.deviceRevocationInProgress

                onClicked: {
                    deviceRevocationOverlay.visible = false
                    deviceRevocationPassword.text = ""
                    root.devicePendingRevocationId = ""
                    root.devicePendingRevocationName = ""
                    root.deviceRevocationError = ""
                }
            }

            Button {
                id: revokeDeviceButton

                width: parent.width
                text: root.deviceRevocationInProgress
                      ? "Signing out…"
                      : "Sign out device"
                color: root.dangerButtonColor

                enabled: !root.deviceRevocationInProgress &&
                         deviceRevocationPassword.text.length > 0

                onClicked: root.submitDeviceRevocation()
            }
        }
    }
    // Matrix device rename dialog
    Rectangle {
        id: deviceRenameOverlay
        anchors.fill: parent
        color: "#000000"
        opacity: 0.78
        visible: false
        z: 120

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        width: Math.min(parent.width - units.gu(6), units.gu(58))
        height: units.gu(31)
        anchors.centerIn: parent
        radius: units.gu(2)
        color: root.panelColor
        border.color: "#596a78"
        border.width: 1
        visible: deviceRenameOverlay.visible
        z: 121

        Column {
            anchors.fill: parent
            anchors.margins: units.gu(3)
            spacing: units.gu(2)

            Text {
                width: parent.width
                text: "Rename Matrix device"
                color: root.primaryText
                font.pixelSize: units.gu(2.4)
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: "Choose a display name for this registered session."
                color: "#c4ced6"
                font.pixelSize: units.gu(1.6)
                wrapMode: Text.WordWrap
            }

            TextField {
                id: deviceRenameInput
                width: parent.width
                placeholderText: "Device display name"
                enabled: !root.deviceRenameInProgress
                onAccepted: root.submitDeviceRename()
            }

            Text {
                width: parent.width
                visible: root.deviceRenameError !== ""
                text: root.deviceRenameError
                color: "#ff8585"
                font.pixelSize: units.gu(1.4)
                wrapMode: Text.WordWrap
            }

            Button {
                width: parent.width
                text: "Cancel"
                color: root.cancelButtonColor
                enabled: !root.deviceRenameInProgress

                onClicked: {
                    deviceRenameOverlay.visible = false
                    deviceRenameInput.text = ""
                    root.devicePendingRenameId = ""
                    root.devicePendingRenameName = ""
                    root.deviceRenameError = ""
                }
            }

            Button {
                width: parent.width
                text: root.deviceRenameInProgress
                      ? "Saving name…"
                      : "Save name"
                color: root.primaryButtonColor
                enabled: !root.deviceRenameInProgress &&
                         deviceRenameInput.text.trim().length > 0

                onClicked: root.submitDeviceRename()
            }
        }
    }

}
