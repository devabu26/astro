import QtQuick 2.7
import Lomiri.Components 1.3
import "Components"

Item {
    id: root

    property var backend
    property string roomId: ""
    property string roomName: "Welcome to Astro"
    property string avatarUrl: ""
    property bool wideLayout: false

    // Timeline loading state.
    property bool isLoadingInitial: roomId !== ""
    property bool isLoadingOlder: false
    property bool hasMoreHistory: true
    property bool pendingInitialScroll: false

    // Scroll state.
    property bool isAtBottom: true
    property bool readReceiptSent: false

    signal backRequested()
    signal sendMessageRequested(string roomId, string message)

    // =====================================
    // Scroll to bottom
    // =====================================

    function messageDateInfo(date) {
        if (!date || isNaN(date.getTime())) {
            return {
                key: "unknown",
                label: "Unknown date"
            }
        }

        function makeKey(value) {
            var month = ("0" + (value.getMonth() + 1)).slice(-2)
            var day = ("0" + value.getDate()).slice(-2)

            return value.getFullYear() + "-" + month + "-" + day
        }

        var key = makeKey(date)
        var today = new Date()

        var todayStart = new Date(
            today.getFullYear(),
            today.getMonth(),
            today.getDate()
        )

        var yesterday = new Date(
            today.getFullYear(),
            today.getMonth(),
            today.getDate() - 1
        )

        var label = Qt.formatDate(date, "dddd, d MMMM yyyy")

        if (key === makeKey(todayStart)) {
            label = "Today"
        } else if (key === makeKey(yesterday)) {
            label = "Yesterday"
        }

        return {
            key: key,
            label: label
        }
    }

    function scrollToBottom() {
        var bottomPosition = Math.max(
            0,
            messageList.contentHeight - messageList.height
        )

        bottomScrollAnimation.stop()
        bottomScrollAnimation.to = bottomPosition
        bottomScrollAnimation.start()
    }

    function updateBottomState() {
        var bottomPosition = Math.max(
            0,
            messageList.contentHeight - messageList.height
        )

        root.isAtBottom =
            messageList.contentY >= bottomPosition - units.gu(5)

        root.markRoomReadIfAtBottom()
    }

    function markRoomReadIfAtBottom() {
        if (!root.isAtBottom) {
            root.readReceiptSent = false
            return
        }

        if (
            root.readReceiptSent ||
            root.isLoadingInitial ||
            root.pendingInitialScroll ||
            !root.backend ||
            root.roomId === ""
        ) {
            return
        }

        root.readReceiptSent = true
        root.backend.markRoomAsRead(root.roomId)
    }

    // =====================================
    // Initial room scroll
    // =====================================

    function finishInitialScroll() {
        if (!root.pendingInitialScroll)
            return

        if (root.isLoadingInitial)
            return

        var bottomPosition = Math.max(
            0,
            messageList.contentHeight - messageList.height
        )

        messageList.contentY = bottomPosition

        root.pendingInitialScroll = false
        root.updateBottomState()

        console.log(
            "ASTRO: Initial scroll completed. contentY:",
            messageList.contentY,
            "bottom:",
            bottomPosition
        )

        // Do not automatically fetch older history here.
        // Pagination is triggered by user scrolling or the
        // Load older messages control.
    }

    // =====================================
    // Older-message pagination
    // =====================================

    function maybeLoadOlderMessages() {
        if (!root.backend)
            return

        if (root.roomId === "")
            return

        if (root.isLoadingInitial || root.pendingInitialScroll)
            return

        if (root.isLoadingOlder)
            return

        if (!root.hasMoreHistory || messageModel.count === 0)
            return

        if (messageList.contentY <= units.gu(6)) {
            root.isLoadingOlder = true

            console.log(
                "ASTRO: Requesting older messages for",
                root.roomId
            )

            root.backend.requestOlderRoomTimeline(root.roomId)
        }
    }

    // =====================================
    // Message model
    // =====================================

    ListModel {
        id: messageModel
    }

    // =====================================
    // Room switching
    // =====================================

    onRoomIdChanged: {
        bottomScrollAnimation.stop()

        messageModel.clear()
        messageComposer.clearInput()
        messageComposer.sending = false

        // Reset the old room's scroll position.
        messageList.contentY = 0

        hasMoreHistory = true
        isLoadingOlder = false
        isLoadingInitial = roomId !== ""
        isAtBottom = true
        readReceiptSent = false
        pendingInitialScroll = false
    }

    // =====================================
    // Backend connections
    // =====================================

    Connections {
        target: root.backend

        // =====================================
        // Initial timeline response
        // =====================================

        onMatrixTimelineRetrieved: {
            if (roomId !== root.roomId)
                return

            if (error !== "") {
                root.isLoadingInitial = false
                root.pendingInitialScroll = false

                console.log(
                    "ASTRO: Timeline retrieval failed:",
                    error
                )
                return
            }

            try {
                var result = JSON.parse(json)
                var messages = result.messages || []

                messageModel.clear()

                root.hasMoreHistory =
                    result.reached_start !== true

                root.isLoadingOlder = false

                // Keep the initial-loading state active until
                // the message layout is ready for scrolling.
                root.pendingInitialScroll = true

                for (var i = 0; i < messages.length; i++) {
                    var message = messages[i]

                    console.log(
                        "ASTRO DEBUG: Timeline message:",
                        "sender =", message.sender,
                        "encrypted =", message.is_encrypted,
                        "body =", message.body
                    )

                    var date = new Date(
                        Number(message.timestamp)
                    )
                    var dateInfo = root.messageDateInfo(date)

                    messageModel.append({
                        eventId: message.event_id || "",
                        sender: message.sender || "",
                        body: message.body || "",
                        timestamp: date.toLocaleTimeString(),
                        dateKey: dateInfo.key,
                        dateLabel: dateInfo.label,
                        isOwn: message.is_own === true,
                        isEncrypted: message.is_encrypted === true
                    })
                }

                console.log(
                    "ASTRO: Loaded",
                    messageModel.count,
                    "messages for",
                    roomId
                )

                // Allow the Flickable and delegates to finish
                // their initial layout before scrolling.
                Qt.callLater(function() {
                    if (root.roomId !== roomId)
                        return

                    root.isLoadingInitial = false
                    root.finishInitialScroll()
                })

            } catch (parseError) {
                root.isLoadingInitial = false
                root.pendingInitialScroll = false

                console.log(
                    "ASTRO: Could not parse room timeline:",
                    parseError
                )
            }
        }

        // =====================================
        // Older timeline response
        // =====================================

        onMatrixOlderTimelineRetrieved: {
            if (roomId !== root.roomId)
                return

            if (error !== "") {
                root.isLoadingOlder = false

                console.log(
                    "ASTRO: Older timeline retrieval failed:",
                    error
                )
                return
            }

            try {
                var result = JSON.parse(json)
                var messages = result.messages || []

                var oldContentHeight = messageList.contentHeight
                var oldContentY = messageList.contentY

                // Record all currently displayed event IDs.
                var seenEventIds = ({})

                for (var i = 0; i < messageModel.count; i++) {
                    var existingId = messageModel.get(i).eventId

                    if (existingId !== "")
                        seenEventIds[existingId] = true
                }

                // Find the first existing event in the
                // updated complete timeline.
                var firstLoadedIndex = -1

                for (var j = 0; j < messages.length; j++) {
                    var candidateId =
                        messages[j].event_id || ""

                    if (
                        candidateId !== "" &&
                        seenEventIds[candidateId] === true
                    ) {
                        firstLoadedIndex = j
                        break
                    }
                }

                var insertedCount = 0

                if (firstLoadedIndex >= 0) {
                    // Insert older events in chronological order.
                    for (var k = 0; k < firstLoadedIndex; k++) {
                        var message = messages[k]
                        var eventId = message.event_id || ""

                        if (
                            eventId === "" ||
                            seenEventIds[eventId] === true
                        ) {
                            continue
                        }

                        var date = new Date(
                            Number(message.timestamp)
                        )
                        var dateInfo = root.messageDateInfo(date)

                        messageModel.insert(insertedCount, {
                            eventId: eventId,
                            sender: message.sender || "",
                            body: message.body || "",
                            timestamp: date.toLocaleTimeString(),
                            dateKey: dateInfo.key,
                            dateLabel: dateInfo.label,
                            isOwn: message.is_own === true,
                            isEncrypted: message.is_encrypted === true
                        })

                        seenEventIds[eventId] = true
                        insertedCount++
                    }

                } else {
                    console.log(
                        "ASTRO: Could not find an overlap between loaded messages and the updated timeline"
                    )
                }

                // Stop when the beginning has been reached,
                // or when no additional messages were inserted.
                root.hasMoreHistory =
                    result.reached_start !== true &&
                    insertedCount > 0

                console.log(
                    "ASTRO: Inserted",
                    insertedCount,
                    "older messages for",
                    root.roomId
                )

                Qt.callLater(function() {
                    if (root.roomId !== roomId)
                        return

                    root.isLoadingOlder = false

                    Qt.callLater(function() {
                        if (root.roomId !== roomId)
                            return

                        // Account for the newly inserted messages
                        // and the disappearing loading indicator.
                        var heightDifference =
                            messageList.contentHeight -
                            oldContentHeight

                        // Preserve the visible reading position.
                        messageList.contentY = Math.max(
                            0,
                            oldContentY + heightDifference
                        )

                        root.updateBottomState()
                    })
                })

            } catch (parseError) {
                root.isLoadingOlder = false

                console.log(
                    "ASTRO: Could not parse older timeline:",
                    parseError
                )
            }
        }

        // =====================================
        // Live timeline updates
        // =====================================

        onMatrixLiveTimelineUpdated: {
            if (root.roomId === "")
                return

            try {
                var result = JSON.parse(json)
                var updates = result.updates || []

                // Only follow incoming messages automatically
                // when the user was already at the bottom.
                var shouldStickToBottom = root.isAtBottom
                var addedMessage = false

                for (var u = 0; u < updates.length; u++) {
                    var update = updates[u]

                    if (update.room_id !== root.roomId)
                        continue

                    if (
                        update.operation !== "upsert" ||
                        !update.message
                    ) {
                        continue
                    }

                    var message = update.message
                    var eventId = message.event_id || ""
                    var existingIndex = -1

                    // Check whether this event is already displayed.
                    // This also handles updates to existing events,
                    // such as an encrypted event being decrypted.
                    if (eventId !== "") {
                        for (
                            var i = 0;
                            i < messageModel.count;
                            i++
                        ) {
                            if (
                                messageModel.get(i).eventId === eventId
                            ) {
                                existingIndex = i
                                break
                            }
                        }
                    }

                    var date = new Date(
                        Number(message.timestamp)
                    )
                    var dateInfo = root.messageDateInfo(date)

                    var messageData = {
                        eventId: eventId,
                        sender: message.sender || "",
                        body: message.body || "",
                        timestamp: date.toLocaleTimeString(),
                        dateKey: dateInfo.key,
                        dateLabel: dateInfo.label,
                        isOwn: message.is_own === true,
                        isEncrypted: message.is_encrypted === true
                    }

                    if (existingIndex >= 0) {
                        // Update an existing event in place.
                        messageModel.set(
                            existingIndex,
                            messageData
                        )
                    } else {
                        // Append a genuinely new event.
                        messageModel.append(messageData)
                        addedMessage = true
                    }
                }

                // Do not interrupt someone reading older messages.
                // Only scroll automatically if they were already
                // at the bottom before the new message arrived.
                if (addedMessage && shouldStickToBottom) {
                    // A new event may need a new read receipt.
                    root.readReceiptSent = false

                    var updatedRoomId = root.roomId

                    Qt.callLater(function() {
                        if (root.roomId === updatedRoomId)
                            root.scrollToBottom()
                    })
                }

            } catch (parseError) {
                console.log(
                    "ASTRO: Could not process live timeline update:",
                    parseError
                )
            }
        }

        // =====================================
        // Message send response
        // =====================================

        onMatrixMessageSent: {
            if (roomId !== root.roomId)
                return

            messageComposer.sending = false

            if (error !== "") {
                console.log(
                    "ASTRO: Message send failed:",
                    error
                )
                return
            }

            console.log(
                "ASTRO: Message sent successfully:",
                eventId
            )

            // Clear the composer only after Matrix confirms
            // that the message was sent.
            messageComposer.clearInput()

            // Reload the timeline so the confirmed event is
            // displayed using the normal timeline renderer.
            root.pendingInitialScroll = true
            root.backend.requestRoomTimeline(root.roomId)
        }
    }

    // =====================================
    // Smooth scroll animation
    // =====================================

    NumberAnimation {
        id: bottomScrollAnimation

        target: messageList
        property: "contentY"

        duration: 250
        easing.type: Easing.OutCubic
    }

    // =====================================
    // Main conversation
    // =====================================

    Rectangle {
        id: conversation

        anchors.fill: parent
        color: "#101820"

        Item {
            anchors.fill: parent
            clip: true

            Column {
                id: conversationContent

                anchors {
                    fill: parent

                    // Keep the composer above the Ubuntu Touch keyboard.
                    bottomMargin: Qt.inputMethod.visible
                                ? Qt.inputMethod.keyboardRectangle.height
                                : 0
                }

                spacing: 0

                // =====================================
                // Conversation header
                // =====================================

                Rectangle {
                    width: parent.width
                    height: units.gu(8)

                    color: "#1b2935"

                    // Back button on narrow layouts.
                    Rectangle {
                        anchors {
                            left: parent.left
                            leftMargin: units.gu(2)
                            verticalCenter: parent.verticalCenter
                        }

                        width: units.gu(5)
                        height: units.gu(5)

                        radius: units.gu(2.5)
                        color: "#293c4c"

                        visible: !root.wideLayout

                        Text {
                            anchors.centerIn: parent

                            text: "‹"
                            color: "white"

                            font.pixelSize: units.gu(3)
                        }

                        MouseArea {
                            anchors.fill: parent

                            onClicked: {
                                root.backRequested()
                            }
                        }
                    }

                    Image {
                        id: conversationAvatar

                        anchors {
                            left: parent.left
                            leftMargin: root.wideLayout
                                        ? units.gu(2)
                                        : units.gu(9)
                            verticalCenter: parent.verticalCenter
                        }

                        width: units.gu(5)
                        height: units.gu(5)

                        source: root.avatarUrl
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        visible: source !== ""
                    }

                    Text {
                        anchors {
                            left: conversationAvatar.right
                            leftMargin: units.gu(2)
                            verticalCenter: parent.verticalCenter
                            right: parent.right
                            rightMargin: units.gu(1)
                        }

                        text: root.roomName
                        color: "white"

                        font.pixelSize: units.gu(2)
                        font.bold: true

                        elide: Text.ElideRight
                    }
                }

                // =====================================
                // Message history
                // =====================================

                Item {
                    width: parent.width

                    height: parent.height -
                            units.gu(8) -
                            units.gu(9)

                    Flickable {
                        id: messageList

                        anchors.fill: parent

                        contentWidth: width
                        contentHeight: messageColumn.height

                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        onContentYChanged: {
                            root.updateBottomState()
                        }

                        onContentHeightChanged: {
                            if (
                                root.pendingInitialScroll &&
                                !root.isLoadingInitial
                            ) {
                                Qt.callLater(function() {
                                    root.finishInitialScroll()
                                })
                            } else {
                                root.updateBottomState()
                            }
                        }

                        onMovementEnded: {
                            root.updateBottomState()
                            root.maybeLoadOlderMessages()
                        }

                        Column {
                            id: messageColumn

                            width: messageList.width
                            spacing: units.gu(1)

                            // =====================================
                            // Older-history loading indicator
                            // =====================================

                            Item {
                                width: parent.width

                                height: root.isLoadingOlder
                                        ? units.gu(7)
                                        : 0

                                visible: root.isLoadingOlder

                                LoadingIndicator {
                                    anchors.fill: parent

                                    label: "Loading older messages…"
                                    spinnerSize: units.gu(3)
                                    labelSize: units.gu(1.5)

                                    active: root.isLoadingOlder
                                }
                            }

                            // Manual history loading. This is particularly
                            // useful when the initial five messages do not
                            // fill the screen and cannot be scrolled.
                            Rectangle {
                                width: parent.width
                                height: root.hasMoreHistory &&
                                        !root.isLoadingOlder
                                        ? units.gu(5)
                                        : 0

                                visible: root.hasMoreHistory &&
                                         !root.isLoadingOlder

                                radius: units.gu(1)
                                color: "#293c4c"

                                Text {
                                    anchors.centerIn: parent
                                    text: "Load older messages"
                                    color: "#d5e1eb"
                                    font.pixelSize: units.gu(1.6)
                                }

                                MouseArea {
                                    anchors.fill: parent

                                    onClicked: {
                                        root.maybeLoadOlderMessages()
                                    }
                                }
                            }

                            Item {
                                width: parent.width
                                height: units.gu(2)
                            }

                            Text {
                                visible: messageModel.count === 0 &&
                                         !root.isLoadingInitial

                                width: parent.width

                                text: "No messages to display"
                                color: "#9aabba"

                                horizontalAlignment: Text.AlignHCenter
                                font.pixelSize: units.gu(1.8)
                            }

                            // =====================================
                            // Message list
                            // =====================================

                            Repeater {
                                model: messageModel

                                delegate: Item {
                                    width: messageList.width
                                    height: messageDelegateColumn.height

                                    Column {
                                        id: messageDelegateColumn

                                        width: parent.width
                                        spacing: units.gu(0.5)

                                        Text {
                                            width: parent.width

                                            visible: index === 0 ||
                                                messageModel.get(index - 1).dateKey !== model.dateKey

                                            text: model.dateLabel

                                            color: "#9aabba"
                                            font.pixelSize: units.gu(1.6)
                                            font.bold: true

                                            horizontalAlignment: Text.AlignHCenter
                                        }

                                        MessageBubble {
                                            width: messageList.width

                                            sender: model.sender
                                            body: model.body
                                            timestamp: model.timestamp

                                            isOwn: model.isOwn
                                            isEncrypted: model.isEncrypted
                                        }
                                    }
                                }
                            }

                            Item {
                                width: parent.width
                                height: units.gu(2)
                            }
                        }
                    }

                    // =====================================
                    // Scroll-to-bottom button
                    // =====================================

                    Rectangle {
                        id: scrollBottomButton

                        anchors {
                            right: parent.right
                            bottom: parent.bottom
                            rightMargin: units.gu(2)
                            bottomMargin: units.gu(2)
                        }

                        width: units.gu(6)
                        height: units.gu(6)

                        radius: units.gu(3)

                        color: "#245b78"

                        visible: messageModel.count > 0 &&
                                 !root.isAtBottom &&
                                 !root.isLoadingInitial

                        z: 5

                        opacity: visible ? 1 : 0

                        Behavior on opacity {
                            NumberAnimation {
                                duration: 150
                            }
                        }

                        Text {
                            anchors.centerIn: parent

                            text: "↓"
                            color: "white"

                            font.pixelSize: units.gu(3)
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent

                            onClicked: {
                                root.scrollToBottom()
                            }
                        }
                    }

                    // =====================================
                    // Initial room-startup loading overlay
                    // =====================================

                    Rectangle {
                        anchors.fill: parent

                        z: 10

                        visible: root.isLoadingInitial
                        color: "#101820"

                        LoadingIndicator {
                            anchors.fill: parent

                            label: "Loading messages…"
                            spinnerSize: units.gu(5)
                            labelSize: units.gu(1.8)

                            active: root.isLoadingInitial
                        }
                    }
                }

                // =====================================
                // Message composer
                // =====================================

                MessageComposer {
                    id: messageComposer

                    width: parent.width

                    onSendRequested: {
                        if (root.roomId === "")
                            return

                        if (!root.backend || sending)
                            return

                        sending = true

                        console.log(
                            "ASTRO: Sending message to room",
                            root.roomId
                        )

                        root.backend.sendRoomMessage(
                            root.roomId,
                            message
                        )
                    }
                }
            }
        }
    }
}