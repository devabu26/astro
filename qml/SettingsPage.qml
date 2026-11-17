
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
                    spacing: 0

                    // Equal left and right page margins
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

                        // Lock Button
                        Button {
                            width: parent.width
                            text: "Lock Astro"

                            onClicked: {
                                root.backend.closeStorage()
                            }
                        }

                        // Log-out Button
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

                        Item {
                            width: parent.width
                            height: units.gu(2)
                        }
                    }
                }
            }
        }
    }
}
