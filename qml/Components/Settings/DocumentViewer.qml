import QtQuick 2.7
import Lomiri.Components 1.3
import "MarkdownRenderer.js" as MarkdownRenderer

Column {
    id: root

    property string documentTitle: ""
    property url documentUrl: ""
    property string documentText: ""
    property string errorMessage: ""
    property bool loading: false
    property var activeRequest: null

    readonly property bool isMarkdown: documentTitle === "README"
    readonly property string renderedDocument: isMarkdown
        ? MarkdownRenderer.markdownToHtml(documentText)
        : documentText

    width: parent ? parent.width : units.gu(40)
    spacing: units.gu(1)

    function loadDocument() {
        if (!documentUrl || documentUrl.toString() === "")
            return

        if (activeRequest)
            activeRequest.abort()

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

    Component.onDestruction: {
        if (activeRequest)
            activeRequest.abort()
    }

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

    Text {
        id: documentContent

        width: parent.width
        text: root.renderedDocument
        color: "#dce4eb"
        font.family: "Ubuntu"
        font.pixelSize: units.gu(1.55)
        wrapMode: Text.Wrap
        textFormat: root.isMarkdown ? Text.RichText : Text.PlainText
        linkColor: "#b9a6ff"
        visible: !root.loading && root.errorMessage === ""
        onLinkActivated: {
            if (link.indexOf("https://") === 0
                    || link.indexOf("http://") === 0) {
                Qt.openUrlExternally(link)
            }
        }
    }
}
