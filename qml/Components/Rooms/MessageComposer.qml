
import QtQuick 2.7
import Lomiri.Components 1.3

Rectangle {
    id: root

    height: inputFrame.height + units.gu(3) +
            (root.statusText !== "" ? units.gu(2.5) : 0) +
            (root.contextLabel !== "" ? units.gu(2.5) : 0)
    color: "#1b2935"

    signal sendRequested(string message)
    signal attachRequested(string caption)
    signal contextCancelRequested()

    property bool sending: false
    property bool pasteVisible: false
    property bool attachmentBusy: false
    property bool attachmentMenuVisible: false
    property string statusText: ""
    property string contextMode: ""
    property string contextLabel: ""

    // Expose the plus button so RoomView can position its
    // unclipped attachment-menu overlay relative to it.
    property alias attachmentButtonItem: attachButton

    function submitMessage() {
        var message = root.currentMessage().trim()

        if (message === "" || root.sending || root.attachmentBusy)
            return

        root.sendRequested(message)
    }

    function dismissKeyboard() {
        messageInput.focus = false
    }

    function setInputText(value) {
        Qt.inputMethod.reset()
        messageInput.text = String(value || "")
        messageInput.cursorPosition = messageInput.text.length
        messageInput.deselect()
        messageInput.forceActiveFocus()
    }

    function pasteFromClipboard() {
        messageInput.forceActiveFocus()
        messageInput.paste()
    }

    function currentMessage() {
    var text = messageInput.text
    var cursor = messageInput.cursorPosition

    return text.slice(0, cursor)
            + messageInput.preeditText
            + text.slice(cursor)
    }

    function insertEmoji(emoji) {
        if (!emoji || root.sending || root.attachmentBusy)
            return

        var start = messageInput.selectionStart
        var end = messageInput.selectionEnd

        if (start !== end) {
            messageInput.remove(start, end)
        } else {
            start = messageInput.cursorPosition
        }

        messageInput.insert(start, emoji)
        messageInput.cursorPosition = start + emoji.length
        messageInput.forceActiveFocus()
    }

    function clearInput() {
        // Cancel any active keyboard composition.
        Qt.inputMethod.reset()

        // Clear the committed text immediately.
        messageInput.text = ""
        messageInput.cursorPosition = 0
        messageInput.deselect()

        // Clear again after the input method has processed
        // its composition reset. This prevents stale pre-edit
        // text from reappearing in the editor.
        Qt.callLater(function() {
            Qt.inputMethod.reset()
            messageInput.text = ""
            messageInput.cursorPosition = 0
            messageInput.deselect()
        })
    }

    Rectangle {
        id: inputFrame

        anchors {
            left: parent.left
            right: sendButton.left
            leftMargin: units.gu(1.5)
            rightMargin: units.gu(1)
            verticalCenter: parent.verticalCenter
        }

        height: messageInput.lineCount <= 1
                ? units.gu(6)
                : Math.min(
                    units.gu(16),
                    units.gu(12) +
                    Math.max(0, messageInput.lineCount - 2) * units.gu(2)
                )

        Behavior on height {
            NumberAnimation {
                duration: 200
                easing.type: Easing.OutCubic
            }
        }

        clip: true
        radius: units.gu(2)
        color: "#263744"

        Rectangle {
            id: attachButton

            anchors {
                right: clipboardButton.left
                rightMargin: units.gu(0.25)
                verticalCenter: parent.verticalCenter
            }

            width: units.gu(4)
            height: units.gu(4)
            radius: units.gu(1)
            color: attachMouse.pressed ? "#40596b" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "+"
                color: "#b8cbd8"
                font.pixelSize: units.gu(3)
            }

            MouseArea {
                id: attachMouse
                anchors.fill: parent
                enabled: !root.sending && !root.attachmentBusy

                onClicked: {
                    root.pasteVisible = false
                    root.attachmentMenuVisible =
                        !root.attachmentMenuVisible
                }
            }
        }

        TextEdit {
            id: messageInput

            anchors {
                left: parent.left
                right: attachButton.left
                top: parent.top
                bottom: parent.bottom
                leftMargin: units.gu(1)
                rightMargin: units.gu(0.5)
                topMargin: units.gu(1)
                bottomMargin: units.gu(1)
            }

            color: "white"
            selectionColor: "#477fa0"
            font.pixelSize: units.gu(1.8)
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.PlainText
            readOnly: root.sending || root.attachmentBusy
            verticalAlignment: TextEdit.AlignVCenter
            selectByMouse: true
            clip: true

            Keys.onReturnPressed: {
                if (event.modifiers & Qt.ShiftModifier) {
                    messageInput.insert(
                        messageInput.cursorPosition,
                        "\\n"
                    )
                } else {
                    root.submitMessage()
                }

                event.accepted = true
            }

            Text {
                anchors.fill: parent
                anchors.leftMargin: units.gu(0.5)
                text: "Also write a message…"
                color: "#91a4b2"
                font.pixelSize: units.gu(1.8)
                verticalAlignment: Text.AlignVCenter

                visible: messageInput.text.length === 0
                         && messageInput.preeditText.length === 0

                enabled: false
            }
        }

        Rectangle {
            id: clipboardButton

            anchors {
                right: parent.right
                rightMargin: units.gu(1)
                verticalCenter: parent.verticalCenter
            }

            width: units.gu(4)
            height: units.gu(4)
            radius: units.gu(1)
            color: clipboardMouse.pressed ? "#40596b" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "▤"
                color: "#b8cbd8"
                font.pixelSize: units.gu(2.5)
            }

            MouseArea {
                id: clipboardMouse
                anchors.fill: parent

                onClicked: {
                    root.attachmentMenuVisible = false
                    root.pasteVisible = !root.pasteVisible
                }
            }
        }
    }

    Text {
        id: contextLabelText

        anchors {
            left: inputFrame.left
            right: contextCancelButton.left
            bottom: inputFrame.top
            bottomMargin: units.gu(0.2)
        }

        height: units.gu(2.2)
        text: root.contextLabel
        color: "white"
        font.pixelSize: units.gu(1.5)
        font.bold: root.contextMode === "edit"
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        visible: root.contextLabel !== ""
    }

    Rectangle {
        id: contextCancelButton

        anchors {
            right: inputFrame.right
            bottom: inputFrame.top
            bottomMargin: units.gu(0.2)
        }

        width: units.gu(2.5)
        height: units.gu(2.2)
        radius: units.gu(0.5)
        color: contextCancelMouse.pressed ? "#526e82" : "transparent"
        visible: root.contextLabel !== ""

        Text {
            anchors.centerIn: parent
            text: "×"
            color: "white"
            font.pixelSize: units.gu(2)
        }

        MouseArea {
            id: contextCancelMouse
            anchors.fill: parent
            onClicked: root.contextCancelRequested()
        }
    }

    Text {
        id: attachmentStatusLabel

        anchors {
            left: inputFrame.left
            right: inputFrame.right
            bottom: root.contextLabel !== ""
                    ? contextLabelText.top : inputFrame.top
            bottomMargin: units.gu(0.2)
        }

        height: units.gu(2.2)
        text: root.statusText
        color: "#b8cbd8"
        font.pixelSize: units.gu(1.4)
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        visible: root.statusText !== ""
    }

    // Floating text-editing toolbar.
    Rectangle {
        id: pastePopover

        anchors {
            right: inputFrame.right
            bottom: inputFrame.top
            bottomMargin: units.gu(1)
        }

        width: units.gu(25)
        height: units.gu(6)
        radius: units.gu(1.5)
        color: "#354b5b"
        border.color: "#587184"
        visible: root.pasteVisible && !root.sending && !root.attachmentBusy
        z: 10

        Row {
            anchors.fill: parent
            anchors.margins: units.gu(0.5)
            spacing: units.gu(0.5)

            Repeater {
                model: [
                    { label: "Paste", action: "paste" },
                    { label: "Copy", action: "copy" },
                    { label: "Select All", action: "selectAll" }
                ]

                delegate: Rectangle {
                    width: (parent.width - units.gu(1)) / 3
                    height: parent.height
                    radius: units.gu(1)
                    color: toolbarMouse.pressed
                           ? "#526e82" : "transparent"

                    opacity: modelData.action === "copy" &&
                             messageInput.selectionStart ===
                             messageInput.selectionEnd ? 0.45 : 1

                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: "white"
                        font.pixelSize: units.gu(1.5)
                    }

                    MouseArea {
                        id: toolbarMouse
                        anchors.fill: parent

                        enabled: modelData.action !== "copy" ||
                                 messageInput.selectionStart !==
                                 messageInput.selectionEnd

                        onClicked: {
                            if (modelData.action === "paste") {
                                messageInput.forceActiveFocus()
                                messageInput.paste()
                                root.pasteVisible = false
                            } else if (modelData.action === "copy") {
                                messageInput.copy()
                                root.pasteVisible = false
                            } else if (modelData.action === "selectAll") {
                                messageInput.forceActiveFocus()
                                messageInput.cursorPosition = 0
                                messageInput.select(messageInput.text.length, 0)                            }
                        }
                    }
                }
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

       color: root.currentMessage().trim().length > 0 &&
              !root.sending && !root.attachmentBusy
       ? "#245b78" : "#293c4c"

opacity: root.currentMessage().trim().length > 0 &&
         !root.sending && !root.attachmentBusy
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
         && !root.sending && !root.attachmentBusy

            onClicked: {
                root.submitMessage()
            }
        }
    }

}
