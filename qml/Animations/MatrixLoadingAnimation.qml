
import QtQuick 2.7

// =============================================================
// Matrix loading animation
//
// A small diamond appears in the centre of the ChatPage,
// travels to the sidebar loading strip, and pulses there
// until Matrix room synchronisation finishes.
// =============================================================

Item {
    id: root

    // The parent controls when the animation starts and stops.
    property bool active: false
    property bool exitPending: false
    property bool resetting: false

    // Destination is the centre point of the diamond,
    // expressed in this component's coordinates.
    property real targetX: units.gu(4)
    property real targetY: units.gu(12)

    // Keep the animation above the rest of the ChatPage.
    anchors.fill: parent
    z: 100

    visible: false
    opacity: 1

    // =========================================================
    // Diamond artwork
    // =========================================================

    Item {
        id: diamond

        width: units.gu(7)
        height: units.gu(7)

        // The diamond starts in the centre of the screen.
        x: (root.width - width) / 2
        y: (root.height - height) / 2

        // Soft outer glow.
        Rectangle {
            anchors.centerIn: parent

            width: units.gu(6)
            height: units.gu(6)

            rotation: 45
            color: "#397d88"
            opacity: 0.35
            radius: units.gu(0.5)
        }

        // Main four-pointed diamond.
        Rectangle {
            anchors.centerIn: parent

            width: units.gu(3.5)
            height: units.gu(3.5)

            rotation: 45
            color: "#8ccae6"
            radius: units.gu(0.3)
        }
    }

    // =========================================================
    // Centre-to-sidebar travel animation
    // =========================================================

    ParallelAnimation {
        id: travelAnimation

        PropertyAnimation {
            target: diamond
            property: "x"
            to: root.targetX - diamond.width / 2
            duration: 1000
            easing.type: Easing.InOutCubic
        }

        PropertyAnimation {
            target: diamond
            property: "y"
            to: root.targetY - diamond.height / 2
            duration: 1000
            easing.type: Easing.InOutCubic
        }

        PropertyAnimation {
            target: diamond
            property: "scale"
            to: 0.65
            duration: 1000
            easing.type: Easing.InOutCubic
        }

        onStopped: {
            if (root.resetting)
                return

            if (root.active) {
                pulseAnimation.start()
            } else if (root.exitPending) {
                root.exitPending = false
                exitAnimation.start()
            }
        }
    }

    // =========================================================
    // Gentle pulse while Matrix synchronises
    // =========================================================

    SequentialAnimation {
        id: pulseAnimation

        loops: Animation.Infinite

        PropertyAnimation {
            target: diamond
            property: "scale"
            to: 0.78
            duration: 700
            easing.type: Easing.InOutSine
        }

        PropertyAnimation {
            target: diamond
            property: "scale"
            to: 0.65
            duration: 700
            easing.type: Easing.InOutSine
        }
    }

    // =========================================================
    // Fade away when synchronisation finishes
    // =========================================================

    ParallelAnimation {
        id: exitAnimation

        PropertyAnimation {
            target: root
            property: "opacity"
            to: 0
            duration: 350
            easing.type: Easing.InOutQuad
        }

        PropertyAnimation {
            target: diamond
            property: "scale"
            to: 0.2
            duration: 350
            easing.type: Easing.InOutQuad
        }

        onStopped: {
            if (!root.active)
                root.visible = false
        }
    }

    // =========================================================
    // =========================================================
    // Animation recovery after page navigation
    // =========================================================

    function resumeIfNeeded() {
       if (!root.active || !root.parent || !root.parent.visible)
           return

       if (travelAnimation.running || pulseAnimation.running)
           return

       resetting = true
       exitAnimation.stop()
       exitPending = false

       visible = true
       opacity = 1

       diamond.x = targetX - diamond.width / 2
       diamond.y = targetY - diamond.height / 2
       diamond.scale = 0.65

       resetting = false
       pulseAnimation.start()
    }

    Timer {
       interval: 750
       repeat: true
       running: root.active && root.parent !== null && root.parent.visible
       onTriggered: root.resumeIfNeeded()
    }

    Connections {
       target: root.parent
       onVisibleChanged: {
           if (root.parent && root.parent.visible && root.active)
               Qt.callLater(root.resumeIfNeeded)
       }
    }

    // Animation state control
    // =========================================================

    onActiveChanged: {
        if (active) {
            // Prevent stopped callbacks from starting old animations.
            resetting = true

            travelAnimation.stop()
            pulseAnimation.stop()
            exitAnimation.stop()

            exitPending = false

            // Restore the animation's initial appearance and position.
            visible = true
            opacity = 1

            diamond.x = (width - diamond.width) / 2
            diamond.y = (height - diamond.height) / 2
            diamond.scale = 1

            resetting = false

            // Start after the current layout/navigation changes settle.
            Qt.callLater(function() {
                if (!root.active)
                    return

                if (travelAnimation.running ||
                    pulseAnimation.running ||
                    exitAnimation.running)
                    return

                // Recalculate the centre using the current dimensions.
                diamond.x = (root.width - diamond.width) / 2
                diamond.y = (root.height - diamond.height) / 2

                travelAnimation.start()
            })

        } else {
            resetting = true

            pulseAnimation.stop()

            resetting = false

            if (travelAnimation.running) {
                exitPending = true
            } else if (visible) {
                exitAnimation.start()
            }
        }
    }
}