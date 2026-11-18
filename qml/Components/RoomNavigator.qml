
import QtQuick 2.7
import Lomiri.Components 1.3
import QtGraphicalEffects 1.0

Item {
    id: root

    property var roomsModel
    property string selectedRoom: ""
    property var expandedSpaceIds: []

    signal roomSelected(string name, string preview)

    ListModel {
        id: navigationModel
    }

    // =========================================================
    // Space relationship helpers
    // =========================================================

    function parentIdsFor(room) {
        var parents = room.parentSpaceIds || []

        // Qt Quick ListModel on older Ubuntu Touch releases
        // handles string roles more reliably than array roles.
        if (typeof parents === "string") {
            try {
                parents = JSON.parse(parents)
            } catch (error) {
                console.log("ASTRO: Invalid parent Space JSON:", error)
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

    function toggleSpace(spaceId) {
        var updated = expandedSpaceIds.slice()
        var index = updated.indexOf(spaceId)

        if (index === -1)
            updated.push(spaceId)
        else
            updated.splice(index, 1)

        expandedSpaceIds = updated
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
            depth: depth,
            expanded: false
        })
    }

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
        console.log("ASTRO TRACE: rebuild entered")
        navigationModel.clear()
        console.log("ASTRO TRACE: navigationModel cleared")

        if (!root.roomsModel) {
            console.log("ASTRO TRACE: no roomsModel; rebuild finished")
            return
        }

        console.log("ASTRO TRACE: roomsModel count:", root.roomsModel.count)

        var i
        var spaceCount = 0
        var standaloneCount = 0
        var dmCount = 0

        for (i = 0; i < root.roomsModel.count; i++) {
            var room = root.roomsModel.get(i)

            if (room.kind === "space" && !hasJoinedParent(room))
                spaceCount++

            if (room.kind === "room" && !hasJoinedParent(room))
                standaloneCount++

            if (room.kind === "dm")
                dmCount++
        }

        console.log("ASTRO TRACE: counts calculated:",
                    spaceCount, standaloneCount, dmCount)

        if (spaceCount > 0) {
            console.log("ASTRO TRACE: adding SPACES section")
            appendSection("SPACES")
            console.log("ASTRO TRACE: SPACES section added")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind !== "space")
                    continue

                console.log("ASTRO TRACE: checking Space:", room.name)

                if (!hasJoinedParent(room)) {
                    console.log("ASTRO TRACE: appending top-level Space:", room.name)
                    appendSpace(i, 0, [])
                    console.log("ASTRO TRACE: Space appended:", room.name)
                }
            }
        }

        if (standaloneCount > 0) {
            console.log("ASTRO TRACE: adding ROOMS section")
            appendSection("ROOMS")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "room" && !hasJoinedParent(room))
                    appendRoom(i, 0, "room")
            }

            console.log("ASTRO TRACE: standalone rooms appended")
        }

        if (dmCount > 0) {
            console.log("ASTRO TRACE: adding DIRECT MESSAGES section")
            appendSection("DIRECT MESSAGES")

            for (i = 0; i < root.roomsModel.count; i++) {
                room = root.roomsModel.get(i)

                if (room.kind === "dm")
                    appendRoom(i, 0, "dm")
            }

            console.log("ASTRO TRACE: direct messages appended")
        }

        console.log("ASTRO TRACE: rebuild finished; navigation rows:",
                    navigationModel.count)
    }

    // Rebuild when the model is assigned or replaced.
    onRoomsModelChanged: rebuild()

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
            width: roomList.width

            property real avatarSize:
                rowType === "room" && depth > 0
                ? units.gu(4.5)
                : depth > 0
                  ? units.gu(5.25)
                  : units.gu(6)

            height: rowType === "section"
                    ? units.gu(4)
                    : depth === 0
                      ? units.gu(9)
                      : depth === 1
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

                visible: rowType === "section"

                text: name
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

                visible: rowType !== "section"

                radius: units.gu(1)

                color: root.selectedRoom === name &&
                       rowType !== "space"
                       ? "#293c4c"
                       : "transparent"

                Row {
                    anchors {
                        fill: parent
                        leftMargin: units.gu(1) +
                                    depth * units.gu(2)
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

                        visible: rowType === "space"

                        text: expanded ? "▾" : "▸"
                        color: "#b4c4d1"

                        font.pixelSize: units.gu(2)

                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    // -----------------------------------------
                    // Room avatar
                    // -----------------------------------------

                    Rectangle {
                        width: avatarSize
                        height: avatarSize

                        radius: avatarSize / 2
                        clip: true
                        color: colour

                        anchors.verticalCenter: parent.verticalCenter

                        Image {
                            id: roomAvatarImage

                            anchors.fill: parent
                            anchors.margins: units.gu(0.15)

                            fillMode: Image.PreserveAspectCrop

                            asynchronous: true
                            cache: true
                            smooth: true

                            source: avatarUrl
                            visible: false

                            onStatusChanged: {
                                if (status === Image.Error) {
                                    console.log(
                                        "ASTRO: Could not display room avatar:",
                                        roomId
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
                                avatarUrl === "" ||
                                roomAvatarImage.status !== Image.Ready

                            text: kind === "space" ? "◆" :
                                  kind === "dm" ? "●" :
                                  avatar

                            color: "white"
                            font.bold: true

                            font.pixelSize:
                                rowType === "room" && depth > 0
                                ? units.gu(1.9)
                                : kind === "room"
                                  ? units.gu(2.5)
                                  : units.gu(2.2)
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
                               (rowType === "space"
                                ? units.gu(2)
                                : 0)

                        spacing: units.gu(0.5)

                        Text {
                            width: parent.width

                            text: name
                            color: "white"

                            font.bold: true
                            font.pixelSize:
                                depth === 0
                                ? units.gu(1.8)
                                : rowType === "space"
                                  ? units.gu(1.65)
                                  : units.gu(1.5)

                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width

                            text: preview
                            color: "#9aabba"

                            font.pixelSize:
                                depth === 0
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
                        if (rowType === "space") {
                            root.toggleSpace(roomId)
                        } else {
                            root.roomSelected(name, preview)
                        }
                    }
                }
            }
        }
    }
}
