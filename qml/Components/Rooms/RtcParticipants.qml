import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property var memberships: []
    property var avatarCache: ({})
    property var backend
    property string roomId: ""
    property var callState: ({})
    property bool expanded: false
    property bool actionPending: false
    property bool pendingMembershipValue: false
    property var membershipOverride: null
    property string actionError: ""

    signal joinCallRequested()


    readonly property bool joinedByMe:
        membershipOverride === null
            ? callState.joinedByMe === true
            : membershipOverride === true

    function performRtcAction() {
        if (actionPending || !backend || !roomId)
            return

        actionError = ""
        pendingMembershipValue = !joinedByMe
        actionPending = true

        if (joinedByMe)
            backend.leaveRtcRoom(roomId)
        else
            backend.joinRtcRoom(roomId)
    }

    onRoomIdChanged: {
        actionPending = false
        pendingMembershipValue = false
        membershipOverride = null
        actionError = ""
        expanded = false
    }

    onCallStateChanged: {
        if (membershipOverride !== null &&
                callState.joinedByMe === (membershipOverride === true))
            membershipOverride = null
    }

    Connections {
        target: root.backend
        ignoreUnknownSignals: true

        onMatrixRtcActionFinished: {
            if (roomId !== root.roomId || !root.actionPending)
                return

            root.actionPending = false
            root.actionError = error || ""

            if (root.actionError.length > 0) {
                root.membershipOverride = null
                root.expanded = true
            } else {
                root.membershipOverride =
                    root.callState.joinedByMe === root.pendingMembershipValue
                        ? null : root.pendingMembershipValue
            }
        }
    }

    function uniqueParticipants() {
        var result = []
        var seen = ({})

        for (var i = 0; i < memberships.length; i++) {
            var item = memberships[i]
            var sender = item.sender || ""

            if (!sender || seen[sender])
                continue

            seen[sender] = true
            result.push(item)
        }

        return result
    }

    property var participants: uniqueParticipants()

    height: participants.length === 0 &&
            !(backend && roomId.length > 0) && !joinedByMe
            ? 0
            : expanded
              ? units.gu(16) +
                Math.min(units.gu(25),
                         participants.length * units.gu(5)) +
                (actionError.length > 0 ? units.gu(3) : 0)
              : units.gu(4)

    visible: participants.length > 0 ||
             (backend && roomId.length > 0) || joinedByMe
    clip: true

    Rectangle {
        anchors.fill: parent
        color: "#1b2935"
    }

    Row {
        id: participantStrip

        x: units.gu(2)
        y: 0
        height: units.gu(4)
        spacing: -units.gu(0.8)

        Repeater {
            model: Math.min(root.participants.length, 7)

            delegate: Rectangle {
                width: units.gu(3.2)
                height: units.gu(3.2)
                radius: width / 2
                color: "#426b8a"
                border.width: units.gu(0.2)
                border.color: "#1b2935"
                clip: true

                property var participant: root.participants[index]
                property string name:
                    participant.display_name || participant.sender || "?"

                Text {
                    anchors.centerIn: parent
                    text: parent.name.length
                          ? parent.name.charAt(0).toUpperCase()
                          : "?"
                    color: "white"
                    font.bold: true
                    font.pixelSize: units.gu(1.4)
                }

                Image {
                    anchors.fill: parent
                    source: root.avatarCache[parent.participant.avatar_url || ""] || ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    visible: status === Image.Ready
                }
            }
        }

        Rectangle {
            visible: root.participants.length > 7
            width: units.gu(3.2)
            height: units.gu(3.2)
            radius: width / 2
            color: "#354b5b"
            border.width: units.gu(0.2)
            border.color: "#1b2935"

            Text {
                anchors.centerIn: parent
                text: "+" + (root.participants.length - 7)
                color: "white"
                font.pixelSize: units.gu(1.2)
                font.bold: true
            }
        }

        Text {
            height: units.gu(3.2)
            verticalAlignment: Text.AlignVCenter
            leftPadding: units.gu(1)
            text: root.participants.length +
                  (root.participants.length === 1
                   ? " in call" : " in call")
            color: "#c5d3dc"
            font.pixelSize: units.gu(1.4)
        }
    }

    MouseArea {
        anchors.fill: parent
        z: 1
        onClicked: root.expanded = !root.expanded
    }

    Rectangle {
        id: participantPanel

        x: units.gu(1)
        y: units.gu(4)
        width: parent.width - units.gu(2)
        height: parent.height - units.gu(4)

        radius: units.gu(1)
        color: "#263b4a"
        visible: root.expanded
        z: 2

        Column {
            anchors.fill: parent
            spacing: 0

            Row {
                width: parent.width
                height: units.gu(3)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: units.gu(1)
                    text: "People in this call"
                    color: "white"
                    font.bold: true
                    font.pixelSize: units.gu(1.5)
                }

                Item {
                    width: parent.width -
                           units.gu(18)
                    height: 1
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "×"
                    color: "#c5d3dc"
                    font.pixelSize: units.gu(2.5)
                }
            }

            Rectangle {
                width: parent.width
                height: units.gu(4)
                radius: units.gu(0.5)
                color: root.joinedByMe ? "#315d48" : "#31516a"

                Text {
                    anchors.centerIn: parent
                    text: root.actionPending
                          ? "Updating call presence..."
                          : root.joinedByMe
                            ? "Leave Call"
                            : "Join Call"
                    color: "white"
                    font.pixelSize: units.gu(1.4)
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !root.actionPending &&
                             !!root.backend && root.roomId.length > 0
                    onClicked: {
                        if (root.joinedByMe)
                            root.performRtcAction()
                        else
                            root.joinCallRequested()
                    }
                }
            }

            Text {
                width: parent.width
                height: root.actionError.length > 0
                       ? units.gu(3) : 0
                visible: root.actionError.length > 0
                text: root.actionError
                color: "#ffaaa5"
                font.pixelSize: units.gu(1.15)
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            ListView {
                id: participantList

                width: parent.width
                height: parent.height -
                        units.gu(7) -
                        (root.actionError.length > 0 ? units.gu(3) : 0)
                clip: true
                model: root.participants
                spacing: 0

                delegate: Row {
                    width: participantList.width
                    height: units.gu(5)
                    spacing: units.gu(1)

                    property var participant: modelData
                    property string name:
                        participant.display_name ||
                        participant.sender || "Unknown participant"

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: units.gu(3.5)
                        height: units.gu(3.5)
                        radius: width / 2
                        color: "#426b8a"
                        clip: true

                        Text {
                            anchors.centerIn: parent
                            text: parent.parent.name.charAt(0).toUpperCase()
                            color: "white"
                            font.bold: true
                        }

                        Image {
                            anchors.fill: parent
                            source: root.avatarCache[parent.parent.participant.avatar_url || ""] || ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            visible: status === Image.Ready
                        }
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - units.gu(5)
                        spacing: units.gu(0.2)

                        Text {
                            width: parent.width
                            text: parent.parent.name
                            color: "white"
                            font.pixelSize: units.gu(1.5)
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: parent.parent.participant.origin &&
                                  parent.parent.participant.origin.claimed
                                  ? "Device: " +
                                    (parent.parent.participant.origin.claimed.device_id || "Unknown")
                                  : parent.parent.participant.sender || ""
                            color: "#a9bac6"
                            font.pixelSize: units.gu(1.1)
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
}
