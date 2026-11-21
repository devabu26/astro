import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property int nextNotificationId: 1

    signal openRequested(string roomId, string eventId)

    visible: notificationModel.count > 0
    height: bannerColumn.implicitHeight

    ListModel {
        id: notificationModel
    }

    function clearNotifications() {
        notificationModel.clear()
    }

    function showNotification(title, body, roomId, eventId, roomName) {
        // Keep the stack compact rather than covering the conversation.
        while (notificationModel.count >= 4)
            notificationModel.remove(0)

        notificationModel.append({
            notificationId: root.nextNotificationId++,
            title: title || "New message",
            body: body || "",
            roomId: roomId || "",
            eventId: eventId || "",
            roomName: roomName || ""
        })
    }

    Column {
        id: bannerColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: units.gu(1)

        Repeater {
            model: notificationModel

            delegate: Rectangle {
                id: card

                property string cardId: String(model.notificationId)

                width: root.width
                height: cardContent.implicitHeight + units.gu(3)

                radius: units.gu(1)
                color: "#303b46"
                border.width: units.dp(1)
                border.color: "#536574"
                clip: true

                function dismiss(direction) {
                    hideTimer.stop()
                    dismissAnimation.to =
                        direction < 0 ? -card.width : card.width
                    dismissAnimation.start()
                }

                function removeFromModel() {
                    for (var i = 0; i < notificationModel.count; i++) {
                        if (String(
                            notificationModel.get(i).notificationId
                        ) === card.cardId) {
                            notificationModel.remove(i)
                            return
                        }
                    }
                }

                Timer {
                    id: hideTimer
                    interval: 7000
                    repeat: false
                    running: true
                    onTriggered: card.dismiss(1)
                }

                NumberAnimation {
                    id: dismissAnimation
                    target: card
                    property: "x"
                    duration: 170
                    easing.type: Easing.OutCubic

                    onStopped: card.removeFromModel()
                }

                NumberAnimation {
                    id: snapBackAnimation
                    target: card
                    property: "x"
                    to: 0
                    duration: 150
                    easing.type: Easing.OutCubic
                }

                Column {
                    id: cardContent
                    anchors.fill: parent
                    anchors.margins: units.gu(1.5)
                    spacing: units.gu(0.5)

                    Text {
                        width: parent.width
                        text: model.title
                        color: "white"
                        font.bold: true
                        font.pixelSize: units.gu(1.8)
                        wrapMode: Text.Wrap
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        visible: model.roomName !== ""
                        text: model.roomName
                        color: "#b9cbd8"
                        font.pixelSize: units.gu(1.45)
                        wrapMode: Text.NoWrap
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: model.body
                        color: "#f0f3f5"
                        font.pixelSize: units.gu(1.6)
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        visible: model.roomId !== "" && model.eventId !== ""
                        text: "Tap to open message  ·  Swipe to dismiss"
                        color: "#a9bac7"
                        font.pixelSize: units.gu(1.2)
                    }
                }

                MouseArea {
                    anchors.fill: parent

                    drag.target: card
                    drag.axis: Drag.XAxis
                    drag.minimumX: -card.width
                    drag.maximumX: card.width
                    drag.threshold: units.gu(1)

                    onPressed: {
                        hideTimer.stop()
                        snapBackAnimation.stop()
                        dismissAnimation.stop()
                    }

                    onReleased: {
                        if (Math.abs(card.x) > card.width * 0.28) {
                            card.dismiss(card.x < 0 ? -1 : 1)
                        } else if (card.x !== 0) {
                            snapBackAnimation.start()
                            hideTimer.restart()
                        } else {
                            hideTimer.restart()
                        }
                    }

                    onClicked: {
                        if (card.x !== 0)
                            return

                        root.openRequested(
                            model.roomId,
                            model.eventId
                        )

                        card.dismiss(1)
                    }
                }
            }
        }
    }
}
