import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string source: ""
    property real imageHeightLimit: units.gu(40)

    signal imageClicked(string imageUrl)
    signal swipeStarted()
    signal swipeMoved(real dx, real dy)
    signal swipeEnded(bool cancelled)

    height: messageImage.status === Image.Ready &&
            messageImage.implicitWidth > 0
            ? Math.min(
                  width * messageImage.implicitHeight /
                  messageImage.implicitWidth,
                    root.imageHeightLimit
              )
            : 0

            AnimatedImage {
                id: messageImage

                anchors.fill: parent
                source: root.source
                playing: true
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true

                MouseArea {
                    anchors.fill: parent
                    enabled: root.source !== ""

                    property real swipePressX: 0
                    property real swipePressY: 0
                    property bool swipeClaimed: false

                    onPressed: {
                        swipePressX = mouse.x
                        swipePressY = mouse.y
                        swipeClaimed = false
                        root.swipeStarted()
                    }

                    onPositionChanged: {
                        if (!pressed)
                            return

                        var dx = mouse.x - swipePressX
                        var dy = mouse.y - swipePressY

                        if (!swipeClaimed &&
                            Math.abs(dx) > units.gu(0.65) &&
                            Math.abs(dx) > Math.abs(dy) * 1.05) {
                            swipeClaimed = true
                            preventStealing = true
                        }

                        if (swipeClaimed)
                            root.swipeMoved(dx, dy)
                    }

                    onReleased: {
                        if (swipeClaimed)
                            root.swipeEnded(false)

                        preventStealing = false
                    }

                    onCanceled: {
                        if (swipeClaimed)
                            root.swipeEnded(true)

                        swipeClaimed = false
                        preventStealing = false
                    }

                    onClicked: {
                        if (!swipeClaimed)
                            root.imageClicked(root.source)

                        swipeClaimed = false
                    }
                }
            }
}
