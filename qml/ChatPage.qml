
import QtQuick 2.7
import Qt.labs.settings 1.0
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
    property string selectedRoomKind: ""
    property bool selectedRoomEncrypted: false

    // MatrixRTC membership snapshots, indexed by room ID.
    property var rtcMembershipsByRoom: ({})
    property var rtcCallStatesByRoom: ({})

    // LiveKit audio state and participants, indexed by room ID.
    property var rtcAudioStatesByRoom: ({})

    // Notification navigation request passed to RoomView.
    property string pendingNotificationEventId: ""
    property int notificationRequestSerial: 0
    property string storageResult: ""

    property bool roomsLoading: false
    property bool moreMenuVisible: false
    property bool sortMenuVisible: false

    // Keep the room ordering choice across app restarts. Qt.labs.settings
    // stores this in the application's private settings location.
    Settings {
        id: roomNavigationSettings
        category: "RoomNavigation"
        property string sortMode: "name"
    }

    function savedRoomSortMode() {
        return roomNavigationSettings.sortMode === "recent"
                ? "recent" : "name"
    }

    function rtcParticipantDisplayName(name, identity) {
        var displayName = String(name || "").trim()
        var matrixIdentity = String(identity || "").trim()

        if (displayName && displayName.charAt(0) !== "@")
            return displayName

        var source = displayName.charAt(0) === "@"
                ? displayName : matrixIdentity

        if (source.charAt(0) === "@") {
            var separator = source.indexOf(":")
            var localpart = separator > 1
                    ? source.substring(1, separator)
                    : source.substring(1)

            if (localpart.length > 0) {
                return localpart.charAt(0).toUpperCase() +
                       localpart.substring(1)
            }
        }

        return displayName || matrixIdentity || "Participant"
    }

    // Dismiss the search keyboard without clearing the search.
    function dismissSearchKeyboard() {
        chatSearchField.focus = false
        Qt.inputMethod.hide()
    }

    // =========================================================
    // Room data model
    // =========================================================

    // Background tap target. Kept behind the page controls so
    // room rows, buttons and other interactive elements retain
    // their normal click handling.
    MouseArea {
        anchors.fill: parent
        z: -1

        onClicked: {
            root.dismissSearchKeyboard()
        }
    }

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


function roomNameForId(id) {
    if (!id)
        return ""

    for (var i = 0; i < roomModel.count; i++) {
        var room = roomModel.get(i)
        if (room.roomId === id)
            return room.name
    }

    return id
}

function roomIsEncrypted(id) {
    if (!id)
        return false

    for (var i = 0; i < roomModel.count; i++) {
        var room = roomModel.get(i)
        if (room.roomId === id)
            return room.isEncrypted === true
    }

    // Fail closed when the room is not in the current room model.
    return false
}

function openNotification(roomId, eventId) {
    if (!roomId)
        return

    var found = false

    for (var i = 0; i < roomModel.count; i++) {
        var room = roomModel.get(i)

        if (room.roomId !== roomId)
            continue

        root.selectedRoomId = roomId
        root.selectedRoom = room.name
        root.selectedPreview = room.preview
        root.selectedRoomAvatarUrl = room.avatarUrl || ""
        root.selectedRoomKind = room.kind || "room"
        root.selectedRoomEncrypted = room.isEncrypted === true
        found = true
        break
    }

    // The room may not yet be present in the current room list.
    if (!found) {
        root.selectedRoomId = roomId
        root.selectedRoom = roomId
        root.selectedPreview = ""
        root.selectedRoomAvatarUrl = ""
        root.selectedRoomKind = "room"
        root.selectedRoomEncrypted = false
    }

    root.showConversation = true
    root.pendingNotificationEventId = eventId || ""
    root.notificationRequestSerial++

    // Always retrieve a fresh timeline snapshot. This also recovers
    // when a notification targets the room already displayed but
    // its current timeline is empty or failed to load.
    if (root.backend)
        root.backend.requestRoomTimeline(roomId)
}

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
                isEncrypted: room.is_encrypted === true,
                unreadCount: Number(room.unread_count || 0),
                mentionCount: Number(room.mention_count || 0),
                parentSpaceIds: JSON.stringify(
                    room.parent_space_ids || []
                ),
                lastActivity: Number(room.last_activity || 0),
                spaceLastActivity: Number(room.space_last_activity || 0)
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
        // Initial loading ends once the first room response arrives.
        root.stopRoomLoading()

    } catch (error) {
        console.log(
            "ASTRO: Could not parse Matrix rooms:",
            error
        )

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

                onMatrixRtcMembershipsUpdated: {
            try {
                var result = JSON.parse(json)
                var updates = result.updates || []

                var updatedMemberships = ({})
                var updatedCallStates = ({})

                for (var roomId in root.rtcMembershipsByRoom) {
                    updatedMemberships[roomId] =
                        root.rtcMembershipsByRoom[roomId]
                }

                for (var roomId in root.rtcCallStatesByRoom) {
                    updatedCallStates[roomId] =
                        root.rtcCallStatesByRoom[roomId]
                }

                for (var i = 0; i < updates.length; i++) {
                    var item = updates[i]

                    if (!item.room_id)
                        continue

                    if (Array.isArray(item.memberships))
                        updatedMemberships[item.room_id] = item.memberships

                    updatedCallStates[item.room_id] = {
                        slotAvailable: item.slot_available === true,
                        joinedByMe: item.joined_by_me === true
                    }
                }

                if (updates.length > 0) {
                    root.rtcMembershipsByRoom = updatedMemberships
                    root.rtcCallStatesByRoom = updatedCallStates

                    console.log(
                        "ASTRO RTC: Updated room call states:",
                        updates.length
                    )
                }
            } catch (error) {
                console.warn(
                    "ASTRO: Could not process MatrixRTC membership updates:",
                    error
                )
            }
        }

        onMatrixRtcAudioEventsUpdated: {
            try {
                var result = JSON.parse(json)
                var events = result.events || []
                var updatedStates = ({})

                for (var roomId in root.rtcAudioStatesByRoom) {
                    var previous = root.rtcAudioStatesByRoom[roomId]
                    updatedStates[roomId] = {
                        active: previous.active === true,
                        reconnecting: previous.reconnecting === true,
                        connectionState: previous.connectionState || "",
                        participants: (previous.participants || []).slice(0),
                        lastError: previous.lastError || ""
                    }
                }

                for (var i = 0; i < events.length; i++) {
                    var event = events[i]
                    var eventRoomId = event.room_id

                    if (!eventRoomId)
                        continue

                    if (!updatedStates[eventRoomId]) {
                        updatedStates[eventRoomId] = {
                            active: false,
                            reconnecting: false,
                            connectionState: "",
                            participants: [],
                            lastError: ""
                        }
                    }

                    var state = updatedStates[eventRoomId]
                    var participants = state.participants
                    var participantIndex = -1

                    for (var p = 0; p < participants.length; p++) {
                        if (participants[p].identity === event.identity) {
                            participantIndex = p
                            break
                        }
                    }

                    if (event.type === "participant_connected" ||
                            event.type === "participant_active") {
                        var participant = {
                            identity: event.identity || "",
                            name: root.rtcParticipantDisplayName(
                                event.name, event.identity
                            ),
                            speaking: event.speaking === true,
                            audioLevel: Number(event.audio_level || 0),
                            hasTrack: participantIndex >= 0 &&
                                      participants[participantIndex].hasTrack === true
                        }

                        if (participantIndex >= 0)
                            participants[participantIndex] = participant
                        else
                            participants.push(participant)

                    } else if (event.type === "participant_disconnected") {
                        if (participantIndex >= 0)
                            participants.splice(participantIndex, 1)

                    } else if (event.type === "track_subscribed") {
                        if (participantIndex >= 0) {
                            participants[participantIndex].hasTrack = true
                        } else if (event.identity) {
                            // A camera subscription is itself authoritative evidence
                            // of a participant. Do not depend on event ordering.
                            participants.push({
                                identity: event.identity,
                                name: root.rtcParticipantDisplayName(
                                    event.name, event.identity
                                ),
                                speaking: false,
                                audioLevel: 0,
                                hasTrack: true
                            })
                        }

                    } else if (event.type === "track_unsubscribed") {
                        if (participantIndex >= 0) {
                            participants[participantIndex].hasTrack = false
                        }

                    } else if (event.type === "reconnecting") {
                        state.reconnecting = true

                    } else if (event.type === "reconnected") {
                        state.reconnecting = false
                        state.active = true

                    } else if (event.type === "connection_state_changed") {
                        state.connectionState = event.state || ""
                        var connectionState =
                            String(event.state || "").toLowerCase()

                        if (connectionState.indexOf("connected") >= 0) {
                            state.active = true
                            state.reconnecting = false
                        } else if (connectionState.indexOf("disconnected") >= 0) {
                            state.active = false
                            state.reconnecting = false
                        }

                    } else if (event.type === "disconnected") {
                        state.active = false
                        state.reconnecting = false
                        state.connectionState = "Disconnected"
                        state.participants = []
                    }
                }

                if (events.length > 0)
                    root.rtcAudioStatesByRoom = updatedStates

            } catch (error) {
                console.warn(
                    "ASTRO: Could not process LiveKit audio events:",
                    error
                )
            }
        }

        onMatrixRtcAudioActionFinished: {
            var updatedStates = ({})

            for (var existingRoomId in root.rtcAudioStatesByRoom) {
                var previous = root.rtcAudioStatesByRoom[existingRoomId]
                updatedStates[existingRoomId] = {
                    active: previous.active === true,
                    reconnecting: previous.reconnecting === true,
                    connectionState: previous.connectionState || "",
                    participants: (previous.participants || []).slice(0),
                    lastError: previous.lastError || ""
                }
            }

            if (!updatedStates[roomId]) {
                updatedStates[roomId] = {
                    active: false,
                    reconnecting: false,
                    connectionState: "",
                    participants: [],
                    lastError: ""
                }
            }

            updatedStates[roomId].lastError = error || ""
            root.rtcAudioStatesByRoom = updatedStates
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
                        // Keep the heading and its dropdown above the search/navigation controls.
                        z: 20

                        // Room ordering selector sits beside the existing overflow menu.
                        Rectangle {
                            id: roomSortButton

                            anchors {
                                right: moreButton.left
                                rightMargin: units.gu(0.5)
                                verticalCenter: parent.verticalCenter
                            }

                            width: units.gu(8)
                            height: units.gu(5)
                            radius: units.gu(1)
                            color: root.sortMenuVisible ? "#40566a" : "transparent"

                            Text {
                                anchors.centerIn: parent
                                text: roomNavigator.sortMode === "recent" ? "Recent ▾" : "A–Z ▾"
                                color: "#b4c4d1"
                                font.pixelSize: units.gu(1.35)
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    root.moreMenuVisible = false
                                    root.sortMenuVisible = !root.sortMenuVisible
                                }
                            }
                        }

                        Rectangle {
                            id: roomSortMenu

                            anchors {
                                top: parent.bottom
                                right: parent.right
                            }

                            width: units.gu(19)
                            height: units.gu(10)
                            radius: units.gu(1)
                            color: "#263744"
                            border.color: "#526575"
                            border.width: units.dp(1)
                            visible: root.sortMenuVisible
                            z: 30

                            Column {
                                anchors.fill: parent
                                anchors.margins: units.gu(0.5)
                                spacing: units.gu(0.5)

                                Repeater {
                                    model: [
                                        { label: "A–Z", value: "name" },
                                        { label: "Recent", value: "recent" }
                                    ]

                                    delegate: Rectangle {
                                        width: parent.width
                                        height: units.gu(4)
                                        radius: units.gu(0.5)
                                        color: roomNavigator.sortMode === modelData.value
                                               ? "#40566a" : "#344b5c"

                                        Text {
                                            anchors {
                                                left: parent.left
                                                leftMargin: units.gu(1.5)
                                                verticalCenter: parent.verticalCenter
                                            }
                                            text: modelData.label
                                            color: "white"
                                            font.pixelSize: units.gu(1.5)
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: {
                                                // Update both the live view and
                                                // the persisted preference.
                                                roomNavigator.sortMode = modelData.value
                                                roomNavigationSettings.sortMode = modelData.value
                                                root.sortMenuVisible = false
                                            }
                                        }
                                    }
                                }
                            }
                        }

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
                                    root.sortMenuVisible = false
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
                        sortMode: root.savedRoomSortMode()
                        rtcMembershipsByRoom: root.rtcMembershipsByRoom
                        rtcCallStatesByRoom: root.rtcCallStatesByRoom

                        onRoomSelected: {
                            root.dismissSearchKeyboard()

                            root.selectedRoomId = roomId
                            root.selectedRoom = name
                            root.selectedPreview = preview
                            root.selectedRoomAvatarUrl = ""
                            root.selectedRoomKind = "room"

                            for (var i = 0; i < roomModel.count; i++) {
                                if (roomModel.get(i).roomId === roomId) {
                                    root.selectedRoomAvatarUrl =
                                        roomModel.get(i).avatarUrl
                                    root.selectedRoomKind =
                                        roomModel.get(i).kind || "room"
                                    root.selectedRoomEncrypted =
                                        roomModel.get(i).isEncrypted === true
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
                isEncrypted: root.selectedRoomEncrypted
                rtcMemberships:
                    root.rtcMembershipsByRoom[root.selectedRoomId] || []
                rtcCallState:
                    root.rtcCallStatesByRoom[root.selectedRoomId] || ({})
                rtcAudioState:
                    root.rtcAudioStatesByRoom[root.selectedRoomId] || ({})

                avatarFallbackColour: {
                       for (var i = 0; i < roomModel.count; i++) {
                            var room = roomModel.get(i)
                            if (room.roomId === root.selectedRoomId)
                                return room.colour || "#a16b9b"
                            }
                            return "#a16b9b"
                    }

                wideLayout: root.wideLayout
                targetEventId: root.pendingNotificationEventId
                targetRequestSerial: root.notificationRequestSerial

                onBackRequested: {
                    roomView.collapseRtcParticipants()
                    root.showConversation = false
                }

                onCallRequested: {
                    if (!root.mainRoot)
                        return

                    root.mainRoot.openCallPage(roomId, roomName, false)
                }

                onJoinCallRequested: {
                    if (!root.mainRoot)
                        return

                    root.mainRoot.openCallPage(roomId, roomName, true)
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