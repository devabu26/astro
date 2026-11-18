
import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property string label: "Loading…"
    property real spinnerSize: units.gu(3)
    property real labelSize: units.gu(1.5)
    property bool active: true

    Column {
        anchors.centerIn: parent
        spacing: units.gu(0.3)

        Text {
            id: spinner

            anchors.horizontalCenter: parent.horizontalCenter

            text: "⟳"
            color: "#80b9e8"

            font.pixelSize: root.spinnerSize

            RotationAnimation on rotation {
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
                running: root.active
            }
        }

        Text {
            text: root.label
            color: "#9aabba"

            font.pixelSize: root.labelSize
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
