import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property var backend

    Column {
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }

        spacing: units.gu(2)

        Label {
            text: "Account"
            fontSize: "x-large"
            color: "white"
        }

        Label {
            text: "Manage your Matrix account and session."
            color: "#91a4b5"
            wrapMode: Text.WordWrap
        }

        Rectangle {
            width: parent.width
            height: units.gu(12)
            radius: units.gu(2)
            color: "#1b2935"

            Column {
                anchors {
                    fill: parent
                    margins: units.gu(2)
                }

                spacing: units.gu(1)

                Label {
                    text: "Matrix account"
                    color: "white"
                    font.bold: true
                }

                Label {
                    text: "Account information will appear here."
                    color: "#91a4b5"
                    wrapMode: Text.WordWrap
                }
            }
        }

        Label {
            text: "SESSION"
            color: "#a78bfa"
            fontSize: "small"
        }

        Button {
            width: parent.width
            text: "Log out of Matrix"
            color: UbuntuColors.orange

            onClicked: {
                if (root.backend)
                    root.backend.logoutMatrix()
            }
        }

        Label {
            text: "Logging out ends your current Matrix session."
            color: "#91a4b5"
            wrapMode: Text.WordWrap
        }
    }
}