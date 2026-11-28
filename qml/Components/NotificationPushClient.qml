import QtQuick 2.7
import Ubuntu.PushNotifications 0.1

Item {
    id: root

    signal notificationReceived(string payload)
    signal pushTokenAvailable(string token)

    function clearPersistent(tags) {
        if (tags && tags.length > 0)
            pushClient.clearPersistent(tags)
    }

    PushClient {
        id: pushClient
        appId: "astro.madabu_astro"

        Component.onCompleted: {
            getNotifications()
        }

        onTokenChanged: {
            if (pushClient.token && pushClient.token.length > 0) {
                root.pushTokenAvailable(pushClient.token)
            }
        }

        onNotificationsChanged: {
            for (var i = 0; i < notifications.length; i++) {
                root.notificationReceived(notifications[i])
            }
        }

        onError: {
            console.warn("ASTRO: Ubuntu push client error:", error)
        }
    }

    Connections {
        target: Qt.application

        onStateChanged: {
            if (Qt.application.state === Qt.ApplicationActive)
                pushClient.getNotifications()
        }
    }
}
