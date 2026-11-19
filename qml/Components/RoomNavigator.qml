
import QtQuick 2.7
import Lomiri.Components 1.3
import QtGraphicalEffects 1.0

Item {
    id: root

    property var roomsModel
    property string selectedRoom: ""

    // Navigation controls supplied by ChatPage.
    property string filterMode: "all"
    property string searchText: ""

    property var expandedSpaceIds: []

    signal roomSelected(string roomId, string name, string preview)

    ListModel {
        id: navigationModel
    }

    // =========================================================
    // Search
    // =========================================================

    function roomMatchesSearch(room) {
        var query = root.searchText.toLowerCase().trim()

        if (query === "")
            return true

        var name = String(room.name || "").toLowerCase()
        var preview = String(room.preview || "").toLowerCase()
        var roomId = String(room.roomId || "").toLowerCase()

        return name.indexOf(query) !== -1 ||
               preview.indexOf(query) !== -1 ||
               roomId.indexOf(query) !== -1
    }

    // =========================================================
    // Space relationship helpers
    // =========================================================

    function parentIdsFor(room) {
        var parents = room.parentSpaceIds || []

        if (typeof parents === "string") {
            try {
                parents = JSON.parse(parents)
            } catch (error) {
                console.log(
                    "ASTRO: Invalid parent Space JSON:",
                    error
                )
                return []
            }
        }

        return parents || []
    }

    function hasParent(room, parentId) {
        var parents = parentIdsFor(room)

        for (var i = 0; i < parents.length; i++) {
            if (String(parents[i]) === String(parentId))
                return true
        }

        return false
    }

    function hasJoinedParent(room) {
        if (!root.roomsModel)
            return false

        var parents = parentIdsFor(room)

        for (var p = 0; p < parents.length; p++) {
            for (var i = 0; i < root.roomsModel.count; i++) {
                var candidate = root.roomsModel.get(i)

                if (candidate.kind === "space" &&
                    String(candidate.roomId) === String(parents[p])) {
                    return true
                }
            }
        }

        return false
    }

    function isExpanded(spaceId) {
        return expandedSpaceIds.indexOf(spaceId) !== -1
    }

    function findNavigationRow(roomId, rowType) {
        for (var i = 0; i < navigationModel.count; i++) {
            var row = navigationModel.get(i)

            if (
                String(row.roomId || "") === String(roomId) &&
                row.rowType === rowType
            ) {
                return i
            }
        }

        return -1
    }

    function toggleSpace(spaceId) {
        // Remember the first visible row and its offset before
        // rebuilding the model.
        var probeY = roomList.contentY + 2
        var visibleIndex = roomList.indexAt(2, probeY)
        var anchorRoomId = ""
        var anchorRowType = ""
        var anchorOffset = 0

        if (visibleIndex >= 0) {
            var visibleRow = navigationModel.get(visibleIndex)
            var visibleItem = roomList.itemAt(2, probeY)

            if (visibleRow && visibleItem) {
                anchorRoomId = String(visibleRow.roomId || "")
                anchorRowType = visibleRow.rowType
                anchorOffset = visibleItem.y - roomList.contentY
            }
        }

        var updated = expandedSpaceIds.slice()
        var index = updated.indexOf(spaceId)

        if (index === -1)
            updated.push(spaceId)
        else
            updated.splice(index, 1)

        expandedSpaceIds = updated
        rebuild()

        // Restore the same row after the rebuilt model has laid out.
        Qt.callLater(function() {
            var targetIndex = findNavigationRow(
                anchorRoomId,
                anchorRowType
            )

            // The previous visible row may have disappeared because
            // the Space containing it was collapsed.
            if (targetIndex < 0)
                targetIndex = findNavigationRow(spaceId, "space")

            if (targetIndex < 0)
                return

            roomList.positionViewAtIndex(
                targetIndex,
                ListView.Beginning
            )

            // Apply the offset immediately to avoid a second
            // visible repositioning on the next event-loop cycle.
            roomList.contentY = Math.max(
                0,
                roomList.contentY - anchorOffset
            )
        })
    }

    function expandAllSpaces() {
        var updated = []

        if (root.roomsModel) {
            for (var i = 0; i < root.roomsModel.count; i++) {
                var room = root.roomsModel.get(i)

                if (room.kind === "space")
                    updated.push(room.roomId)
            }
        }

        expandedSpaceIds = updated
        rebuild()
    }

    function collapseAllSpaces() {
        expandedSpaceIds = []
        rebuild()
    }

    // =========================================================
    // Navigation model helpers
    // =========================================================

    function appendSection(title) {
        navigationModel.append({
            rowType: "section",
            name: title
        })
    }

    function appendRoom(index, depth, rowType) {
        var room = root.roomsModel.get(index)

        navigationModel.append({
            rowType: rowType,
            roomIndex: index,
            roomId: room.roomId,
            name: room.name,
            preview: room.preview,
            avatar: room.avatar,
            avatarUrl: room.avatarUrl,
            colour: room.colour,
            kind: room.kind,
            unreadCount: Number(room.unreadCount || 0),
            mentionCount: Number(room.mentionCount || 0),
            depth: depth,
            expanded: false
        })
    }

    function updateUnreadCounts(roomId, unreadCount, mentionCount) {
        for (var i = 0; i < navigationModel.count; i++) {
            var entry = navigationModel.get(i)

            if (entry.roomId === roomId && entry.rowType !== "space") {
                navigationModel.setProperty(
                    i, "unreadCount", unreadCount
                )
                navigationModel.setProperty(
                    i, "mentionCount", mentionCount
                )
            }
        }

        refreshSpaceUnreadCounts()
    }

    // =========================================================
    // Cumulative Space unread counts
    // =========================================================

    // Calculate unread totals recursively for a Space.
    // Each descendant room is counted only once per Space.

    function collectSpaceUnreadCounts(spaceId, ancestors, countedRooms) {
        if (ancestors.indexOf(spaceId) !== -1)
            return { unreadCount: 0, mentionCount: 0 }

        var nextAncestors = ancestors.slice()
        nextAncestors.push(spaceId)

        var totals = {
            unreadCount: 0,
            mentionCount: 0
        }

        for (var i = 0; i < root.roomsModel.count; i++) {
            var child = root.roomsModel.get(i)

            if (!hasParent(child, spaceId))
                continue

            if (child.kind === "space") {
                var nested = collectSpaceUnreadCounts(
                    child.roomId,
                    nextAncestors,
                    countedRooms
                )

                totals.unreadCount += nested.unreadCount
                totals.mentionCount += nested.mentionCount

            } else if (child.kind === "room") {
                if (countedRooms.indexOf(child.roomId) !== -1)
                    continue

                countedRooms.push(child.roomId)

                totals.unreadCount += Number(child.unreadCount || 0)
                totals.mentionCount += Number(child.mentionCount || 0)
            }
        }

        return totals
    }

    function spaceUnreadCounts(spaceId) {
        return collectSpaceUnreadCounts(spaceId, [], [])
    }

    function refreshSpaceUnreadCounts() {
        for (var i = 0; i < navigationModel.count; i++) {
            var entry = navigationModel.get(i)

            if (entry.rowType !== "space")
                continue

            var totals = spaceUnreadCounts(entry.roomId)

            navigationModel.setProperty(
                i, "unreadCount", totals.unreadCount
            )

            navigationModel.setProperty(
                i, "mentionCount", totals.mentionCount
            )
        }
    }

    // =========================================================
    // Append Space
    // =========================================================

    function appendSpace(index, depth, ancestors) {
        var space = root.roomsModel.get(index)
        var spaceId = space.roomId

        // Prevent infinite recursion if the Space graph contains
        // a cyclic relationship.

        if (ancestors.indexOf(spaceId) !== -1) {
            console.log(
                "ASTRO: Ignoring cyclic Space relationship:",
                space.name,
                spaceId
            )
            return
        }

        var expanded = isExpanded(spaceId)
        var unreadTotals = spaceUnreadCounts(spaceId)

        navigationModel.append({
            rowType: "space",
            roomIndex: index,
            roomId: spaceId,
            name: space.name,
            preview: space.preview,
            avatar: space.avatar,
            avatarUrl: space.avatarUrl,
            colour: space.colour,
            kind: "space",
            unreadCount: unreadTotals.unreadCount,
            mentionCount: unreadTotals.mentionCount,
            depth: depth,
            expanded: expanded
        })

        if (!expanded)
            return

        var nextAncestors = ancestors.slice()
        nextAncestors.push(spaceId)

        // Add child Spaces and ordinary rooms.

        for (var i = 0; i < root.roomsModel.count; i++) {
            var child = root.roomsModel.get(i)

            if (!hasParent(child, spaceId))
                continue

            if (child.kind === "space") {
                appendSpace(i, depth + 1, nextAncestors)
            } else if (child.kind === "room") {
                appendRoom(i, depth + 1, "room")
            }
        }
    }

    // =========================================================
    // Rebuild navigation
    // =========================================================

    function rebuild() {
        navigationModel.clear()

        if (!root.roomsModel)
            return

        var i
        var room

        // -----------------------------------------------------
        // Search results
        //
        // Search is deliberately independent of the selected
        // filter, so a search can find rooms inside collapsed
        // Spaces and direct messages.
        // -----------------------------------------------------

        if (root.searchText.trim() !== "") {
            appendSection("SEARCH RESULTS")

            var resultCount = 0

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (!roomMatchesSearch(room))
                    continue

                appendRoom(
                    i,
                    0,
                    room.kind === "dm" ? "dm" : "room"
                )

                resultCount++
            }

            if (resultCount === 0)
                appendSection("NO MATCHING CONVERSATIONS")

            return
        }

        // -----------------------------------------------------
        // Count available entries
        // -----------------------------------------------------

        var spaceCount = 0
        var standaloneCount = 0
        var dmCount = 0
        var allRoomCount = 0

        for (i = 0; i < root.roomsModel.count; i++) {
            room = root.roomsModel.get(i)

            if (room.kind === "space" && !hasJoinedParent(room))
                spaceCount++

            if (room.kind === "room") {
                allRoomCount++

                if (!hasJoinedParent(room))
                    standaloneCount++
            }

            if (room.kind === "dm")
                dmCount++
        }

        // -----------------------------------------------------
        // Spaces filter
        // -----------------------------------------------------

        if (root.filterMode === "spaces") {
            appendSection("SPACES")

            if (spaceCount === 0) {
                appendSection("NO JOINED SPACES")
                return
            }

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "space" && !hasJoinedParent(room))
                    appendSpace(i, 0, [])
            }

            return
        }

        // -----------------------------------------------------
        // Direct messages filter
        // -----------------------------------------------------

        if (root.filterMode === "direct") {
            appendSection("DIRECT MESSAGES")

            if (dmCount === 0) {
                appendSection("NO DIRECT MESSAGES")
                return
            }

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "dm")
                    appendRoom(i, 0, "dm")
            }

            return
        }

        // -----------------------------------------------------
        // Ordinary rooms filter
        //
        // Include rooms nested inside Spaces as well as
        // standalone rooms. They are displayed as a flat list.
        // -----------------------------------------------------

        if (root.filterMode === "rooms") {
            appendSection("ROOMS")

            if (allRoomCount === 0) {
                appendSection("NO ROOMS")
                return
            }

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "room")
                    appendRoom(i, 0, "room")
            }

            return
        }

        // -----------------------------------------------------
        // All conversations
        //
        // Preserve the original hierarchical navigation.
        // -----------------------------------------------------

        if (spaceCount > 0) {
            appendSection("SPACES")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "space" && !hasJoinedParent(room))
                    appendSpace(i, 0, [])
            }
        }

        if (standaloneCount > 0) {
            appendSection("ROOMS")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "room" && !hasJoinedParent(room))
                    appendRoom(i, 0, "room")
            }
        }

        if (dmCount > 0) {
            appendSection("DIRECT MESSAGES")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "dm")
                    appendRoom(i, 0, "dm")
            }
        }
    }

    onRoomsModelChanged: rebuild()
    onFilterModeChanged: rebuild()
    onSearchTextChanged: rebuild()

    Component.onCompleted: rebuild()

    // =========================================================
    // Navigation list
    // =========================================================

    ListView {
        id: roomList

        anchors.fill: parent
        model: navigationModel
        clip: true

        delegate: Item {
            id: entry

            // Explicit ListModel role access for Qt 5 compatibility.

            property string entryRowType: model.rowType || ""
            property string entryName: model.name || ""
            property string entryPreview: model.preview || ""
            property string entryAvatar: model.avatar || ""
            property string entryAvatarUrl: model.avatarUrl || ""
            property string entryColour: model.colour || "#526575"
            property string entryKind: model.kind || ""
            property string entryRoomId: model.roomId || ""
            property int entryUnreadCount: Number(model.unreadCount || 0)
            property int entryMentionCount: Number(model.mentionCount || 0)
            property int entryDepth: model.depth === undefined ? 0 : model.depth
            property bool entryExpanded: model.expanded === true

            width: roomList.width

            property real avatarSize:
                entryRowType === "room" && entryDepth > 0
                ? units.gu(4.5)
                : entryDepth > 0
                  ? units.gu(5.25)
                  : units.gu(6)

            height: entryRowType === "section"
                    ? units.gu(4)
                    : entryDepth === 0
                      ? units.gu(9)
                      : entryDepth === 1
                        ? units.gu(8)
                        : units.gu(7.5)

            // -------------------------------------------------
            // Section heading
            // -------------------------------------------------

            Text {
                anchors {
                    left: parent.left
                    leftMargin: units.gu(2)
                    bottom: parent.bottom
                    bottomMargin: units.gu(0.5)
                }

                visible: entryRowType === "section"

                text: entryName
                color: "#91a4b5"

                font.pixelSize: units.gu(1.4)
                font.bold: true
            }

            // -------------------------------------------------
            // Room or Space entry
            // -------------------------------------------------

            Rectangle {
                anchors {
                    fill: parent
                    margins: units.gu(0.5)
                }

                visible: entryRowType !== "section"

                radius: units.gu(1)

                color: root.selectedRoom === entryName &&
                       entryRowType !== "space"
                       ? "#293c4c"
                       : "transparent"

                Row {
                    anchors {
                        fill: parent
                        leftMargin: units.gu(1) +
                                    entryDepth * units.gu(2)
                        rightMargin: units.gu(1)
                        topMargin: units.gu(1)
                        bottomMargin: units.gu(1)
                    }

                    spacing: units.gu(1.5)

                    // -----------------------------------------
                    // Space expand/collapse indicator
                    // -----------------------------------------

                    Text {
                        width: units.gu(2)
                        height: units.gu(6)

                        visible: entryRowType === "space"

                        text: entryExpanded ? "▾" : "▸"
                        color: "#b4c4d1"

                        font.pixelSize: units.gu(2)

                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    // -----------------------------------------
                    // Room avatar and unread badge
                    // -----------------------------------------

                    Rectangle {
                        id: roomAvatar

                        width: avatarSize
                        height: avatarSize

                        radius: avatarSize / 2

                        // Keep the badge visible outside the avatar.
                        clip: false

                        color: entryColour

                        anchors.verticalCenter: parent.verticalCenter

                        Image {
                            id: roomAvatarImage

                            anchors.fill: parent
                            anchors.margins: units.gu(0.15)

                            fillMode: Image.PreserveAspectCrop

                            asynchronous: true
                            cache: true
                            smooth: true

                            source: entryAvatarUrl
                            visible: false

                            onStatusChanged: {
                                if (status === Image.Error) {
                                    console.log(
                                        "ASTRO: Could not display room avatar:",
                                        entryRoomId
                                    )
                                }
                            }
                        }

                        OpacityMask {
                            anchors.fill: roomAvatarImage
                            source: roomAvatarImage
                            visible: roomAvatarImage.status === Image.Ready

                            maskSource: Rectangle {
                                width: roomAvatarImage.width
                                height: roomAvatarImage.height
                                radius: width / 2
                                color: "white"
                            }
                        }

                        Text {
                            anchors.centerIn: parent

                            visible:
                                entryAvatarUrl === "" ||
                                roomAvatarImage.status !== Image.Ready

                            text: entryKind === "space" ? "◆" :
                                  entryKind === "dm" ? "●" :
                                  entryAvatar

                            color: "white"
                            font.bold: true

                            font.pixelSize:
                                entryRowType === "room" && entryDepth > 0
                                ? units.gu(1.9)
                                : entryKind === "room"
                                  ? units.gu(2.5)
                                  : units.gu(2.2)
                        }

                        // -------------------------------------
                        // Unread count badge
                        // -------------------------------------
                        Rectangle {
                            id: avatarUnreadBadge

                            visible: entryUnreadCount > 0

                            width: Math.max(
                                units.gu(2.4),
                                avatarUnreadBadgeText.implicitWidth + units.gu(1.1)
                            )

                            height: units.gu(2.4)
                            radius: height / 2

                            color: entryMentionCount > 0
                                ? "#c94f58"
                                : "#526d85"

                            anchors {
                                right: parent.right
                                bottom: parent.bottom
                                rightMargin: -units.gu(0.5)
                                bottomMargin: -units.gu(0.5)
                            }

                            z: 10

                            scale: 1.0

                            SequentialAnimation {
                                id: unreadPulse

                                NumberAnimation {
                                    target: avatarUnreadBadge
                                    property: "scale"
                                    to: 1.3
                                    duration: 130
                                    easing.type: Easing.OutQuad
                                }

                                NumberAnimation {
                                    target: avatarUnreadBadge
                                    property: "scale"
                                    to: 0.9
                                    duration: 100
                                    easing.type: Easing.InOutQuad
                                }

                                NumberAnimation {
                                    target: avatarUnreadBadge
                                    property: "scale"
                                    to: 1.0
                                    duration: 130
                                    easing.type: Easing.OutBack
                                }
                            }

                            Connections {
                                target: entry

                                onEntryUnreadCountChanged: {
                                    if (avatarUnreadBadge.visible)
                                        unreadPulse.restart()
                                }
                            }

                            Text {
                                id: avatarUnreadBadgeText

                                anchors.centerIn: parent

                                text: entryUnreadCount > 99
                                    ? "99+"
                                    : String(entryUnreadCount)

                                color: "white"
                                font.bold: true
                                font.pixelSize: units.gu(1.15)
                            }
                        }

                    }

                    // -----------------------------------------
                    // Name and preview
                    // -----------------------------------------

                    Column {
                        anchors.verticalCenter: parent.verticalCenter

                        width: parent.width -
                               avatarSize -
                               units.gu(2) -
                               (entryRowType === "space"
                                ? units.gu(2)
                                : 0)

                        spacing: units.gu(0.5)

                        Text {
                            width: parent.width

                            text: entryName
                            color: "white"

                            font.bold: true
                            font.pixelSize:
                                entryDepth === 0
                                ? units.gu(1.8)
                                : entryRowType === "space"
                                  ? units.gu(1.65)
                                  : units.gu(1.5)

                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width

                            text: entryPreview
                            color: "#9aabba"

                            font.pixelSize:
                                entryDepth === 0
                                ? units.gu(1.5)
                                : units.gu(1.25)

                            elide: Text.ElideRight
                        }
                    }
                }

                // ---------------------------------------------
                // Entry interaction
                // ---------------------------------------------

                MouseArea {
                    anchors.fill: parent

                    onClicked: {
                        if (entryRowType === "space") {
                            root.toggleSpace(entryRoomId)
                        } else {
                            root.roomSelected(
                                entryRoomId,
                                entryName,
                                entryPreview
                            )
                        }
                    }
                }
            }
        }
    }
}
