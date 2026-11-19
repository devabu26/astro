
import QtQuick 2.7
import Lomiri.Components 1.3
import QtQuick.Layouts 1.3
import QtGraphicalEffects 1.0
import "Animations"
import "Components"

Item {
    id: root

    // =========================================================
    // Properties
    // =========================================================

    property var backend
    property var mainRoot

    property bool wideLayout: width >= units.gu(75)
    property bool fullscreenLandscapeVideo:
        roomView.fullscreenVideoActive && width > height
    property bool showConversation: false

    property string selectedRoomId: ""
    property string selectedRoom: "Welcome to Astro"
    property string selectedPreview: "Your Matrix client starts here."
    property string selectedRoomAvatarUrl: ""
    property string storageResult: ""

    property bool roomsLoading: false
    property bool moreMenuVisible: false

    // =========================================================
    // Room data model
    // =========================================================

    ListModel {
        id: roomModel
    }

    // =========================================================
    // Matrix room loading state
    // =========================================================

    function startRoomLoading() {
        roomsLoading = true

        console.log(
            "ASTRO: Matrix room synchronisation started"
        )
    }

    function stopRoomLoading() {
        roomsLoading = false

        console.log(
            "ASTRO: Matrix room synchronisation finished"
        )
    }

    // =========================================================
    // Room data loading
    // =========================================================


function loadRooms(json, finished) {
    try {
        var data = JSON.parse(json)

        console.log("ASTRO UNREAD DEBUG: Initial room response:", data.rooms.length)

        for (var d = 0; d < data.rooms.length; d++) {
            console.log(
                "ASTRO UNREAD DEBUG: Initial:",
                data.rooms[d].name,
                "ID:", data.rooms[d].room_id,
                "Unread:", data.rooms[d].unread_count,
                "Mentions:", data.rooms[d].mention_count
            )
        }

        if (!data || !data.rooms) {
            console.log(
                "ASTRO: Invalid Matrix rooms response"
            )

            if (finished)
                root.stopRoomLoading()

            return
        }

        roomModel.clear()

        for (var i = 0; i < data.rooms.length; i++) {
            var room = data.rooms[i]

            var roomName = room.name ||
                           room.room_id ||
                           "Unnamed room"

            var kind = room.kind || "room"

            var preview = room.topic || (
                kind === "space" ? "Matrix Space" :
                kind === "dm" ? "Direct message" :
                "Matrix room"
            )

            if (kind === "space")
                preview = "Space · " + preview
            else if (kind === "dm")
                preview = "Direct message · " + preview

            var avatar = roomName.length > 0
                         ? roomName.charAt(0).toUpperCase()
                         : "#"

            var colour = kind === "space" ? "#5265a4" :
                         kind === "dm" ? "#398875" :
                         "#a16b9b"

            var roomId = room.room_id || ""
            var mxcUri = room.avatar_url || ""

            console.log(
                "ASTRO SPACE DEBUG:",
                roomName,
                "kind:", kind,
                "roomId:", roomId,
                "parents:",
                JSON.stringify(room.parent_space_ids || [])
            )

            roomModel.append({
                name: roomName,
                preview: preview,
                avatar: avatar,
                avatarUrl: "",
                colour: colour,
                roomId: roomId,
                kind: kind,
                unreadCount: Number(room.unread_count || 0),
                mentionCount: Number(room.mention_count || 0),
                parentSpaceIds: JSON.stringify(
                    room.parent_space_ids || []
                )
            })

            if (root.backend &&
                roomId !== "" &&
                mxcUri !== "") {

                root.backend.requestRoomAvatar(
                    roomId,
                    mxcUri
                )
            }
        }

        roomNavigator.rebuild()

        console.log(
            "ASTRO: Loaded",
            data.rooms.length,
            "Matrix rooms and Spaces",
            "finished:", finished
        )

        // Cached results populate the UI immediately.
        // Keep the animation running until network sync finishes.
        if (finished)
            root.stopRoomLoading()

    } catch (error) {
        console.log(
            "ASTRO: Could not parse Matrix rooms:",
            error
        )

        if (finished)
            root.stopRoomLoading()
    }
}


    // =========================================================
    // Room avatar updates
    // =========================================================

    function updateRoomAvatar(roomId, dataUrl) {
        if (!roomId || !dataUrl)
            return

        for (var i = 0; i < roomModel.count; i++) {
            if (roomModel.get(i).roomId === roomId) {
                roomModel.setProperty(
                    i,
                    "avatarUrl",
                    dataUrl
                )

                if (root.selectedRoomId === roomId)
                    root.selectedRoomAvatarUrl = dataUrl

                roomNavigator.rebuild()

                return
            }
        }
    }

    // =========================================================
    // Backend room-avatar signals
    // =========================================================

    Connections {
        target: root.backend

        onRoomAvatarRetrieved: {
            root.updateRoomAvatar(roomId, dataUrl)
        }

        onRoomAvatarFailed: {
            console.log(
                "ASTRO: Room avatar retrieval failed:",
                roomId,
                error
            )
        }

        onMatrixLiveTimelineUpdated: {
            try {
                var result = JSON.parse(json)
                var counts = result.unread_counts || []

                console.log(
                    "ASTRO UNREAD DEBUG: Live signal received:",
                    counts.length,
                    "room count updates"
                )

                for (var c = 0; c < counts.length; c++) {
                    var item = counts[c]

                    console.log(
                        "ASTRO UNREAD DEBUG: Live room:",
                        item.room_id,
                        "Unread:", item.unread_count,
                        "Mentions:", item.mention_count
                    )

                    var roomIndex = -1

                    for (var i = 0; i < roomModel.count; i++) {
                        if (roomModel.get(i).roomId === item.room_id) {
                            roomIndex = i
                            break
                        }
                    }

                    if (roomIndex < 0)
                        continue

                    var unread = Number(item.unread_count || 0)
                    var mentions = Number(item.mention_count || 0)

                    roomModel.setProperty(
                        roomIndex, "unreadCount", unread
                    )
                    roomModel.setProperty(
                        roomIndex, "mentionCount", mentions
                    )

                    roomNavigator.updateUnreadCounts(
                        item.room_id, unread, mentions
                    )
                }
            } catch (error) {
                console.log(
                    "ASTRO: Could not update unread counts:",
                    error
                )
            }
        }
    }

    // =========================================================
    // Main application background
    // =========================================================

    Rectangle {
        id: applicationBackground

        anchors.fill: parent
        color: "#101820"
        visible: backend.storageReady

        Row {
            anchors.fill: parent
            spacing: 0

            // =================================================
            // Conversations sidebar
            // =================================================

            Rectangle {
                id: sidebar

                width: root.fullscreenLandscapeVideo
                       ? 0
                       : root.wideLayout
                         ? units.gu(32)
                         : parent.width

                height: parent.height
                color: "#17232e"

                visible: !root.fullscreenLandscapeVideo &&
                         (root.wideLayout || !root.showConversation)

                Column {
                    anchors.fill: parent
                    spacing: 0

                    // =========================================
                    // Sidebar heading and More menu button
                    // =========================================

                    Item {
                        width: parent.width
                        height: units.gu(8)

                        Text {
                            anchors {
                                left: parent.left
                                leftMargin: units.gu(2)
                                verticalCenter: parent.verticalCenter
                            }

                            text: "CONVERSATIONS"
                            color: "#91a4b5"
                            font.pixelSize: units.gu(1.5)
                            font.bold: true
                        }

                        Rectangle {
                            id: moreButton

                            anchors {
                                right: parent.right
                                rightMargin: units.gu(1)
                                verticalCenter: parent.verticalCenter
                            }

                            width: units.gu(5)
                            height: units.gu(5)

                            radius: units.gu(1)

                            color: root.moreMenuVisible
                                   ? "#40566a"
                                   : "transparent"

                            Text {
                                anchors.centerIn: parent

                                text: "⋯"
                                color: "#b4c4d1"

                                font.pixelSize: units.gu(3)
                            }

                            MouseArea {
                                anchors.fill: parent

                                onClicked: {
                                    root.moreMenuVisible =
                                        !root.moreMenuVisible
                                }
                            }
                        }
                    }

                    // =========================================
                    // Matrix synchronisation status bar
                    // =========================================

                    Item {
                        id: matrixLoadingBar

                        width: parent.width
                        height: root.roomsLoading
                               ? units.gu(8)
                               : 0

                        visible: root.roomsLoading
                        clip: true

                        Text {
                            anchors {
                                left: parent.left
                                leftMargin: units.gu(7)
                                verticalCenter: parent.verticalCenter
                            }

                            text: "Synchronising Matrix..."
                            color: "#b4c4d1"
                            font.pixelSize: units.gu(1.6)
                        }
                    }

                    // =========================================
                    // Search and segmented navigation
                    // =========================================

                    Item {
                        id: chatNavigationControls

                        width: parent.width
                        height: units.gu(15)
                        z: 10

                        Column {
                            anchors {
                                fill: parent
                                leftMargin: units.gu(1)
                                rightMargin: units.gu(1)
                                topMargin: units.gu(0.5)
                            }

                            spacing: units.gu(0.6)

                            // ---------------------------------
                            // Search field
                            // ---------------------------------

                            Row {
                                width: parent.width
                                height: units.gu(5)

                                spacing: units.gu(0.5)

                                Rectangle {
                                    width: parent.width -
                                           units.gu(5.5)

                                    height: parent.height

                                    radius: units.gu(1)
                                    color: "#263744"

                                    TextField {
                                        id: chatSearchField

                                        anchors {
                                            fill: parent
                                            leftMargin: units.gu(1)
                                            rightMargin: units.gu(1)
                                        }

                                        placeholderText:
                                            "Search rooms, people or Spaces"

                                        color: "white"
                                        font.pixelSize: units.gu(1.6)

                                        onTextChanged: {
                                            roomNavigator.searchText = text
                                        }
                                    }
                                }

                                Rectangle {
                                    width: units.gu(5)
                                    height: parent.height

                                    radius: units.gu(1)
                                    color: "#263744"

                                    Text {
                                        anchors.centerIn: parent

                                        text: "×"
                                        color: "#b4c4d1"

                                        font.pixelSize: units.gu(3)
                                    }

                                    MouseArea {
                                        anchors.fill: parent

                                        onClicked: {
                                            chatSearchField.text = ""
                                            chatSearchField.forceActiveFocus()
                                        }
                                    }
                                }
                            }

                            // ---------------------------------
                            // Segmented navigation background
                            // ---------------------------------

                            Rectangle {
                                id: navigationSegmentBackground

                                width: parent.width
                                height: units.gu(8)

                                radius: units.gu(1.5)
                                color: "#263744"

                                Row {
                                    anchors {
                                        fill: parent
                                        margins: units.gu(0.4)
                                    }

                                    spacing: units.gu(0.25)

                                    Repeater {
                                        model: [
                                            {
                                                label: "All",
                                                value: "all",
                                                icon: "view-list-symbolic"
                                            },
                                            {
                                                label: "Spaces",
                                                value: "spaces",
                                                icon: "view-grid-symbolic"
                                            },
                                            {
                                                label: "Direct",
                                                value: "direct",
                                                icon: "contact"
                                            },
                                            {
                                                label: "Rooms",
                                                value: "rooms",
                                                icon: "tag"
                                            }
                                        ]

                                        delegate: Rectangle {
                                            width: (
                                                parent.width -
                                                units.gu(0.75)
                                            ) / 4

                                            height: parent.height

                                            radius: units.gu(1.1)

                                            color:
                                                roomNavigator.filterMode ===
                                                modelData.value
                                                ? "#40566a"
                                                : "transparent"

                                            Column {
                                                anchors.centerIn: parent

                                                spacing: units.gu(0.35)

                                                Icon {
                                                    anchors.horizontalCenter:
                                                        parent.horizontalCenter

                                                    name: modelData.icon

                                                    width: units.gu(2.4)
                                                    height: units.gu(2.4)

                                                    color:
                                                        roomNavigator.filterMode ===
                                                        modelData.value
                                                        ? "white"
                                                        : "#aabac7"
                                                }

                                                Text {
                                                    anchors.horizontalCenter:
                                                        parent.horizontalCenter

                                                    text: modelData.label

                                                    color:
                                                        roomNavigator.filterMode ===
                                                        modelData.value
                                                        ? "white"
                                                        : "#aabac7"

                                                    font.pixelSize:
                                                        units.gu(1.25)

                                                    font.bold:
                                                        roomNavigator.filterMode ===
                                                        modelData.value
                                                }
                                            }

                                            MouseArea {
                                                anchors.fill: parent

                                                onClicked: {
                                                    root.moreMenuVisible = false

                                                    roomNavigator.filterMode =
                                                        modelData.value
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // =====================================
                        // More submenu
                        // =====================================

                        Rectangle {
                            id: chatMoreMenu

                            anchors {
                                top: parent.bottom
                                right: parent.right
                                rightMargin: units.gu(1)
                            }

                            width: units.gu(23)
                            height: units.gu(11)

                            radius: units.gu(1)
                            color: "#263744"

                            border.color: "#526575"
                            border.width: units.dp(1)

                            visible: root.moreMenuVisible
                            z: 20

                            Column {
                                anchors.fill: parent
                                anchors.margins: units.gu(0.5)

                                spacing: units.gu(0.5)

                                Rectangle {
                                    width: parent.width
                                    height: units.gu(4.5)

                                    radius: units.gu(0.5)
                                    color: "#344b5c"

                                    Text {
                                        anchors.centerIn: parent

                                        text: "Expand all Spaces"
                                        color: "white"

                                        font.pixelSize: units.gu(1.5)
                                    }

                                    MouseArea {
                                        anchors.fill: parent

                                        onClicked: {
                                            roomNavigator.expandAllSpaces()
                                            root.moreMenuVisible = false
                                        }
                                    }
                                }

                                Rectangle {
                                    width: parent.width
                                    height: units.gu(4.5)

                                    radius: units.gu(0.5)
                                    color: "#344b5c"

                                    Text {
                                        anchors.centerIn: parent

                                        text: "Collapse all Spaces"
                                        color: "white"

                                        font.pixelSize: units.gu(1.5)
                                    }

                                    MouseArea {
                                        anchors.fill: parent

                                        onClicked: {
                                            roomNavigator.collapseAllSpaces()
                                            root.moreMenuVisible = false
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // =========================================
                    // Matrix room navigation
                    // =========================================

                    RoomNavigator {
                        id: roomNavigator

                        width: parent.width

                        height: parent.height -
                                units.gu(31) -
                                (root.roomsLoading
                                 ? units.gu(8)
                                 : 0)

                        roomsModel: roomModel
                        selectedRoom: root.selectedRoom

                        onRoomSelected: {
                            root.selectedRoomId = roomId
                            root.selectedRoom = name
                            root.selectedPreview = preview
                            root.selectedRoomAvatarUrl = ""

                            for (var i = 0; i < roomModel.count; i++) {
                                if (roomModel.get(i).roomId === roomId) {
                                    root.selectedRoomAvatarUrl =
                                        roomModel.get(i).avatarUrl
                                    break
                                }
                            }

                            root.showConversation = true

                            if (root.backend && roomId !== "") {
                                console.log(
                                    "ASTRO: Requesting timeline for",
                                    roomId
                                )

                                root.backend.requestRoomTimeline(roomId)
                            }
                        }
                    }

                    // =========================================
                    // Sidebar settings button
                    // =========================================

                    Rectangle {
                        width: parent.width
                        height: units.gu(8)

                        color: "#1b2935"

                        Button {
                            anchors {
                                fill: parent
                                margins: units.gu(1)
                            }

                            text: "Settings"

                            onClicked: {
                                root.mainRoot.showingSettings = true
                            }
                        }
                    }
                }
            }

            // =================================================
            // Conversation view
            // =================================================

            RoomView {
                id: roomView

                width: root.fullscreenLandscapeVideo
                       ? parent.width
                       : root.wideLayout
                         ? parent.width - sidebar.width
                         : parent.width

                height: parent.height

                visible: root.wideLayout ||
                         root.showConversation

                backend: root.backend
                roomId: root.selectedRoomId
                roomName: root.selectedRoom
                avatarUrl: root.selectedRoomAvatarUrl
                wideLayout: root.wideLayout

                onBackRequested: {
                    root.showConversation = false
                }
            }
        }
    }

    // =========================================================
    // Floating Matrix loading animation
    // =========================================================

    MatrixLoadingAnimation {
        id: matrixLoadingAnimation

        anchors.fill: parent

        active: root.roomsLoading

        targetX: units.gu(4)
        targetY: units.gu(12)
    }
}
