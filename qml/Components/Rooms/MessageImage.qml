import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string source: ""

    signal imageClicked(string imageUrl)

    height: messageImage.status === Image.Ready &&
            messageImage.implicitWidth > 0
            ? Math.min(
                  width * messageImage.implicitHeight /
                  messageImage.implicitWidth,
                  units.gu(40)
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

            onClicked: root.imageClicked(root.source)
        }
    }
}
