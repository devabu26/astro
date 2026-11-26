import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string caller: ""
    property string roomId: ""
    property string eventId: ""
    property string callIntent: "audio"

    signal answerRequested(string roomId, string eventId)

    visible: roomId !== ""
    height: visible ? banner.implicitHeight : 0

    function showIncomingCall(sender, incomingRoomId, incomingEventId, intent) {
        caller = sender || "Someone"
        roomId = incomingRoomId || ""
        eventId = incomingEventId || ""
        callIntent = intent === "video" ? "video" : "audio"
        expiryTimer.restart()
    }

    function clear() {
        expiryTimer.stop()
        caller = ""
        roomId = ""
        eventId = ""
        callIntent = "audio"
    }

    Timer {
        id: expiryTimer
        interval: 30000
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
                text: root.callIntent === "video" ? "Incoming video call" : "Incoming voice call"
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
                    text: "Answer"
                    color: LomiriColors.green
                    onClicked: {
                        root.answerRequested(root.roomId, root.eventId)
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
