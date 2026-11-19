import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string imageUrl: ""
    property bool opened: false

    signal closeRequested()

    function resetView() {
        viewerImage.scale = 1
        viewerImage.x = 0
        viewerImage.y = 0
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
                bottomMargin: units.gu(3)
            }

            z: 3
            text: "Double-tap or pinch to zoom · Drag to move · Tap outside to close"
            color: "#d0ffffff"
            font.pixelSize: units.gu(1.6)
        }
    }
}
