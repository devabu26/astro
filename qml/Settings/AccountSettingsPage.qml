import QtQuick 2.7
import Lomiri.Components 1.3

Item {
    id: root

    property var backend

    property string matrixUserId: ""
    property string matrixDisplayName: ""
    property string matrixAvatarData: ""
    property string profileError: ""
    property bool profileLoading: false

    function refreshAccountProfile() {
        if (!root.backend)
            return

        root.profileLoading = true
        root.profileError = ""
        root.backend.refreshMatrixAccountProfile()
    }

    Component.onCompleted: refreshAccountProfile()

    onVisibleChanged: {
        if (visible)
            refreshAccountProfile()
    }

    Connections {
        target: root.backend

        onMatrixAccountProfileRetrieved: {
            root.profileLoading = false
            root.profileError = ""

            if (error !== "") {
                root.profileError = error
                return
            }

            try {
                var profile = JSON.parse(json || "{}")
                root.matrixUserId = profile.user_id || ""
                root.matrixDisplayName =
                    profile.display_name || root.matrixUserId
                root.matrixAvatarData = ""

                if (profile.avatar_url && root.backend)
                    root.backend.requestSenderAvatar(
                        "account-profile",
                        profile.avatar_url
                    )
            } catch (parseError) {
                root.profileError = "Could not read your Matrix account profile."
                console.warn("ASTRO: Could not parse Matrix account profile:", parseError)
            }
        }

        onSenderAvatarRetrieved: {
            if (requestKey === "account-profile")
                root.matrixAvatarData = dataUrl
        }

        onSenderAvatarFailed: {
            if (requestKey === "account-profile")
                root.matrixAvatarData = ""
        }

        onMatrixLogoutSucceeded: {
            root.matrixUserId = ""
            root.matrixDisplayName = ""
            root.matrixAvatarData = ""
            root.profileError = ""
        }

        onMatrixLoginSucceeded: root.refreshAccountProfile()
        onMatrixSessionRestored: {
            if (restored)
                root.refreshAccountProfile()
        }
        onStorageReadyChanged: {
            if (root.backend && root.backend.storageReady)
                root.refreshAccountProfile()
        }
    }

    Column {
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }

        spacing: units.gu(2)

        Rectangle {
            width: parent.width
            height: Math.max(units.gu(17), accountProfileContent.height + units.gu(4))
            radius: units.gu(2)
            color: "#1b2935"

            Row {
                id: accountProfileContent
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: units.gu(2)
                }
                spacing: units.gu(2)

                Rectangle {
                    width: units.gu(9)
                    height: width
                    radius: width / 2
                    color: "#344451"
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: root.matrixAvatarData
                        visible: source !== ""
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: root.matrixAvatarData === ""
                        text: root.matrixDisplayName.length > 0
                              ? root.matrixDisplayName.charAt(0).toUpperCase()
                              : "?"
                        color: "white"
                        fontSize: "x-large"
                    }
                }

                Column {
                    width: parent.width - units.gu(11)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: units.gu(0.7)

                    Label {
                        text: "Matrix account"
                        color: "#a78bfa"
                        fontSize: "small"
                    }

                    Label {
                        width: parent.width
                        text: root.matrixDisplayName !== ""
                              ? root.matrixDisplayName
                              : (root.profileLoading
                                 ? "Loading account…"
                                 : "Account profile unavailable")
                        color: "white"
                        fontSize: "large"
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        width: parent.width
                        text: root.matrixUserId
                        color: "#91a4b5"
                        wrapMode: Text.WordWrap
                        visible: text !== ""
                    }

                    Label {
                        width: parent.width
                        text: root.profileError
                        color: "#ff8585"
                        wrapMode: Text.WordWrap
                        visible: text !== ""
                    }

                    Button {
                        text: root.profileLoading
                              ? "Refreshing…"
                              : "Refresh account profile"
                        color: "#385267"
                        enabled: !root.profileLoading
                        onClicked: root.refreshAccountProfile()
                    }
                }
            }
        }

        Label {
            text: "SESSION"
            color: "#a78bfa"
            fontSize: "small"
        }

        Button {
            width: parent.width
            text: "Log out of Matrix"
            color: UbuntuColors.orange

            onClicked: {
                if (root.backend)
                    root.backend.logoutMatrix()
            }
        }

        Label {
            text: "Logging out ends your current Matrix session."
            color: "#91a4b5"
            wrapMode: Text.WordWrap
        }
    }
}