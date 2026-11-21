import QtQuick 2.7
import Lomiri.Components 1.3

Column {
    id: root

    property string documentTitle: ""
    property url documentUrl: ""
    property string documentText: ""
    property string errorMessage: ""
    property bool loading: false
    property var activeRequest: null

    width: parent ? parent.width : units.gu(40)
    spacing: units.gu(1)

    function loadDocument() {
        if (!documentUrl || documentUrl.toString() === "")
            return

        loading = true
        errorMessage = ""
        documentText = ""

        var request = new XMLHttpRequest()
        activeRequest = request

        request.onreadystatechange = function() {
            if (request.readyState !== 4)
                return

            if (activeRequest !== request)
                return

            loading = false

            if (request.status === 0
                    || (request.status >= 200 && request.status < 300)) {
                documentText = request.responseText
            } else {
                errorMessage = "Unable to load this document."
            }

            activeRequest = null
        }

        request.open("GET", documentUrl)
        request.send()
    }

    onDocumentUrlChanged: loadDocument()

    Component.onCompleted: loadDocument()

    Text {
        width: parent.width
        text: root.documentTitle
        color: "#f4f6f8"
        font.pixelSize: units.gu(2.4)
        font.bold: true
        visible: root.documentTitle !== ""
    }

    Text {
        width: parent.width
        text: root.loading ? "Loading document..." : root.errorMessage
        color: root.errorMessage !== "" ? "#e58b91" : "#a8b7c5"
        font.pixelSize: units.gu(1.5)
        wrapMode: Text.WordWrap
        visible: root.loading || root.errorMessage !== ""
    }
    Flickable {
        id: documentViewport

        width: parent.width
        height: units.gu(55)

        clip: true
        interactive: true
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds

        contentWidth: width
        contentHeight: Math.max(documentContent.implicitHeight, height)

        Text {
            id: documentContent

            width: documentViewport.width

            text: root.documentText
            color: "#dce4eb"
            font.family: "monospace"
            font.pixelSize: units.gu(1.45)
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
    }
}
