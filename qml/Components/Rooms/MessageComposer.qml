
import QtQuick 2.7
import Lomiri.Components 1.3

Rectangle {
    id: root

    height: units.gu(9)
    color: "#1b2935"

    signal sendRequested(string message)

    property bool sending: false

    function submitMessage() {
        var message = root.currentMessage().trim()

        if (message === "")
            return

        root.sendRequested(message)
    }

    function currentMessage() {
    var text = messageInput.text
    var cursor = messageInput.cursorPosition

    return text.slice(0, cursor)
            + messageInput.preeditText
            + text.slice(cursor)
    }

    function clearInput() {
        Qt.inputMethod.reset()
        messageInput.text = ""
        messageInput.cursorPosition = 0
    }

    Rectangle {
        anchors {
            left: parent.left
            right: sendButton.left
            leftMargin: units.gu(1.5)
            rightMargin: units.gu(1)
            verticalCenter: parent.verticalCenter
        }

        height: units.gu(6)

        radius: units.gu(2)
        color: "#263744"

        TextEdit {
            id: messageInput

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                bottom: parent.bottom

                leftMargin: units.gu(1.5)
                rightMargin: units.gu(1.5)
                topMargin: units.gu(1)
                bottomMargin: units.gu(1)
            }

            color: "white"
            selectionColor: "#477fa0"

            font.pixelSize: units.gu(1.8)

            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.PlainText
            readOnly: root.sending

            verticalAlignment: TextEdit.AlignVCenter

            selectByMouse: true

            Keys.onReturnPressed: {
                if (event.modifiers & Qt.ShiftModifier) {
                    messageInput.insert(
                        messageInput.cursorPosition,
                        "\n"
                    )
                } else {
                    root.submitMessage()
                }

                event.accepted = true
            }

            Text {
                anchors {
                    left: parent.left
                    top: parent.top
                    topMargin: units.gu(0.5)
                }

                text: "Write a message…"

                color: "#91a4b2"
                font.pixelSize: units.gu(1.8)

                visible: messageInput.text.length === 0
         && messageInput.preeditText.length === 0

                enabled: false
            }
        }
    }

    Rectangle {
        id: sendButton

        anchors {
            right: parent.right
            rightMargin: units.gu(1.5)
            verticalCenter: parent.verticalCenter
        }

        width: units.gu(6)
        height: units.gu(6)

        radius: units.gu(3)

       color: root.currentMessage().trim().length > 0 && !root.sending
       ? "#245b78" : "#293c4c"

opacity: root.currentMessage().trim().length > 0 && !root.sending
         ? 1 : 0.55

        Text {
            anchors.centerIn: parent

            text: "➤"
            color: "white"

            font.pixelSize: units.gu(2.5)
        }

        MouseArea {
            anchors.fill: parent

            enabled: root.currentMessage().trim().length > 0
         && !root.sending

            onClicked: {
                root.submitMessage()
            }
        }
    }
}
