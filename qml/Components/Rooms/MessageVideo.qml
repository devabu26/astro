
import QtQuick 2.7
import QtMultimedia 5.8
import Lomiri.Components 1.3
import Ubuntu.Content 1.1

Item {
    id: root

    property string requestKey: ""
    property string mediaSourceJson: ""
    property string mimeType: ""
    property string thumbnailUrl: ""
    property var backend
    property var fullscreenHost

    // The player and output are owned by RoomView and shared by all videos.
    property var sharedPlayer: null
    property var sharedVideoOutput: null
    property string sharedPlaybackKey: ""

    readonly property bool isActivePlayback:
        root.sharedPlaybackKey !== "" &&
        root.sharedPlaybackKey === root.requestKey

    readonly property bool displayedPlaying:
        root.isActivePlayback &&
        root.sharedPlayer &&
        root.sharedPlayer.playbackState === MediaPlayer.PlayingState

    readonly property int displayedPosition:
        root.isActivePlayback && root.sharedPlayer
        ? root.sharedPlayer.position : 0

    readonly property int displayedDuration:
        root.isActivePlayback && root.sharedPlayer
        ? root.sharedPlayer.duration : 0

    signal fullscreenRequested(
        var output,
        var originalParent,
        var mediaPlayer,
        var downloadItem
    )

    signal playbackRequested(
        string requestKey,
        string fileUrl,
        var outputHost,
        var owner
    )

    signal playbackReleased(
        string requestKey,
        var outputHost,
        var owner
    )

    property string videoUrl: ""
    property bool downloading: false
    property bool exporting: false
    property bool destinationPickerOpen: false
    property string pendingExportUrl: ""
    property string exportStatus: ""
    property var activeTransfer: null
    property bool playbackFinished: false
    property string errorMessage: ""
    property bool controlsVisible: true

    signal playbackStarted()
    signal swipeStarted()
    signal swipeMoved(real dx, real dy)
    signal swipeEnded(bool cancelled)

    width: parent ? parent.width : units.gu(40)
    height: units.gu(25)

    function requestVideo() {
        if (root.videoUrl !== "") {
            if (!root.sharedPlayer) {
                root.errorMessage = "Video playback is unavailable"
                return
            }

            if (root.isActivePlayback) {
                if (root.playbackFinished) {
                    root.sharedPlayer.seek(0)
                    root.playbackFinished = false
                }

                root.errorMessage = ""
            }

            root.playbackRequested(
                root.requestKey,
                root.videoUrl,
                videoOutputHost,
                root
            )
            return
        }

        if (root.downloading)
            return

        if (!root.backend ||
            !root.requestKey ||
            !root.mediaSourceJson) {
            root.errorMessage = "Video is unavailable"
            return
        }

        root.downloading = true
        root.errorMessage = ""

        root.backend.requestMessageVideo(
            root.requestKey,
            root.mediaSourceJson,
            root.mimeType
        )
    }

    function revealControls() {
        root.controlsVisible = true
        embeddedControlsTimer.restart()
    }

    function formatVideoTime(milliseconds) {
        var seconds = Math.floor(Math.max(0, milliseconds) / 1000)
        var minutes = Math.floor(seconds / 60)
        seconds = seconds % 60

        return minutes + ":" +
               (seconds < 10 ? "0" : "") + seconds
    }

    function seekVideo(mouseX, trackWidth) {
        if (!root.isActivePlayback ||
            !root.sharedPlayer ||
            root.displayedDuration <= 0 ||
            trackWidth <= 0)
            return

        var fraction = Math.max(0, Math.min(1, mouseX / trackWidth))
        root.sharedPlayer.seek(fraction * root.displayedDuration)
        root.revealControls()
    }

function togglePlayback() {
    console.log(
        "[AstroVideo] togglePlayback",
        "requestKey:", root.requestKey,
        "activeKey:", root.sharedPlaybackKey,
        "isActive:", root.isActivePlayback,
        "hasPlayer:", !!root.sharedPlayer,
        "playbackState:",
            root.sharedPlayer
                ? root.sharedPlayer.playbackState : -1,
        "status:",
            root.sharedPlayer
                ? root.sharedPlayer.status : -1,
        "finished:", root.playbackFinished,
        "downloading:", root.downloading
    )

    if (root.downloading)
        return

    if (root.isActivePlayback &&
        root.sharedPlayer &&
        root.sharedPlayer.playbackState === MediaPlayer.PlayingState) {
        console.log("[AstroVideo] ACTION: pause")
        root.sharedPlayer.pause()
    } else {
        console.log("[AstroVideo] ACTION: request playback")
        root.requestVideo()
    }

    root.revealControls()
}

    function requestFullscreen() {
        if (!root.fullscreenHost ||
            !root.isActivePlayback ||
            !root.sharedVideoOutput ||
            !root.sharedPlayer)
            return

        root.fullscreenRequested(
            root.sharedVideoOutput,
            videoOutputHost,
            root.sharedPlayer,
            root
        )
    }

    function handleVideoRetrieved(key, fileUrl) {
        if (key !== root.requestKey)
            return

        root.downloading = false

        if (!fileUrl) {
            root.errorMessage = "The downloaded video is empty"
            return
        }

        root.videoUrl = fileUrl
        root.playbackFinished = false
        root.errorMessage = ""

        // RoomView stops any previous playback, assigns the new source,
        // reparents the shared output, and starts playback.
        root.playbackRequested(
            root.requestKey,
            root.videoUrl,
            videoOutputHost,
            root
        )
    }

    function handleVideoFailed(key, error) {
        if (key !== root.requestKey)
            return

        root.downloading = false
        root.errorMessage = error || "Video download failed"
    }

    // Export the already-downloaded original video file.
    function exportVideo() {
        if (root.videoUrl === "" || root.exporting)
            return

        root.exporting = true
        root.pendingExportUrl = root.videoUrl
        root.exportStatus = "Choose where to save the video"
        root.destinationPickerOpen = true
    }

    Connections {
        target: root.backend

        onMessageVideoRetrieved: {
            root.handleVideoRetrieved(requestKey, fileUrl)
        }

        onMessageVideoFailed: {
            root.handleVideoFailed(requestKey, error)
        }
    }

    // Listen to the shared player, but only update this bubble's state
    // while this message owns the shared playback session.
    Connections {
        target: root.sharedPlayer

        onPlaybackStateChanged: {
            if (!root.isActivePlayback)
                return

            if (root.sharedPlayer.playbackState ===
                MediaPlayer.PlayingState) {
                root.playbackFinished = false
                root.playbackStarted()
            }
        }

        onStatusChanged: {
            if (!root.isActivePlayback)
                return

            if (root.sharedPlayer.status === MediaPlayer.EndOfMedia)
                root.playbackFinished = true
        }

        onError: {
            if (!root.isActivePlayback)
                return

            if (error !== MediaPlayer.NoError) {
                root.errorMessage =
                    errorString || "Video playback failed"
            }
        }
    }

    Timer {
        id: embeddedControlsTimer

        interval: 3500
        repeat: false

        onTriggered: {
            if (root.displayedPlaying)
                root.controlsVisible = false
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: units.gu(1)
        color: root.isActivePlayback ? "transparent" : "#080d12"
        clip: true

        // RoomView reparents its single VideoOutput into this host when
        // this message becomes the active video.
        Item {
            id: videoOutputHost
            anchors.fill: parent
        }

        Image {
            anchors.fill: parent
            source: root.thumbnailUrl
            visible: root.videoUrl === "" && source !== ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
        }

        MouseArea {
            anchors.fill: parent
            z: 1

            property real swipePressX: 0
            property real swipePressY: 0
            property bool swipeClaimed: false

            onPressed: {
                swipePressX = mouse.x
                swipePressY = mouse.y
                swipeClaimed = false
                root.swipeStarted()
            }

            onPositionChanged: {
                if (!pressed)
                    return

                var dx = mouse.x - swipePressX
                var dy = mouse.y - swipePressY

                if (!swipeClaimed &&
                    Math.abs(dx) > units.gu(0.65) &&
                    Math.abs(dx) > Math.abs(dy) * 1.05) {
                    swipeClaimed = true
                    preventStealing = true
                }

                if (swipeClaimed)
                    root.swipeMoved(dx, dy)
            }

            onReleased: {
                if (swipeClaimed)
                    root.swipeEnded(false)

                preventStealing = false
            }

            onCanceled: {
                if (swipeClaimed)
                    root.swipeEnded(true)

                swipeClaimed = false
                preventStealing = false
            }

            onDoubleClicked: {
                if (!swipeClaimed)
                    root.requestFullscreen()
            }

            onClicked: {
                if (!swipeClaimed) {
                    root.revealControls()
                    root.togglePlayback()
                }

                swipeClaimed = false
            }
        }

        Rectangle {
            anchors.centerIn: parent
            z: 2

            width: units.gu(9)
            height: units.gu(9)
            radius: width / 2

            color: "#bb263744"

            visible: !root.downloading &&
                     root.controlsVisible &&
                     (root.videoUrl === "" ||
                      !root.displayedPlaying)

            Canvas {
                anchors.centerIn: parent
                width: units.gu(3.5)
                height: units.gu(4)
                visible: !root.playbackFinished

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
                text: "↻"
                color: "#111111"
                font.pixelSize: units.gu(4)
                visible: root.playbackFinished
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.togglePlayback()
            }
        }

        ActivityIndicator {
            anchors.centerIn: parent
            z: 4

            running: root.downloading ||
                     (root.isActivePlayback &&
                      root.sharedPlayer &&
                      (root.sharedPlayer.status === MediaPlayer.LoadingMedia ||
                       root.sharedPlayer.status === MediaPlayer.BufferingMedia ||
                       root.sharedPlayer.status === MediaPlayer.StalledMedia))
            visible: running
        }

        Text {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                margins: units.gu(1)
            }

            z: 5

            visible: root.errorMessage !== ""
            text: root.errorMessage

            color: "#d6aaff"
            font.pixelSize: units.gu(1.5)
            wrapMode: Text.Wrap
        }

        // Embedded controls belong only to the message currently
        // displaying the shared player.
        Rectangle {
            id: embeddedPlaybackControls

            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                margins: units.gu(1)
            }

            height: units.gu(9)
            radius: units.gu(1)
            z: 7
            color: "#dd101820"

            visible: root.isActivePlayback &&
                     root.videoUrl !== "" &&
                     !root.downloading &&
                     root.controlsVisible

            Column {
                anchors.fill: parent
                anchors.margins: units.gu(1)
                spacing: units.gu(0.5)

                Item {
                    id: embeddedSeekTrack

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
                        width: root.displayedDuration > 0
                               ? parent.width * root.displayedPosition /
                                 root.displayedDuration
                               : 0
                        height: units.gu(0.6)
                        radius: height / 2
                        color: "#55b8e8"
                    }

                    MouseArea {
                        anchors.fill: parent

                        onPressed: {
                            root.seekVideo(mouse.x, width)
                        }

                        onPositionChanged: {
                            if (pressed)
                                root.seekVideo(mouse.x, width)
                        }
                    }
                }

                Row {
                    width: parent.width
                    height: units.gu(5)
                    spacing: units.gu(1)

                    Rectangle {
                        width: units.gu(5)
                        height: units.gu(5)
                        radius: width / 2
                        color: "#bb263744"

                        Canvas {
                            anchors.centerIn: parent
                            width: units.gu(2)
                            height: units.gu(2.5)
                            visible: !root.displayedPlaying

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
                            font.pixelSize: units.gu(2.5)
                            visible: root.displayedPlaying
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.togglePlayback()
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter

                        text: root.formatVideoTime(root.displayedPosition) +
                              " / " +
                              root.formatVideoTime(root.displayedDuration)

                        color: "#ffffff"
                        font.pixelSize: units.gu(1.5)
                    }

                    Item {
                        width: Math.max(
                            0,
                            parent.width - units.gu(25)
                        )
                        height: 1
                    }

                    Rectangle {
                        width: units.gu(5)
                        height: units.gu(5)
                        radius: width / 2
                        color: "#245b78"

                        Text {
                            anchors.centerIn: parent
                            text: "↓"
                            color: "#ffffff"
                            font.pixelSize: units.gu(3)
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.videoUrl !== "" &&
                                     !root.exporting
                            onClicked: root.exportVideo()
                        }
                    }

                    Rectangle {
                        width: units.gu(5)
                        height: units.gu(5)
                        radius: width / 2
                        color: "#245b78"

                        Text {
                            anchors.centerIn: parent
                            text: "⛶"
                            color: "#ffffff"
                            font.pixelSize: units.gu(2.5)
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.requestFullscreen()
                        }
                    }
                }
            }
        }
    }

    // =====================================
    // Content Hub video export
    // =====================================

    ContentPeerPicker {
        id: videoExportPeerPicker

        parent: root.fullscreenHost && root.fullscreenHost.visible
                ? root.fullscreenHost
                : (root.fullscreenHost ? root.fullscreenHost.parent : root)
        anchors.fill: parent
        z: 20
        visible: root.destinationPickerOpen

        contentType: ContentType.All
        handler: ContentHandler.Destination

        onPeerSelected: {
            peer.selectionType = ContentTransfer.Single
            root.activeTransfer = peer.request()

            if (!root.activeTransfer) {
                root.exportStatus = "Could not start file transfer"
                root.exporting = false
                root.destinationPickerOpen = false
                root.pendingExportUrl = ""
                return
            }

            root.exportStatus = "Preparing video transfer…"
        }

        onCancelPressed: {
            root.destinationPickerOpen = false
            root.exporting = false
            root.pendingExportUrl = ""
            root.exportStatus = ""
        }
    }

    Component {
        id: videoExportContentItemComponent
        ContentItem {}
    }

    Connections {
        target: root.activeTransfer

        onStateChanged: {
            var transfer = root.activeTransfer

            if (!transfer)
                return

            if (transfer.state === ContentTransfer.InProgress) {
                var item = videoExportContentItemComponent.createObject(
                    root, { "url": root.pendingExportUrl }
                )

                if (!item) {
                    root.exportStatus = "Could not prepare the video"
                    transfer.state = ContentTransfer.Aborted
                    return
                }

                transfer.items = [item]
                transfer.state = ContentTransfer.Charged
                root.exportStatus = "Sending video to Files…"
            }
            else if (transfer.state === ContentTransfer.Collected) {
                console.log("Astro: Video Content Hub transfer collected")
                console.log("Astro: Waiting for Files to finish handling the video")
                root.exportStatus = "Waiting for Files to finish saving the video…"
            }
            else if (transfer.state === ContentTransfer.Aborted) {
                transfer.finalize()
                root.activeTransfer = null
                root.exporting = false
                root.pendingExportUrl = ""
                root.destinationPickerOpen = false
                root.exportStatus = "Video transfer failed or was cancelled"
            }
        }
    }

    ContentTransferHint {
        parent: root.fullscreenHost && root.fullscreenHost.visible
                ? root.fullscreenHost
                : (root.fullscreenHost ? root.fullscreenHost.parent : root)
        anchors.fill: parent
        z: 30
        activeTransfer: root.activeTransfer
    }

    Text {
        parent: root.fullscreenHost && root.fullscreenHost.visible
                ? root.fullscreenHost
                : (root.fullscreenHost ? root.fullscreenHost.parent : root)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: units.gu(18)
        z: 31
        visible: root.exportStatus !== ""
        text: root.exportStatus
        color: "white"
        font.pixelSize: units.gu(1.7)
    }

    Component.onDestruction: {
        // Never stop the shared player here. RoomView decides whether
        // this component still owns the active playback session.
        root.playbackReleased(
            root.requestKey,
            videoOutputHost,
            root
        )
    }
}
