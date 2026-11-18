
import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string sender: ""
    property string body: ""
    property string timestamp: ""
    property bool isOwn: false
    property bool isEncrypted: false

    width: parent ? parent.width : 0
    height: messageBubble.height + units.gu(1)

    Row {
        anchors {
            left: parent.left
            right: parent.right
        }

        layoutDirection: root.isOwn
                        ? Qt.RightToLeft
                        : Qt.LeftToRight

        Rectangle {
            id: messageBubble

            width: root.width * 0.78
            height: messageContent.height + units.gu(2)

            radius: units.gu(1.5)

            color: root.isOwn
                   ? "#245b78"
                   : "#263744"

            Column {
                id: messageContent

                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: units.gu(1)
                }

                spacing: units.gu(0.5)

                Text {
                    width: parent.width

                    text: root.isOwn ? "You" : root.sender

                    color: "#80b9e8"
                    font.bold: true
                    font.pixelSize: units.gu(1.5)

                    elide: Text.ElideRight
                }

                Row {
                    width: parent.width
                    spacing: units.gu(0.7)

                    Text {
                        visible: root.isEncrypted

                        text: "🔒"
                        color: "#a9d6ed"

                        font.pixelSize: units.gu(1.8)
                    }

                    Text {
                        width: parent.width -
                               (root.isEncrypted ? units.gu(3) : 0)

                        text: root.isEncrypted
                              ? "Encrypted message — unable to decrypt"
                              : root.body

                        color: root.isEncrypted
                               ? "#a9d6ed"
                               : "white"

                        font.italic: root.isEncrypted
                        font.pixelSize: units.gu(1.8)

                        wrapMode: Text.Wrap
                    }
                }

                Text {
                    width: parent.width

                    text: root.timestamp
                    color: "#a9bac6"

                    font.pixelSize: units.gu(1.3)

                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }
}
