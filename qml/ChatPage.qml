
import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend
    property var mainRoot
    property bool wideLayout: width >= units.gu(75)
    property bool showConversation: false
    property string selectedRoom: "Welcome to Astro"
    property string selectedPreview: "Your Matrix client starts here."
    property string storageResult: ""

    ListModel {
        id: roomModel

        ListElement {
            name: "Welcome to Astro"
            preview: "Your Matrix client starts here."
            avatar: "A"
            colour: "#5265a4"
        }

        ListElement {
            name: "General"
            preview: "Welcome everyone!"
            avatar: "G"
            colour: "#398875"
        }

        ListElement {
            name: "Development"
            preview: "Working on the next release."
            avatar: "D"
            colour: "#a16b9b"
        }

        ListElement {
            name: "Friends"
            preview: "See you later!"
            avatar: "F"
            colour: "#bd8651"
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#101820"
        visible: backend.storageReady

        Row {
            anchors.fill: parent
            spacing: 0

            // Conversations sidebar
            Rectangle {
                id: sidebar

                width: root.wideLayout
                       ? units.gu(32)
                       : parent.width

                height: parent.height
                color: "#17232e"

                visible: root.wideLayout ||
                         !root.showConversation

                Column {
                    anchors.fill: parent
                    spacing: 0

                    Item {
                        width: parent.width
                        height: units.gu(8)

                        Text {
                            anchors {
                                left: parent.left
                                leftMargin: units.gu(2)
                                verticalCenter: parent.verticalCenter
                            }

                            text: "CONVERSATIONS"
                            color: "#91a4b5"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }
                    }

                    ListView {
                        id: roomList

                        width: parent.width
                        height: parent.height -
                                units.gu(16)

                        model: roomModel
                        clip: true

                        delegate: Item {
                            width: roomList.width
                            height: units.gu(9)

                            Rectangle {
                                anchors {
                                    fill: parent
                                    margins: units.gu(0.5)
                                }

                                radius: units.gu(1)
                                color: root.selectedRoom === name
                                       ? "#293c4c"
                                       : "transparent"

                                Row {
                                    anchors.fill: parent
                                    anchors.margins: units.gu(1)
                                    spacing: units.gu(1.5)

                                    Rectangle {
                                        width: units.gu(6)
                                        height: units.gu(6)
                                        radius: units.gu(3)
                                        color: colour

                                        anchors.verticalCenter:
                                            parent.verticalCenter

                                        Text {
                                            anchors.centerIn: parent
                                            text: avatar
                                            color: "white"
                                            font.bold: true
                                            font.pixelSize: units.gu(2.5)
                                        }
                                    }

                                    Column {
                                        anchors.verticalCenter:
                                            parent.verticalCenter

                                        width: parent.width -
                                               units.gu(8)

                                        spacing: units.gu(0.5)

                                        Text {
                                            width: parent.width
                                            text: name
                                            color: "white"
                                            font.bold: true
                                            font.pixelSize: units.gu(1.8)
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            width: parent.width
                                            text: preview
                                            color: "#9aabba"
                                            font.pixelSize: units.gu(1.5)
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent

                                onClicked: {
                                    root.selectedRoom = name
                                    root.selectedPreview = preview
                                    root.showConversation = true
                                }
                            }
                        }
                    }

                    // Settings button at bottom of sidebar
                    Rectangle {
                        width: parent.width
                        height: units.gu(8)
                        color: "#1b2935"

                        Button {
                            anchors {
                                fill: parent
                                margins: units.gu(1)
                            }

                            text: "Settings"

                            onClicked: {
                                root.mainRoot.showingSettings = true
                            }
                        }
                    }
                }
            }

            // Conversation view
            Rectangle {
                id: conversation

                width: root.wideLayout
                       ? parent.width - sidebar.width
                       : parent.width

                height: parent.height
                color: "#101820"

                visible: root.wideLayout ||
                         root.showConversation

                Flickable {
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: conversationContent.height
                    clip: true

                    Column {
                        id: conversationContent

                        width: parent.width
                        spacing: 0

                        // Conversation header
                        Rectangle {
                            width: parent.width
                            height: units.gu(8)
                            color: "#1b2935"

                            Rectangle {
                                anchors {
                                    left: parent.left
                                    leftMargin: units.gu(2)
                                    verticalCenter: parent.verticalCenter
                                }

                                width: units.gu(5)
                                height: units.gu(5)
                                radius: units.gu(2.5)
                                color: "#293c4c"
                                visible: !root.wideLayout

                                Text {
                                    anchors.centerIn: parent
                                    text: "‹"
                                    color: "white"
                                    font.pixelSize: units.gu(3)
                                }

                                MouseArea {
                                    anchors.fill: parent

                                    onClicked: {
                                        root.showConversation = false
                                    }
                                }
                            }

                            Text {
                                anchors {
                                    left: parent.left
                                    leftMargin: root.wideLayout
                                                ? units.gu(2)
                                                : units.gu(9)
                                    verticalCenter: parent.verticalCenter
                                    right: parent.right
                                    rightMargin: units.gu(1)
                                }

                                text: root.selectedRoom
                                color: "white"
                                font.pixelSize: units.gu(2)
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }

                        Column {
                            width: parent.width
                            spacing: units.gu(2)

                            Item {
                                width: parent.width
                                height: units.gu(3)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "✦"
                                color: "#80b9e8"
                                font.pixelSize: units.gu(7)
                            }

                            Text {
                                width: parent.width
                                text: root.selectedRoom
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                            }

                            Text {
                                width: parent.width
                                text: root.selectedPreview
                                color: "#9aabba"
                                font.pixelSize: units.gu(1.8)
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
