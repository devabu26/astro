import QtQuick 2.7
import Qt.labs.settings 1.0
import Lomiri.Components 1.3

Item {
    id: root

    property var backend
    property var notificationPreferences

    readonly property color pageColor: "#101820"
    readonly property color panelColor: "#1b2935"
    readonly property color raisedColor: "#223342"
    readonly property color accentColor: "#a78bfa"
    readonly property color primaryText: "#f4f6f8"
    readonly property color secondaryText: "#a8b7c5"

    implicitHeight: contentColumn.height

    property var privacyOptions: [
        {
            value: "sender_room",
            title: "Sender and room only",
            description: "Show who sent the message and which room it belongs to. Message contents are not included."
        },
        {
            value: "full_message",
            title: "Full message preview",
            description: "Show the sender, room and message text in the notification."
        },
        {
            value: "private",
            title: "Private",
            description: "Show a generic new-message notification without sender or room details."
        }
    ]

    Column {
        id: contentColumn
        width: parent.width
        spacing: units.gu(2)

        Text {
            width: parent.width
            text: "Notification delivery"
            color: root.primaryText
            font.pixelSize: units.gu(2.2)
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Rectangle {
            width: parent.width
            height: pushContent.height + units.gu(3)
            radius: units.gu(1)
            color: root.panelColor
            border.width: units.dp(1)
            border.color: "#344653"

            Column {
                id: pushContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: units.gu(1.5)
                spacing: units.gu(0.6)

                Row {
                    width: parent.width
                    spacing: units.gu(1)

                    Column {
                        width: parent.width - pushSwitch.width - units.gu(1)
                        spacing: units.gu(0.3)

                        Text {
                            width: parent.width
                            text: "Push notifications"
                            color: root.primaryText
                            font.pixelSize: units.gu(1.8)
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            width: parent.width
                            text: "Allow the Matrix server to send notifications through the Ubuntu push service, including when Astro is in the background or closed."
                            color: root.secondaryText
                            font.pixelSize: units.gu(1.5)
                            wrapMode: Text.WordWrap
                        }
                    }

                    Switch {
                        id: pushSwitch
                        checked: root.notificationPreferences.pushNotificationsEnabled

                        onCheckedChanged: {
                            if (root.notificationPreferences
                                    && checked !== root.notificationPreferences.pushNotificationsEnabled) {
                                root.notificationPreferences.pushNotificationsEnabled = checked

                                if (root.backend)
                                    root.backend.setPushNotificationsEnabled(checked)
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: bannerContent.height + units.gu(3)
            radius: units.gu(1)
            color: root.panelColor
            border.width: units.dp(1)
            border.color: "#344653"

            Column {
                id: bannerContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: units.gu(1.5)
                spacing: units.gu(0.6)

                Row {
                    width: parent.width
                    spacing: units.gu(1)

                    Column {
                        width: parent.width - bannerSwitch.width - units.gu(1)
                        spacing: units.gu(0.3)

                        Text {
                            width: parent.width
                            text: "Banner notifications"
                            color: root.primaryText
                            font.pixelSize: units.gu(1.8)
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            width: parent.width
                            text: "Show notification banners inside Astro while you are using the app."
                            color: root.secondaryText
                            font.pixelSize: units.gu(1.5)
                            wrapMode: Text.WordWrap
                        }
                    }

                    Switch {
                        id: bannerSwitch
                        checked: root.notificationPreferences.bannerNotificationsEnabled

                        onCheckedChanged: {
                            if (root.notificationPreferences
                                    && checked !== root.notificationPreferences.bannerNotificationsEnabled) {
                                root.notificationPreferences.bannerNotificationsEnabled = checked
                            }
                        }
                    }
                }
            }
        }

        Text {
            width: parent.width
            text: "These controls are independent: Turning them off does not stop Astro from syncing or prevent messages from arriving."
            color: root.secondaryText
            font.pixelSize: units.gu(1.5)
            wrapMode: Text.WordWrap
        }

        Text {
            width: parent.width
            text: "Notification privacy"
            color: root.primaryText
            font.pixelSize: units.gu(2.2)
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            width: parent.width
            text: "Choose how much information Astro includes in message notifications. This controls the information intended for push notifications as well as notification previews."
            color: root.secondaryText
            font.pixelSize: units.gu(1.6)
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: root.privacyOptions

            delegate: Rectangle {
                width: contentColumn.width
                height: optionContent.height + units.gu(3)
                radius: units.gu(1)
                color: root.notificationPreferences.privacyLevel === modelData.value
                       ? root.raisedColor : root.panelColor

                border.width: root.notificationPreferences.privacyLevel === modelData.value
                              ? units.dp(2) : units.dp(1)
                border.color: root.notificationPreferences.privacyLevel === modelData.value
                              ? root.accentColor : "#344653"

                Column {
                    id: optionContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: units.gu(1.5)
                    spacing: units.gu(0.7)

                    Row {
                        width: parent.width
                        spacing: units.gu(1)

                        Text {
                            width: parent.width - selectedIndicator.width - units.gu(1)
                            text: modelData.title
                            color: root.primaryText
                            font.pixelSize: units.gu(1.8)
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            id: selectedIndicator
                            text: root.notificationPreferences.privacyLevel === modelData.value
                                  ? "✓" : ""
                            color: root.accentColor
                            font.pixelSize: units.gu(2)
                            font.bold: true
                        }
                    }

                    Text {
                        width: parent.width
                        text: modelData.description
                        color: root.secondaryText
                        font.pixelSize: units.gu(1.5)
                        wrapMode: Text.WordWrap
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.notificationPreferences.privacyLevel = modelData.value
                      if (root.backend)
                          root.backend.setPushPrivacyLevel(modelData.value)
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: privacyNote.height + units.gu(3)
            radius: units.gu(1)
            color: "#192630"

            Text {
                id: privacyNote
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: units.gu(1.5)

                text: "Privacy note: Room IDs and sender information are metadata, but they can still reveal who you communicate with. Full message previews may expose private conversations on your lock screen or to the push service."
                color: root.secondaryText
                font.pixelSize: units.gu(1.4)
                wrapMode: Text.WordWrap
            }
        }
    }
}
