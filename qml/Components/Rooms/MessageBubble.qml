import QtQuick 2.7
import Lomiri.Components 1.3
import QtGraphicalEffects 1.0

Item {
    id: root

    signal imageClicked(string imageUrl)
    signal videoFullscreenRequested(var output, var originalParent, var mediaPlayer,var downloadItem)
    signal pasteRequested()
    signal selectionRequested(string selectionId)
    signal selectionCleared(string selectionId)
    signal messageTouched(string selectionId)
    signal messageTapRequested(string selectionId, bool wasAllowed)
    signal reactionRequested(string eventId, string reactionKey)
    signal reactionPickerRequested(string eventId, real anchorX, real anchorY)
    signal messageActionTrayToggled(string selectionId, bool visible)
    signal messageActionRequested(string eventId, string action)
    signal videoPlaybackRequested(
        string requestKey,
        string fileUrl,
        var outputHost,
        var owner
    )
    signal videoPlaybackReleased(
        string requestKey,
        var outputHost,
        var owner
    )

    property string sender: ""
    property string senderDisplayName: ""
    property string avatarUrl: ""
    property bool showAvatar: true

    property string body: ""
    property string timestamp: ""
    property string reactionsJson: "[]"
    property string imageUrl: ""
    property bool isImage: false
    property bool isSticker: false
    property bool isVideo: false

    property var backend
    property var fullscreenHost
    property string videoRequestKey: ""
    property string videoMediaSourceJson: ""
    property string videoThumbnailUrl: ""
    property string videoMimeType: ""
    property var sharedVideoPlayer: null
    property var sharedVideoOutput: null
    property string sharedPlaybackKey: ""

    property bool isOwn: false
    property bool isEncrypted: false
    property bool collapsed: false
    property bool selectionToolbarVisible: false
    property string selectionId: ""
    property bool actionTrayVisible: false
    property bool touchSwipeHandled: false
    property real swipeOffset: 0
    property real swipeStartOffset: 0
    property real swipeProgress: 0
    property bool swipeClosingGesture: false
    property real mediaSwipeLastDx: 0

    onActionTrayVisibleChanged: {
        if (!actionTrayVisible) {
            swipeOffset = 0
            swipeProgress = 0
        }
    }

    Behavior on swipeOffset {
        NumberAnimation {
            duration: 110
            easing.type: Easing.OutCubic
        }
    }

    property bool touchSelectionMode: false
    property int touchSelectionAnchor: -1
    property real touchPressX: 0
    property real touchPressY: 0
    property bool tapSelectionAllowed: true
    property bool tapSelectionWasAllowed: false
    property bool touchMoved: false
    property bool messageSelected: false

    function beginMediaSwipe() {
        root.messageTouched(root.selectionId)
        root.swipeStartOffset = root.swipeOffset
        root.swipeClosingGesture = false
        root.touchSwipeHandled = false
        root.mediaSwipeLastDx = 0
    }

    function updateMediaSwipe(dx, dy) {
        if (root.touchSelectionMode)
            return

        var horizontalSwipe =
            Math.abs(dx) > units.gu(0.65) &&
            Math.abs(dx) > Math.abs(dy) * 1.05

        if (!horizontalSwipe)
            return

        root.touchSwipeHandled = true
        root.mediaSwipeLastDx = dx

        var limit = units.gu(18)
        var closingDistance = units.gu(0.8)
        var movingOpposite =
            root.actionTrayVisible &&
            root.swipeStartOffset * dx < 0 &&
            Math.abs(dx) > closingDistance

        if (root.swipeClosingGesture || movingOpposite) {
            root.swipeClosingGesture = true
            root.swipeOffset = 0
            root.swipeProgress = 0
        } else {
            var candidate = root.swipeStartOffset + dx
            root.swipeOffset = Math.max(
                -limit,
                Math.min(limit, candidate)
            )
            root.swipeProgress = Math.min(
                1.0,
                Math.abs(root.swipeOffset) / limit
            )
        }
    }

    function finishMediaSwipe(cancelled) {
        if (cancelled) {
            // A cancelled gesture restores the current tray state.
            if (root.actionTrayVisible) {
                root.swipeOffset =
                    root.swipeStartOffset < 0
                    ? -units.gu(15) : units.gu(15)
                root.swipeProgress = 1.0
            } else {
                root.swipeOffset = 0
                root.swipeProgress = 0
            }
        } else if (root.touchSwipeHandled) {
            var shouldOpen = !root.swipeClosingGesture

            if (shouldOpen) {
                var direction = root.mediaSwipeLastDx < 0 ? -1 : 1
                root.swipeOffset = direction * units.gu(15)
                root.swipeProgress = 1.0
            } else {
                root.swipeOffset = 0
                root.swipeProgress = 0
            }

            root.messageActionTrayToggled(
                root.selectionId,
                shouldOpen
            )
        } else {
            // No recognised swipe: leave the tray where it was.
            if (root.actionTrayVisible) {
                root.swipeOffset =
                    root.swipeStartOffset < 0
                    ? -units.gu(15) : units.gu(15)
                root.swipeProgress = 1.0
            } else {
                root.swipeOffset = 0
                root.swipeProgress = 0
            }
        }

        root.touchSwipeHandled = false
        root.swipeClosingGesture = false
        root.mediaSwipeLastDx = 0
    }

    function isEmojiBaseCodePoint(cp) {
        return (cp >= 0x1F000 && cp <= 0x1FAFF) ||
               (cp >= 0x2600 && cp <= 0x27BF) ||
               (cp >= 0x2300 && cp <= 0x23FF) ||
               (cp >= 0x2190 && cp <= 0x21FF) ||
               cp === 0x00A9 || cp === 0x00AE ||
               cp === 0x203C || cp === 0x2049 ||
               cp === 0x2122 || cp === 0x2139 ||
               cp === 0x24C2 || cp === 0x3030 ||
               cp === 0x303D || cp === 0x3297 ||
               cp === 0x3299 || cp === 0x2B50 ||
               cp === 0x2B55;
    }

    function emojiMessageFontSize() {
        var normalSize = units.gu(1.8);

        if (root.isEncrypted || root.isSticker)
            return normalSize;

        var value = String(root.body || "");
        var count = 0;
        var regionalCount = 0;
        var joined = false;

        if (value.length === 0)
            return normalSize;

        for (var i = 0; i < value.length; i++) {
            var cp = value.charCodeAt(i);

            if (cp >= 0xD800 && cp <= 0xDBFF &&
                    i + 1 < value.length) {
                var low = value.charCodeAt(i + 1);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 +
                         ((cp - 0xD800) << 10) +
                         (low - 0xDC00);
                    i++;
                }
            }

            // Allow whitespace between emoji.
            if (cp === 0x20 || cp === 0x09 ||
                    cp === 0x0A || cp === 0x0D ||
                    cp === 0x00A0) {
                regionalCount = 0;
                joined = false;
                continue;
            }

            // Keycap emoji: 1️⃣, #️⃣, etc.
            if (cp === 0x23 || cp === 0x2A ||
                    (cp >= 0x30 && cp <= 0x39)) {
                var next = i + 1;

                if (value.charCodeAt(next) === 0xFE0F)
                    next++;

                if (value.charCodeAt(next) === 0x20E3) {
                    count++;
                    i = next;
                    continue;
                }

                return normalSize;
            }

            // Emoji presentation selectors, skin tones and tag characters.
            if (cp === 0xFE0F || cp === 0xFE0E ||
                    (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
                    (cp >= 0xE0020 && cp <= 0xE007F))
                continue;

            // Zero-width joiner connects a multi-code-point emoji.
            if (cp === 0x200D) {
                joined = true;
                continue;
            }

            // Regional indicator pairs form flags.
            if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
                if (regionalCount % 2 === 0)
                    count++;

                regionalCount++;
                joined = false;
                continue;
            }

            regionalCount = 0;

            if (isEmojiBaseCodePoint(cp)) {
                if (!joined)
                    count++;

                joined = false;
                continue;
            }

            // Any ordinary character means normal text sizing.
            return normalSize;
        }

        if (count === 0)
            return normalSize;

        if (count <= 3)
            return units.gu(4);

        if (count <= 6)
            return units.gu(3);

        return units.gu(Math.max(2.1, 3 - (count - 6) * 0.12));
    }

    function parsedReactions() {
        try {
            var value = JSON.parse(root.reactionsJson || "[]")
            return value instanceof Array ? value : []
        } catch (error) {
            console.log("ASTRO: Could not parse message reactions:", error)
            return []
        }
    }

    function scheduleSelectionToolbar() {
        if (bodyText.selectedText.length === 0) {
            selectionToolbarTimer.stop()
            selectionToolbarVisible = false
            root.selectionCleared(root.selectionId)
        } else {
            selectionToolbarTimer.restart()
        }
    }

    function clearTextSelection() {
        selectionToolbarTimer.stop()
        bodyText.deselect()
        selectionToolbarVisible = false
        touchSelectionMode = false
        touchSelectionAnchor = -1
        root.selectionCleared(root.selectionId)
    }

    function beginTouchTextSelection(x, y) {
        console.log("ASTRO SELECT: begin called", x, y)

        var value = bodyText.text

        if (value.length === 0)
            return

        var position = bodyText.positionAt(x, y)
        console.log("ASTRO SELECT: text position", position,
                    "length", value.length)
        position = Math.max(0, Math.min(position, value.length - 1))

        var start = position
        var end = position + 1

        while (start > 0 && !/\s/.test(value.charAt(start - 1)))
            start--

        while (end < value.length && !/\s/.test(value.charAt(end)))
            end++

        touchSelectionAnchor =
            position - start < (end - start) / 2 ? end : start

        touchSelectionMode = true
        bodyText.select(start, end)
        console.log("ASTRO SELECT: selected", start, end,
                    "text:", bodyText.selectedText)
        selectionToolbarTimer.stop()
    }

    function updateTouchTextSelection(x, y) {
        if (!touchSelectionMode || touchSelectionAnchor < 0)
            return

        var position = bodyText.positionAt(x, y)
        bodyText.select(touchSelectionAnchor, position)
        selectionToolbarTimer.stop()
    }

    width: parent ? parent.width : 0
    visible: !root.collapsed
    height: root.collapsed ? 0 : (messageBubble.height + (reactionFlow.visible
             ? reactionFlow.height - units.gu(1.5) + units.gu(1)
             : units.gu(1)))

    Row {
        anchors {
            left: parent.left
            right: parent.right
        }

        spacing: units.gu(1)

        layoutDirection: root.isOwn
                        ? Qt.RightToLeft
                        : Qt.LeftToRight

        Item {
            id: avatarContainer

            width: units.gu(5)
            height: units.gu(5)

            Rectangle {
                id: avatarBackground

                anchors.fill: parent
                radius: width / 2
                color: "#40566a"
                visible: root.showAvatar && senderAvatarImage.status !== Image.Ready

                Text {
                    anchors.centerIn: parent

                    text: {
                        var name = root.senderDisplayName ||
                                   root.sender

                        if (!name)
                            return "?"

                        if (name.charAt(0) === "@")
                            name = name.substring(1)

                        return name.charAt(0).toUpperCase()
                    }
                    color: "white"
                    font.bold: true
                    font.pixelSize: units.gu(2)
                    visible: root.showAvatar && senderAvatarImage.status !== Image.Ready
                }
            }

            Image {
                id: senderAvatarImage

                anchors.fill: parent

                source: root.avatarUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: true
                visible: root.showAvatar && status === Image.Ready

                layer.enabled: true
                layer.effect: OpacityMask {
                    maskSource: Rectangle {
                        width: senderAvatarImage.width
                        height: senderAvatarImage.height
                        radius: width / 2
                    }
                }
            }

            visible: true
        }

        Rectangle {
            id: messageBubble
            transform: Translate { x: root.swipeOffset }
            width: root.width - units.gu(7)
            height: messageContent.height + units.gu(2)

            radius: units.gu(1.5)

            color: Qt.tint(
                Qt.lighter(
                    root.isOwn ? "#245b78" : "#263744",
                    1.0 + root.swipeProgress * 0.32
                ),
                Qt.rgba(0.10, 1.0, 0.28, root.swipeProgress * 0.55)
            )
            border.width: root.messageSelected ? units.gu(0.25) : 0
            border.color: "#83d9a5"
            Column {
                id: messageContent

                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: units.gu(1)
                }

                spacing: units.gu(0.5)

                Text {
                    width: parent.width

                    text: root.isOwn
                          ? "You"
                          : (root.senderDisplayName || root.sender)

                    color: "#80b9e8"
                    font.bold: true
                    font.pixelSize: units.gu(1.5)

                    elide: Text.ElideRight
                }

                MessageImage {
                    width: root.isSticker
                        ? Math.min(parent.width, units.gu(18))
                        : parent.width

                    imageHeightLimit: root.isSticker
                        ? units.gu(18)
                        : units.gu(40)

                    source: root.imageUrl
                    visible: root.isImage && source !== ""

                    onImageClicked: root.imageClicked(imageUrl)
                    onSwipeStarted: root.beginMediaSwipe()
                    onSwipeMoved: root.updateMediaSwipe(dx, dy)
                    onSwipeEnded: root.finishMediaSwipe(cancelled)
                }

                Loader {
                    width: parent.width
                    height: root.isVideo ? units.gu(25) : 0
                    active: root.isVideo
                    visible: root.isVideo

                    sourceComponent: Component {
                    MessageVideo {
                        width: parent.width
                        height: units.gu(25)

                        backend: root.backend
                        requestKey: root.videoRequestKey
                        mediaSourceJson: root.videoMediaSourceJson
                        thumbnailUrl: root.videoThumbnailUrl
                        mimeType: root.videoMimeType
                        fullscreenHost: root.fullscreenHost

                        sharedPlayer: root.sharedVideoPlayer
                        sharedVideoOutput: root.sharedVideoOutput
                        sharedPlaybackKey: root.sharedPlaybackKey

                        onSwipeStarted: root.beginMediaSwipe()
                        onSwipeMoved: root.updateMediaSwipe(dx, dy)
                        onSwipeEnded: root.finishMediaSwipe(cancelled)

                        onPlaybackRequested: {
                            root.videoPlaybackRequested(
                                requestKey,
                                fileUrl,
                                outputHost,
                                owner
                            )
                        }

                        onPlaybackReleased: {
                            root.videoPlaybackReleased(
                                requestKey,
                                outputHost,
                                owner
                            )
                        }

                        onFullscreenRequested: {
                            root.videoFullscreenRequested(
                                output,
                                originalParent,
                                mediaPlayer,
                                downloadItem
                            )
                        }
                    }
                    }
                }

                Item {
                    id: bodyTextContainer

                    width: parent.width
                    height: bodyText.height
                    visible: !root.isSticker
                    Text {
                        id: encryptionLock

                        visible: root.isEncrypted
                        width: visible ? implicitWidth : 0
                        height: bodyText.height

                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter

                        text: "🔒"
                        color: "#a9d6ed"
                        font.pixelSize: units.gu(1.8)
                        verticalAlignment: Text.AlignVCenter
                    }

                    TextEdit {
                        id: bodyText

                        x: encryptionLock.width +
                           (root.isEncrypted ? units.gu(0.7) : 0)

                        width: parent.width - x
                        height: Math.max(font.pixelSize * 1.5, contentHeight)

                        text: root.isEncrypted
                              ? "Encrypted message — unable to decrypt"
                              : root.body

                        color: root.isEncrypted
                               ? "#a9d6ed"
                               : "white"
                        selectionColor: "#477fa0"

                        font.italic: root.isEncrypted
                        font.pixelSize: root.emojiMessageFontSize()

                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true

                        onSelectionStartChanged: root.scheduleSelectionToolbar()
                        onSelectionEndChanged: root.scheduleSelectionToolbar()
                        onSelectedTextChanged: root.scheduleSelectionToolbar()
                    }

                    MultiPointTouchArea {
                        id: textTouchSelection

                        anchors.fill: bodyText
                        minimumTouchPoints: 1
                        maximumTouchPoints: 1
                        mouseEnabled: false

                        onPressed: {
                            console.log("ASTRO SELECT: touch pressed",
                                        touchPoints.length)

                            if (touchPoints.length === 0)
                                return

                            root.messageTouched(root.selectionId)
                            root.tapSelectionWasAllowed = root.tapSelectionAllowed
                            root.touchMoved = false
                            root.touchPressX = touchPoints[0].x
                            root.touchPressY = touchPoints[0].y
                            root.swipeStartOffset = root.swipeOffset
                            root.swipeClosingGesture = false
                            root.touchSwipeHandled = false
                            longPressTimer.restart()
                        }

                        onUpdated: {
                            if (touchPoints.length === 0)
                                return

                            var point = touchPoints[0]

                            if (!root.touchSelectionMode) {
                                var dx = point.x - root.touchPressX
                                var dy = point.y - root.touchPressY
                                if (Math.sqrt(dx * dx + dy * dy) > units.gu(2.0))
                                    root.touchMoved = true
                                var limit = units.gu(18)
                            var closingDistance = units.gu(1.0)

                            // Ignore vertical movement. Only a clearly
                            // horizontal gesture may move the bubble.
                            var horizontalSwipe =
                                Math.abs(dx) > units.gu(0.35) &&
                                Math.abs(dx) > Math.abs(dy) * 1.05

                            if (horizontalSwipe)
                                root.touchSwipeHandled = true

                            if (root.touchSwipeHandled) {
                                var movingOpposite =
                                    root.actionTrayVisible &&
                                    root.swipeStartOffset * dx < 0 &&
                                    Math.abs(dx) > closingDistance

                                if (root.swipeClosingGesture ||
                                        movingOpposite) {
                                    root.swipeClosingGesture = true
                                    root.swipeOffset = 0
                                    root.swipeProgress = 0
                                } else {
                                    var candidate =
                                        root.swipeStartOffset + dx

                                    root.swipeOffset = Math.max(
                                        -limit,
                                        Math.min(limit, candidate)
                                    )

                                    root.swipeProgress = Math.min(
                                        1.0,
                                        Math.abs(root.swipeOffset) / limit
                                    )
                                }
                            }
                            } else {
                                root.updateTouchTextSelection(
                                    point.x,
                                    point.y
                                )
                            }
                        }

                        onGestureStarted: {
                            if (root.touchSelectionMode)
                                gesture.grab()
                        }

                        onReleased: {
                            longPressTimer.stop()

                            if (root.touchSwipeHandled) {
                                var shouldOpen = !root.swipeClosingGesture &&
                                    root.touchSwipeHandled
                                if (shouldOpen) {
                                    root.swipeOffset = root.swipeOffset < 0
                                        ? -units.gu(15) : units.gu(15)
                                    root.swipeProgress = 1.0
                                } else {
                                    root.swipeOffset = 0
                                    root.swipeProgress = 0
                                }
                                root.messageActionTrayToggled(
                                    root.selectionId,
                                    shouldOpen
                                )
                            } else {
                                // A non-swipe release must never leave
                                // the bubble at an intermediate position.
                                if (root.actionTrayVisible) {
                                    root.swipeOffset =
                                        root.swipeStartOffset < 0
                                        ? -units.gu(15) : units.gu(15)
                                    root.swipeProgress = 1.0
                                } else {
                                    root.swipeOffset = 0
                                    root.swipeProgress = 0
                                }
                            }
                            if (!root.isImage && !root.isVideo &&
                                    !root.touchSwipeHandled &&
                                    !root.touchSelectionMode &&
                                    !root.touchMoved) {
                                root.messageTapRequested(
                                    root.selectionId,
                                    root.tapSelectionWasAllowed
                                )
                            }
                            root.touchSwipeHandled = false
                            root.swipeClosingGesture = false
                            root.touchSwipeHandled = false

                            if (root.touchSelectionMode)
                                root.scheduleSelectionToolbar()

                            root.touchSelectionMode = false
                            root.touchSelectionAnchor = -1
                        }

                        onCanceled: {
                            console.log("ASTRO SELECT: touch canceled")
                            longPressTimer.stop()
                            if (root.actionTrayVisible) {
                                root.swipeOffset =
                                    root.swipeStartOffset < 0
                                    ? -units.gu(15) : units.gu(15)
                                root.swipeProgress = 1.0
                            } else {
                                root.swipeOffset = 0
                                root.swipeProgress = 0
                            }

                            root.touchSwipeHandled = false
                            root.swipeClosingGesture = false
                            root.touchSelectionMode = false
                            root.touchSelectionAnchor = -1
                        }

                        Timer {
                            id: longPressTimer
                            interval: 900
                            repeat: false

                            onTriggered: {
                                console.log("ASTRO SELECT: long press timer fired",
                                            textTouchSelection.touchPoints.length)

                                root.beginTouchTextSelection(
                                    root.touchPressX,
                                    root.touchPressY
                                )
                            }
                        }
                    }
                }



                Row {
                    width: parent.width
                    height: units.gu(4)
                    spacing: 0

                    Text {
                        width: parent.width
                        height: parent.height

                        text: root.timestamp
                        color: "#a9bac6"

                        font.pixelSize: units.gu(1.3)
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                    }

                    Rectangle {
                        width: 0
                        height: units.gu(4)
                        radius: width / 2
                        visible: false
                        color: emojiButtonMouse.pressed
                               ? "#526e82" : "#354957"

                        Text {
                            anchors.centerIn: parent
                            text: "☺"
                            color: "#ffffff"
                            font.pixelSize: units.gu(2.5)
                        }

                        MouseArea {
                            id: emojiButtonMouse
                            anchors.fill: parent

                            onClicked: {
                                if (root.selectionId === "")
                                    return

                                var anchor = emojiButtonMouse.mapToItem(
                                    root, 0, 0
                                )

                                root.reactionPickerRequested(
                                    root.selectionId,
                                    anchor.x,
                                    anchor.y
                                )
                            }
                        }
                    }
                }
            }

            Row {
                id: messageActionTray

                transform: Translate { x: -root.swipeOffset }
                x: root.swipeOffset > 0
                   ? units.gu(0.7)
                   : parent.width - width - units.gu(0.7)
                y: units.gu(0.7)

                spacing: units.gu(0.4)
                visible: root.actionTrayVisible
                z: 10

                Repeater {
                    model: [
                        { glyph: "☺", action: "react" },
                        { glyph: "↩", action: "reply" },
                        { glyph: "▢", action: "copy" },
                        { glyph: "⋯", action: "more" }
                    ]

                    delegate: Rectangle {
                        width: units.gu(4)
                        height: units.gu(4)
                        radius: units.gu(1)
                        color: actionMouse.pressed ? "#66849a" : "#354957"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.glyph
                            color: "white"
                            font.pixelSize: units.gu(2.3)
                        }

                        MouseArea {
                            id: actionMouse
                            anchors.fill: parent

                            onClicked: {
                                if (modelData.action === "react") {
                                    var anchor = messageActionTray.mapToItem(
                                        root, 0, 0
                                    )
                                    root.reactionPickerRequested(
                                        root.selectionId,
                                        anchor.x,
                                        anchor.y + messageActionTray.height
                                    )
                                } else {
                                    root.messageActionRequested(
                                        root.selectionId,
                                        modelData.action
                                    )
                                }
                            }
                        }
                    }
                }
            }

            Flow {
                id: reactionFlow
                x: units.gu(1)
            y: parent.height - units.gu(1.5)
            width: parent.width - units.gu(2)
            height: implicitHeight
            z: 2
                spacing: units.gu(0.5)
                visible: reactionRepeater.count > 0

                Repeater {
                    id: reactionRepeater
                    model: root.parsedReactions()

                    delegate: Rectangle {
                        property var reaction: modelData

                        width: reactionLabel.implicitWidth + units.gu(1.6)
                        height: units.gu(4)
                        radius: units.gu(2)
                        color: reaction.is_own ? "#315f78" : "#354957"
                        border.width: reaction.is_own ? 1 : 0
                        border.color: "#80b9e8"

                        Text {
                            id: reactionLabel
                            anchors.centerIn: parent
                            text: (reaction.key || "") + " " +
                                  String(reaction.count || 0)
                            color: "white"
                            font.pixelSize: units.gu(1.6)
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (reaction.key && root.selectionId) {
                                    root.reactionRequested(
                                        root.selectionId,
                                        reaction.key
                                    )
                                }
                            }
                        }
                    }
                }
            }

            Timer {
                id: selectionToolbarTimer
                interval: 350
                repeat: false

                onTriggered: {
                    if (bodyText.selectedText.length > 0) {
                        root.selectionToolbarVisible = true
                        root.selectionRequested(root.selectionId)
                    } else {
                        root.selectionToolbarVisible = false
                        root.selectionCleared(root.selectionId)
                    }
                }
            }

            Rectangle {
                id: selectionToolbar

                anchors.right: parent.right
                y: units.gu(0.5)

                width: units.gu(25)
                height: units.gu(6)
                radius: units.gu(1.5)
                color: "#354b5b"
                border.color: "#587184"
                visible: root.selectionToolbarVisible
                z: 20

                Row {
                    anchors.fill: parent
                    anchors.margins: units.gu(0.5)
                    spacing: units.gu(0.5)

                    Repeater {
                        model: [
                            { label: "Copy", action: "copy" },
                            { label: "Paste", action: "paste" },
                            { label: "Select All", action: "selectAll" }
                        ]

                        delegate: Rectangle {
                            width: (parent.width - units.gu(1)) / 3
                            height: parent.height
                            radius: units.gu(1)
                            color: selectionActionMouse.pressed
                                   ? "#526e82" : "transparent"

                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: "white"
                                font.pixelSize: units.gu(1.5)
                            }

                            MouseArea {
                                id: selectionActionMouse
                                anchors.fill: parent

                                onClicked: {
                                    if (modelData.action === "copy") {
                                        bodyText.copy()
                                        root.clearTextSelection()
                                    } else if (modelData.action === "paste") {
                                        if (bodyText.selectedText.length > 0) {
                                            bodyText.copy()
                                        } else {
                                            bodyText.selectAll()
                                            bodyText.copy()
                                        }

                                        root.pasteRequested()
                                        root.clearTextSelection()
                                    } else if (modelData.action === "selectAll") {
                                        bodyText.selectAll()
                                        root.selectionToolbarVisible =
                                            bodyText.text.length > 0
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }


}