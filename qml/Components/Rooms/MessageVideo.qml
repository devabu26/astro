import QtQuick 2.7
import QtMultimedia 5.8
import Lomiri.Components 1.3

Item {
    id: root

    property string requestKey: ""
    property string mediaSourceJson: ""
    property string mimeType: ""
    property string thumbnailUrl: ""
    property var backend
    property var fullscreenHost

    signal fullscreenRequested(var output, var originalParent, var mediaPlayer)

    property string videoUrl: ""
    property bool downloading: false
    property bool playbackFinished: false
    property string errorMessage: ""
    property bool controlsVisible: true

    signal playbackStarted()

    width: parent ? parent.width : units.gu(40)
    height: units.gu(25)

    function requestVideo() {
        if (root.videoUrl !== "") {
            if (root.playbackFinished) {
                player.seek(0)
                root.playbackFinished = false
            }

            root.errorMessage = ""
            player.play()
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
        if (player.duration <= 0 || trackWidth <= 0)
            return

        var fraction = Math.max(0, Math.min(1, mouseX / trackWidth))
        player.seek(fraction * player.duration)
        root.revealControls()
    }

    function togglePlayback() {
        if (root.downloading)
            return

        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause()
        } else {
            root.requestVideo()
        }

        root.revealControls()
    }

    function requestFullscreen() {
        if (!root.fullscreenHost)
            return

        root.fullscreenRequested(videoOutput, videoOutput.parent, player)
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
        player.source = fileUrl
        player.play()
    }

    function handleVideoFailed(key, error) {
        if (key !== root.requestKey)
            return

        root.downloading = false
        root.errorMessage = error || "Video download failed"
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

    Timer {
        id: embeddedControlsTimer

        interval: 3500
        repeat: false

        onTriggered: {
            if (player.playbackState === MediaPlayer.PlayingState)
                root.controlsVisible = false
        }
    }

    MediaPlayer {
        id: player

        autoPlay: false

        onPlaybackStateChanged: {
            if (player.playbackState === MediaPlayer.PlayingState) {
                root.playbackFinished = false
                root.playbackStarted()
            }
        }

        onStatusChanged: {
            if (player.status === MediaPlayer.EndOfMedia) {
                root.playbackFinished = true
            }
        }

        onError: {
            if (error !== MediaPlayer.NoError) {
                root.errorMessage =
                    errorString || "Video playback failed"
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: units.gu(1)
        color: "#080d12"
        clip: true

        VideoOutput {
            id: videoOutput

            anchors.fill: parent
            source: player
            fillMode: VideoOutput.PreserveAspectFit
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

            onClicked: {
                root.revealControls()
            }
        }

        Rectangle {
            anchors.centerIn: parent
            z: 2

            width: units.gu(9)
            height: units.gu(9)
            radius: width / 2

            color: "#ffffff"

            visible: !root.downloading &&
                     root.controlsVisible &&
                     (root.videoUrl === "" ||
                      player.playbackState !== MediaPlayer.PlayingState)

            Canvas {
                anchors.centerIn: parent
                width: units.gu(3.5)
                height: units.gu(4)
                visible: !root.playbackFinished

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.fillStyle = "#111111"
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
                     player.status === MediaPlayer.LoadingMedia ||
                     player.status === MediaPlayer.BufferingMedia ||
                     player.status === MediaPlayer.StalledMedia
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

        // Embedded playback controls.
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

            visible: root.videoUrl !== "" &&
                     !root.downloading &&
                     root.controlsVisible

            Column {
                anchors.fill: parent
                anchors.margins: units.gu(1)
                spacing: units.gu(0.5)

                // Seek bar.
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
                        width: player.duration > 0
                               ? parent.width * player.position /
                                 player.duration
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

                // Playback buttons and time.
                Row {
                    width: parent.width
                    height: units.gu(5)
                    spacing: units.gu(1)

                    Rectangle {
                        width: units.gu(5)
                        height: units.gu(5)
                        radius: width / 2
                        color: "#ffffff"

                        Canvas {
                            anchors.centerIn: parent
                            width: units.gu(2)
                            height: units.gu(2.5)
                            visible: player.playbackState !==
                                     MediaPlayer.PlayingState

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.fillStyle = "#111111"
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
                            color: "#111111"
                            font.pixelSize: units.gu(2.5)
                            visible: player.playbackState ===
                                     MediaPlayer.PlayingState
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.togglePlayback()
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter

                        text: root.formatVideoTime(player.position) +
                              " / " +
                              root.formatVideoTime(player.duration)

                        color: "#ffffff"
                        font.pixelSize: units.gu(1.5)
                    }

                    Item {
                        width: Math.max(
                            0,
                            parent.width - units.gu(17)
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

    Component.onDestruction: {
        player.stop()
    }
}