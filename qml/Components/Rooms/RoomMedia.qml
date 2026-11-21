import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property var backend
    property string roomId: ""

    property var imageCache: ({})
    property var imageRequests: ({})
    property var thumbnailCache: ({})
    property var thumbnailRequests: ({})

    property string viewedImageUrl: ""
    property string viewedEventId: ""
    property string viewedMediaSourceJson: ""
    property string viewedMimeType: "application/octet-stream"
    property string viewedFileName: "media"
    property bool imageViewerOpen: false


    signal imageReady(string targetRoomId, string eventId, string dataUrl)
    signal thumbnailReady(string targetRoomId, string eventId, string dataUrl)

    // =====================================
    // Image cache and requests
    // =====================================

    function messageImageKey(targetRoomId, eventId) {
        if (!targetRoomId || !eventId)
            return ""

        return JSON.stringify([targetRoomId, eventId])
    }

    function cachedMessageImage(targetRoomId, eventId) {
        var key = root.messageImageKey(targetRoomId, eventId)
        return key !== "" ? (root.imageCache[key] || "") : ""
    }

    function cachedMessageThumbnail(targetRoomId, eventId) {
        var key = root.messageImageKey(targetRoomId, eventId)
        return key !== "" ? (root.thumbnailCache[key] || "") : ""
    }

    function requestMessageThumbnail(eventId, mediaSource, width, height) {
        if (!root.backend || !root.roomId || !eventId || !mediaSource)
            return

        var key = root.messageImageKey(root.roomId, eventId)

        if (root.thumbnailCache[key] || root.thumbnailRequests[key])
            return

        var sourceJson = JSON.stringify(mediaSource)
        if (!sourceJson)
            return

        var requests = Object.assign({}, root.thumbnailRequests)
        requests[key] = {
            roomId: root.roomId,
            eventId: eventId
        }
        root.thumbnailRequests = requests

        var targetWidth = Number(width) || 480
        var targetHeight = Number(height) || 270

        targetWidth = Math.max(1, Math.min(1024, Math.round(targetWidth)))
        targetHeight = Math.max(1, Math.min(1024, Math.round(targetHeight)))

        root.backend.requestMessageThumbnail(
            key,
            sourceJson,
            targetWidth,
            targetHeight
        )
    }

    function storeMessageThumbnail(requestKey, dataUrl) {
        var request = root.thumbnailRequests[requestKey]
        if (!request)
            return

        var requests = Object.assign({}, root.thumbnailRequests)
        delete requests[requestKey]
        root.thumbnailRequests = requests

        if (!dataUrl)
            return

        var key = root.messageImageKey(request.roomId, request.eventId)
        var cache = Object.assign({}, root.thumbnailCache)
        cache[key] = dataUrl
        root.thumbnailCache = cache

        root.thumbnailReady(request.roomId, request.eventId, dataUrl)
    }

    function requestMessageImage(eventId, mediaSource) {
        if (!root.backend || !root.roomId || !eventId || !mediaSource)
            return

        var key = root.messageImageKey(root.roomId, eventId)

        if (root.imageCache[key] || root.imageRequests[key])
            return

        var sourceJson = JSON.stringify(mediaSource)
        if (!sourceJson)
            return

        var requests = Object.assign({}, root.imageRequests)
        requests[key] = {
            roomId: root.roomId,
            eventId: eventId
        }
        root.imageRequests = requests

        root.backend.requestMessageImage(key, sourceJson)
    }

    function storeMessageImage(requestKey, dataUrl) {
        var request = root.imageRequests[requestKey]
        if (!request)
            return

        var requests = Object.assign({}, root.imageRequests)
        delete requests[requestKey]
        root.imageRequests = requests

        if (!dataUrl)
            return

        var key = root.messageImageKey(
            request.roomId,
            request.eventId
        )

        var cache = Object.assign({}, root.imageCache)
        cache[key] = dataUrl
        root.imageCache = cache

        root.imageReady(
            request.roomId,
            request.eventId,
            dataUrl
        )
    }

    Connections {
        target: root.backend

        onMessageImageRetrieved: {
            root.storeMessageImage(requestKey, dataUrl)
        }

        onMessageImageFailed: {
            var requests = Object.assign({}, root.imageRequests)
            delete requests[requestKey]
            root.imageRequests = requests

            console.log(
                "ASTRO: Message image download failed:",
                error
            )
        }

        onMessageThumbnailRetrieved: {
            root.storeMessageThumbnail(requestKey, dataUrl)
        }

        onMessageThumbnailFailed: {
            var requests = Object.assign({}, root.thumbnailRequests)
            delete requests[requestKey]
            root.thumbnailRequests = requests

            console.log(
                "ASTRO: Message thumbnail download failed:",
                error
            )
        }
    }

    function openImageViewer(
        imageUrl,
        eventId,
        mediaSourceJson,
        mimeType,
        fileName
    ) {
        if (!imageUrl)
            return

        root.viewedImageUrl = imageUrl
        root.viewedEventId = eventId || ""
        root.viewedMediaSourceJson = mediaSourceJson || ""
        root.viewedMimeType = mimeType || "application/octet-stream"
        root.viewedFileName = fileName || "media"

        root.imageViewerOpen = true
        imageViewer.resetView()
    }

    function closeImageViewer() {
        root.imageViewerOpen = false
        root.viewedImageUrl = ""
        root.viewedEventId = ""
        root.viewedMediaSourceJson = ""
        root.viewedMimeType = "application/octet-stream"
        root.viewedFileName = "media"

        imageViewer.resetView()
    }

    // =====================================
    // Image viewer presentation
    // =====================================

    ImageViewer {
        id: imageViewer

        anchors.fill: parent
        z: 1000

        imageUrl: root.viewedImageUrl
        opened: root.imageViewerOpen
        backend: root.backend

        mediaSourceJson: root.viewedMediaSourceJson
        mediaMimeType: root.viewedMimeType
        mediaFileName: root.viewedFileName

        onCloseRequested: root.closeImageViewer()
    }

}
