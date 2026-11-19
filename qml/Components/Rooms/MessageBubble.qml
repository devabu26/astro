import QtQuick 2.7
import Lomiri.Components 1.3
import QtGraphicalEffects 1.0

Item {
    id: root

    signal imageClicked(string imageUrl)
    signal videoFullscreenRequested(var output, var originalParent, var mediaPlayer)

    property string sender: ""
    property string senderDisplayName: ""
    property string avatarUrl: ""
    property bool showAvatar: true

    property string body: ""
    property string timestamp: ""
    property string imageUrl: ""
    property bool isImage: false
    property bool isVideo: false

    property var backend
    property var fullscreenHost
    property string videoRequestKey: ""
    property string videoMediaSourceJson: ""
    property string videoThumbnailUrl: ""
    property string videoMimeType: ""

    property bool isOwn: false
    property bool isEncrypted: false

    width: parent ? parent.width : 0
    height: messageBubble.height + units.gu(1)

    Row {
        anchors {
            left: parent.left
            right: parent.right
        }

        spacing: units.gu(1)

        layoutDirection: root.isOwn
                        ? Qt.RightToLeft
                        : Qt.LeftToRight

        Item {
            id: avatarContainer

            width: units.gu(5)
            height: units.gu(5)

            Rectangle {
                id: avatarBackground

                anchors.fill: parent
                radius: width / 2
                color: "#40566a"
                visible: root.showAvatar && senderAvatarImage.status !== Image.Ready

                Text {
                    anchors.centerIn: parent

                    text: {
                        var name = root.senderDisplayName ||
                                   root.sender

                        if (!name)
                            return "?"

                        if (name.charAt(0) === "@")
                            name = name.substring(1)

                        return name.charAt(0).toUpperCase()
                    }
                    color: "white"
                    font.bold: true
                    font.pixelSize: units.gu(2)
                    visible: root.showAvatar && senderAvatarImage.status !== Image.Ready
                }
            }

            Image {
                id: senderAvatarImage

                anchors.fill: parent

                source: root.avatarUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                visible: root.showAvatar && status === Image.Ready

                layer.enabled: true
                layer.effect: OpacityMask {
                    maskSource: Rectangle {
                        width: senderAvatarImage.width
                        height: senderAvatarImage.height
                        radius: width / 2
                    }
                }
            }

            visible: true
        }

        Rectangle {
            id: messageBubble

            width: root.width - units.gu(7)
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

                    text: root.isOwn
                          ? "You"
                          : (root.senderDisplayName || root.sender)

                    color: "#80b9e8"
                    font.bold: true
                    font.pixelSize: units.gu(1.5)

                    elide: Text.ElideRight
                }

                MessageImage {
                    width: parent.width

                    source: root.imageUrl
                    visible: root.isImage && source !== ""

                    onImageClicked: root.imageClicked(imageUrl)
                }

                Loader {
                    width: parent.width
                    height: root.isVideo ? units.gu(25) : 0
                    active: root.isVideo
                    visible: root.isVideo

                    sourceComponent: Component {
                        MessageVideo {
                            width: parent.width
                            height: units.gu(25)

                            backend: root.backend
                            requestKey: root.videoRequestKey
                            mediaSourceJson: root.videoMediaSourceJson
                            thumbnailUrl: root.videoThumbnailUrl
                            mimeType: root.videoMimeType
                            fullscreenHost: root.fullscreenHost

                            onFullscreenRequested: {
                                root.videoFullscreenRequested(
                                    output,
                                    originalParent,
                                    mediaPlayer
                                )
                            }
                        }
                    }
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
