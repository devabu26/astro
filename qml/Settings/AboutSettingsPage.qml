import QtQuick 2.7
import Lomiri.Components 1.3
import "../Components/Settings"
import "../BuildInfo.js" as BuildInfo

Column {
    id: root

    property string aboutDocument: ""
    property bool wideLayout: false

    readonly property color panelColor: "#1b2935"
    readonly property color raisedColor: "#223342"
    readonly property color accentColor: "#a78bfa"
    readonly property color primaryText: "#f4f6f8"
    readonly property color secondaryText: "#a8b7c5"

    width: parent ? parent.width : units.gu(40)
    spacing: units.gu(1)

    // Called by the parent Settings header.
    // Returns true if a document was closed.
    function goBack() {
        if (aboutDocument !== "") {
            aboutDocument = ""
            return true
        }

        return false
    }

    // On wide layouts, the parent header does not show a back button
    // while a category is selected, so provide one here.
    Row {
        width: parent.width
        spacing: units.gu(1)
        visible: root.wideLayout && root.aboutDocument !== ""

        Rectangle {
            width: units.gu(5)
            height: units.gu(5)
            radius: units.gu(2.5)
            color: aboutBackMouse.pressed
                   ? root.raisedColor : "transparent"

            Text {
                anchors.centerIn: parent
                text: "‹"
                color: root.primaryText
                font.pixelSize: units.gu(3.2)
            }

            MouseArea {
                id: aboutBackMouse
                anchors.fill: parent
                onClicked: root.goBack()
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.aboutDocument
            color: root.primaryText
            font.pixelSize: units.gu(2.2)
            font.bold: true
        }
    }

                        // ABOUT ASTRO
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.aboutDocument === ""
                            height: visible ? implicitHeight : 0

                            Rectangle {
                                width: parent.width
                                height: units.gu(14)
                                radius: units.gu(2)
                                color: root.panelColor

                                Column {
                                    anchors.centerIn: parent
                                    spacing: units.gu(1)

                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "✦"
                                        color: root.accentColor
                                        font.pixelSize: units.gu(4)
                                    }

                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "Astro"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(2.4)
                                        font.bold: true
                                    }

                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "Matrix client for Ubuntu Touch"
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.4)
                                    }

                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "Version " + BuildInfo.version
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.3)
                                    }
                                }
                            }

                            Text {
                                text: "PROJECT INFORMATION"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(9)
                                radius: units.gu(1.5)
                                color: readmeMouse.pressed
                                       ? root.raisedColor : root.panelColor

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    spacing: units.gu(2)

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "▤"
                                        color: root.accentColor
                                        font.pixelSize: units.gu(2.8)
                                    }

                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - units.gu(8)
                                        spacing: units.gu(0.4)

                                        Text {
                                            text: "README"
                                            color: root.primaryText
                                            font.pixelSize: units.gu(1.8)
                                            font.bold: true
                                        }

                                        Text {
                                            text: "Project overview and documentation"
                                            color: root.secondaryText
                                            font.pixelSize: units.gu(1.35)
                                        }
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "›"
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(3)
                                    }
                                }

                                MouseArea {
                                    id: readmeMouse
                                    anchors.fill: parent
                                    onClicked: root.aboutDocument = "README"
                                }
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(9)
                                radius: units.gu(1.5)
                                color: licenseMouse.pressed
                                       ? root.raisedColor : root.panelColor

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    spacing: units.gu(2)

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "⚖"
                                        color: root.accentColor
                                        font.pixelSize: units.gu(2.8)
                                    }

                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - units.gu(8)
                                        spacing: units.gu(0.4)

                                        Text {
                                            text: "License"
                                            color: root.primaryText
                                            font.pixelSize: units.gu(1.8)
                                            font.bold: true
                                        }

                                        Text {
                                            text: "GNU General Public License v3"
                                            color: root.secondaryText
                                            font.pixelSize: units.gu(1.35)
                                        }
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "›"
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(3)
                                    }
                                }

                                MouseArea {
                                    id: licenseMouse
                                    anchors.fill: parent
                                    onClicked: root.aboutDocument = "License"
                                }
                            }
                        }

                        // README AND LICENCE DOCUMENT VIEWER
                        DocumentViewer {
                            width: parent.width
                            visible: root.aboutDocument !== ""
                            height: visible ? implicitHeight : 0

                            documentTitle: root.aboutDocument
                            documentUrl: root.aboutDocument === "README"
                                          ? Qt.resolvedUrl("../../README.md")
                                          : Qt.resolvedUrl("../../LICENSE")
                        }


}
