import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string body: ""

    width: parent ? parent.width : 0
    height: eventText.implicitHeight + units.gu(1.5)

    Text {
        id: eventText

        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
        }

        text: "[" + root.body + "]"
        color: "#91a3af"
        font.pixelSize: units.gu(1.5)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
