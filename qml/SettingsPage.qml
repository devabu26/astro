import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend
    signal backRequested()

    Rectangle {
        anchors.fill: parent
        color: "#101820"

        Column {
            anchors.fill: parent
            spacing: 0

            // Header
            Rectangle {
                width: parent.width
                height: units.gu(8)
                color: "#1b2935"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: units.gu(2)
                    anchors.rightMargin: units.gu(2)
                    spacing: units.gu(2)

                    Rectangle {
                        width: units.gu(5)
                        height: units.gu(5)
                        radius: units.gu(2.5)
                        color: "#293c4c"
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            anchors.centerIn: parent
                            text: "‹"
                            color: "white"
                            font.pixelSize: units.gu(3)
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.backRequested()
                        }
                    }

                    Text {
                        text: "Settings"
                        color: "white"
                        font.pixelSize: units.gu(2.5)
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Flickable {
                width: parent.width
                height: parent.height - units.gu(8)
                contentWidth: width
                contentHeight: settingsContent.height
                clip: true

                Column {
                    id: settingsContent
                    width: parent.width
                    spacing: units.gu(2)

                    Column {
                        id: settingsInner
                        width: parent.width - units.gu(4)
                        x: units.gu(2)
                        spacing: units.gu(2)

                        // Account
                        Text {
                            text: "ACCOUNT"
                            color: "#80b9e8"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: units.gu(7)
                            radius: units.gu(1)
                            color: "#1b2935"

                            Column {
                                anchors {
                                    left: parent.left
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: units.gu(2)
                                }

                                Text {
                                    text: "Matrix account"
                                    color: "white"
                                    font.pixelSize: units.gu(1.8)
                                }

                                Text {
                                    text: "Manage your signed-in account"
                                    color: "#91a4b5"
                                    font.pixelSize: units.gu(1.5)
                                }
                            }
                        }

                        // Security
                        Text {
                            text: "SECURITY"
                            color: "#80b9e8"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: units.gu(7)
                            radius: units.gu(1)
                            color: "#1b2935"

                            Column {
                                anchors {
                                    left: parent.left
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: units.gu(2)
                                }

                                Text {
                                    text: "Encrypted storage"
                                    color: "white"
                                    font.pixelSize: units.gu(1.8)
                                }

                                Text {
                                    text: "Your local Astro data is protected"
                                    color: "#91a4b5"
                                    font.pixelSize: units.gu(1.5)
                                }
                            }
                        }

                        Button {
                            width: parent.width
                            text: "Lock Astro"

                            onClicked: {
                                root.backend.closeStorage()
                            }
                        }

                        Button {
                            width: parent.width
                            text: "Log out of Matrix"

                            onClicked: {
                                root.backend.logoutMatrix()
                            }
                        }

                        // Appearance
                        Text {
                            text: "APPEARANCE"
                            color: "#80b9e8"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: units.gu(7)
                            radius: units.gu(1)
                            color: "#1b2935"

                            Column {
                                anchors {
                                    left: parent.left
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: units.gu(2)
                                }

                                Text {
                                    text: "Theme"
                                    color: "white"
                                    font.pixelSize: units.gu(1.8)
                                }

                                Text {
                                    text: "Dark appearance"
                                    color: "#91a4b5"
                                    font.pixelSize: units.gu(1.5)
                                }
                            }
                        }

                        // About
                        Text {
                            text: "ABOUT"
                            color: "#80b9e8"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: units.gu(7)
                            radius: units.gu(1)
                            color: "#1b2935"

                            Column {
                                anchors {
                                    left: parent.left
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: units.gu(2)
                                }

                                Text {
                                    text: "Astro"
                                    color: "white"
                                    font.pixelSize: units.gu(1.8)
                                }

                                Text {
                                    text: "Matrix client for Ubuntu Touch"
                                    color: "#91a4b5"
                                    font.pixelSize: units.gu(1.5)
                                }
                            }
                        }

                        // Spacer
                        Item {
                            width: parent.width
                            height: units.gu(4)
                        }

                        // Danger zone
                        Text {
                            text: "DANGER ZONE"
                            color: "#e57373"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: units.gu(6)
                            radius: units.gu(1)
                            color: "#1b2935"
                            border.color: "#8b3030"
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "Reset Astro / Delete All Data"
                                color: "#e53935"
                                font.pixelSize: units.gu(1.8)
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent

                                onPressed: {
                                    parent.color = "#292020"
                                }

                                onReleased: {
                                    parent.color = "#1b2935"
                                }

                                onClicked: {
                                    resetOverlay.visible = true
                                }
                            }
                        }

                        Item {
                            width: parent.width
                            height: units.gu(4)
                        }
                    }
                }
            }
        }
    }

    // Reset confirmation overlay
    Rectangle {
        id: resetOverlay

        anchors.fill: parent
        color: "#000000"
        opacity: 0.75
        visible: false
        z: 100

        MouseArea {
            anchors.fill: parent
        }
    }

    Rectangle {
        id: resetConfirmation

        width: parent.width - units.gu(6)
        height: units.gu(40)
        anchors.centerIn: parent
        radius: units.gu(2)
        color: "#1b2935"
        border.color: "#FFFFFF"
        border.width: 1
        visible: resetOverlay.visible
        z: 101

        Column {
            anchors {
                fill: parent
                margins: units.gu(3)
            }

            spacing: units.gu(2)

            Text {
                width: parent.width
                text: "Delete all Astro data?"
                color: "white"
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

            Rectangle {
                width: parent.width
                height: units.gu(6)
                radius: units.gu(1)
                color: "#293c4c"

                Text {
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: "white"
                    font.pixelSize: units.gu(1.8)
                }

                MouseArea {
                    anchors.fill: parent

                    onClicked: {
                        resetOverlay.visible = false
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: units.gu(6)
                radius: units.gu(1)
                color: "#3a2020"
                border.color: "#e53935"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "Delete Everything"
                    color: "#e53935"
                    font.pixelSize: units.gu(1.8)
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent

                    onClicked: {
                        resetOverlay.visible = false
                        root.backend.deleteAllData()
                    }
                }
            }
        }
    }
}
