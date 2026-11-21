import QtQuick 2.7
import Lomiri.Components 1.3
import Ubuntu.Content 1.1

Item {
    id: root

    property string imageUrl: ""
    property bool opened: false

    property var backend
    property string mediaSourceJson: ""
    property string mediaMimeType: "application/octet-stream"
    property string mediaFileName: "media"

    property bool exporting: false
    property bool destinationPickerOpen: false
    property string exportRequestKey: ""
    property string pendingExportUrl: ""
    property string exportStatus: ""
    property var activeTransfer: null


    signal closeRequested()

    function resetView() {
        viewerImage.scale = 1
        viewerImage.x = 0
        viewerImage.y = 0
    }

    function exportMedia() {
        if (!root.backend ||
            !root.mediaSourceJson ||
            root.exporting) {
            return
        }

        root.exporting = true
        root.exportStatus = "Preparing media…"

        root.exportRequestKey =
            "media-export-" + Date.now().toString()

        root.backend.exportMessageImage(
            root.exportRequestKey,
            root.mediaSourceJson,
            root.mediaMimeType,
            root.mediaFileName
        )
    }

    Connections {
        target: root.backend

        onMessageImageExported: {
            if (requestKey !== root.exportRequestKey)
                return

            root.exporting = false
            root.pendingExportUrl = fileUrl
            root.exportStatus = "Choose where to save the file"
            root.destinationPickerOpen = true
        }

        onMessageImageExportFailed: {
            if (requestKey !== root.exportRequestKey)
                return

            root.exporting = false
            root.exportStatus = "Download failed: " + error
        }
    }

    Rectangle {
        id: imageViewerOverlay

        anchors.fill: parent
        z: 1000
        visible: root.opened
        color: "#e6101720"

        PinchArea {
            id: imagePinchArea

            anchors.fill: parent

            pinch.minimumScale: 1.0
            pinch.maximumScale: 5.0
            pinch.dragAxis: Pinch.XAndYAxis

            // Zoom around the moving midpoint between the fingers.
            onPinchUpdated: {
                var oldScale = viewerImage.scale
                var newScale = Math.max(
                    1.0,
                    Math.min(
                        5.0,
                        oldScale * pinch.scale / pinch.previousScale
                    )
                )

                if (newScale <= 1.001) {
                    viewerImage.scale = 1.0
                    viewerImage.x = 0
                    viewerImage.y = 0
                } else {
                    var ratio = newScale / oldScale

                    var previousX = pinch.previousCenter.x
                    var previousY = pinch.previousCenter.y
                    var currentX = pinch.center.x
                    var currentY = pinch.center.y

                    viewerImage.x +=
                        (currentX - previousX) +
                        (1.0 - ratio) *
                        (previousX - viewerImage.x -
                         viewerImage.width / 2)

                    viewerImage.y +=
                        (currentY - previousY) +
                        (1.0 - ratio) *
                        (previousY - viewerImage.y -
                         viewerImage.height / 2)

                    viewerImage.scale = newScale
                }
            }

            // Taps outside the image close the viewer.
            MouseArea {
                anchors.fill: parent
                z: 0

                onClicked: root.closeRequested()
            }

            // Freely movable image transform container.
            Item {
                id: viewerImage

                width: imagePinchArea.width
                height: imagePinchArea.height
                x: 0
                y: 0
                z: 1

                transformOrigin: Item.Center
                scale: 1.0

                AnimatedImage {
                    id: viewerImageContent

                    anchors.centerIn: parent

                    width: sourceSize.width > 0 &&
                           sourceSize.height > 0
                           ? Math.min(
                                 parent.width,
                                 parent.height *
                                 sourceSize.width / sourceSize.height
                             )
                           : parent.width

                    height: sourceSize.width > 0 &&
                            sourceSize.height > 0
                            ? Math.min(
                                  parent.height,
                                  parent.width *
                                  sourceSize.height / sourceSize.width
                              )
                            : parent.height

                    source: root.imageUrl
                    playing: true
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: true

                    MouseArea {
                        anchors.fill: parent

                        drag.target: viewerImage.scale > 1.01
                                     ? viewerImage : null
                        drag.axis: Drag.XAndYAxis
                        drag.minimumX: -viewerImage.width
                        drag.maximumX: viewerImage.width
                        drag.minimumY: -viewerImage.height
                        drag.maximumY: viewerImage.height

                        onDoubleClicked: {
                            if (viewerImage.scale > 1.0) {
                                viewerImage.scale = 1.0
                                viewerImage.x = 0
                                viewerImage.y = 0
                            } else {
                                var tapX =
                                    viewerImageContent.x + mouse.x
                                var tapY =
                                    viewerImageContent.y + mouse.y

                                var oldScale = viewerImage.scale
                                var newScale = 2.5

                                viewerImage.x +=
                                    (oldScale - newScale) *
                                    (tapX - viewerImage.width / 2)

                                viewerImage.y +=
                                    (oldScale - newScale) *
                                    (tapY - viewerImage.height / 2)

                                viewerImage.scale = newScale
                            }
                        }
                    }
                }
            }
        }


        // =====================================
        // Content Hub media export
        // =====================================

        ContentPeerPicker {
            id: exportPeerPicker

            anchors.fill: parent
            z: 20
            visible: root.destinationPickerOpen

            contentType: ContentType.All
            handler: ContentHandler.Destination

            onPeerSelected: {
                peer.selectionType = ContentTransfer.Single

                root.activeTransfer = peer.request()

                if (!root.activeTransfer) {
                    root.exportStatus = "Could not start file transfer"
                    root.exporting = false
                    root.destinationPickerOpen = false
                    return
                }

                root.exportStatus = "Preparing transfer…"
                console.log("Astro: Waiting for Content Hub transfer to become InProgress")
            }

            onCancelPressed: {
                root.destinationPickerOpen = false
                root.exportStatus = ""
            }
        }

        Component {
            id: exportContentItemComponent
            ContentItem {}
        }

        Connections {
            target: root.activeTransfer

            onStateChanged: {
                var transfer = root.activeTransfer

                if (!transfer)
                    return

                console.log("Astro: Content Hub state:", transfer.state)

                if (transfer.state === ContentTransfer.InProgress) {
                    console.log("Astro: Transfer is InProgress; charging file")

                    var item = exportContentItemComponent.createObject(root, {
                        "url": root.pendingExportUrl
                    })

                    if (!item) {
                        root.exportStatus = "Could not prepare the file"
                        transfer.state = ContentTransfer.Aborted
                        return
                    }

                    transfer.items = [item]
                    transfer.state = ContentTransfer.Charged

                    root.exportStatus = "Sending file to Files…"
                    console.log("Astro: Content Hub transfer charged")
                }
                else if (transfer.state === ContentTransfer.Collected) {
                    console.log("Astro: Content Hub transfer collected")

                    transfer.finalize()

                    root.activeTransfer = null
                    root.exporting = false
                    root.pendingExportUrl = ""
                    root.exportStatus = "Image exported successfully"
                    root.destinationPickerOpen = false
                }
                else if (transfer.state === ContentTransfer.Aborted) {
                    console.warn("Astro: Content Hub transfer aborted")

                    transfer.finalize()

                    root.activeTransfer = null
                    root.exporting = false
                    root.pendingExportUrl = ""
                    root.exportStatus = "File transfer failed or was cancelled"
                    root.destinationPickerOpen = false
                }
            }
        }


        ContentTransferHint {
            anchors.fill: parent
            z: 30
            activeTransfer: root.activeTransfer
        }


        // Download original Matrix media.
        Rectangle {
            id: mediaDownloadButton

            anchors {
                top: parent.top
                left: parent.left
                topMargin: units.gu(2)
                leftMargin: units.gu(2)
            }

            width: units.gu(13)
            height: units.gu(6)
            radius: units.gu(1)
            color: "#bb263744"
            z: 3

            opacity: 1

            Text {
                anchors.centerIn: parent
                text: root.exporting ? "Preparing…" : "↓ Download"
                color: "white"
                font.pixelSize: units.gu(1.7)
            }

            MouseArea {
                anchors.fill: parent

                enabled: root.mediaSourceJson !== "" &&
                         !root.exporting

                onClicked: root.exportMedia()
            }
        }

        // Close button.
        Rectangle {
            anchors {
                top: parent.top
                right: parent.right
                topMargin: units.gu(2)
                rightMargin: units.gu(2)
            }

            width: units.gu(6)
            height: units.gu(6)
            radius: width / 2
            color: "#bb263744"
            z: 3

            Text {
                anchors.centerIn: parent
                text: "×"
                color: "white"
                font.pixelSize: units.gu(4)
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.closeRequested()
            }
        }

        Text {
            anchors {
                bottom: parent.bottom
                horizontalCenter: parent.horizontalCenter
                bottomMargin: units.gu(6)
            }

            z: 4
            visible: root.exportStatus !== ""
            text: root.exportStatus
            color: "white"
            font.pixelSize: units.gu(1.7)
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            anchors {
                bottom: parent.bottom
                horizontalCenter: parent.horizontalCenter
                bottomMargin: units.gu(3)
            }

            z: 3
            text: "Double-tap or pinch to zoom · Drag to move · Tap outside to close"
            color: "#d0ffffff"
            font.pixelSize: units.gu(1.6)
        }
    }
}
