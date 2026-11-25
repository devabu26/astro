
import QtQuick 2.7
import QtMultimedia 5.12
import Lomiri.Components 1.3
import Astro.Backend 1.0

Item {
    id: root

    property var backend
    property string roomId: ""
    property string roomName: ""
    property var audioState: ({})

    property string pendingOperation: ""
    property string actionError: ""

    property bool audioStarted: false
    property bool membershipJoined: false
    property bool muted: false
    property bool pendingMuted: false
    property bool videoStarted: false
    property bool screenShareStarted: false
    property string screenShareRoomId: ""
    property bool cameraFrameGrabPending: false

    signal backRequested()
    signal remoteVideoCleared(string identity, string source)

    readonly property var participants:
        audioState.participants || []

    readonly property bool callActive:
        audioState.active === true || audioStarted

    function startCall() {
        if (!backend || !roomId || pendingOperation.length > 0)
            return

        actionError = ""

        if (audioState.active === true) {
            audioStarted = true
            membershipJoined = true
            return
        }

        pendingOperation = "join"
        backend.joinRtcRoom(roomId)
    }

    function startAudio() {
        if (!backend || !roomId || pendingOperation.length > 0)
            return

        pendingOperation = "start"
        backend.startRtcAudio(roomId)
    }

    function toggleMute() {
        if (!backend || !audioStarted ||
                pendingOperation.length > 0)
            return

        actionError = ""
        pendingMuted = !muted
        pendingOperation = "mute"
        backend.setRtcAudioMuted(roomId, pendingMuted)
    }

    function toggleCamera() {
        if (!backend || !roomId ||
                pendingOperation.length > 0 ||
                !callActive)
            return

        actionError = ""

        if (videoStarted) {
            localCamera.stop()
            pendingOperation = "stopVideo"
            backend.stopRtcVideo(roomId)
        } else {
            if (!QtMultimedia.availableCameras ||
                    QtMultimedia.availableCameras.length === 0) {
                actionError = "No camera is available"
                return
            }

            pendingOperation = "startVideo"
            backend.startRtcVideo(roomId, 360, 480)
        }
    }

    function toggleScreenShare() {
        if (!backend || !roomId ||
                pendingOperation.length > 0 || !callActive)
            return

        actionError = ""
        if (screenShareStarted) {
            pendingOperation = "stopScreenShare"
            backend.stopRtcScreenShare(roomId)
        } else {
            screenShareRoomId = roomId
            pendingOperation = "startScreenShare"
            backend.startRtcScreenShare(roomId, 480, 854)
        }
    }

    function switchCamera() {
        if (!videoStarted || pendingOperation.length > 0)
            return

        var cameras = QtMultimedia.availableCameras
        if (!cameras || cameras.length < 2) {
            actionError = "Only one camera is available"
            return
        }

        var currentIndex = -1
        for (var i = 0; i < cameras.length; ++i) {
            if (cameras[i].deviceId === localCamera.deviceId) {
                currentIndex = i
                break
            }
        }

        if (currentIndex < 0)
            currentIndex = 0

        var nextIndex = (currentIndex + 1) % cameras.length
        localCamera.stop()
        localCamera.deviceId = cameras[nextIndex].deviceId
        Qt.callLater(function() {
            localCamera.start()
        })
        actionError = ""
    }

    function hangUp() {
        if (!backend || !roomId ||
                pendingOperation.length > 0)
            return

        actionError = ""

        if (screenShareStarted) {
            pendingOperation = "stopScreenShareForHangup"
            backend.stopRtcScreenShare(roomId)
            return
        }

        if (videoStarted) {
            localCamera.stop()
            pendingOperation = "stopVideoForHangup"
            backend.stopRtcVideo(roomId)
            return
        }

        stopAudioOrLeave()
    }

    function stopAudioOrLeave() {
        if (!backend || !roomId ||
                pendingOperation.length > 0)
            return

        if (audioStarted || audioState.active === true) {
            pendingOperation = "stop"
            backend.stopRtcAudio(roomId)
        } else {
            leaveMembership()
        }
    }

    function leaveMembership() {
        if (!backend || !roomId ||
                pendingOperation.length > 0)
            return

        pendingOperation = "leave"
        backend.leaveRtcRoom(roomId)
    }

    function statusText() {
        if (pendingOperation === "join")
            return "Joining call..."

        if (pendingOperation === "start")
            return "Connecting audio..."

        if (pendingOperation === "mute")
            return pendingMuted
                    ? "Muting microphone..."
                    : "Unmuting microphone..."

        if (pendingOperation === "startVideo")
            return "Starting camera..."

        if (pendingOperation === "startScreenShare")
            return "Starting screen share..."

        if (pendingOperation === "stopScreenShare")
            return "Stopping screen share..."

        if (pendingOperation === "stopScreenShareForHangup")
            return "Stopping screen share before leaving..."

        if (pendingOperation === "stopVideo")
            return "Stopping camera..."

        if (pendingOperation === "switchCamera")
            return "Switching camera..."

        if (pendingOperation === "stopVideoForHangup")
            return "Stopping camera before leaving..."

        if (pendingOperation === "stop")
            return "Disconnecting audio..."

        if (pendingOperation === "leave")
            return "Leaving call..."

        if (audioState.reconnecting === true)
            return "Reconnecting..."

        if (audioState.active === true)
            return "Connected"

        if (audioStarted)
            return "Connecting..."

        if (membershipJoined)
            return "Call presence joined"

        return "Not connected"
    }

    function participantInitial(name) {
        name = String(name || "P").trim()
        return name.length > 0
                ? name.charAt(0).toUpperCase()
                : "P"
    }

    Timer {
        id: cameraFrameGrabTimer
        interval: 33
        repeat: true
        running: root.videoStarted

        onTriggered: {
            if (root.cameraFrameGrabPending ||
                    !localCameraPreview.visible ||
                    !root.backend ||
                    !root.roomId)
                return

            root.cameraFrameGrabPending = true
            var accepted = localCameraPreview.grabToImage(function(result) {
                root.cameraFrameGrabPending = false
                if (result && root.videoStarted && root.backend)
                    root.backend.processRtcCameraImage(
                        root.roomId,
                        result.image
                    )
            }, Qt.size(360, 480))

            if (!accepted)
                root.cameraFrameGrabPending = false
        }
    }

    onVideoStartedChanged: {
        if (videoStarted)
            localCamera.start()
        else
            localCamera.stop()
    }

    Component.onDestruction: {
        if (backend && screenShareRoomId.length > 0)
            backend.stopRtcScreenShare(screenShareRoomId)
    }

    onAudioStateChanged: {
        if (audioState.active === true) {
            audioStarted = true
            membershipJoined = true
        } else if (audioState.connectionState === "Disconnected" &&
                   pendingOperation.length === 0) {
            if (backend && screenShareRoomId.length > 0)
                backend.stopRtcScreenShare(screenShareRoomId)
            audioStarted = false
            membershipJoined = false
            videoStarted = false
            screenShareStarted = false
            screenShareRoomId = ""
            muted = false
            for (var i = 0; i < participants.length; ++i) {
                remoteVideoCleared(participants[i].identity, "camera")
                remoteVideoCleared(participants[i].identity, "screenshare")
            }
        }
    }

    onRoomIdChanged: {
        if (backend && screenShareRoomId.length > 0)
            backend.stopRtcScreenShare(screenShareRoomId)
        screenShareRoomId = ""

        for (var i = 0; i < participants.length; ++i) {
            remoteVideoCleared(participants[i].identity, "camera")
            remoteVideoCleared(participants[i].identity, "screenshare")
        }

        actionError = ""
        pendingOperation = ""

        videoStarted = false
        screenShareStarted = false

        if (audioState.active === true) {
            audioStarted = true
            membershipJoined = true
        } else {
            audioStarted = false
            membershipJoined = false
            muted = false
        }
    }

    Connections {
        target: root.backend
        ignoreUnknownSignals: true

        onMatrixRtcActionFinished: {
            if (roomId !== root.roomId)
                return

            if (root.pendingOperation === "join") {
                root.pendingOperation = ""

                if (error && error.length > 0) {
                    root.actionError = error
                    return
                }

                root.membershipJoined = true
                root.startAudio()

            } else if (root.pendingOperation === "leave") {
                root.pendingOperation = ""

                if (error && error.length > 0) {
                    root.actionError = error
                    return
                }

                root.membershipJoined = false
                root.audioStarted = false
                root.videoStarted = false
                root.screenShareStarted = false
                root.screenShareRoomId = ""
                root.muted = false
                root.actionError = ""
                root.backRequested()
            }
        }

        onMatrixRtcAudioActionFinished: {
            if (roomId !== root.roomId)
                return

            var operation = root.pendingOperation
            root.pendingOperation = ""

            if (error && error.length > 0) {
                root.actionError = error
                return
            }

            root.actionError = ""

            if (operation === "start") {
                root.audioStarted = true
                root.muted = false

            } else if (operation === "mute") {
                root.muted = root.pendingMuted

            } else if (operation === "stop") {
                root.audioStarted = false
                root.muted = false
                root.leaveMembership()
            }
        }

        onMatrixRtcVideoActionFinished: {
            if (roomId !== root.roomId)
                return

            var operation = root.pendingOperation
            root.pendingOperation = ""

            if (error && error.length > 0) {
                root.actionError = error
                return
            }

            root.actionError = ""

            if (operation === "startVideo") {
                root.videoStarted = true

            } else if (operation === "stopVideo") {
                root.videoStarted = false

            } else if (operation === "switchCamera") {
                root.videoStarted = true

            } else if (operation === "stopVideoForHangup") {
                root.videoStarted = false
                root.stopAudioOrLeave()
            }
        }
    }

    Connections {
        target: root.backend
        ignoreUnknownSignals: true

        onMatrixRtcScreenShareActionFinished: {
            if (roomId !== root.roomId)
                return

            var operation = root.pendingOperation
            root.pendingOperation = ""

            if (error && error.length > 0) {
                root.actionError = error
                if (operation === "startScreenShare") {
                    root.screenShareStarted = false
                    root.screenShareRoomId = ""
                } else if (operation === "stopScreenShare" ||
                           operation === "stopScreenShareForHangup") {
                    root.screenShareStarted = true
                } else {
                    root.screenShareStarted = false
                    root.screenShareRoomId = ""
                }
                return
            }

            root.actionError = ""

            if (operation === "startScreenShare") {
                root.screenShareStarted = true
            } else if (operation === "stopScreenShare") {
                root.screenShareStarted = false
                root.screenShareRoomId = ""
            } else if (operation === "stopScreenShareForHangup") {
                root.screenShareStarted = false
                root.screenShareRoomId = ""
                if (root.videoStarted) {
                    localCamera.stop()
                    root.pendingOperation = "stopVideoForHangup"
                    root.backend.stopRtcVideo(root.roomId)
                } else {
                    root.stopAudioOrLeave()
                }
            }
        }

        onMatrixRtcAudioEventsUpdated: {
            try {
                var payload = JSON.parse(json)
                var events = payload.events || []
                for (var i = 0; i < events.length; ++i) {
                    var event = events[i]
                    if (event.room_id !== root.roomId)
                        continue

                    if (event.type === "track_unsubscribed") {
                        if (event.is_camera === true)
                            root.remoteVideoCleared(event.identity, "camera")
                        if (event.is_screenshare === true)
                            root.remoteVideoCleared(event.identity, "screenshare")
                    } else if (event.type === "participant_disconnected") {
                        root.remoteVideoCleared(event.identity, "camera")
                        root.remoteVideoCleared(event.identity, "screenshare")
                    }
                }
            } catch (error) {
                console.warn("Could not process LiveKit video cleanup:", error)
            }
        }
    }

    Camera {
        id: localCamera
        deviceId: QtMultimedia.defaultCamera.deviceId.length > 0
                 ? QtMultimedia.defaultCamera.deviceId
                 : (QtMultimedia.availableCameras.length > 0
                    ? QtMultimedia.availableCameras[0].deviceId : "")
        captureMode: Camera.CaptureVideo
        viewfinder.maximumFrameRate: 30

        onError: {
            root.actionError = errorString
            console.warn("ASTRO RTC camera error:", errorString)
        }
    }

    VideoOutput {
        id: localCameraPreview
        z: 50
        x: parent.width - width - units.gu(2)
        y: units.gu(10)
        width: units.gu(20)
        height: width * 4 / 3
        visible: root.videoStarted
        source: localCamera
        fillMode: VideoOutput.PreserveAspectCrop
        orientation: 270

        // Drag the local camera preview anywhere within the call page.
        MouseArea {
            anchors.fill: parent
            drag.target: localCameraPreview
            drag.axis: Drag.XAndYAxis
            drag.minimumX: 0
            drag.maximumX: Math.max(0, root.width - localCameraPreview.width)
            drag.minimumY: 0
            drag.maximumY: Math.max(0, root.height - localCameraPreview.height)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#101820"
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            width: parent.width
            height: units.gu(8)
            color: "#172530"

            Row {
                anchors.fill: parent
                anchors.leftMargin: units.gu(2)
                anchors.rightMargin: units.gu(2)
                spacing: units.gu(2)

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: units.gu(5)
                    height: units.gu(5)
                    radius: units.gu(1)
                    color: "#263b4a"

                    Text {
                        anchors.centerIn: parent
                        text: "‹"
                        color: "white"
                        font.pixelSize: units.gu(4)
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (root.pendingOperation.length > 0)
                                return

                            if (root.callActive ||
                                root.membershipJoined ||
                                root.videoStarted) {
                                root.hangUp()
                            } else {
                                root.backRequested()
                            }
                        }
                    }
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - units.gu(12)
                    spacing: units.gu(0.3)

                    Text {
                        width: parent.width
                        text: root.roomName || "Voice call"
                        color: "white"
                        font.pixelSize: units.gu(2)
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: "MatrixRTC"
                        color: "#91a4b5"
                        font.pixelSize: units.gu(1.3)
                    }
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: units.gu(2)
                    height: units.gu(2)
                    radius: width / 2
                    color: root.callActive ? "#52c78a" : "#71808c"
                }
            }
        }

        // Main call stage
        Rectangle {
            width: parent.width
            height: parent.height -
                    units.gu(8) -
                    units.gu(19)
            color: "#101820"

            Column {
                anchors.centerIn: parent
                width: parent.width - units.gu(4)
                spacing: units.gu(2)

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: units.gu(15)
                    height: units.gu(15)
                    radius: width / 2
                    color: "#263f52"
                    visible: root.participants.length === 0

                    Text {
                        anchors.centerIn: parent
                        text: "A"
                        color: "#d8e7f1"
                        font.pixelSize: units.gu(6)
                        font.bold: true
                    }
                }

                Text {
                    width: parent.width
                    text: root.participants.length === 0
                          ? (root.roomName || "Your call")
                          : root.participants.length +
                            (root.participants.length === 1
                             ? " participant"
                             : " participants")
                    color: "white"
                    font.pixelSize: units.gu(2.4)
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: root.participants.length === 0
                }

                Text {
                    width: parent.width
                    text: root.statusText()
                    color: root.callActive ? "#8de0ad" : "#a9bac6"
                    font.pixelSize: units.gu(1.7)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                Text {
                    width: parent.width
                    visible: root.videoStarted
                    text: "Camera is publishing"
                    color: "#8de0ad"
                    font.pixelSize: units.gu(1.4)
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    width: parent.width
                    visible: root.actionError.length > 0
                    text: root.actionError
                    color: "#ffaaa5"
                    font.pixelSize: units.gu(1.4)
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // Participant tiles
            Grid {
                anchors.fill: parent
                anchors.margins: root.participants.length === 1
                                ? 0 : units.gu(2)

                columns: root.participants.length === 1
                        ? 1 : (width >= units.gu(65) ? 3 : 2)

                spacing: units.gu(1)

                Repeater {
                    model: root.participants

                    delegate: Rectangle {
                        width: root.participants.length === 1
                            ? parent.width
                            : (parent.width -
                                (parent.columns - 1) * units.gu(1)) /
                                parent.columns

                        height: root.participants.length === 1
                                ? parent.height
                                : (remoteVideo.hasFrame ||
                                   remoteScreenVideo.hasFrame
                                ? width * 9 / 16
                                : units.gu(10))

                        radius: root.participants.length === 1
                                ? 0 : units.gu(1)

                        clip: true
                        color: modelData.speaking === true
                            ? "#294c43" : "#263b4a"

                        border.width: root.participants.length === 1
                                    ? 0
                                    : (modelData.speaking === true
                                        ? units.gu(0.2) : 0)
                        border.color: "#65d69b"

                        RtcVideoItem {
                            id: remoteScreenVideo
                            anchors.fill: parent
                            z: 1
                            backend: root.backend
                            roomId: root.roomId
                            identity: modelData.identity || ""
                            source: "screenshare"
                            visible: hasFrame
                        }

                        RtcVideoItem {
                            id: remoteVideo
                            anchors.fill: remoteScreenVideo.hasFrame ? null : parent
                            anchors.right: remoteScreenVideo.hasFrame
                                           ? parent.right : undefined
                            anchors.top: remoteScreenVideo.hasFrame
                                         ? parent.top : undefined
                            anchors.margins: remoteScreenVideo.hasFrame
                                               ? units.gu(1) : 0
                            width: remoteScreenVideo.hasFrame
                                   ? units.gu(18) : parent.width
                            height: remoteScreenVideo.hasFrame
                                    ? width * 4 / 3 : parent.height
                            z: 2
                            backend: root.backend
                            roomId: root.roomId
                            identity: modelData.identity || ""
                            source: "camera"
                            visible: hasFrame
                        }

                        Connections {
                            target: root

                            onRemoteVideoCleared: {
                                if (identity !== modelData.identity)
                                    return
                                if (source === "camera")
                                    remoteVideo.clearFrame()
                                else if (source === "screenshare")
                                    remoteScreenVideo.clearFrame()
                            }
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: units.gu(2)
                            visible: remoteVideo.hasFrame ||
                                     remoteScreenVideo.hasFrame
                            text: remoteScreenVideo.hasFrame
                                  ? "Screen shared by " + (modelData.name || modelData.identity || "Participant")
                                  : (modelData.name ||
                                     modelData.identity ||
                                     "Participant")
                            color: "white"
                            font.pixelSize: units.gu(1.8)
                            style: Text.Outline
                            styleColor: "#80000000"
                            z: 2
                        }

                        Row {
                            anchors.centerIn: parent
                            spacing: units.gu(1)
                            visible: !remoteVideo.hasFrame &&
                                     !remoteScreenVideo.hasFrame

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: units.gu(8)
                                height: units.gu(8)
                                radius: width / 2
                                color: "#426b8a"

                                Text {
                                    anchors.centerIn: parent
                                    text: root.participantInitial(
                                            modelData.name ||
                                            modelData.identity
                                        )
                                    color: "white"
                                    font.pixelSize: units.gu(3)
                                    font.bold: true
                                }
                            }

                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: units.gu(0.5)

                                Text {
                                    text: modelData.name ||
                                        modelData.identity ||
                                        "Participant"
                                    color: "white"
                                    font.pixelSize: units.gu(2)
                                }

                                Text {
                                    text: modelData.speaking === true
                                        ? "Speaking" : "Connected"
                                    color: modelData.speaking === true
                                        ? "#8de0ad" : "#a9bac6"
                                    font.pixelSize: units.gu(1.4)
                                }
                            }
                        }
                    }
                }
            }
        }

        // Controls
        Rectangle {
            width: parent.width
            height: units.gu(19)
            color: "#172530"

            Column {
                anchors.fill: parent
                anchors.margins: units.gu(2)
                spacing: units.gu(1)

                Row {
                    width: parent.width
                    height: units.gu(8)
                    spacing: units.gu(1)

                    // Microphone
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: root.muted ? "#8a5555" : "#294c43"
                        opacity: root.callActive ? 1 : 0.4

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.muted ? "×" : "●"
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.muted ? "Muted" : "Mic"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.callActive &&
                                     root.pendingOperation.length === 0
                            onClicked: root.toggleMute()
                        }
                    }

                    // Camera
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: root.videoStarted ? "#294c43" : "#263b4a"
                        opacity: root.callActive ? 1 : 0.4

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.videoStarted ? "●" : "○"
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.videoStarted ? "Camera on" : "Camera"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.callActive &&
                                     root.pendingOperation.length === 0
                            onClicked: root.toggleCamera()
                        }
                    }

                    // Share the Ubuntu Touch display
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: root.screenShareStarted ? "#294c43" : "#263b4a"
                        opacity: root.callActive ? 1 : 0.4

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.screenShareStarted ? "●" : "▣"
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.screenShareStarted ? "Sharing" : "Share"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.callActive &&
                                     root.pendingOperation.length === 0
                            onClicked: root.toggleScreenShare()
                        }
                    }

                    // Switch between available camera devices
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: "#263b4a"
                        opacity: root.videoStarted ? 1 : 0.35

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "↔"
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Flip"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.videoStarted &&
                                     root.pendingOperation.length === 0
                            onClicked: root.switchCamera()
                        }
                    }

                    // Start audio when not connected
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: "#315d48"
                        opacity: root.callActive ? 0.35 : 1

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "▶"
                                color: "white"
                                font.pixelSize: units.gu(2.5)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Start"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !root.callActive &&
                                     root.pendingOperation.length === 0
                            onClicked: root.startCall()
                        }
                    }

                    // Hang up
                    Rectangle {
                        width: (parent.width - units.gu(5)) / 6
                        height: units.gu(7)
                        radius: units.gu(1)
                        color: "#853f43"

                        Column {
                            anchors.centerIn: parent
                            spacing: units.gu(0.4)

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "×"
                                color: "white"
                                font.pixelSize: units.gu(3)
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Leave"
                                color: "white"
                                font.pixelSize: units.gu(1.2)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: root.pendingOperation.length === 0 &&
                                     (root.callActive ||
                                      root.membershipJoined)
                            onClicked: root.hangUp()
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: root.muted
                          ? "Your microphone is muted"
                          : root.screenShareStarted
                            ? "Screen sharing and audio connected"
                            : root.videoStarted
                              ? "Audio and camera connected"
                              : root.callActive
                                ? "Audio connected"
                                : "Not in a call"
                    color: "#91a4b5"
                    font.pixelSize: units.gu(1.3)
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }
}
