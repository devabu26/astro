
import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3

Item {
    id: root

    property var backend
    property var notificationPreferences
    signal backRequested()

    property string selectedCategory: ""
    property bool wideLayout: width >= units.gu(90)

    readonly property color pageColor: "#101820"
    readonly property color panelColor: "#1b2935"
    readonly property color raisedColor: "#223342"
    readonly property color accentColor: "#a78bfa"
    readonly property color primaryText: "#f4f6f8"
    readonly property color secondaryText: "#a8b7c5"

    property var categories: [
        { name: "General", subtitle: "Language and accessibility", symbol: "⚙" },
        { name: "Account", subtitle: "Matrix identity and sign-in", symbol: "♙" },
        { name: "Appearance", subtitle: "Themes, colours and chat layout", symbol: "◐" },
        { name: "Notifications", subtitle: "Messages, alerts and sounds", symbol: "♢" },
        { name: "Privacy & Security", subtitle: "Encryption and account security", symbol: "◆" },
        { name: "Chats & Rooms", subtitle: "Messages and room behaviour", symbol: "▤" },
        { name: "Calls", subtitle: "Audio, video and screen sharing", symbol: "🕽" },
        { name: "Media & Storage", subtitle: "Images, stickers, downloads and cache", symbol: "▧" },
        { name: "Network", subtitle: "Homeserver, sync and connectivity", symbol: "🌐" },
        { name: "About Astro", subtitle: "Version and project information", symbol: "ⓘ" }
    ]

    function selectCategory(name) {
        selectedCategory = name
    }

    function categorySubtitle(name) {
        for (var i = 0; i < categories.length; ++i) {
            if (categories[i].name === name)
                return categories[i].subtitle
        }
        return ""
    }

    Component.onCompleted: {
        if (wideLayout)
            selectedCategory = "General"
    }

    onWidthChanged: {
        if (wideLayout && selectedCategory === "")
            selectedCategory = "General"
    }



    Rectangle {
        anchors.fill: parent
        color: root.pageColor
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            id: header
            width: parent.width
            height: units.gu(8)
            color: root.panelColor

            Row {
                anchors.fill: parent
                anchors.leftMargin: units.gu(2)
                anchors.rightMargin: units.gu(2)
                spacing: units.gu(2)

                Rectangle {
                    width: units.gu(5)
                    height: units.gu(5)
                    radius: units.gu(2.5)
                    color: backMouse.pressed
                           ? root.raisedColor : "transparent"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.wideLayout
                             || root.selectedCategory === ""

                    Text {
                        anchors.centerIn: parent
                        text: "‹"
                        color: root.primaryText
                        font.pixelSize: units.gu(3.2)
                    }

                    MouseArea {
                        id: backMouse
                        anchors.fill: parent

                        onClicked: {
                            if (root.selectedCategory === "About Astro"
                                    && aboutSettingsLoader.item
                                    && aboutSettingsLoader.item.goBack()) {
                                return
                            }

                            if (!root.wideLayout
                                    && root.selectedCategory !== "") {
                                root.selectedCategory = ""
                            } else {
                                root.backRequested()
                            }
                        }
                    }
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: units.gu(0.2)

                    Text {
                        text: root.selectedCategory === ""
                              || root.wideLayout
                              ? "Settings" : root.selectedCategory
                        color: root.primaryText
                        font.pixelSize: units.gu(2.5)
                        font.bold: true
                    }

                    Text {
                        visible: root.selectedCategory !== ""
                                 && !root.wideLayout
                        text: root.categorySubtitle(
                                  root.selectedCategory)
                        color: root.secondaryText
                        font.pixelSize: units.gu(1.35)
                    }
                }
            }
        }

        // Adaptive navigation and detail area
        Row {
            width: parent.width
            height: parent.height - header.height
            spacing: 0

            // Category navigation
            Rectangle {
                id: navigationPanel

                width: root.wideLayout
                       ? Math.min(units.gu(34), parent.width * 0.34)
                       : parent.width

                height: parent.height
                color: root.wideLayout
                       ? "#14212b" : root.pageColor

                visible: root.wideLayout
                         || root.selectedCategory === ""

                Flickable {
                    anchors.fill: parent
                    anchors.margins: units.gu(2)
                    contentWidth: width
                    contentHeight: categoryColumn.height
                    clip: true

                    Column {
                        id: categoryColumn
                        width: parent.width
                        spacing: units.gu(1)

                        Text {
                            text: "YOUR PREFERENCES"
                            color: root.accentColor
                            font.pixelSize: units.gu(1.35)
                            font.bold: true
                            leftPadding: units.gu(1)
                            bottomPadding: units.gu(0.5)
                        }

                        Repeater {
                            model: root.categories

                            delegate: Rectangle {
                                width: categoryColumn.width
                                height: units.gu(8)
                                radius: units.gu(1.5)

                                color: root.selectedCategory === modelData.name
                                       ? "#30304a"
                                       : (categoryMouse.pressed
                                          ? root.raisedColor
                                          : "transparent")

                                border.width:
                                    root.selectedCategory === modelData.name
                                    ? 1 : 0

                                border.color: "#6554a4"

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: units.gu(1.5)
                                    anchors.rightMargin: units.gu(1)
                                    spacing: units.gu(1.5)

                                    Rectangle {
                                        width: units.gu(5)
                                        height: units.gu(5)
                                        radius: units.gu(1.4)

                                        color:
                                            root.selectedCategory === modelData.name
                                            ? "#493d70" : root.raisedColor

                                        anchors.verticalCenter: parent.verticalCenter

                                        Text {
                                            anchors.centerIn: parent
                                            text: modelData.symbol
                                            color: root.accentColor
                                            font.pixelSize: units.gu(2.3)
                                        }
                                    }

                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - units.gu(9)
                                        spacing: units.gu(0.3)

                                        Text {
                                            width: parent.width
                                            text: modelData.name
                                            color: root.primaryText
                                            font.pixelSize: units.gu(1.8)
                                            font.bold:
                                                root.selectedCategory === modelData.name
                                            elide: Text.ElideRight
                                        }

                                        Text {
                                            width: parent.width
                                            text: modelData.subtitle
                                            color: root.secondaryText
                                            font.pixelSize: units.gu(1.3)
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                MouseArea {
                                    id: categoryMouse
                                    anchors.fill: parent
                                    onClicked:
                                        root.selectCategory(modelData.name)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    visible: root.wideLayout
                    width: 1
                    height: parent.height
                    anchors.right: parent.right
                    color: "#30414e"
                }
            }

            // Detail panel
            Item {
                id: detailPanel

                width: root.wideLayout
                       ? parent.width - navigationPanel.width
                       : parent.width

                height: parent.height
                visible: root.wideLayout
                         || root.selectedCategory !== ""

                Flickable {
                    anchors.fill: parent
                    anchors.margins: units.gu(2)
                    contentWidth: width
                    contentHeight: detailColumn.height
                    clip: true

                    Column {
                        id: detailColumn
                        width: parent.width
                        spacing: units.gu(2)

                        Text {
                            text: root.selectedCategory
                            color: root.primaryText
                            font.pixelSize: units.gu(2.8)
                            font.bold: true
                            visible: root.selectedCategory !== ""
                                     && root.selectedCategory !== "About Astro"
                        }

                        Text {
                            text: root.categorySubtitle(
                                      root.selectedCategory)
                            color: root.secondaryText
                            font.pixelSize: units.gu(1.6)
                            visible: root.selectedCategory !== ""
                                     && root.selectedCategory !== "About Astro"
                        }

                        // GENERAL
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "General"

                            Component.onCompleted: {
                                if (root.backend)
                                    root.backend.refreshLifecycleExemption()
                            }

                            Text {
                                width: parent.width
                                text: "BACKGROUND ACTIVITY"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(18)
                                radius: units.gu(1.5)
                                color: root.panelColor

                                Column {
                                    anchors.fill: parent
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    anchors.topMargin: units.gu(1.5)
                                    anchors.bottomMargin: units.gu(1.5)
                                    spacing: units.gu(0.8)

                                    Text {
                                        width: parent.width
                                        text: "Allow Astro to run in the background"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(1.8)
                                        font.bold: true
                                        wrapMode: Text.WordWrap
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Prevent Ubuntu Touch from suspending Astro when you switch apps or turn off the screen. This may increase battery and data usage."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.35)
                                        wrapMode: Text.WordWrap
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Current status: " +
                                              (root.backend
                                               && root.backend.lifecycleExemptionEnabled
                                               ? "Enabled" : "Disabled")
                                        color: root.backend
                                               && root.backend.lifecycleExemptionEnabled
                                               ? "#a8d8ff" : root.secondaryText
                                        font.pixelSize: units.gu(1.45)
                                        font.bold: true
                                    }

                                    Text {
                                        id: utTweaksLink
                                        text: "Open UT Tweak Tool"
                                        color: "#8ecbff"
                                        font.pixelSize: units.gu(1.45)
                                        font.underline: true

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: Qt.openUrlExternally(
                                                "https://open-store.io/app/ut-tweak-tool.sverzegnassi"
                                            )
                                        }
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                visible: root.backend
                                         && root.backend.lifecycleExemptionError !== ""
                                text: root.backend
                                      ? root.backend.lifecycleExemptionError : ""
                                color: "#ff9b9b"
                                font.pixelSize: units.gu(1.35)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // ACCOUNT
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Account"

                            Text {
                                text: "MATRIX ACCOUNT"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(11)
                                radius: units.gu(1.5)
                                color: root.panelColor

                                Column {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: units.gu(0.7)

                                    Text {
                                        text: "Signed-in account"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(1.8)
                                        font.bold: true
                                    }

                                    Text {
                                        width: parent.width
                                        text: "Your Matrix account information will be displayed here."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.4)
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            Text {
                                text: "SESSION"
                                color: root.accentColor
                                font.pixelSize: units.gu(1.4)
                                font.bold: true
                            }

                            Rectangle {
                                width: parent.width
                                height: units.gu(8)
                                radius: units.gu(1.5)
                                color: root.panelColor

                                Column {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: units.gu(2)
                                    anchors.rightMargin: units.gu(2)
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: units.gu(0.5)

                                    Text {
                                        text: "Matrix session"
                                        color: root.primaryText
                                        font.pixelSize: units.gu(1.8)
                                    }

                                    Text {
                                        text: "Manage your current sign-in session."
                                        color: root.secondaryText
                                        font.pixelSize: units.gu(1.4)
                                    }
                                }
                            }

                            Button {
                                width: parent.width
                                text: "Log out of Matrix"

                                onClicked: {
                                    if (root.backend)
                                        root.backend.logoutMatrix()
                                }
                            }

                            Text {
                                width: parent.width
                                text: "Logging out ends your current Matrix session."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.35)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // APPEARANCE
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Appearance"

                            Text {
                                width: parent.width
                                text: "Theme, colours, typography and chat layout preferences will be available here."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.6)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // NOTIFICATIONS
                        Loader {
                            id: notificationSettingsLoader
                            width: parent.width
                            active: root.selectedCategory === "Notifications"
                            sourceComponent: notificationSettingsComponent
                        }

                        // PRIVACY AND SECURITY
                        Loader {
                            width: parent.width
                            active: root.selectedCategory === "Privacy & Security"
                            sourceComponent: securitySettingsComponent
                        }



                        // CHATS AND ROOMS
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Chats & Rooms"

                            Text {
                                width: parent.width
                                text: "Chat and room preferences will be added here when the functionality is implemented."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.6)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // CALLS
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Calls"

                            Text {
                                width: parent.width
                                text: "Audio, video and screen-sharing preferences will be added here when the functionality is implemented."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.6)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // MEDIA AND STORAGE
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Media & Storage"

                            Text {
                                width: parent.width
                                text: "Media, downloads and storage preferences will be added here when the functionality is implemented."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.6)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // NETWORK
                        Column {
                            width: parent.width
                            spacing: units.gu(1)
                            visible: root.selectedCategory === "Network"

                            Text {
                                width: parent.width
                                text: "Homeserver, connectivity and synchronisation information will be added here when implemented."
                                color: root.secondaryText
                                font.pixelSize: units.gu(1.6)
                                wrapMode: Text.WordWrap
                            }
                        }

                        // ABOUT ASTRO
                        Loader {
                            id: aboutSettingsLoader
                            width: parent.width
                            active: root.selectedCategory === "About Astro"
                            sourceComponent: aboutSettingsComponent
                        }

                        Item {
                            width: 1
                            height: units.gu(3)
                        }
                    }
                }
            }
        }
    }


        Component {
        id: aboutSettingsComponent

        AboutSettingsPage {
            width: parent.width
            wideLayout: root.wideLayout
        }
    }

Component {
        id: notificationSettingsComponent

        NotificationSettingsPage {
            width: parent.width
            backend: root.backend
            notificationPreferences: root.notificationPreferences
        }
    }

    Component {
        id: securitySettingsComponent

        SecuritySettingsPage {
            width: parent.width
            backend: root.backend
        }
    }

}
