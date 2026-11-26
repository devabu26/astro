
import QtQuick 2.7
import QtQuick.Window 2.2
import QtMultimedia 5.12
import QtGraphicalEffects 1.0
import Lomiri.Components 1.3
import Astro.Backend 1.0

Item {
    id: root

    property var backend
    property string roomId: ""
    property string roomName: ""
    property bool directCall: false
    property bool incomingCall: false
    property var audioState: ({})
    // Authoritative MatrixRTC membership state shared with the room view.
    property var membershipState: ({})
    // MatrixRTC membership profiles provide the MXC avatar URI for each user.
    property var memberships: []
    property var participantAvatarCache: ({})
    property var participantAvatarCacheUris: ({})
    property var participantAvatarRequests: ({})


    property string pendingOperation: ""
    property string actionError: ""

    property bool audioStarted: false
    property bool membershipJoined: false
    property bool muted: false
    property bool pendingMuted: false
    property bool videoStarted: false
    property bool screenShareStarted: false
    property string screenShareRoomId: ""
    property bool screenShareChoiceVisible: false
    property bool screenShareAudioRequested: false
    property bool cameraFrameGrabPending: false
    property bool screenShareStopDispatched: false
    property int screenShareStopDrainElapsedMs: 0
    property string screenShareStopRoomId: ""
    property bool applicationActive: Qt.application.state === Qt.ApplicationActive
    property int applicationState: backend
                                  ? backend.applicationState
                                  : Qt.application.state
    property int cameraFrameGrabAttempts: 0
    property int cameraFrameGrabCompletions: 0
    property int cameraFrameGrabFailures: 0
    property double cameraFrameGrabStartedAt: 0
    property bool waitingForForegroundCamera: false
    property bool nativeBackgroundCameraStartPending: false
    property bool nativeBackgroundCameraAttempted: false
    property bool nativeBackgroundCameraRunning: false
    property string nativeBackgroundCameraDeviceId: ""
    property bool remoteAnswered: false
    property string ringbackSource: ""
    property bool ringbackRequested: false

    signal backRequested()
    signal remoteVideoCleared(string identity, string source)

    readonly property var participants:
        audioState.participants || []

    readonly property bool callActive:
        audioState.active === true || audioStarted

    function participantProfile(identity) {
        identity = String(identity || "")
        if (!identity)
            return null

        for (var i = 0; i < memberships.length; ++i) {
            var membership = memberships[i]
            if (!membership)
                continue

            var sender = String(membership.sender || "")
            if (!sender)
                continue

            // The legacy MatrixRTC /sfu/get flow gives LiveKit the
            // identity "<matrix_user_id>:<device_id>". Membership
            // profiles are keyed by the Matrix user ID alone.
            if (identity === sender ||
                    identity.indexOf(sender + ":") === 0)
                return membership
        }

        return null
    }

    function participantAvatarMxc(identity) {
        var profile = participantProfile(identity)
        return profile && profile.avatar_url
                ? String(profile.avatar_url) : ""
    }

    function participantDisplayName(identity, fallbackName) {
        var profile = participantProfile(identity)
        return profile && profile.display_name
                ? String(profile.display_name)
                : String(fallbackName || identity || "Participant")
    }

    function participantAvatarRequestKey(identity, mxcUri) {
        return "call-avatar:" +
                encodeURIComponent(roomId) + ":" +
                encodeURIComponent(String(identity || "")) + ":" +
                encodeURIComponent(String(mxcUri || ""))
    }

    function requestParticipantAvatar(identity, mxcUri) {
        identity = String(identity || "")
        mxcUri = String(mxcUri || "")
        if (!backend || !roomId || !identity || !mxcUri)
            return

        if (participantAvatarCache[identity] &&
                participantAvatarCacheUris[identity] === mxcUri)
            return

        if (participantAvatarRequests[identity] === mxcUri)
            return

        var requests = Object.assign({}, participantAvatarRequests)
        requests[identity] = mxcUri
        participantAvatarRequests = requests

        console.info(
            "ASTRO_CALL_AVATAR: requesting",
            "identity:", identity,
            "hasMxc:", true
        )
        backend.requestSenderAvatar(
            participantAvatarRequestKey(identity, mxcUri), mxcUri
        )
    }

    function requestVisibleParticipantAvatars() {
        for (var i = 0; i < participants.length; ++i) {
            var identity = String(participants[i].identity || "")
            var mxcUri = participantAvatarMxc(identity)
            if (identity && mxcUri)
                requestParticipantAvatar(identity, mxcUri)
            else if (identity)
                console.info(
                    "ASTRO_CALL_AVATAR: no MatrixRTC profile yet",
                    "identity:", identity,
                    "membershipCount:", memberships.length
                )
        }
    }

    function finishParticipantAvatarRequest(identity, mxcUri, dataUrl) {
        var requests = Object.assign({}, participantAvatarRequests)
        if (requests[identity] === mxcUri)
            delete requests[identity]
        participantAvatarRequests = requests

        // Ignore stale responses after a room change or avatar update.
        if (roomId === "" || participantAvatarMxc(identity) !== mxcUri)
            return

        if (!dataUrl)
            return

        var cache = Object.assign({}, participantAvatarCache)
        var cacheUris = Object.assign({}, participantAvatarCacheUris)
        cache[identity] = dataUrl
        cacheUris[identity] = mxcUri
        participantAvatarCache = cache
        participantAvatarCacheUris = cacheUris

        console.info(
            "ASTRO_CALL_AVATAR: loaded",
            "identity:", identity
        )
    }

    onMembershipsChanged: requestVisibleParticipantAvatars()
    onParticipantsChanged: requestVisibleParticipantAvatars()

    onRoomIdChanged: {
        participantAvatarCache = ({})
        participantAvatarRequests = ({})
        noAnswerTimer.stop()
        root.stopRingback()
        remoteAnswered = false
        if (backend && screenShareRoomId.length > 0)
            backend.stopRtcScreenShare(screenShareRoomId)
        screenShareRoomId = ""
        screenShareAudioRequested = false
        screenShareChoiceVisible = false

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
            membershipJoined = membershipState.joinedByMe === true
            muted = false
        }
    }

    Connections {
        target: root.backend
        ignoreUnknownSignals: true

        onSenderAvatarRetrieved: {
            var parts = requestKey.substring("call-avatar:".length).split(":")
            if (parts.length !== 3)
                return

            var responseRoom = decodeURIComponent(parts[0])
            var identity = decodeURIComponent(parts[1])
            var mxcUri = decodeURIComponent(parts[2])
            if (responseRoom !== root.roomId)
                return

            root.finishParticipantAvatarRequest(identity, mxcUri, dataUrl)
        }

        onSenderAvatarFailed: {
            var parts = requestKey.substring("call-avatar:".length).split(":")
            if (parts.length !== 3)
                return

            var responseRoom = decodeURIComponent(parts[0])
            var identity = decodeURIComponent(parts[1])
            var mxcUri = decodeURIComponent(parts[2])
            if (responseRoom !== root.roomId)
                return

            var requests = Object.assign({}, root.participantAvatarRequests)
            if (requests[identity] === mxcUri) {
                delete requests[identity]
                root.participantAvatarRequests = requests
            }
            console.warn(
                "ASTRO_CALL_AVATAR: download failed",
                "identity:", identity,
                "error:", error
            )
        }
    }

    function startCall() {
        if (!backend || !roomId || pendingOperation.length > 0)
            return

        actionError = ""

        if (audioState.active === true) {
            audioStarted = true
            membershipJoined = true
            return
        }

        remoteAnswered = false

        // Ringback belongs to the outgoing call attempt, not to the later
        // LiveKit audio-start callback. Start it before joining so the caller
        // hears feedback while MatrixRTC and LiveKit are connecting.
        if (directCall && !incomingCall) {
            startRingback()
        }

        if (membershipJoined) {
            // The room participant control may already have joined MatrixRTC.
            // Reuse that membership instead of issuing a duplicate join.
            startAudio()
        } else {
            pendingOperation = "join"
            if (incomingCall)
                backend.acceptRtcCall(roomId)
            else
                backend.joinRtcRoom(roomId)
        }
    }

    function stopAfterExternalMembershipLeave() {
        if (!backend || !roomId)
            return

        // The room participant panel can leave MatrixRTC independently of
        // this page. Once the shared membership snapshot confirms that leave,
        // close the associated LiveKit session and clear all local media UI.
        noAnswerTimer.stop()
        root.stopRingback()
        localCamera.stop()
        waitingForForegroundCamera = false

        if (screenShareRoomId === roomId)
            backend.stopRtcScreenShare(roomId)

        if (audioStarted || audioState.active === true ||
                videoStarted || screenShareStarted) {
            pendingOperation = "stopAfterExternalLeave"
            backend.stopRtcAudio(roomId)
        } else {
            membershipJoined = false
            audioStarted = false
            videoStarted = false
            screenShareStarted = false
            screenShareAudioRequested = false
            screenShareRoomId = ""
            muted = false
            actionError = ""
            backRequested()
        }
    }

    onMembershipStateChanged: {
        var joined = membershipState.joinedByMe === true

        // Membership can also be changed from the room participant panel.
        // Do not overwrite an in-flight local join/leave operation.
        if (pendingOperation === "join" || pendingOperation === "leave" ||
                pendingOperation === "stopAfterExternalLeave")
            return

        if (membershipJoined && !joined) {
            membershipJoined = false
            stopAfterExternalMembershipLeave()
        } else {
            membershipJoined = joined
        }
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

            // Reflect the local stop immediately. Before releasing Mir's
            // screencast buffers, let an outstanding Qt Quick camera
            // grabToImage() finish (or time out below). This avoids tearing
            // down graphics resources while the scenegraph is reading back
            // the local camera preview.
            screenShareStopRoomId = roomId
            screenShareStopDispatched = false
            screenShareStopDrainElapsedMs = 0
            screenShareStarted = false
            screenShareAudioRequested = false
            screenShareRoomId = ""

            if (cameraFrameGrabPending)
                screenShareStopDrainTimer.start()
            else
                dispatchScreenShareStop()
        } else {
            screenShareChoiceVisible = true
        }
    }

    function dispatchScreenShareStop() {
        if (screenShareStopDispatched ||
                pendingOperation !== "stopScreenShare" ||
                !backend || !screenShareStopRoomId)
            return

        screenShareStopDispatched = true
        screenShareStopDrainTimer.stop()
        console.info(
            "ASTRO_SCREEN_SHARE_DIAG: dispatching stop after camera grab drain",
            "waitedMs:", screenShareStopDrainElapsedMs,
            "cameraGrabPending:", cameraFrameGrabPending
        )
        // Dispatch from the QML/application thread after the camera
        // readback has drained. Do not wait for a Qt Quick render-stage
        // callback here: screen-share teardown must not depend on the
        // scenegraph render loop making progress.
        backend.stopRtcScreenShare(screenShareStopRoomId)
    }

    function beginScreenShare(includeDeviceAudio) {
        if (!backend || !roomId || pendingOperation.length > 0)
            return

        screenShareChoiceVisible = false
        screenShareRoomId = roomId
        screenShareAudioRequested = includeDeviceAudio
        pendingOperation = "startScreenShare"
        backend.startRtcScreenShare(roomId, 480, 854, includeDeviceAudio)
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
        noAnswerTimer.stop()
        root.stopRingback()
        screenShareChoiceVisible = false

        // Stop local capture immediately, but do not make leaving depend on
        // separately unpublishing camera or screen-share tracks. Closing the
        // LiveKit room below tears down every published track in one operation.
        localCamera.stop()
        stopAudioOrLeave()
    }

    function stopAudioOrLeave() {
        if (!backend || !roomId ||
                pendingOperation.length > 0)
            return

        if (audioStarted || audioState.active === true ||
                screenShareStarted || videoStarted) {
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

    function stopRingback() {
        ringbackRequested = false
        ringbackAudio.stop()
    }

    function startRingback(restartPlayback) {
        if (!directCall || remoteAnswered)
            return

        if (ringbackSource.length === 0 && backend)
            ringbackSource = backend.rtcRingbackToneUrl()

        if (ringbackSource.length === 0) {
            console.warn("ASTRO_CALL_RINGBACK: no ringback audio source is available")
            return
        }

        ringbackRequested = true
        if (restartPlayback === true)
            root.stopRingback()

        console.info(
            "ASTRO_CALL_RINGBACK: requested",
            "source:", ringbackSource,
            "audioStatus:", ringbackAudio.status,
            "playbackState:", ringbackAudio.playbackState,
            "restart:", restartPlayback === true
        )

        if ((ringbackAudio.status === Audio.Loaded ||
             ringbackAudio.status === Audio.Buffered) &&
                ringbackAudio.playbackState !== Audio.PlayingState) {
            ringbackAudio.play()
        }

        if (!noAnswerTimer.running)
            noAnswerTimer.restart()
    }

    Audio {
        id: ringbackAudio
        // Prefer the ringtone route when advertised; fall back to the
        // generic music role on Ubuntu Touch backends with limited roles.
        audioRole: supportedAudioRoles().indexOf(Audio.RingtoneRole) >= 0
                   ? Audio.RingtoneRole : Audio.MusicRole
        source: root.ringbackSource
        autoLoad: true
        loops: Audio.Infinite
        volume: 1.0

        onStatusChanged: {
            console.info(
                "ASTRO_CALL_RINGBACK: audio status",
                status,
                "source:", source,
                "hasAudio:", hasAudio,
                "duration:", duration,
                "requested:", root.ringbackRequested
            )

            if (root.ringbackRequested &&
                    (status === Audio.Loaded || status === Audio.Buffered) &&
                    playbackState !== Audio.PlayingState) {
                console.info("ASTRO_CALL_RINGBACK: source ready; starting playback")
                play()
            }
        }

        onPlaybackStateChanged: {
            console.info(
                "ASTRO_CALL_RINGBACK: playback state",
                playbackState,
                "audioRole:", audioRole
            )
        }

        onError: {
            if (error !== Audio.NoError) {
                console.warn(
                    "ASTRO_CALL_RINGBACK: playback error",
                    error,
                    errorString,
                    "source:", source
                )
            }
        }
    }

    Timer {
        id: noAnswerTimer
        interval: 30000
        repeat: false
        onTriggered: {
            if (!root.directCall || root.remoteAnswered)
                return
            root.stopRingback()
            if (root.pendingOperation.length > 0) {
                root.startRingback(true)
                restart()
                return
            }
            root.actionError = "No answer"
            root.hangUp()
        }
    }

    // Start native capture only after Qt confirms that the foreground QML
    // camera has completely unloaded. The native surface accepts CPU-backed
    // frames only; it does not use the Qt Quick preview or its GL textures.
    Timer {
        id: backgroundCameraStartTimer
        interval: 100
        repeat: true
        running: root.videoStarted &&
                 !root.applicationActive &&
                 !root.nativeBackgroundCameraRunning &&
                 !root.nativeBackgroundCameraStartPending &&
                 !root.nativeBackgroundCameraAttempted &&
                 localCamera.cameraStatus === Camera.UnloadedStatus

        onTriggered: root.tryStartBackgroundCamera()
    }

    // The Ubuntu Touch Hybris camera service can still be tearing down
    // its native capture pipeline after QCamera reports UnloadedStatus.
    // Reopening the Qt Quick camera too soon can crash qmlscene inside the
    // platform camera backend. Keep the handover asynchronous and allow the
    // service a longer settling window before reopening the preview.
    Timer {
        id: foregroundCameraRestartTimer
        interval: 1500
        repeat: false
        onTriggered: {
            if (!root.videoStarted ||
                    !root.applicationActive ||
                    !root.waitingForForegroundCamera ||
                    root.nativeBackgroundCameraRunning ||
                    root.nativeBackgroundCameraStartPending)
                return

            root.waitingForForegroundCamera = false
            console.info(
                "ASTRO_BACKGROUND_CAMERA: restarting foreground QML camera"
            )
            localCamera.start()
        }
    }

    Timer {
        id: cameraFrameGrabTimer
        interval: 33
        repeat: true
        // Do not request Qt Quick frame readback while the platform camera
        // backend is still opening. On Ubuntu Touch, grabToImage() against a
        // VideoOutput whose QCamera is only in StartingStatus can race the
        // native camera handover and crash qmlscene.
        running: root.videoStarted &&
                 root.applicationActive &&
                 !root.waitingForForegroundCamera &&
                 !root.nativeBackgroundCameraRunning &&
                 localCamera.cameraStatus === Camera.ActiveStatus &&
                 localCamera.cameraState === Camera.ActiveState &&
                 root.pendingOperation !== "stopScreenShare"

        onTriggered: {
            if (!root.applicationActive ||
                    localCamera.cameraStatus !== Camera.ActiveStatus ||
                    localCamera.cameraState !== Camera.ActiveState ||
                    !localCameraPreview.visible ||
                    !root.backend ||
                    !root.roomId)
                return

            if (root.cameraFrameGrabPending) {
                if (root.cameraFrameGrabStartedAt > 0 &&
                        Date.now() - root.cameraFrameGrabStartedAt < 750)
                    return

                console.warn(
                    "ASTRO_BACKGROUND_CAMERA: clearing stale frame-grab pending flag",
                    "ageMs:", root.cameraFrameGrabStartedAt > 0
                        ? Date.now() - root.cameraFrameGrabStartedAt : -1
                )
                root.cameraFrameGrabPending = false
                root.cameraFrameGrabStartedAt = 0
            }

            root.cameraFrameGrabPending = true
            root.cameraFrameGrabStartedAt = Date.now()
            root.cameraFrameGrabAttempts += 1
            var accepted = localCameraPreview.grabToImage(function(result) {
                root.cameraFrameGrabPending = false
                root.cameraFrameGrabStartedAt = 0
                root.cameraFrameGrabCompletions += 1
                if (result && result.image &&
                        root.videoStarted && root.backend &&
                        root.pendingOperation !== "stopScreenShare") {
                    root.backend.processRtcCameraImage(
                        root.roomId,
                        result.image
                    )
                } else {
                    root.cameraFrameGrabFailures += 1
                    console.warn(
                        "ASTRO_BACKGROUND_CAMERA: frame grab produced no usable image",
                        "attempts:", root.cameraFrameGrabAttempts,
                        "completed:", root.cameraFrameGrabCompletions,
                        "failed:", root.cameraFrameGrabFailures,
                        "applicationState:", root.applicationState
                    )
                }

                // The readback has now released its scenegraph resources;
                // begin screencast teardown without waiting for another tick.
                if (root.pendingOperation === "stopScreenShare" &&
                        !root.screenShareStopDispatched) {
                    root.dispatchScreenShareStop()
                }
            }, Qt.size(360, 480))

            if (!accepted) {
                root.cameraFrameGrabPending = false
                root.cameraFrameGrabStartedAt = 0
                root.cameraFrameGrabFailures += 1
                console.warn(
                    "ASTRO_BACKGROUND_CAMERA: grabToImage was rejected",
                    "attempts:", root.cameraFrameGrabAttempts,
                    "applicationState:", root.applicationState
                )
            }
        }
    }

    // Bound the wait for an in-flight camera readback. A missing callback
    // must never leave the Stop action stuck indefinitely.
    Timer {
        id: screenShareStopDrainTimer
        interval: 16
        repeat: true
        onTriggered: {
            root.screenShareStopDrainElapsedMs += interval
            if (!root.cameraFrameGrabPending ||
                    root.screenShareStopDrainElapsedMs >= 200) {
                root.dispatchScreenShareStop()
            }
        }
    }

    // Keep the QML event-loop heartbeat for the whole call, including after
    // the stop callback. This distinguishes a blocked QML thread from a
    // stalled Qt Quick scenegraph/render thread.
    Timer {
        interval: 2000
        repeat: true
        running: root.visible && root.callActive
        onTriggered: console.info(
            "ASTRO_SCREEN_SHARE_DIAG: QML heartbeat",
            "operation:", root.pendingOperation,
            "active:", root.screenShareStarted,
            "visible:", root.visible,
            "appState:", Qt.application.state
        )
    }

    Component.onCompleted: {
        console.info(
            "ASTRO_CALL_RINGBACK: audio capabilities",
            "availability:", ringbackAudio.availability,
            "supportedRoles:", ringbackAudio.supportedAudioRoles(),
            "selectedRole:", ringbackAudio.audioRole,
            "source:", ringbackSource
        )
        if (backend && directCall) {
            ringbackSource = backend.rtcRingbackToneUrl()
            console.info("ASTRO_CALL_RINGBACK: resolved source", ringbackSource)
        }
    }

    onDirectCallChanged: {
        if (directCall && backend && ringbackSource.length === 0)
            ringbackSource = backend.rtcRingbackToneUrl()
    }

    function tryStartBackgroundCamera() {
        if (applicationActive ||
                !videoStarted ||
                !backend ||
                !roomId ||
                nativeBackgroundCameraRunning ||
                nativeBackgroundCameraStartPending ||
                nativeBackgroundCameraAttempted)
            return

        if (localCamera.cameraState !== Camera.UnloadedState ||
                localCamera.cameraStatus !== Camera.UnloadedStatus)
            return

        // Camera unload can complete before a queued Qt Quick readback callback.
        // Allow that callback to drain before handing the camera device over.
        if (cameraFrameGrabPending) {
            var pendingAge = cameraFrameGrabStartedAt > 0
                    ? Date.now() - cameraFrameGrabStartedAt : 0
            if (pendingAge < 750)
                return

            console.warn(
                "ASTRO_BACKGROUND_CAMERA: frame-grab callback did not drain before handover",
                "ageMs:", pendingAge
            )
            cameraFrameGrabPending = false
            cameraFrameGrabStartedAt = 0
        }

        nativeBackgroundCameraAttempted = true
        nativeBackgroundCameraStartPending = true
        console.info(
            "ASTRO_BACKGROUND_CAMERA: QML camera unloaded; requesting native capture",
            "deviceId:", localCamera.deviceId,
            "cameraStatus:", localCamera.cameraStatus,
            "cameraState:", localCamera.cameraState
        )
        backend.startRtcBackgroundCamera(roomId, localCamera.deviceId)
    }

    function updateCameraForApplicationState(active) {
        applicationActive = active
        applicationState = backend
                ? backend.applicationState : Qt.application.state
        console.info(
            "ASTRO_BACKGROUND_CAMERA: CallPage lifecycle update",
            "state:", applicationState,
            "active:", active,
            "videoStarted:", videoStarted,
            "qmlCameraStatus:", localCamera.cameraStatus,
            "qmlCameraState:", localCamera.cameraState,
            "grabAttempts:", cameraFrameGrabAttempts,
            "grabCompletions:", cameraFrameGrabCompletions,
            "grabFailures:", cameraFrameGrabFailures
        )
        if (!videoStarted || !backend || !roomId)
            return

        foregroundCameraRestartTimer.stop()
        backgroundCameraStartTimer.stop()
        if (active) {
            nativeBackgroundCameraAttempted = false
            // If a background-camera request was already in flight, wait
            // for its confirmed shutdown before reopening the QML camera.
            waitingForForegroundCamera = true
            backend.stopRtcBackgroundCamera(roomId)
        } else {
            // Release the Qt Quick camera first. The native worker camera is
            // requested only after its status reaches UnloadedStatus, so the
            // two camera objects never contend for the physical device.
            waitingForForegroundCamera = false
            nativeBackgroundCameraAttempted = false
            localCamera.cameraState = Camera.UnloadedState
            console.info(
                "ASTRO_BACKGROUND_CAMERA: foreground QML camera unload requested",
                "state:", applicationState,
                "status:", localCamera.cameraStatus,
                "cameraState:", localCamera.cameraState
            )
        }
    }

    onVideoStartedChanged: {
        if (videoStarted) {
            if (applicationActive) {
                localCamera.start()
            } else if (backend && roomId) {
                localCamera.cameraState = Camera.UnloadedState
                nativeBackgroundCameraAttempted = false
            }
        } else {
            foregroundCameraRestartTimer.stop()
            backgroundCameraStartTimer.stop()
            waitingForForegroundCamera = false
            nativeBackgroundCameraStartPending = false
            nativeBackgroundCameraAttempted = false
            nativeBackgroundCameraRunning = false
            nativeBackgroundCameraDeviceId = ""
            cameraFrameGrabPending = false
            cameraFrameGrabStartedAt = 0
            localCamera.cameraState = Camera.UnloadedState
            if (backend && roomId)
                backend.stopRtcBackgroundCamera(roomId)
        }
    }

    Component.onDestruction: {
        noAnswerTimer.stop()
        root.stopRingback()
        localCamera.cameraState = Camera.UnloadedState
        if (backend && roomId)
            backend.stopRtcBackgroundCamera(roomId)
        if (backend && screenShareRoomId.length > 0)
            backend.stopRtcScreenShare(screenShareRoomId)
    }

    onAudioStateChanged: {
        for (var avatarIndex = 0; avatarIndex < participants.length; ++avatarIndex) {
            var avatarIdentity = participants[avatarIndex].identity || ""
            requestParticipantAvatar(
                avatarIdentity, participantAvatarMxc(avatarIdentity)
            )
        }

        // Do not infer that the remote party answered from the participant
        // model here: it can be populated during local session setup. The
        // LiveKit participant_connected/active events below are authoritative.

        if (audioState.active === true) {
            audioStarted = true
            membershipJoined = true
        } else if (audioState.connectionState === "Disconnected" &&
                   pendingOperation.length === 0) {
            if (backend && screenShareRoomId.length > 0)
                backend.stopRtcScreenShare(screenShareRoomId)
            audioStarted = false
            membershipJoined = membershipState.joinedByMe === true
            videoStarted = false
            screenShareStarted = false
            screenShareAudioRequested = false
            screenShareChoiceVisible = false
            screenShareRoomId = ""
            muted = false
            for (var i = 0; i < participants.length; ++i) {
                remoteVideoCleared(participants[i].identity, "camera")
                remoteVideoCleared(participants[i].identity, "screenshare")
            }
        }
    }



    Connections {
        target: root.backend
        ignoreUnknownSignals: true

        onApplicationActiveChanged: {
            root.updateCameraForApplicationState(active)
        }

        onApplicationStateChanged: {
            root.applicationState = state
            console.info(
                "ASTRO_BACKGROUND_CAMERA: backend application state changed:",
                state,
                "active:", state === Qt.ApplicationActive
            )
        }

        onMatrixRtcBackgroundCameraActionFinished: {
            if (roomId !== root.roomId)
                return

            root.nativeBackgroundCameraStartPending = false
            root.nativeBackgroundCameraRunning = running
            if (running) {
                root.nativeBackgroundCameraDeviceId = deviceId || ""
            } else {
                root.nativeBackgroundCameraDeviceId = ""
            }

            if (error && error.length > 0) {
                backgroundCameraStartTimer.stop()
                root.actionError = error
                console.warn(
                    "ASTRO_BACKGROUND_CAMERA: native capture transition failed:",
                    error
                )

                // A failed start/timeout is not proof that QCamera has
                // released the device. On foreground return, request another
                // stop and wait for its error-free unload completion.
                if (root.applicationActive &&
                        root.videoStarted &&
                        root.waitingForForegroundCamera) {
                    root.backend.stopRtcBackgroundCamera(root.roomId)
                }
                return
            }

            if (running && root.applicationActive) {
                // The app returned while native capture was starting. Stop
                // it first; the completion below restarts the QML camera.
                root.waitingForForegroundCamera = true
                root.backend.stopRtcBackgroundCamera(root.roomId)
            } else if (!running &&
                       root.applicationActive &&
                       root.videoStarted &&
                       root.waitingForForegroundCamera &&
                       root.pendingOperation !== "stopVideo" &&
                       root.pendingOperation !== "stopVideoForHangup") {
                // Mir/Qt camera resources can still be settling immediately
                // after QCamera reports UnloadedStatus. Defer the QML restart
                // to the next event-loop window to avoid overlapping GL teardown.
                console.info(
                    "ASTRO_BACKGROUND_CAMERA: native device released; scheduling QML camera restart"
                )
                foregroundCameraRestartTimer.restart()
            }
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
                root.screenShareAudioRequested = false
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
                if (operation === "join" || operation === "start") {
                    noAnswerTimer.stop()
                    root.stopRingback()
                }
                if (operation === "stop") {
                    // Even if LiveKit shutdown reports an error, withdraw
                    // MatrixRTC membership so the user is not trapped in the call.
                    root.audioStarted = false
                    root.videoStarted = false
                    root.screenShareStarted = false
                    root.screenShareAudioRequested = false
                    root.screenShareRoomId = ""
                    root.leaveMembership()
                } else if (operation === "stopAfterExternalLeave") {
                    // MatrixRTC membership was already removed by the room
                    // participant control. Do not issue a second leave.
                    root.membershipJoined = false
                    root.audioStarted = false
                    root.videoStarted = false
                    root.screenShareStarted = false
                    root.screenShareAudioRequested = false
                    root.screenShareRoomId = ""
                    root.muted = false
                    root.backRequested()
                }
                return
            }

            root.actionError = ""

            if (operation === "start") {
                root.audioStarted = true
                root.muted = false
                // LiveKit may reconfigure the platform audio route while
                // connecting. Restart ringback after that transition.
                if (root.directCall && !root.remoteAnswered)
                    root.startRingback(true);

            } else if (operation === "mute") {
                root.muted = root.pendingMuted

            } else if (operation === "stop") {
                root.audioStarted = false
                root.muted = false
                root.leaveMembership()

            } else if (operation === "stopAfterExternalLeave") {
                // The room participant control already left MatrixRTC. Only
                // finish closing LiveKit/media here; never leave a second time.
                root.membershipJoined = false
                root.audioStarted = false
                root.videoStarted = false
                root.screenShareStarted = false
                root.screenShareAudioRequested = false
                root.screenShareRoomId = ""
                root.muted = false
                root.actionError = ""
                root.backRequested()
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
            console.info(
                "ASTRO_SCREEN_SHARE_DIAG: QML completion handler entered",
                "roomMatches:", roomId === root.roomId,
                "operation:", root.pendingOperation,
                "error:", error
            )
            if (roomId !== root.roomId) {
                console.info("ASTRO_SCREEN_SHARE_DIAG: ignored completion for another room")
                return
            }

            var operation = root.pendingOperation
            if (operation !== "startScreenShare" &&
                    operation !== "stopScreenShare" &&
                    operation !== "stopScreenShareForHangup") {
                console.info(
                    "ASTRO_SCREEN_SHARE_DIAG: ignored stale completion",
                    "operation:", operation,
                    "error:", error
                )
                return
            }

            root.pendingOperation = ""
            if (operation === "stopScreenShare" ||
                    operation === "stopScreenShareForHangup") {
                screenShareStopDrainTimer.stop()
                root.screenShareStopRoomId = ""
                root.screenShareStopDispatched = false
            }

            if (error && error.length > 0) {
                root.actionError = error
                if (operation === "startScreenShare") {
                    root.screenShareStarted = false
                    root.screenShareAudioRequested = false
                    root.screenShareRoomId = ""
                } else if (operation === "stopScreenShare" ||
                           operation === "stopScreenShareForHangup") {
                    // Local capture has already been stopped. A signalling
                    // error must not leave the UI claiming that capture is
                    // still running or invite another stop against dead state.
                    root.screenShareStarted = false
                    root.screenShareAudioRequested = false
                    root.screenShareRoomId = ""
                } else {
                    root.screenShareStarted = false
                    root.screenShareAudioRequested = false
                    root.screenShareRoomId = ""
                }
                console.info(
                    "ASTRO_SCREEN_SHARE_DIAG: QML completion handler finished with error",
                    "operation:", operation,
                    "pendingOperation:", root.pendingOperation,
                    "screenShareStarted:", root.screenShareStarted
                )
                return
            }

            root.actionError = ""

            if (operation === "startScreenShare") {
                root.screenShareStarted = true
            } else if (operation === "stopScreenShare") {
                root.screenShareStarted = false
                root.screenShareAudioRequested = false
                root.screenShareRoomId = ""
            } else if (operation === "stopScreenShareForHangup") {
                root.screenShareStarted = false
                root.screenShareAudioRequested = false
                root.screenShareRoomId = ""
                if (root.videoStarted) {
                    localCamera.stop()
                    root.pendingOperation = "stopVideoForHangup"
                    root.backend.stopRtcVideo(root.roomId)
                } else {
                    root.stopAudioOrLeave()
                }
            }

            console.info(
                "ASTRO_SCREEN_SHARE_DIAG: QML completion handler finished",
                "operation:", operation,
                "pendingOperation:", root.pendingOperation,
                "screenShareStarted:", root.screenShareStarted,
                "screenShareRoomId:", root.screenShareRoomId
            )
        }

        onMatrixRtcAudioEventsUpdated: {
            try {
                var payload = JSON.parse(json)
                var events = payload.events || []
                for (var i = 0; i < events.length; ++i) {
                    var event = events[i]
                    if (event.room_id !== root.roomId)
                        continue

                    if (event.type === "participant_connected" ||
                            event.type === "participant_active") {
                        root.remoteAnswered = true
                        noAnswerTimer.stop()
                        root.stopRingback()
                    }

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

        onCameraStatusChanged: {
            console.info(
                "ASTRO_BACKGROUND_CAMERA: QML camera status:",
                cameraStatus,
                "cameraState:", cameraState,
                "applicationState:", root.applicationState,
                "active:", root.applicationActive,
                "videoStarted:", root.videoStarted,
                "error:", errorString
            )
            if (cameraStatus === Camera.UnloadedStatus &&
                    !root.applicationActive) {
                console.info(
                    "ASTRO_BACKGROUND_CAMERA: QML camera confirmed unloaded"
                )
            }
        }

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

        // Drag and flick the local camera preview within the call page.
        MouseArea {
            id: localCameraPreviewDrag
            anchors.fill: parent
            drag.target: localCameraPreview
            drag.axis: Drag.XAndYAxis
            drag.minimumX: 0
            drag.maximumX: Math.max(0, root.width - localCameraPreview.width)
            drag.minimumY: 0
            drag.maximumY: Math.max(0, root.height - localCameraPreview.height)

            property real lastPointerX: 0
            property real lastPointerY: 0
            property real velocityX: 0
            property real velocityY: 0
            property double lastSampleTime: 0
            property real flickTargetX: localCameraPreview.x
            property real flickTargetY: localCameraPreview.y

            onPressed: {
                previewFlick.stop()
                var point = mapToItem(root, mouse.x, mouse.y)
                lastPointerX = point.x
                lastPointerY = point.y
                lastSampleTime = Date.now()
                velocityX = 0
                velocityY = 0
            }

            onPositionChanged: {
                if (!pressed)
                    return

                var point = mapToItem(root, mouse.x, mouse.y)
                var now = Date.now()
                var elapsed = now - lastSampleTime

                if (elapsed > 0) {
                    var sampleX = (point.x - lastPointerX) / elapsed
                    var sampleY = (point.y - lastPointerY) / elapsed

                    // Smooth noisy touch samples while retaining the flick.
                    velocityX = velocityX * 0.55 + sampleX * 0.45
                    velocityY = velocityY * 0.55 + sampleY * 0.45
                }

                lastPointerX = point.x
                lastPointerY = point.y
                lastSampleTime = now
            }

            onReleased: {
                var maxVelocity = 2.4
                var vx = Math.max(-maxVelocity,
                                  Math.min(maxVelocity, velocityX))
                var vy = Math.max(-maxVelocity,
                                  Math.min(maxVelocity, velocityY))
                var projection = 190

                flickTargetX = Math.max(
                    0,
                    Math.min(root.width - localCameraPreview.width,
                             localCameraPreview.x + vx * projection)
                )
                flickTargetY = Math.max(
                    0,
                    Math.min(root.height - localCameraPreview.height,
                             localCameraPreview.y + vy * projection)
                )

                var distance = Math.sqrt(
                    Math.pow(flickTargetX - localCameraPreview.x, 2) +
                    Math.pow(flickTargetY - localCameraPreview.y, 2)
                )

                if (distance > units.gu(1))
                    previewFlick.start()
            }

            ParallelAnimation {
                id: previewFlick

                NumberAnimation {
                    target: localCameraPreview
                    property: "x"
                    to: localCameraPreviewDrag.flickTargetX
                    duration: 420
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    target: localCameraPreview
                    property: "y"
                    to: localCameraPreviewDrag.flickTargetY
                    duration: 420
                    easing.type: Easing.OutCubic
                }
            }
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
                            // Navigation must never end an active call.
                            // Only the explicit Leave button invokes hangUp().
                            root.backRequested()
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

                                // A LiveKit track-unsubscribe can arrive while
                                // Qt Quick is processing a frame from this item.
                                // Defer texture/frame teardown until the current
                                // signal delivery and scenegraph update have
                                // unwound; clearing a video item re-entrantly can
                                // leave the call page's render path stalled.
                                if (source === "camera") {
                                    Qt.callLater(function() {
                                        if (remoteVideo)
                                            remoteVideo.clearFrame()
                                    })
                                } else if (source === "screenshare") {
                                    Qt.callLater(function() {
                                        if (remoteScreenVideo)
                                            remoteScreenVideo.clearFrame()
                                    })
                                }
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

                            // Give a lone participant a proper call-stage
                            // portrait instead of the small multi-party avatar.
                            // Keep it proportional on different device sizes.
                            property real avatarSize:
                                root.participants.length === 1
                                ? Math.min(
                                      Math.min(parent.width, parent.height) * 0.44,
                                      units.gu(32)
                                  )
                                : units.gu(8)

                            property string participantIdentity:
                                String(modelData.identity || "")
                            property string participantMxc:
                                root.participantAvatarMxc(participantIdentity)

                            Component.onCompleted: {
                                root.requestParticipantAvatar(
                                    participantIdentity, participantMxc
                                )
                            }

                            onParticipantMxcChanged: {
                                root.requestParticipantAvatar(
                                    participantIdentity, participantMxc
                                )
                            }

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.avatarSize
                                height: parent.avatarSize
                                radius: width / 2
                                color: "#426b8a"
                                clip: true

                                Text {
                                    anchors.centerIn: parent
                                    text: root.participantInitial(
                                            root.participantDisplayName(
                                                parent.parent.participantIdentity,
                                                modelData.name
                                            )
                                        )
                                    color: "white"
                                    font.pixelSize: parent.parent.avatarSize * 0.38
                                    font.bold: true
                                    visible: !participantAvatarMask.visible
                                }

                                Image {
                                    id: participantAvatarImage
                                    anchors.fill: parent
                                    source: root.participantAvatarCache[
                                        parent.parent.participantIdentity
                                    ] || ""
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                }

                                OpacityMask {
                                    id: participantAvatarMask
                                    anchors.fill: parent
                                    source: participantAvatarImage
                                    maskSource: Rectangle {
                                        width: participantAvatarMask.width
                                        height: participantAvatarMask.height
                                        radius: width / 2
                                        color: "white"
                                    }
                                    cached: true
                                    visible: participantAvatarImage.status === Image.Ready
                                }
                            }

                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: units.gu(0.5)

                                Text {
                                    text: root.participantDisplayName(
                                        parent.parent.participantIdentity,
                                        modelData.name
                                    )
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
                                text: "Join Call"
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
                                text: "Leave Call"
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
                            ? (root.screenShareAudioRequested
                               ? "Screen sharing with device audio connected"
                               : "Screen sharing connected")
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
    // Ask before starting display capture so device playback is never shared implicitly.
    Item {
        anchors.fill: parent
        visible: root.screenShareChoiceVisible
        z: 2000

        Rectangle {
            anchors.fill: parent
            color: "#99000000"

            MouseArea {
                anchors.fill: parent
                onClicked: {}
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - units.gu(4), units.gu(42))
            height: choiceColumn.height + units.gu(4)
            radius: units.gu(1)
            color: "#1b2935"

            Column {
                id: choiceColumn
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: units.gu(2)
                }
                spacing: units.gu(1.5)

                Text {
                    width: parent.width
                    text: "Share your screen"
                    color: "white"
                    font.pixelSize: units.gu(2.3)
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    width: parent.width
                    text: "Would you also like to share audio playing on this device? Your microphone setting is unchanged."
                    color: "#b8c7d3"
                    font.pixelSize: units.gu(1.5)
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }

                Button {
                    width: parent.width
                    text: "Share screen only"
                    onClicked: root.beginScreenShare(false)
                }

                Button {
                    width: parent.width
                    text: "Share screen + device audio"
                    color: "#287a55"
                    onClicked: root.beginScreenShare(true)
                }

                Button {
                    width: parent.width
                    text: "Cancel"
                    onClicked: root.screenShareChoiceVisible = false
                }
            }
        }
    }

}
