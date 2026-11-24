import QtQuick 2.7
import QtMultimedia 5.8
import Lomiri.Components 1.3
import Lomiri.Content 1.1
import "Components"
import "Components/Rooms"

Item {
    id: root

    // Current MatrixRTC memberships for this room.
    property var rtcMemberships: []
    property var rtcCallState: ({})

    onRtcMembershipsChanged: root.requestRtcParticipantAvatars()



    property bool composerEmojiPickerVisible: false

    // Active reply/edit target.
    property string composerContextEventId: ""

    // Pending message deletion.
    property string pendingDeleteRoomId: ""
    property string pendingDeleteEventId: ""
    property string pendingDeleteBody: ""
    property bool deleteConfirmationVisible: false
    property bool deleteRequestInFlight: false


    // ---------------------------------------------------------
    // Matrix attachment upload state
    // ---------------------------------------------------------

    property bool attachmentPickerOpen: false
    property var activeAttachmentTransfer: null
    property string attachmentCaption: ""
    property bool attachmentBatchActive: false
    property string attachmentBatchRoomId: ""
    property int attachmentRequestSerial: 0
    property int attachmentPendingCount: 0
    property int attachmentCompletedCount: 0
    property int attachmentSuccessfulCount: 0
    property string attachmentLastError: ""
    property var attachmentRequests: ({})

    function openAttachmentPicker(caption) {
        if (!root.backend ||
            root.roomId === "" ||
            root.attachmentBatchActive ||
            messageComposer.sending ||
            messageComposer.attachmentBusy) {
            return
        }

        root.attachmentCaption = caption
        root.attachmentPickerOpen = true
        messageComposer.dismissKeyboard()
    }

    function finishAttachmentTransfer() {
        var transfer = root.activeAttachmentTransfer
        root.activeAttachmentTransfer = null
        root.attachmentPickerOpen = false

        if (transfer &&
            transfer.state !== ContentTransfer.Finalized) {
            transfer.finalize()
        }
    }

    function startAttachmentUploads(items) {
        if (!root.backend || root.roomId === "") {
            root.finishAttachmentTransfer()
            return
        }

        var queued = []
        var requests = {}

        for (var i = 0; items && i < items.length; i++) {
            var item = items[i]

            if (!item || !item.url)
                continue

            var fileUrl = String(item.url)

            if (fileUrl === "")
                continue

            var fileName = item.text
                ? String(item.text)
                : "Attachment " + (i + 1)

            root.attachmentRequestSerial++

            var requestKey =
                "astro-upload-" +
                Date.now().toString() + "-" +
                root.attachmentRequestSerial.toString()

            queued.push({
                requestKey: requestKey,
                fileUrl: fileUrl,
                fileName: fileName
            })

            requests[requestKey] = {
                roomId: root.roomId,
                fileName: fileName
            }
        }

        if (queued.length === 0) {
            messageComposer.statusText = "No usable files were selected"
            root.finishAttachmentTransfer()
            return
        }

        root.attachmentRequests = requests
        root.attachmentBatchRoomId = root.roomId
        root.attachmentBatchActive = true
        root.attachmentPendingCount = queued.length
        root.attachmentCompletedCount = 0
        root.attachmentSuccessfulCount = 0
        root.attachmentLastError = ""

        messageComposer.attachmentBusy = true
        messageComposer.statusText =
            queued.length === 1
            ? "Uploading attachment…"
            : "Uploading " + queued.length + " attachments…"

        // Register every request before dispatching any work.
        // This also handles immediate Backend validation errors.
        for (var j = 0; j < queued.length; j++) {
            root.backend.sendRoomAttachment(
                queued[j].requestKey,
                root.roomId,
                queued[j].fileUrl,
                j === 0 ? root.attachmentCaption : ""
            )
        }
    }


    // RoomView owns the single video player and video output.
    property string activeVideoKey: ""
    property var activeVideoOwner: null
    property var activeVideoOutputHost: null

    // The active player is replaced for each different video.
    // The VideoOutput itself remains persistent.
    property var roomSharedVideoPlayer: null

    Component {
        id: sharedVideoPlayerComponent

        MediaPlayer {
            autoPlay: false

            onPlaybackStateChanged: {
                console.log(
                    "[AstroVideoDiag] playbackState changed:",
                    playbackState,
                    "status:", status,
                    "source:", source
                )
            }

            onStatusChanged: {
                console.log(
                    "[AstroVideoDiag] status changed:",
                    status,
                    "playbackState:", playbackState,
                    "source:", source
                )

                console.log(
                    "[AstroVideoDiag] output orientation:",
                    roomSharedVideoOutputItem.orientation,
                    "autoOrientation:",
                    roomSharedVideoOutputItem.autoOrientation
                )
            }
        }
    }

    // Keep one renderer alive for the lifetime of RoomView.
    property var roomSharedVideoOutput: roomSharedVideoOutputItem

    Component.onCompleted: {
        root.requestRtcParticipantAvatars()

        if (!root.replaceSharedVideoPlayer())
            console.error("[AstroVideo] Initial MediaPlayer creation failed")
    }


    property string pendingSharedVideoKey: ""
    property string pendingSharedVideoUrl: ""
    property var pendingSharedVideoHost: null
    property var pendingSharedVideoOwner: null

    VideoOutput {
        id: roomSharedVideoOutputItem

        anchors.fill: parent
        source: null
        fillMode: VideoOutput.PreserveAspectFit
        visible: false
        z: 0
    }

    Timer {
        id: sharedVideoOutputRecreateTimer

        interval: 0
        repeat: false

        onTriggered: {
            var requestKey = root.pendingSharedVideoKey
            var fileUrl = root.pendingSharedVideoUrl
            var outputHost = root.pendingSharedVideoHost
            var owner = root.pendingSharedVideoOwner

            root.pendingSharedVideoKey = ""
            root.pendingSharedVideoUrl = ""
            root.pendingSharedVideoHost = null
            root.pendingSharedVideoOwner = null

            if (!requestKey || !fileUrl || !outputHost || !owner)
                return

            if (root.activeVideoKey !== requestKey ||
                    root.activeVideoOwner !== owner)
                return

            // Reuse the persistent renderer instead of recreating
            // the VideoOutput and its underlying graphics surface.
            var output = root.roomSharedVideoOutput

            if (!output) {
                console.error("[AstroVideo] Shared VideoOutput is unavailable")
                root.activeVideoKey = ""
                root.activeVideoOwner = null
                root.activeVideoOutputHost = null
                return
            }

            output.parent = outputHost
            output.visible = true
            root.activeVideoOutputHost = outputHost

            var player = root.replaceSharedVideoPlayer()

            if (!player) {
                console.error("[AstroVideo] Could not create replacement player")
                root.activeVideoKey = ""
                root.activeVideoOwner = null
                root.activeVideoOutputHost = null
                return
            }

            player.source = fileUrl
            player.play()
        }
    }


    property var backend

    property string openMessageActionsId: ""
    property var activeMessageBubble: null
    property string reactionPickerEventId: ""
    property real reactionPickerX: 0
    property real reactionPickerY: 0
    property bool reactionPickerVisible: false
    property string roomId: ""
    property string roomName: "Welcome to Astro"
    property string avatarUrl: ""
    property string avatarFallbackColour: "#a16b9b"

    // Event requested by an in-app notification.
    property string targetEventId: ""
    property int targetRequestSerial: 0
    property string pendingTargetEventId: ""
    property int targetPaginationStartCount: -1


    // Sender avatar cache, indexed by Matrix MXC URI.
    property var senderAvatarCache: ({})
    property var senderAvatarRequests: ({})
    property bool wideLayout: false

    // Timeline loading state.
    property bool isLoadingInitial: roomId !== ""
    property bool isLoadingOlder: false
    property bool hasMoreHistory: true
    property bool pendingInitialScroll: false
    property bool roomMenuVisible: false
    property string timelineLoadError: ""

    // Scroll state.
    property bool isAtBottom: true
    property real previousMessageContentHeight: 0
    property bool readReceiptSent: false
    property string selectedMessageEventId: ""

    function refreshTimeline() {
        if (
            !root.backend ||
            root.roomId === "" ||
            root.isLoadingInitial ||
            root.isLoadingOlder
        ) {
            return
        }

        root.roomMenuVisible = false

        messageComposer.dismissKeyboard()
        bottomScrollAnimation.stop()

        // Clear transient message UI and the visible history.
        root.selectedMessageEventId = ""
        root.openMessageActionsId = ""
        root.reactionPickerVisible = false
        root.reactionPickerEventId = ""

        root.stopSharedVideo()
      messageModel.clear()
        messageList.contentY = 0

        // Prepare the initial timeline response path.
        root.hasMoreHistory = true
        root.isLoadingOlder = false
        root.isLoadingInitial = true
        root.pendingInitialScroll = false
        root.pendingTargetEventId = ""
        root.targetPaginationStartCount = -1
        root.isAtBottom = true
        root.previousMessageContentHeight = 0
        root.timelineLoadError = ""

        console.log(
            "ASTRO: Refreshing room timeline:",
            root.roomId
        )

        // Purge this room's Matrix SDK event cache, then reload it.
        root.backend.requestRefreshRoomTimeline(root.roomId)
    }

    function tryNavigateToPendingEvent() {
        if (root.pendingTargetEventId === "")
            return

        if (root.roomId === "" ||
            root.isLoadingInitial ||
            root.isLoadingOlder)
            return

        // First search the messages already in the model.
        for (var i = 0; i < messageModel.count; i++) {
            if (messageModel.get(i).eventId !==
                root.pendingTargetEventId)
                continue

            var delegate = messageRepeater.itemAt(i)

            // Wait for the Repeater delegate to be laid out.
            if (!delegate) {
                Qt.callLater(root.tryNavigateToPendingEvent)
                return
            }

            var position = delegate.mapToItem(
                messageList.contentItem, 0, 0
            )

            var desiredY = position.y -
                (messageList.height - delegate.height) / 2

            var maximumY = Math.max(
                0,
                messageList.contentHeight - messageList.height
            )

            messageList.contentY = Math.max(
                0, Math.min(maximumY, desiredY)
            )

            // Reuse the existing message-selection highlight.
            root.selectedMessageEventId =
                root.pendingTargetEventId

            root.pendingTargetEventId = ""
            root.targetPaginationStartCount = -1
            root.updateBottomState()

            console.log(
                "ASTRO: Notification target located:",
                root.selectedMessageEventId
            )
            return
        }

        // The event is not in the loaded portion of the timeline.
        if (!root.hasMoreHistory) {
            console.warn(
                "ASTRO: Notification event is not present in room history:",
                root.pendingTargetEventId
            )
            root.pendingTargetEventId = ""
            root.targetPaginationStartCount = -1
            return
        }

        if (!root.backend || messageModel.count === 0)
            return

        // Each successful page must add messages. This prevents
        // endless requests if the server returns no usable overlap.
        if (root.targetPaginationStartCount >= 0 &&
            messageModel.count <= root.targetPaginationStartCount) {
            console.warn(
                "ASTRO: Stopping notification history search; no older messages were added"
            )
            root.pendingTargetEventId = ""
            root.targetPaginationStartCount = -1
            return
        }

        root.targetPaginationStartCount = messageModel.count
        root.isLoadingOlder = true

        console.log(
            "ASTRO: Loading older history to locate notification event:",
            root.pendingTargetEventId
        )

        root.backend.requestOlderRoomTimeline(root.roomId)
    }

    onTargetRequestSerialChanged: {
        root.pendingTargetEventId = root.targetEventId
        root.targetPaginationStartCount = -1
        Qt.callLater(root.tryNavigateToPendingEvent)
    }

    signal backRequested()
    signal sendMessageRequested(string roomId, string message)

    property var fullscreenVideoOutput: null
    readonly property bool fullscreenVideoActive:
        fullscreenVideoOutput !== null
    property var fullscreenVideoOriginalParent: null
    property var fullscreenVideoPlayer: null
    property var fullscreenVideoDownloadItem: null
    property bool fullscreenControlsVisible: true

    function formatVideoTime(milliseconds) {
        var totalSeconds = Math.floor(Math.max(0, milliseconds) / 1000)
        var minutes = Math.floor(totalSeconds / 60)
        var seconds = totalSeconds % 60

        return minutes + ":" +
               (seconds < 10 ? "0" : "") + seconds
    }

    function revealFullscreenControls() {
        fullscreenControlsVisible = true
        fullscreenControlsTimer.restart()
    }

    function seekFullscreenVideo(mouseX, trackWidth) {
        if (!fullscreenVideoPlayer ||
            fullscreenVideoPlayer.duration <= 0 ||
            trackWidth <= 0)
            return

        var fraction = Math.max(0, Math.min(1, mouseX / trackWidth))
        fullscreenVideoPlayer.seek(
            fraction * fullscreenVideoPlayer.duration
        )
    }

    function replaceSharedVideoPlayer() {
        var oldPlayer = root.roomSharedVideoPlayer
        var output = root.roomSharedVideoOutput

        var newPlayer = sharedVideoPlayerComponent.createObject(root)

        if (!newPlayer)
            return null

        // Reconnect the persistent renderer to the fresh decoder.
        root.roomSharedVideoPlayer = newPlayer

        if (output)
            output.source = newPlayer

        // Retain the old source until its player is destroyed.
        if (oldPlayer) {
            oldPlayer.stop()
            oldPlayer.destroy()
        }

        console.log("[AstroVideoDiag] Created fresh MediaPlayer")

        return newPlayer
    }

    function playMessageVideo(requestKey, fileUrl, outputHost, owner) {
        if (!requestKey || !fileUrl || !outputHost || !owner)
            return

        if (root.fullscreenVideoOutput)
            root.closeFullscreenVideo()

        var sameVideo = root.activeVideoKey === requestKey &&
                        root.activeVideoOwner === owner

        // Resuming the same video does not require a new renderer.
        if (sameVideo &&
                root.roomSharedVideoOutput &&
                root.roomSharedVideoPlayer.source.toString() === fileUrl) {
            root.roomSharedVideoOutput.parent = outputHost
            root.roomSharedVideoOutput.visible = true
            root.activeVideoOutputHost = outputHost

            if (root.roomSharedVideoPlayer.status === MediaPlayer.EndOfMedia)
                root.roomSharedVideoPlayer.seek(0)

            root.roomSharedVideoPlayer.play()
            return
        }

        // Cancel any pending renderer creation before switching again.
        sharedVideoOutputRecreateTimer.stop()

        // Changing the source in the recreation timer replaces the
        // previous media. Do not call stop() here: the Ubuntu Touch
        // media backend can reset its URI when stop() is called.

        // Keep the renderer alive while the deferred request is prepared.
        // The timer moves this same output into the new message host.
        if (root.roomSharedVideoOutput) {
            root.roomSharedVideoOutput.parent = root
            root.roomSharedVideoOutput.visible = false
        }

        root.activeVideoKey = requestKey
        root.activeVideoOwner = owner
        root.activeVideoOutputHost = outputHost

        root.pendingSharedVideoKey = requestKey
        root.pendingSharedVideoUrl = fileUrl
        root.pendingSharedVideoHost = outputHost
        root.pendingSharedVideoOwner = owner

        sharedVideoOutputRecreateTimer.start()
    }

    function releaseMessageVideo(requestKey, outputHost, owner) {
        if (root.activeVideoKey !== requestKey ||
                root.activeVideoOwner !== owner)
            return

        if (root.fullscreenVideoOutput)
            root.closeFullscreenVideo()

        sharedVideoOutputRecreateTimer.stop()

        root.pendingSharedVideoKey = ""
        root.pendingSharedVideoUrl = ""
        root.pendingSharedVideoHost = null
        root.pendingSharedVideoOwner = null

        root.roomSharedVideoPlayer.stop()


        // Retain the VideoOutput; only detach and hide it when idle.
        if (root.roomSharedVideoOutput) {
            root.roomSharedVideoOutput.parent = root
            root.roomSharedVideoOutput.visible = false
        }

        root.activeVideoKey = ""
        root.activeVideoOwner = null
        root.activeVideoOutputHost = null
    }

    function stopSharedVideo() {
        console.log(
            "[AstroVideoDiag] stopSharedVideo ENTER",
            "visible:", root.visible,
            "activeKey:", root.activeVideoKey,
            "playbackState:", root.roomSharedVideoPlayer.playbackState,
            "status:", root.roomSharedVideoPlayer.status,
            "source:", root.roomSharedVideoPlayer.source
        )

        if (root.fullscreenVideoOutput)
            root.closeFullscreenVideo()

        sharedVideoOutputRecreateTimer.stop()

        root.pendingSharedVideoKey = ""
        root.pendingSharedVideoUrl = ""
        root.pendingSharedVideoHost = null
        root.pendingSharedVideoOwner = null

        root.roomSharedVideoPlayer.stop()
        console.log(
            "[AstroVideoDiag] player.stop() returned",
            "playbackState:", root.roomSharedVideoPlayer.playbackState,
            "status:", root.roomSharedVideoPlayer.status
        )



        console.log(
            "[AstroVideoDiag] source retained after stop",
            "playbackState:", root.roomSharedVideoPlayer.playbackState,
            "status:", root.roomSharedVideoPlayer.status,
            "source:", root.roomSharedVideoPlayer.source
        )

        // Retain the VideoOutput; only detach and hide it when idle.
        if (root.roomSharedVideoOutput) {
            root.roomSharedVideoOutput.parent = root
            root.roomSharedVideoOutput.visible = false
        }

        root.activeVideoKey = ""
        root.activeVideoOwner = null
        root.activeVideoOutputHost = null
    }

    function openFullscreenVideo(
        output,
        originalParent,
        mediaPlayer,
        downloadItem
    ) {
        if (!output || !originalParent || !mediaPlayer)
            return

        if (root.fullscreenVideoOutput)
            closeFullscreenVideo()

        root.fullscreenVideoOutput = output
        root.fullscreenVideoOriginalParent = originalParent
        root.fullscreenVideoPlayer = mediaPlayer
        root.fullscreenVideoDownloadItem = downloadItem
        root.fullscreenControlsVisible = true

        fullscreenControlsTimer.restart()

        output.parent = fullscreenVideoOverlay
        fullscreenVideoOverlay.visible = true
    }

    function closeFullscreenVideo() {
        if (root.fullscreenVideoOutput &&
            root.fullscreenVideoOriginalParent) {
            root.fullscreenVideoOutput.parent =
                root.fullscreenVideoOriginalParent
        }

        root.fullscreenVideoOutput = null
        root.fullscreenVideoOriginalParent = null
        root.fullscreenVideoPlayer = null
        root.fullscreenVideoDownloadItem = null
        root.fullscreenControlsVisible = true
        fullscreenControlsTimer.stop()
        fullscreenVideoOverlay.visible = false
    }

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

    function handleMessageSelection(selectionId) {
        if (!selectionId)
            return

        root.selectedMessageEventId = selectionId

        for (var i = 0; i < messageRepeater.count; i++) {
            var delegate = messageRepeater.itemAt(i)

            if (
                delegate &&
                delegate.messageSelectionId !== selectionId
            ) {
                delegate.clearMessageSelection()
            }
        }
    }

    function handleMessageSelectionCleared(selectionId) {
        if (root.selectedMessageEventId === selectionId)
            root.selectedMessageEventId = ""
    }

    function handleMessageTouched(selectionId) {
        if (
            !selectionId ||
            !root.selectedMessageEventId ||
            root.selectedMessageEventId === selectionId
        ) {
            return
        }

        var previousSelectionId = root.selectedMessageEventId

        for (var i = 0; i < messageRepeater.count; i++) {
            var delegate = messageRepeater.itemAt(i)

            if (
                delegate &&
                delegate.messageSelectionId === previousSelectionId
            ) {
                delegate.clearMessageSelection()
                break
            }
        }
    }

    function handleMessageTap(selectionId, wasAllowed) {
        if (!selectionId)
            return

        var currentSelectionId = root.selectedMessageEventId

        if (currentSelectionId !== "") {
            for (var i = 0; i < messageRepeater.count; i++) {
                var delegate = messageRepeater.itemAt(i)

                if (delegate &&
                        delegate.messageSelectionId === currentSelectionId) {
                    delegate.clearMessageSelection()
                    break
                }
            }

            return
        }

        if (wasAllowed)
            root.selectedMessageEventId = selectionId
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
    // Sender avatars
    // =====================================

    function cachedSenderAvatar(uri) {
        if (!uri)
            return ""

        return root.senderAvatarCache[uri] || ""
    }

    function requestSenderAvatar(sender, uri) {
        if (!root.backend || !root.roomId || !sender || !uri)
            return

        if (root.cachedSenderAvatar(uri) !== "")
            return

        // Include the room, sender and URI so responses remain
        // identifiable even if the user changes rooms.
        var requestKey = JSON.stringify([
            root.roomId,
            sender,
            uri
        ])

        if (root.senderAvatarRequests[requestKey])
            return

        var requests = Object.assign({}, root.senderAvatarRequests)
        requests[requestKey] = {
            roomId: root.roomId,
            sender: sender,
            uri: uri
        }
        root.senderAvatarRequests = requests

        root.backend.requestSenderAvatar(requestKey, uri)
    }

    function requestRtcParticipantAvatars() {
        if (!root.backend || !root.roomId)
            return

        var memberships = root.rtcMemberships || []

        for (var i = 0; i < memberships.length; i++) {
            var participant = memberships[i]
            var sender = participant.sender || ""
            var uri = participant.avatar_url || ""

            if (sender !== "" && uri !== "" &&
                root.cachedSenderAvatar(uri) === "") {
                root.requestSenderAvatar(sender, uri)
            }
        }
    }

    function storeSenderAvatar(requestKey, dataUrl) {
        var request = root.senderAvatarRequests[requestKey]

        if (!request)
            return

        var requests = Object.assign({}, root.senderAvatarRequests)
        delete requests[requestKey]
        root.senderAvatarRequests = requests

        if (!dataUrl)
            return

        var cache = Object.assign({}, root.senderAvatarCache)
        cache[request.uri] = dataUrl
        root.senderAvatarCache = cache

        // Only update messages belonging to the room that
        // originally requested this avatar.
        if (root.roomId !== request.roomId)
            return

        for (var i = 0; i < messageModel.count; i++) {
            if (messageModel.get(i).senderAvatarUri === request.uri) {
                messageModel.setProperty(i, "senderAvatarUrl", dataUrl)
            }
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

    onVisibleChanged: {
        console.log(
            "[AstroVideoDiag] RoomView visible changed:",
            visible,
            "activeKey:", root.activeVideoKey,
            "playbackState:", root.roomSharedVideoPlayer.playbackState
        )

        if (!visible)
            root.stopSharedVideo()
    }

    onRoomIdChanged: {
        root.requestRtcParticipantAvatars()

        root.roomMenuVisible = false
        root.timelineLoadError = ""
        root.selectedMessageEventId = ""
        bottomScrollAnimation.stop()

        root.stopSharedVideo()
      messageModel.clear()
        messageComposer.clearInput()
        messageComposer.sending = false
        messageComposer.statusText = ""
        messageComposer.contextMode = ""
        messageComposer.contextLabel = ""
        root.composerContextEventId = ""


        root.deleteConfirmationVisible = false
        root.deleteRequestInFlight = false
        root.pendingDeleteRoomId = ""
        root.pendingDeleteEventId = ""
        root.pendingDeleteBody = ""

        // Reset the old room's scroll position.
        messageList.contentY = 0

        hasMoreHistory = true
        isLoadingOlder = false
        isLoadingInitial = roomId !== ""
        isAtBottom = true
        previousMessageContentHeight = 0
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
                root.timelineLoadError = error
                root.pendingTargetEventId = ""
                root.targetPaginationStartCount = -1

                console.log(
                    "ASTRO: Timeline retrieval failed:",
                    error
                )
                return
            }

            try {
                var result = JSON.parse(json)
                var messages = result.messages || []

                root.timelineLoadError = ""
                root.stopSharedVideo()
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
                    "STICKER DEBUG:",
                    "msgtype =", message.msgtype,
                    "body =", message.body,
                    "media =", !!message.media_source,
                    "mime =", message.media_mimetype
                )

                    var date = new Date(
                        Number(message.timestamp)
                    )
                    var dateInfo = root.messageDateInfo(date)

                    root.requestSenderAvatar(
                        message.sender || "",
                        message.sender_avatar || ""
                    )
                    if (
                        (message.msgtype || "m.text") === "m.image" ||
                        (message.msgtype || "m.text") === "m.sticker"
                    ) {
                        roomMedia.requestMessageImage(
                            message.event_id || "",
                            message.media_source || null
                        )
                    }
                    if ((message.msgtype || "m.text") === "m.video" && message.media_thumbnail_source) {
                        roomMedia.requestMessageThumbnail(
                            message.event_id || "",
                            message.media_thumbnail_source,
                            message.media_thumbnail_width || 480,
                            message.media_thumbnail_height || 270
                        )
                    }

                    messageModel.append({
                        eventId: message.event_id || "",
                        sender: message.sender || "",
                        senderDisplayName: message.sender_display_name || "",
                        senderAvatarUri: message.sender_avatar || "",
                        senderAvatarUrl: root.cachedSenderAvatar(
                            message.sender_avatar || ""
                        ),
                        body: message.body || "",
                        timestamp: date.toLocaleTimeString(),
                        timestampMs: Number(message.timestamp),
                        dateKey: dateInfo.key,
                        dateLabel: dateInfo.label,
                        imageUrl: roomMedia.cachedMessageImage(root.roomId, message.event_id || ""),
                        mediaSourceJson: message.media_source
                            ? JSON.stringify(message.media_source)
                            : "",
                        mediaMimetype: message.media_mimetype || "",
                        mediaThumbnailSourceJson: message.media_thumbnail_source
                            ? JSON.stringify(message.media_thumbnail_source)
                            : "",
                        mediaThumbnailWidth: Number(message.media_thumbnail_width) || 480,
                        mediaThumbnailHeight: Number(message.media_thumbnail_height) || 270,
                        videoThumbnailUrl: roomMedia.cachedMessageThumbnail(root.roomId, message.event_id || ""),
                        msgtype: message.msgtype || "m.text",
                        isOwn: message.is_own === true,
                        isEncrypted: message.is_encrypted === true,
                        reactionsJson: JSON.stringify(message.reactions || [])
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
                    root.tryNavigateToPendingEvent()
                })

            } catch (parseError) {
                root.isLoadingInitial = false
                root.pendingInitialScroll = false
                root.timelineLoadError = "parse"

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
                root.pendingTargetEventId = ""
                root.targetPaginationStartCount = -1

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

                        root.requestSenderAvatar(
                            message.sender || "",
                            message.sender_avatar || ""
                        )
                        if (
                        (message.msgtype || "m.text") === "m.image" ||
                        (message.msgtype || "m.text") === "m.sticker"
                    ) {
                            roomMedia.requestMessageImage(
                                message.event_id || "",
                                message.media_source || null
                            )
                        }
                        if ((message.msgtype || "m.text") === "m.video" && message.media_thumbnail_source) {
                            roomMedia.requestMessageThumbnail(
                                message.event_id || "",
                                message.media_thumbnail_source,
                                message.media_thumbnail_width || 480,
                                message.media_thumbnail_height || 270
                            )
                        }

                        messageModel.insert(insertedCount, {
                            eventId: eventId,
                            sender: message.sender || "",
                            senderDisplayName: message.sender_display_name || "",
                            senderAvatarUri: message.sender_avatar || "",
                            senderAvatarUrl: root.cachedSenderAvatar(
                                message.sender_avatar || ""
                            ),
                            body: message.body || "",
                            timestamp: date.toLocaleTimeString(),
                        timestampMs: Number(message.timestamp),
                            dateKey: dateInfo.key,
                            dateLabel: dateInfo.label,
                            imageUrl: roomMedia.cachedMessageImage(root.roomId, message.event_id || ""),
                            mediaSourceJson: message.media_source
                            ? JSON.stringify(message.media_source)
                            : "",
                            mediaMimetype: message.media_mimetype || "",
                            mediaThumbnailSourceJson: message.media_thumbnail_source
                                ? JSON.stringify(message.media_thumbnail_source)
                                : "",
                            mediaThumbnailWidth: Number(message.media_thumbnail_width) || 480,
                            mediaThumbnailHeight: Number(message.media_thumbnail_height) || 270,
                            videoThumbnailUrl: roomMedia.cachedMessageThumbnail(root.roomId, message.event_id || ""),
                            msgtype: message.msgtype || "m.text",
                            isOwn: message.is_own === true,
                            isEncrypted: message.is_encrypted === true,
                        reactionsJson: JSON.stringify(message.reactions || [])
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
                    result.reached_start !== true

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
                        root.tryNavigateToPendingEvent()
                    })
                })

            } catch (parseError) {
                root.isLoadingOlder = false
                root.pendingTargetEventId = ""
                root.targetPaginationStartCount = -1

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
                var addedLatestMessage = false

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

                    root.requestSenderAvatar(
                        message.sender || "",
                        message.sender_avatar || ""
                    )
                    if (
                        (message.msgtype || "m.text") === "m.image" ||
                        (message.msgtype || "m.text") === "m.sticker"
                    ) {
                        roomMedia.requestMessageImage(
                            message.event_id || "",
                            message.media_source || null
                        )
                    }
                    if ((message.msgtype || "m.text") === "m.video" && message.media_thumbnail_source) {
                        roomMedia.requestMessageThumbnail(
                            message.event_id || "",
                            message.media_thumbnail_source,
                            message.media_thumbnail_width || 480,
                            message.media_thumbnail_height || 270
                        )
                    }

                    var messageData = {
                        eventId: eventId,
                        sender: message.sender || "",
                        senderDisplayName: message.sender_display_name || "",
                        senderAvatarUri: message.sender_avatar || "",
                        senderAvatarUrl: root.cachedSenderAvatar(
                            message.sender_avatar || ""
                        ),
                        body: message.body || "",
                        timestamp: date.toLocaleTimeString(),
                        timestampMs: Number(message.timestamp),
                        dateKey: dateInfo.key,
                        dateLabel: dateInfo.label,
                        imageUrl: roomMedia.cachedMessageImage(root.roomId, message.event_id || ""),
                        mediaSourceJson: message.media_source
                            ? JSON.stringify(message.media_source)
                            : "",
                        mediaMimetype: message.media_mimetype || "",
                        mediaThumbnailSourceJson: message.media_thumbnail_source
                            ? JSON.stringify(message.media_thumbnail_source)
                            : "",
                        mediaThumbnailWidth: Number(message.media_thumbnail_width) || 480,
                        mediaThumbnailHeight: Number(message.media_thumbnail_height) || 270,
                        videoThumbnailUrl: roomMedia.cachedMessageThumbnail(root.roomId, message.event_id || ""),
                        msgtype: message.msgtype || "m.text",
                        isOwn: message.is_own === true,
                        isEncrypted: message.is_encrypted === true,
                        reactionsJson: JSON.stringify(message.reactions || [])
                    }

                    if (existingIndex >= 0) {
                        // Update an existing event in place.
                        messageModel.set(
                            existingIndex,
                            messageData
                        )
                    } else {
                        // Insert unseen events by their original Matrix
                        // timestamp. Decryption can make older events
                        // appear after newer events have already loaded.
                        var insertionIndex = messageModel.count

                        if (isFinite(messageData.timestampMs)) {
                            for (
                                var position = 0;
                                position < messageModel.count;
                                position++
                            ) {
                                var existingTimestamp = Number(
                                    messageModel.get(position).timestampMs
                                )

                                if (
                                    isFinite(existingTimestamp) &&
                                    existingTimestamp > messageData.timestampMs
                                ) {
                                    insertionIndex = position
                                    break
                                }
                            }
                        }

                        messageModel.insert(
                            insertionIndex,
                            messageData
                        )

                        // Only follow the event automatically if it was
                        // inserted at the newest end of the timeline.
                        if (insertionIndex === messageModel.count - 1)
                            addedLatestMessage = true
                    }
                }

                // Do not interrupt someone reading older messages.
                // Only scroll automatically if they were already
                // at the bottom before the new message arrived.
                if (addedLatestMessage && shouldStickToBottom) {
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

        onMatrixReactionToggled: {
            if (error !== "") {
                console.log("ASTRO: Could not toggle reaction:", error)
            }
        }

        // =====================================
        // Sender avatar responses
        // =====================================

        onSenderAvatarRetrieved: {
            root.storeSenderAvatar(requestKey, dataUrl)
        }

        onSenderAvatarFailed: {
            // Remove failed requests so a later timeline update
            // can retry the download.
            var requests = Object.assign(
                {},
                root.senderAvatarRequests
            )
            delete requests[requestKey]
            root.senderAvatarRequests = requests

            console.log(
                "ASTRO: Sender avatar download failed:",
                error
            )
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

        }


        onMatrixMessageActionFinished: {
            if (action === "delete") {
                if (roomId !== root.pendingDeleteRoomId ||
                    eventId !== root.pendingDeleteEventId) {
                    return
                }

                root.deleteRequestInFlight = false

                if (error !== "") {
                    console.log(
                        "ASTRO: Message deletion failed:",
                        error
                    )
                    messageComposer.statusText = error
                    return
                }

                // Remove the deleted event from the currently displayed timeline.
                for (
                    var messageIndex = messageModel.count - 1;
                    messageIndex >= 0;
                    messageIndex--
                ) {
                    if (messageModel.get(messageIndex).eventId === eventId) {
                        messageModel.remove(messageIndex)
                        break
                    }
                }

                root.deleteConfirmationVisible = false
                root.pendingDeleteRoomId = ""
                root.pendingDeleteEventId = ""
                root.pendingDeleteBody = ""
                messageComposer.statusText = ""
                return
            }

            if (roomId !== root.roomId ||
                eventId !== root.composerContextEventId) {
                return
            }

            if (action !== "reply" && action !== "edit")
                return

            messageComposer.sending = false

            if (error !== "") {
                console.log(
                    "ASTRO: Message " + action + " failed:",
                    error
                )
                messageComposer.statusText = error
                return
            }

            messageComposer.clearInput()
            messageComposer.contextMode = ""
            messageComposer.contextLabel = ""
            root.composerContextEventId = ""
            messageComposer.statusText = ""
        }


        onMatrixAttachmentProgress: {
            var details = root.attachmentRequests[requestKey]

            if (!details ||
                details.roomId !== root.roomId ||
                total <= 0) {
                return
            }

            var percent = Math.min(
                100,
                Math.floor(current * 100 / total)
            )

            messageComposer.statusText =
                "Uploading " + details.fileName +
                " (" + percent + "%)"
        }

        onMatrixAttachmentSent: {
            var details = root.attachmentRequests[requestKey]

            if (!details)
                return

            var requests = Object.assign(
                {},
                root.attachmentRequests
            )

            delete requests[requestKey]
            root.attachmentRequests = requests

            root.attachmentCompletedCount++

            if (error === "") {
                root.attachmentSuccessfulCount++

                console.log(
                    "ASTRO: Attachment sent:",
                    details.fileName,
                    eventId
                )
            } else {
                root.attachmentLastError =
                    details.fileName + ": " + error

                console.warn(
                    "ASTRO: Attachment upload failed:",
                    root.attachmentLastError
                )
            }

            if (details.roomId === root.roomId &&
                root.attachmentCompletedCount <
                    root.attachmentPendingCount) {
                messageComposer.statusText =
                    "Uploaded " +
                    root.attachmentCompletedCount +
                    " of " +
                    root.attachmentPendingCount +
                    " attachments…"
            }

            if (root.attachmentCompletedCount >=
                root.attachmentPendingCount) {

                var batchRoomId = root.attachmentBatchRoomId

                root.attachmentBatchActive = false
                messageComposer.attachmentBusy = false

                if (batchRoomId === root.roomId) {
                    if (root.attachmentSuccessfulCount ===
                        root.attachmentPendingCount) {
                        messageComposer.clearInput()
                        messageComposer.statusText = ""
                    } else {
                        messageComposer.statusText =
                            root.attachmentSuccessfulCount +
                            " of " +
                            root.attachmentPendingCount +
                            " attachments sent. " +
                            root.attachmentLastError
                    }
                }

                root.finishAttachmentTransfer()
            }
        }
    }

    Connections {
        target: roomMedia

        onImageReady: {
            if (targetRoomId !== root.roomId)
                return

            for (var i = 0; i < messageModel.count; i++) {
                if (messageModel.get(i).eventId === eventId) {
                    messageModel.setProperty(i, "imageUrl", dataUrl)
                    break
                }
            }
        }

        onThumbnailReady: {
            if (targetRoomId !== root.roomId)
                return

            for (var i = 0; i < messageModel.count; i++) {
                if (messageModel.get(i).eventId === eventId) {
                    messageModel.setProperty(i, "videoThumbnailUrl", dataUrl)
                    break
                }
            }
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
                    z: 10

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
                                console.log(
                                    "[AstroVideoDiag] Back button clicked",
                                    "visible:", root.visible,
                                    "activeKey:", root.activeVideoKey,
                                    "playbackState:",
                                    root.roomSharedVideoPlayer.playbackState
                                )

                                messageComposer.dismissKeyboard()
                                root.stopSharedVideo()
                                root.backRequested()
                            }
                        }
                    }

                    Rectangle {
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
                        radius: units.gu(1)

                        color: root.avatarFallbackColour
                        clip: true

                        Text {
                            anchors.centerIn: parent
                            text: root.roomName.length > 0
                                ? root.roomName.charAt(0).toUpperCase()
                                : "#"
                            color: "white"
                            font.pixelSize: units.gu(2.5)
                            font.bold: true
                            visible: conversationAvatarImage.status !== Image.Ready
                        }

                        Image {
                            id: conversationAvatarImage
                            anchors.fill: parent

                            source: root.avatarUrl
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                            visible: status === Image.Ready
                        }
                    }

                    Text {
                        anchors {
                            left: conversationAvatar.right
                            leftMargin: units.gu(2)
                            verticalCenter: parent.verticalCenter
                            right: roomMenuButton.left
                            rightMargin: units.gu(1)
                        }

                        text: root.roomName
                        color: "white"

                        font.pixelSize: units.gu(2)
                        font.bold: true

                        elide: Text.ElideRight
                    }

                    // Room options button.
                    Rectangle {
                        id: roomMenuButton

                        anchors {
                            right: parent.right
                            rightMargin: units.gu(1)
                            verticalCenter: parent.verticalCenter
                        }

                        width: units.gu(5)
                        height: units.gu(5)
                        radius: units.gu(1)

                        color: roomMenuMouse.pressed
                               ? "#40596b" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "⋮"
                            color: "white"
                            font.pixelSize: units.gu(3)
                        }

                        MouseArea {
                            id: roomMenuMouse
                            anchors.fill: parent
                            enabled: !root.isLoadingInitial &&
                                     !root.isLoadingOlder

                            onClicked: {
                                root.roomMenuVisible =
                                    !root.roomMenuVisible
                            }
                        }
                    }

                    // Room options dropdown.
                    Rectangle {
                        id: roomOptionsMenu

                        anchors {
                            right: parent.right
                            rightMargin: units.gu(1)
                            top: parent.bottom
                        }

                        width: units.gu(27)
                        height: units.gu(6)

                        radius: units.gu(1.5)
                        color: "#354b5b"
                        border.color: "#587184"

                        visible: root.roomMenuVisible
                        z: 100

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: units.gu(0.5)

                            radius: units.gu(1)
                            color: refreshTimelineMouse.pressed
                                   ? "#526e82" : "transparent"

                            opacity: root.isLoadingInitial ||
                                     root.isLoadingOlder ? 0.5 : 1

                            Text {
                                anchors {
                                    left: parent.left
                                    leftMargin: units.gu(2)
                                    verticalCenter: parent.verticalCenter
                                }

                                text: "Refresh timeline"
                                color: "white"
                                font.pixelSize: units.gu(1.8)
                            }

                            MouseArea {
                                id: refreshTimelineMouse
                                anchors.fill: parent

                                enabled: !root.isLoadingInitial &&
                                         !root.isLoadingOlder

                                onClicked: root.refreshTimeline()
                            }
                        }
                    }
                }

                // MatrixRTC participant strip and expandable list.
                RtcParticipants {
                    id: rtcParticipants
                    width: parent.width
                    memberships: root.rtcMemberships
                    avatarCache: root.senderAvatarCache
                    backend: root.backend
                    roomId: root.roomId
                    callState: root.rtcCallState
                }

                // =====================================
                // Message history
                // =====================================

                Item {
                    width: parent.width

                    height: parent.height -
                            units.gu(8) -
                            rtcParticipants.height -
                            messageComposer.height

                    Flickable {
                        id: messageList

                        anchors.fill: parent

                        contentWidth: width
                        contentHeight: messageColumn.height

                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        onContentYChanged: {
                            root.updateBottomState()
                            root.maybeLoadOlderMessages()
                        }

                        onHeightChanged: {
                            var shouldStayAtBottom = root.isAtBottom

                            Qt.callLater(function() {
                                if (shouldStayAtBottom) {
                                    messageList.contentY = Math.max(
                                        0,
                                        messageList.contentHeight -
                                            messageList.height
                                    )
                                }

                                root.updateBottomState()
                            })
                        }

                        onContentHeightChanged: {
                            var oldHeight =
                                root.previousMessageContentHeight

                            var wasAtBottom =
                                oldHeight > 0 &&
                                messageList.contentY >=
                                    Math.max(
                                        0,
                                        oldHeight - messageList.height
                                    ) - units.gu(5)

                            root.previousMessageContentHeight =
                                messageList.contentHeight

                            if (
                                root.pendingInitialScroll &&
                                !root.isLoadingInitial
                            ) {
                                Qt.callLater(function() {
                                    root.finishInitialScroll()
                                })
                            } else if (wasAtBottom) {
                                // Keep the viewport anchored immediately
                                // when media changes the timeline height.
                                messageList.contentY = Math.max(
                                    0,
                                    messageList.contentHeight -
                                        messageList.height
                                )
                                root.updateBottomState()
                            } else {
                                root.updateBottomState()
                            }
                        }

                        onMovementEnded: {
                            root.updateBottomState()
                            root.maybeLoadOlderMessages()
                        }

                        // Tapping unused chat space clears message
                        // selection and dismisses the composer keyboard.
                        MouseArea {
                            anchors.fill: parent
                            z: 0

                            onClicked: {
                                for (var i = 0; i < messageRepeater.count; i++) {
                                    var delegate = messageRepeater.itemAt(i)

                                    if (delegate)
                                        delegate.clearMessageSelection()
                                }

                                messageComposer.dismissKeyboard()
                            }
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

                                text: root.timelineLoadError !== ""
                                      ? "Couldn't load messages. Use the menu to retry."
                                      : "No messages to display"
                                color: "#9aabba"

                                horizontalAlignment: Text.AlignHCenter
                                font.pixelSize: units.gu(1.8)
                            }

                            // =====================================
                            // Message list
                            // =====================================

                            Repeater {
                                id: messageRepeater
                                model: messageModel

                                delegate: Item {
                                    id: messageDelegate

                                    property string messageSelectionId:
                                        model.eventId !== ""
                                        ? model.eventId
                                        : "message-" + index

                                    function clearMessageSelection() {
                                        messageBubbleItem.clearTextSelection()
                                    }

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

                                        RoomEventBubble {
                                            width: messageList.width
                                            visible: model.msgtype === "m.room.event"
                                            body: model.body
                                        }

                                        MessageBubble {
                                            id: messageBubbleItem

                                            collapsed: model.msgtype === "m.room.event"

                                            width: messageList.width
                                            selectionId: messageDelegate.messageSelectionId
                                            actionTrayVisible: root.openMessageActionsId !== "" && root.openMessageActionsId === messageDelegate.messageSelectionId
                                            tapSelectionAllowed: root.selectedMessageEventId === "" || root.selectedMessageEventId === messageDelegate.messageSelectionId
                                            messageSelected: !messageBubbleItem.isImage && !messageBubbleItem.isVideo && root.selectedMessageEventId === messageDelegate.messageSelectionId

                                            onMessageActionTrayToggled: {
                                                root.openMessageActionsId = visible ? selectionId : ""
                                                root.activeMessageBubble = visible ? messageBubbleItem : null
                                            }

                                            onMessageActionRequested: {
                                                if (!model.eventId)
                                                    return

                                                root.openMessageActionsId = ""
                                                root.activeMessageBubble = null

                                                if (action === "reply") {
                                                    root.composerContextEventId = model.eventId
                                                    messageComposer.contextMode = "reply"

                                                    var replyName =
                                                        model.senderDisplayName ||
                                                        model.sender

                                                    messageComposer.contextLabel =
                                                        "replying to: " + replyName

                                                    messageComposer.forceActiveFocus()
                                                } else if (action === "edit") {
                                                    if (!model.isOwn ||
                                                        model.msgtype !== "m.text") {
                                                        return
                                                    }

                                                    root.composerContextEventId = model.eventId
                                                    messageComposer.contextMode = "edit"
                                                    messageComposer.contextLabel = "(edit)"
                                                    messageComposer.setInputText(model.body || "")
                                                } else if (action === "delete") {
                                                    if (!model.isOwn)
                                                        return

                                                    root.pendingDeleteRoomId = root.roomId
                                                    root.pendingDeleteEventId = model.eventId
                                                    root.pendingDeleteBody = model.body || ""
                                                    root.deleteRequestInFlight = false
                                                    root.deleteConfirmationVisible = true
                                                }
                                            }

                                            onReactionPickerRequested: {
                                                var anchor = messageBubbleItem.mapToItem(
                                                    root,
                                                    anchorX,
                                                    anchorY
                                                )

                                                root.reactionPickerEventId = eventId
                                                root.reactionPickerX = anchor.x
                                                root.reactionPickerY = anchor.y
                                                root.reactionPickerVisible = true
                                            }

                                            onReactionRequested: {
                                                if (root.backend) {
                                                    root.backend.toggleRoomReaction(
                                                        root.roomId,
                                                        eventId,
                                                        reactionKey
                                                    )
                                                }
                                            }

                                            onSelectionRequested: {
                                                root.handleMessageSelection(selectionId)
                                            }

                                            onSelectionCleared: {
                                                root.handleMessageSelectionCleared(selectionId)
                                            }

                                            onMessageTouched: {
                                                messageComposer.dismissKeyboard()
                                                 if (root.openMessageActionsId !== "" &&
                                                        root.openMessageActionsId !== selectionId) {
                                                    root.openMessageActionsId = ""
                                                    root.activeMessageBubble = null
                                                }
                                            }

                                            onMessageTapRequested: {
                                                root.handleMessageTap(selectionId, wasAllowed)
                                            }

                                            sender: model.sender
                                            senderDisplayName: model.senderDisplayName
                                            msgtype: model.msgtype
                                            avatarUrl: model.senderAvatarUrl
                                            showAvatar: index === 0 ||
                                                messageModel.get(index - 1).sender !== model.sender
                                            imageUrl: model.imageUrl
                                            isImage: model.msgtype === "m.image" ||
                                                model.msgtype === "m.sticker"
                                            isSticker: model.msgtype === "m.sticker"

                                            isVideo: model.msgtype === "m.video"
                                            backend: root.backend
                                            videoRequestKey: JSON.stringify([
                                                root.roomId,
                                                model.eventId
                                            ])
                                            videoMediaSourceJson: model.mediaSourceJson
                                            videoThumbnailUrl: model.videoThumbnailUrl
                                            videoMimeType: model.mediaMimetype
                                            fullscreenHost: fullscreenVideoOverlay
                                            sharedVideoPlayer: root.roomSharedVideoPlayer
                                            sharedVideoOutput: roomSharedVideoOutput
                                            sharedPlaybackKey: root.activeVideoKey

                                            onVideoPlaybackRequested: {
                                                root.playMessageVideo(
                                                    requestKey,
                                                    fileUrl,
                                                    outputHost,
                                                    owner
                                                )
                                            }

                                            onVideoPlaybackReleased: {
                                                root.releaseMessageVideo(
                                                    requestKey,
                                                    outputHost,
                                                    owner
                                                )
                                            }

                                            onVideoFullscreenRequested: {
                                                root.openFullscreenVideo(
                                                    output,
                                                    originalParent,
                                                    mediaPlayer,
                                                    downloadItem
                                                )
                                            }
                                            onImageClicked: {
                                                messageComposer.dismissKeyboard()
                                                 roomMedia.openImageViewer(
                                                    imageUrl,
                                                    model.eventId,
                                                    model.mediaSourceJson,
                                                    model.mediaMimetype,
                                                    model.body
                                                )
                                            }

                                            onPasteRequested: {
                                                messageComposer.pasteFromClipboard()
                                            }

                                            body: model.body
                                            timestamp: model.timestamp
                                            reactionsJson: model.reactionsJson

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
                                messageComposer.dismissKeyboard()
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

                    onContextCancelRequested: {
                        contextMode = ""
                        contextLabel = ""
                        root.composerContextEventId = ""
                        clearInput()
                    }

                    onSendRequested: {
                        if (root.roomId === "")
                            return

                        if (!root.backend || sending)
                            return

                        sending = true

                        if (contextMode === "edit") {
                            root.backend.editRoomMessage(
                                root.roomId,
                                root.composerContextEventId,
                                message
                            )
                        } else if (contextMode === "reply") {
                            root.backend.sendRoomReply(
                                root.roomId,
                                root.composerContextEventId,
                                message
                            )
                        } else {
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

                    onAttachRequested: {
                        root.openAttachmentPicker(caption)
                    }
                }
            }
        }
    }

    // =====================================
    // =====================================
    // =====================================
    // =====================================
    // Attachment-menu outside-tap dismissal.
    // This overlay is at RoomView level, outside the clipped
    // conversation container.
    MouseArea {
        anchors.fill: parent
        z: 900

        visible: messageComposer.attachmentMenuVisible
        enabled: visible

        onClicked: {
            messageComposer.attachmentMenuVisible = false
        }
    }

    // Attachment submenu, rendered outside the clipped
    // conversation hierarchy.
    // Attachment submenu. File is functional; the other entries
    // are placeholders for later implementation.
    Rectangle {
        id: attachmentMenu

        x: Math.max(
            units.gu(1),
            Math.min(
                root.width - width - units.gu(1),
                messageComposer.attachmentButtonItem.mapToItem(
                    root, 0, 0
                ).x + messageComposer.attachmentButtonItem.width - width
            )
        )

        width: units.gu(27)
        height: units.gu(18)
        radius: units.gu(1.5)
        color: "#354b5b"
        border.color: "#587184"

        //complex but works...
        // Keep the known-good position, adjusting for the open keyboard.
        y: root.height
   - height
   - units.gu(2)
   - messageComposer.height
   - (Qt.inputMethod.visible
      ? Math.min(
            Qt.inputMethod.keyboardRectangle.height,
            root.height - messageComposer.height - height
        )
      : 0)

        visible: messageComposer.attachmentMenuVisible
        z: 901

        Column {
            anchors.fill: parent
            anchors.margins: units.gu(0.5)
            spacing: 0

            Rectangle {
                width: parent.width
                height: units.gu(5.5)
                radius: units.gu(1)
                color: fileOptionMouse.pressed
                       ? "#526e82" : "transparent"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: units.gu(1.5)
                    spacing: units.gu(1.5)

                    Text {
                        width: units.gu(3)
                        height: parent.height
                        text: "📎"
                        color: "white"
                        font.pixelSize: units.gu(2)
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        height: parent.height
                        text: "File"
                        color: "white"
                        font.pixelSize: units.gu(1.8)
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                MouseArea {
                    id: fileOptionMouse
                    anchors.fill: parent
                    enabled: !messageComposer.sending && !messageComposer.attachmentBusy

                    onClicked: {
                        messageComposer.attachmentMenuVisible = false
                        messageComposer.attachRequested(
                            messageComposer.currentMessage().trim()
                        )
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: units.gu(5.5)
                radius: units.gu(1)
                color: emojiOptionMouse.pressed
                       ? "#526e82" : "transparent"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: units.gu(1.5)
                    spacing: units.gu(1.5)

                    Text {
                        width: units.gu(3)
                        height: parent.height
                        text: "☺"
                        color: "white"
                        font.pixelSize: units.gu(2.2)
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        height: parent.height
                        text: "Emoji"
                        color: "white"
                        font.pixelSize: units.gu(1.8)
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                MouseArea {
                    id: emojiOptionMouse
                    anchors.fill: parent
                    enabled: !messageComposer.sending &&
                             !messageComposer.attachmentBusy

                    onClicked: {
                        messageComposer.attachmentMenuVisible = false
                        messageComposer.pasteVisible = false
                        root.composerEmojiPickerVisible = true
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: units.gu(5.5)
                color: "transparent"
                opacity: 0.45

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: units.gu(1.5)
                    spacing: units.gu(1.5)

                    Text {
                        width: units.gu(3)
                        height: parent.height
                        text: "▧"
                        color: "white"
                        font.pixelSize: units.gu(2.2)
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        height: parent.height
                        text: "GIF / Sticker"
                        color: "white"
                        font.pixelSize: units.gu(1.8)
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
    // Room media
    // =====================================

    Timer {
        id: fullscreenControlsTimer

        interval: 3500
        repeat: false

        onTriggered: {
            if (root.fullscreenVideoPlayer &&
                root.fullscreenVideoPlayer.playbackState ===
                    MediaPlayer.PlayingState) {
                root.fullscreenControlsVisible = false
            }
        }
    }

    Rectangle {
        id: fullscreenVideoOverlay

        anchors.fill: parent
        z: 2000
        visible: false
        color: "#080d12"

        MouseArea {
            anchors.fill: parent
            z: 1

            onDoubleClicked: root.closeFullscreenVideo()

            onClicked: {
                if (!root.fullscreenVideoPlayer)
                    return

                if (root.fullscreenVideoPlayer.playbackState ===
                        MediaPlayer.PlayingState) {
                    root.fullscreenVideoPlayer.pause()
                } else {
                    if (root.fullscreenVideoPlayer.status ===
                            MediaPlayer.EndOfMedia) {
                        root.fullscreenVideoPlayer.seek(0)
                    }

                    root.fullscreenVideoPlayer.play()
                }

                root.revealFullscreenControls()
            }
        }

        // Close button.
        Rectangle {
            anchors {
                top: parent.top
                right: parent.right
                margins: units.gu(2)
            }

            width: units.gu(6)
            height: units.gu(6)
            radius: width / 2
            z: 10
            color: "#bb263744"
            opacity: root.fullscreenControlsVisible ? 1 : 0

            Behavior on opacity {
                NumberAnimation { duration: 180 }
            }

            Text {
                anchors.centerIn: parent
                text: "×"
                color: "white"
                font.pixelSize: units.gu(4)
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.closeFullscreenVideo()
            }
        }

        // Playback controls and seek bar.
        Rectangle {
            id: fullscreenPlaybackControls

            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                margins: units.gu(2)
            }

            height: units.gu(15)
            radius: units.gu(1)
            color: "#dd101820"
            z: 5
            opacity: root.fullscreenControlsVisible ? 1 : 0
            visible: opacity > 0

            Behavior on opacity {
                NumberAnimation { duration: 180 }
            }

            Column {
                anchors.fill: parent
                anchors.margins: units.gu(1.5)
                spacing: units.gu(1)

                // Seek track.
                Item {
                    id: seekTrack

                    width: parent.width
                    height: units.gu(2)

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: units.gu(0.6)
                        radius: height / 2
                        color: "#657580"
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.fullscreenVideoPlayer &&
                               root.fullscreenVideoPlayer.duration > 0
                               ? parent.width *
                                 root.fullscreenVideoPlayer.position /
                                 root.fullscreenVideoPlayer.duration
                               : 0
                        height: units.gu(0.6)
                        radius: height / 2
                        color: "#55b8e8"
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: root.fullscreenVideoPlayer &&
                           root.fullscreenVideoPlayer.duration > 0
                           ? Math.max(0, Math.min(
                               parent.width - width,
                               parent.width *
                               root.fullscreenVideoPlayer.position /
                               root.fullscreenVideoPlayer.duration - width / 2
                             ))
                           : 0
                        width: units.gu(2)
                        height: units.gu(2)
                        radius: width / 2
                        color: "#ffffff"
                    }

                    MouseArea {
                        anchors.fill: parent
                        onPressed: {
                            root.seekFullscreenVideo(mouse.x, width)
                            root.revealFullscreenControls()
                        }
                        onPositionChanged: {
                            if (pressed) {
                                root.seekFullscreenVideo(mouse.x, width)
                                root.revealFullscreenControls()
                            }
                        }
                    }
                }

                Row {
                    width: parent.width
                    height: units.gu(7)
                    spacing: units.gu(1)

                    Rectangle {
                        width: units.gu(7)
                        height: units.gu(7)
                        radius: width / 2
                        color: "#bb263744"

                        Canvas {
                            anchors.centerIn: parent
                            width: units.gu(2)
                            height: units.gu(2.5)
                            visible: !root.fullscreenVideoPlayer ||
                                     root.fullscreenVideoPlayer.playbackState !==
                                         MediaPlayer.PlayingState

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.fillStyle = "#ffffff"
                                ctx.beginPath()
                                ctx.moveTo(0, 0)
                                ctx.lineTo(width, height / 2)
                                ctx.lineTo(0, height)
                                ctx.closePath()
                                ctx.fill()
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "Ⅱ"
                            color: "#ffffff"
                            font.pixelSize: units.gu(3)
                            visible: root.fullscreenVideoPlayer &&
                                     root.fullscreenVideoPlayer.playbackState ===
                                         MediaPlayer.PlayingState
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (!root.fullscreenVideoPlayer)
                                    return

                                if (root.fullscreenVideoPlayer.playbackState ===
                                    MediaPlayer.PlayingState) {
                                    root.fullscreenVideoPlayer.pause()
                                } else {
                                    if (root.fullscreenVideoPlayer.status ===
                                        MediaPlayer.EndOfMedia) {
                                        root.fullscreenVideoPlayer.seek(0)
                                    }

                                    root.fullscreenVideoPlayer.play()
                                }

                                root.revealFullscreenControls()
                            }
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.fullscreenVideoPlayer
                              ? root.formatVideoTime(
                                    root.fullscreenVideoPlayer.position
                                ) + " / " +
                                root.formatVideoTime(
                                    root.fullscreenVideoPlayer.duration
                                )
                              : "0:00 / 0:00"
                        color: "white"
                        font.pixelSize: units.gu(1.8)
                    }

                    Item {
                        width: Math.max(0, parent.width - units.gu(27))
                        height: 1
                    }

                    Rectangle {
                        width: units.gu(7)
                        height: units.gu(7)
                        radius: width / 2
                        color: "#245b78"

                        Text {
                            anchors.centerIn: parent
                            text: "↓"
                            color: "white"
                            font.pixelSize: units.gu(3.5)
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.fullscreenVideoDownloadItem !== null &&
                                     !root.fullscreenVideoDownloadItem.exporting

                            onClicked: {
                                if (root.fullscreenVideoDownloadItem)
                                    root.fullscreenVideoDownloadItem.exportVideo()
                            }
                        }
                    }
                }
            }

        }
    }

    MouseArea {
        id: dismissMessageActionsArea
        anchors.fill: parent
        z: 900
        visible: root.openMessageActionsId !== ""
        enabled: visible
        propagateComposedEvents: true

        onPressed: {
            var insideActiveMessage = false

            if (root.activeMessageBubble) {
                var point = root.activeMessageBubble.mapFromItem(
                    root, mouse.x, mouse.y
                )
                insideActiveMessage =
                    point.x >= 0 &&
                    point.y >= 0 &&
                    point.x <= root.activeMessageBubble.width &&
                    point.y <= root.activeMessageBubble.height
            }

            if (!insideActiveMessage) {
                root.openMessageActionsId = ""
                root.activeMessageBubble = null
            }

            mouse.accepted = false
        }

        onClicked: mouse.accepted = false
    }

    RoomMedia {
        id: roomMedia

        anchors.fill: parent
        z: 1000

        backend: root.backend
        roomId: root.roomId
    }


    // Room-wide reaction picker overlay. Kept outside the clipped message list.
    MouseArea {
        anchors.fill: parent
        z: 2999
        visible: root.reactionPickerVisible
        enabled: visible
        preventStealing: true

        onClicked: root.reactionPickerVisible = false
    }

    EmojiReactionPicker {
        id: roomEmojiReactionPicker

        z: 3000
        visible: root.reactionPickerVisible

        x: Math.max(
            units.gu(1),
            Math.min(
                root.width - width - units.gu(1),
                root.reactionPickerX - width + units.gu(4)
            )
        )

        y: root.reactionPickerY >= height + units.gu(2)
           ? root.reactionPickerY - height - units.gu(1)
           : Math.min(
                 root.height - height - units.gu(1),
                 root.reactionPickerY + units.gu(5)
             )

        onReactionSelected: {
            root.reactionPickerVisible = false

            if (root.backend && root.reactionPickerEventId !== "") {
                root.backend.toggleRoomReaction(
                    root.roomId,
                    root.reactionPickerEventId,
                    reactionKey
                )
            }
        }

        onDismissed: root.reactionPickerVisible = false
    }

    // Composer emoji picker overlay.
    MouseArea {
        anchors.fill: parent
        z: 3100
        visible: root.composerEmojiPickerVisible
        enabled: visible
        preventStealing: true

        onClicked: root.composerEmojiPickerVisible = false
    }

    EmojiReactionPicker {
        id: composerEmojiPicker

        pickerTitle: "Insert emoji"
        z: 3101
        visible: root.composerEmojiPickerVisible

        x: Math.max(
            units.gu(1),
            (root.width - width) / 2
        )

        y: Math.max(
            units.gu(1),
            root.height
                - height
                - units.gu(2)
                - messageComposer.height
                - (Qt.inputMethod.visible
                ? Math.min(
                        Qt.inputMethod.keyboardRectangle.height,
                        root.height - messageComposer.height - height
                    )
                : 0)
        )

        onReactionSelected: {
            root.composerEmojiPickerVisible = false
            messageComposer.insertEmoji(reactionKey)
        }

        onDismissed: root.composerEmojiPickerVisible = false
    }


    // =====================================
    // Content Hub attachment import
    // =====================================

    ContentPeerPicker {
        id: attachmentPeerPicker

        anchors.fill: parent
        z: 10000
        visible: root.attachmentPickerOpen

        contentType: ContentType.All
        handler: ContentHandler.Source
        showTitle: true
        headerText: "Choose files to attach"

        onPeerSelected: {
            peer.selectionType = ContentTransfer.Multiple
            root.activeAttachmentTransfer = peer.request()
            root.attachmentPickerOpen = false

            if (!root.activeAttachmentTransfer) {
                messageComposer.statusText =
                    "Could not start file selection"
            }
        }

        onCancelPressed: {
            root.attachmentPickerOpen = false
        }
    }

    Connections {
        target: root.activeAttachmentTransfer

        onStateChanged: {
            var transfer = root.activeAttachmentTransfer

            if (!transfer)
                return

            if (transfer.state === ContentTransfer.Charged) {
                root.startAttachmentUploads(transfer.items)
            }
            else if (transfer.state === ContentTransfer.Aborted) {
                if (!root.attachmentBatchActive) {
                    messageComposer.statusText =
                        "File selection cancelled"
                    root.finishAttachmentTransfer()
                }
            }
        }
    }

    ContentTransferHint {
        anchors.fill: parent
        z: 10001
        activeTransfer: root.activeAttachmentTransfer
    }



    // =====================================
    // Message deletion confirmation
    // =====================================

    Rectangle {
        id: deleteConfirmationOverlay

        anchors.fill: parent
        color: "#99000000"
        visible: root.deleteConfirmationVisible
        z: 12000

        MouseArea {
            anchors.fill: parent
            onClicked: {
                // Keep taps on the backdrop from reaching the room.
            }
        }

        Rectangle {
            width: Math.min(
                parent.width - units.gu(4),
                units.gu(38)
            )
            height: Math.min(parent.height - units.gu(4), deleteDialogContent.implicitHeight + units.gu(4))
            anchors.centerIn: parent

            radius: units.gu(1.5)
            color: "#263744"
            border.color: "#7890a2"

            Column {
                id: deleteDialogContent
                anchors.fill: parent
                anchors.margins: units.gu(2)
                spacing: units.gu(1.2)

                Text {
                    width: parent.width
                    text: "Delete message?"
                    color: "white"
                    font.bold: true
                    font.pixelSize: units.gu(2.2)
                }

                Text {
                    width: parent.width
                    height: units.gu(5.5)
                    text: root.pendingDeleteBody
                    color: "#d3dce3"
                    font.pixelSize: units.gu(1.6)
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                    maximumLineCount: 4
                    verticalAlignment: Text.AlignTop
                }

                Text {
                    width: parent.width
                    text: "This will remove the message for room participants."
                    color: "#b8cbd8"
                    font.pixelSize: units.gu(1.4)
                    wrapMode: Text.Wrap
                }

                Item {
                    width: parent.width
                    height: units.gu(4.5)

                    Row {
                        width: parent.width
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: units.gu(1)

                        Rectangle {
                            width: (parent.width - units.gu(1)) / 2
                            height: units.gu(4.5)
                            radius: units.gu(0.8)
                            color: cancelDeleteMouse.pressed
                                   ? "#526e82" : "#354957"

                            Text {
                                anchors.centerIn: parent
                                text: "Cancel"
                                color: "white"
                                font.pixelSize: units.gu(1.6)
                            }

                            MouseArea {
                                id: cancelDeleteMouse
                                anchors.fill: parent
                                enabled: !root.deleteRequestInFlight

                                onClicked: {

                                    root.deleteConfirmationVisible = false
                                    root.pendingDeleteRoomId = ""
                                    root.pendingDeleteEventId = ""
                                    root.pendingDeleteBody = ""
                                }
                            }
                        }

                        Rectangle {
                            width: (parent.width - units.gu(1)) / 2
                            height: units.gu(4.5)
                            radius: units.gu(0.8)
                            color: confirmDeleteMouse.pressed
                                   ? "#a34f55" : "#803d43"
                            opacity: root.deleteRequestInFlight ? 0.5 : 1

                            Text {
                                anchors.centerIn: parent
                                text: root.deleteRequestInFlight
                                      ? "Deleting…" : "Delete"
                                color: "white"
                                font.bold: true
                                font.pixelSize: units.gu(1.6)
                            }

                            MouseArea {
                                id: confirmDeleteMouse
                                anchors.fill: parent
                                enabled: !root.deleteRequestInFlight

                                onClicked: {
                                    if (!root.backend ||
                                        root.pendingDeleteRoomId === "" ||
                                        root.pendingDeleteEventId === "") {
                                        return
                                    }

                                    root.deleteRequestInFlight = true
                                    root.backend.redactRoomEvent(
                                        root.pendingDeleteRoomId,
                                        root.pendingDeleteEventId
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }

}
