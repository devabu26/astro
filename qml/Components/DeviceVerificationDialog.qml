import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    anchors.fill: parent
    visible: false
    z: 1000

    // Verification request details
    property string userId: ""
    property string flowId: ""
    property string roomId: ""
    property bool isOutgoing: false

    // Verification state
    property string verificationState: "request"
    property var emojis: []

    property bool busy: false
    property string errorMessage: ""

    signal acceptRequested(string userId, string flowId)
    signal cancelRequested(string userId, string flowId)
    signal confirmRequested(string userId, string flowId)
    signal mismatchRequested(string userId, string flowId)

    function showRequest(user, flow, room) {
        userId = user
        flowId = flow
        roomId = room || ""
        isOutgoing = false

        verificationState = "request"
        emojis = []
        busy = false
        errorMessage = ""

        visible = true
    }

    function showOutgoingRequest(user, flow) {
        userId = user
        flowId = flow
        roomId = ""
        isOutgoing = true

        verificationState = "outgoing_waiting"
        emojis = []
        busy = false
        errorMessage = ""

        visible = true
    }

    function updateVerificationState(state, emojiList) {
        verificationState = state
        emojis = emojiList || []
        busy = false
        errorMessage = ""

        if (state === "done" || state === "cancelled") {
            busy = false
        }
    }

    function showError(message) {
        busy = false
        errorMessage = message
    }

    function closeDialog() {
        visible = false
        busy = false
        errorMessage = ""
        emojis = []
        verificationState = "request"
        isOutgoing = false
    }

    // Dark background overlay
    Rectangle {
        anchors.fill: parent
        color: "#99000000"

        MouseArea {
            anchors.fill: parent
            onClicked: {
                // Prevent clicks passing through to the chat.
            }
        }
    }

    // Main verification panel
    Rectangle {
        id: panel

        anchors.centerIn: parent

        width: Math.min(
            parent.width - units.gu(4),
            units.gu(42)
        )

        height: Math.min(
            contentColumn.height + units.gu(4),
            parent.height - units.gu(6)
        )

        radius: units.gu(1)
        color: "#1b2935"

        Column {
            id: contentColumn

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: units.gu(2)
            }

            spacing: units.gu(1.5)

            Text {
                width: parent.width

                text: "Verify device"
                color: "white"

                font.pixelSize: units.gu(2.5)
                font.bold: true

                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width

                text: root.isOutgoing
                      ? "Verify another session belonging to:"
                      : "Verify that this device belongs to:"
                color: "#b8c7d3"

                font.pixelSize: units.gu(1.6)
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width

                text: root.userId
                color: "#80b9e8"

                font.pixelSize: units.gu(1.7)
                font.bold: true

                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width

                visible: root.roomId !== ""

                text: "Room verification request"
                color: "#9aabba"

                font.pixelSize: units.gu(1.4)
                horizontalAlignment: Text.AlignHCenter
            }

            Rectangle {
                width: parent.width
                height: 1
                color: "#405361"
            }

            // Incoming request
            Column {
                width: parent.width
                spacing: units.gu(1)

                visible: root.verificationState === "request" &&
                         !root.isOutgoing

                Text {
                    width: parent.width

                    text: "This device wants to verify your session."

                    color: "white"
                    font.pixelSize: units.gu(1.7)

                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    width: parent.width

                    text: "Accept only if you recognise this verification request."

                    color: "#b8c7d3"
                    font.pixelSize: units.gu(1.4)

                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Row {
                    width: parent.width
                    spacing: units.gu(1)

                    Button {
                        width: (parent.width - units.gu(1)) / 2

                        text: "Reject"
                        color: "#703b43"
                        enabled: !root.busy

                        onClicked: {
                            root.busy = true
                            root.cancelRequested(
                                root.userId,
                                root.flowId
                            )
                        }
                    }

                    Button {
                        width: (parent.width - units.gu(1)) / 2

                        text: "Accept"
                        color: "#287a55"
                        enabled: !root.busy

                        onClicked: {
                            root.busy = true
                            root.acceptRequested(
                                root.userId,
                                root.flowId
                            )
                        }
                    }
                }
            }

            // Waiting for the other device
            Column {
                width: parent.width
                spacing: units.gu(1)

                visible:
                    root.verificationState === "outgoing_waiting" ||
                    root.verificationState === "started" ||
                    root.verificationState === "accepted" ||
                    root.verificationState === "created"

                Text {
                    width: parent.width

                    text: "Waiting for the other device…"
                    color: "white"

                    font.pixelSize: units.gu(1.7)
                    horizontalAlignment: Text.AlignHCenter
                }

                LoadingIndicator {
                    width: parent.width
                    height: units.gu(7)

                    active: root.visible
                    label: "Establishing secure verification"
                }
            }

            // SAS emoji comparison
            Column {
                width: parent.width
                spacing: units.gu(1)

                visible:
                    root.verificationState === "keys_exchanged" ||
                    root.verificationState === "confirmed"

                Text {
                    width: parent.width

                    text: "Compare these emoji"
                    color: "white"

                    font.pixelSize: units.gu(2)
                    font.bold: true

                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    width: parent.width

                    text: "Check that the emoji appear in the same order on both devices."

                    color: "#b8c7d3"
                    font.pixelSize: units.gu(1.4)

                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Grid {
                    width: parent.width

                    columns: 4
                    spacing: units.gu(1)

                    Repeater {
                        model: root.emojis

                        delegate: Column {
                            width: (parent.width - units.gu(3)) / 4
                            spacing: units.gu(0.3)

                            Text {
                                width: parent.width

                                text: modelData.symbol || ""
                                color: "white"

                                font.pixelSize: units.gu(4)

                                horizontalAlignment: Text.AlignHCenter
                            }

                            Text {
                                width: parent.width

                                text: modelData.description || ""
                                color: "#b8c7d3"

                                font.pixelSize: units.gu(1.1)

                                wrapMode: Text.Wrap
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }

                Text {
                    width: parent.width

                    visible: root.verificationState === "confirmed"

                    text: "Waiting for verification to finish…"
                    color: "#80b9e8"

                    font.pixelSize: units.gu(1.5)
                    horizontalAlignment: Text.AlignHCenter
                }

                Row {
                    width: parent.width
                    spacing: units.gu(1)

                    visible: root.verificationState === "keys_exchanged"

                    Button {
                        width: (parent.width - units.gu(1)) / 2

                        text: "Not a match"
                        color: "#703b43"
                        enabled: !root.busy

                        onClicked: {
                            root.busy = true
                            root.mismatchRequested(
                                root.userId,
                                root.flowId
                            )
                        }
                    }

                    Button {
                        width: (parent.width - units.gu(1)) / 2

                        text: "They match"
                        color: "#287a55"
                        enabled: !root.busy

                        onClicked: {
                            root.busy = true
                            root.confirmRequested(
                                root.userId,
                                root.flowId
                            )
                        }
                    }
                }
            }

            // Successful verification
            Text {
                width: parent.width

                visible: root.verificationState === "done"

                text: "✓  Device successfully verified"
                color: "#70d69b"

                font.pixelSize: units.gu(1.8)
                font.bold: true

                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }


            // Cancelled verification
            Text {
                width: parent.width

                visible:
                    root.verificationState === "cancelled" ||
                    root.verificationState === "mismatched"

                text: root.verificationState === "mismatched"
                    ? "Verification rejected: the emoji did not match."
                    : "Verification was cancelled."

                color: "#e59a9a"

                font.pixelSize: units.gu(1.7)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }


            // Error message
            Text {
                width: parent.width

                visible: root.errorMessage !== ""

                text: root.errorMessage
                color: "#ff8585"

                font.pixelSize: units.gu(1.4)
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }

            // Busy indicator
            LoadingIndicator {
                width: parent.width
                height: units.gu(6)

                visible: root.busy
                active: root.busy

                label: "Please wait…"
            }

            // Close / cancel button
            Button {
                width: parent.width

                text: root.verificationState === "done" ||
                    root.verificationState === "cancelled" ||
                    root.verificationState === "mismatched"
                    ? "Close" : "Cancel"

                onClicked: {
                    if (root.verificationState === "done" ||
                        root.verificationState === "cancelled" ||
                        root.verificationState === "mismatched") {
                        root.closeDialog()
                    } else if (!root.busy) {
                        root.busy = true

                        root.cancelRequested(
                            root.userId,
                            root.flowId
                        )
                    }
                }
            }
        }
    }
}