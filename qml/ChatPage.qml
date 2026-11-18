
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
    property bool showConversation: false

    property string selectedRoomId: ""
    property string selectedRoom: "Welcome to Astro"
    property string selectedPreview: "Your Matrix client starts here."
    property string selectedRoomAvatarUrl: ""
    property string storageResult: ""

    // True while the initial Matrix room sync is in progress.
    property bool roomsLoading: false

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

    function loadRooms(json) {
        try {
            var data = JSON.parse(json)

            if (!data || !data.rooms) {
                console.log(
                    "ASTRO: Invalid Matrix rooms response"
                )
                return
            }

            roomModel.clear()

            // Keep the loading animation active while
            // the room model is being populated.

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

                // Add the room before requesting its avatar.
                // The avatar response will update this model entry.
                roomModel.append({
                    name: roomName,
                    preview: preview,
                    avatar: avatar,
                    avatarUrl: "",
                    colour: colour,
                    roomId: roomId,
                    kind: kind,
                    parentSpaceIds: JSON.stringify(room.parent_space_ids || [])
                })

                // Request the avatar asynchronously.
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

            // Room population is complete.
            root.stopRoomLoading()

            console.log(
                "ASTRO: Loaded",
                data.rooms.length,
                "Matrix rooms and Spaces"
            )

        } catch (error) {
            console.log(
                "ASTRO: Could not parse Matrix rooms:",
                error
            )
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

                width: root.wideLayout
                       ? units.gu(32)
                       : parent.width

                height: parent.height
                color: "#17232e"

                visible: root.wideLayout ||
                         !root.showConversation

                Column {
                    anchors.fill: parent
                    spacing: 0

                    // =========================================
                    // Sidebar heading
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
                    // =========================================
                    // Matrix room navigation
                    // =========================================

                    RoomNavigator {
                        id: roomNavigator
                        width: parent.width

                        height: parent.height -
                                units.gu(16) -
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

                width: root.wideLayout
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
    //
    // This overlays the ChatPage, allowing the diamond to
    // travel from the screen centre into the sidebar bar.
    // =========================================================

    MatrixLoadingAnimation {
        id: matrixLoadingAnimation

        anchors.fill: parent

        active: root.roomsLoading

        // The loading strip is immediately below the
        // eight-gu-high sidebar heading.
        targetX: units.gu(4)
        targetY: units.gu(12)
    }
}
