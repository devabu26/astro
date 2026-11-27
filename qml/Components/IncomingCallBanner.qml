import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string caller: ""
    property string roomId: ""
    property string eventId: ""
    property string callIntent: "unknown"
    property double expiresAtMs: 0

    signal acceptRequested(string roomId, string eventId)
    signal declineRequested(string roomId, string eventId)
    signal alertRequested()

    visible: roomId !== ""
    height: visible ? banner.height : 0

    function showIncomingCall(sender, incomingRoomId, incomingEventId, intent, incomingExpiryMs) {
        var expiry = Number(incomingExpiryMs) || 0
        if (expiry > 0 && expiry <= Date.now())
            return

        caller = sender || "Someone"
        roomId = incomingRoomId || ""
        eventId = incomingEventId || ""
        callIntent = intent === "video" || intent === "audio" ? intent : "unknown"
        expiresAtMs = expiry

        // Follow the MatrixRTC notification lifetime rather than a fixed UI
        // timeout. A missing expiry is left visible until dismissed or handled.
        expiryTimer.stop()
        if (expiresAtMs > 0) {
            expiryTimer.interval = Math.max(1, expiresAtMs - Date.now())
            expiryTimer.start()
        }
        alertRequested()
    }

    function clear() {
        expiryTimer.stop()
        caller = ""
        roomId = ""
        eventId = ""
        callIntent = "unknown"
        expiresAtMs = 0
    }

    Timer {
        id: expiryTimer
        repeat: false
        onTriggered: root.clear()
    }

    Rectangle {
        id: banner
        anchors.left: parent.left
        anchors.right: parent.right
        height: contentColumn.implicitHeight + units.gu(3)
        radius: units.gu(1)
        color: "#263b4a"
        border.width: units.dp(1)
        border.color: "#4e819e"

        Column {
            id: contentColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: units.gu(1.5)
            spacing: units.gu(1)

            Label {
                width: parent.width
                text: root.callIntent === "video" ? "Incoming video call" :
                    (root.callIntent === "audio" ? "Incoming voice call" : "Incoming call")
                color: "white"
                font.bold: true
                fontSize: "large"
                wrapMode: Text.NoWrap
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                text: root.caller
                color: "#d5e5ee"
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Row {
                spacing: units.gu(1)

                Button {
                    text: "Accept"
                    color: UbuntuColors.green
                    onClicked: {
                        root.acceptRequested(root.roomId, root.eventId)
                        root.clear()
                    }
                }

                Button {
                    text: "Decline"
                    color: UbuntuColors.red
                    onClicked: {
                        root.declineRequested(root.roomId, root.eventId)
                        root.clear()
                    }
                }

                Button {
                    text: "Dismiss"
                    onClicked: root.clear()
                }
            }
        }
    }
}
