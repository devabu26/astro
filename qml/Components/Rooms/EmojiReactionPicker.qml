import QtQuick 2.7
import Lomiri.Components 1.3

Rectangle {
    id: picker

    signal reactionSelected(string reactionKey)
    signal dismissed()

    property string searchText: ""
    property string selectedCategory: "all"

    width: Math.min(units.gu(38), parent ? parent.width - units.gu(2) : units.gu(38))
    height: units.gu(32)

    radius: units.gu(2)
    color: "#1e303d"
    border.width: 1
    border.color: "#587184"
    clip: true

    property var categories: [
        { key: "all", icon: "✨", label: "All" },
        { key: "faces", icon: "😀", label: "Faces" },
        { key: "hands", icon: "👋", label: "Hands" },
        { key: "hearts", icon: "❤️", label: "Hearts" },
        { key: "animals", icon: "🐱", label: "Animals" },
        { key: "food", icon: "🍕", label: "Food" },
        { key: "activities", icon: "⚽", label: "Activities" },
        { key: "symbols", icon: "💡", label: "Symbols" }
    ]

    property var emojiList: [
        { emoji: "😀", name: "grinning", category: "faces" },
        { emoji: "😃", name: "smiley", category: "faces" },
        { emoji: "😄", name: "smile", category: "faces" },
        { emoji: "😁", name: "grin", category: "faces" },
        { emoji: "😆", name: "laughing", category: "faces" },
        { emoji: "😂", name: "joy", category: "faces" },
        { emoji: "🤣", name: "rofl", category: "faces" },
        { emoji: "😊", name: "blush", category: "faces" },
        { emoji: "😍", name: "heart eyes", category: "faces" },
        { emoji: "🥰", name: "love", category: "faces" },
        { emoji: "😘", name: "kiss", category: "faces" },
        { emoji: "😎", name: "cool", category: "faces" },
        { emoji: "🤔", name: "thinking", category: "faces" },
        { emoji: "🤨", name: "raised eyebrow", category: "faces" },
        { emoji: "😢", name: "cry", category: "faces" },
        { emoji: "😭", name: "sob", category: "faces" },
        { emoji: "😡", name: "angry", category: "faces" },
        { emoji: "🤯", name: "mind blown", category: "faces" },
        { emoji: "🥳", name: "party", category: "faces" },
        { emoji: "🤩", name: "star struck", category: "faces" },

        { emoji: "👍", name: "thumbs up", category: "hands" },
        { emoji: "👎", name: "thumbs down", category: "hands" },
        { emoji: "👏", name: "clap", category: "hands" },
        { emoji: "🙌", name: "raised hands", category: "hands" },
        { emoji: "🙏", name: "pray", category: "hands" },
        { emoji: "🤝", name: "handshake", category: "hands" },
        { emoji: "👋", name: "wave", category: "hands" },
        { emoji: "💪", name: "strong", category: "hands" },
        { emoji: "🤞", name: "fingers crossed", category: "hands" },
        { emoji: "✌️", name: "peace", category: "hands" },

        { emoji: "❤️", name: "red heart", category: "hearts" },
        { emoji: "🧡", name: "orange heart", category: "hearts" },
        { emoji: "💛", name: "yellow heart", category: "hearts" },
        { emoji: "💚", name: "green heart", category: "hearts" },
        { emoji: "💙", name: "blue heart", category: "hearts" },
        { emoji: "💜", name: "purple heart", category: "hearts" },
        { emoji: "🖤", name: "black heart", category: "hearts" },
        { emoji: "💔", name: "broken heart", category: "hearts" },
        { emoji: "💕", name: "two hearts", category: "hearts" },
        { emoji: "💖", name: "sparkling heart", category: "hearts" },

        { emoji: "🐱", name: "cat", category: "animals" },
        { emoji: "🐶", name: "dog", category: "animals" },
        { emoji: "🦊", name: "fox", category: "animals" },
        { emoji: "🐸", name: "frog", category: "animals" },
        { emoji: "🐼", name: "panda", category: "animals" },
        { emoji: "🦄", name: "unicorn", category: "animals" },

        { emoji: "🍕", name: "pizza", category: "food" },
        { emoji: "🍔", name: "burger", category: "food" },
        { emoji: "🍟", name: "fries", category: "food" },
        { emoji: "🍩", name: "donut", category: "food" },
        { emoji: "☕", name: "coffee", category: "food" },
        { emoji: "🍺", name: "beer", category: "food" },
        { emoji: "🍰", name: "cake", category: "food" },

        { emoji: "⚽", name: "football", category: "activities" },
        { emoji: "🏀", name: "basketball", category: "activities" },
        { emoji: "🎮", name: "gaming", category: "activities" },
        { emoji: "🎉", name: "celebration", category: "activities" },
        { emoji: "🎵", name: "music", category: "activities" },
        { emoji: "🏆", name: "trophy", category: "activities" },

        { emoji: "🔥", name: "fire", category: "symbols" },
        { emoji: "✨", name: "sparkles", category: "symbols" },
        { emoji: "⭐", name: "star", category: "symbols" },
        { emoji: "💯", name: "hundred", category: "symbols" },
        { emoji: "❗", name: "exclamation", category: "symbols" },
        { emoji: "❓", name: "question", category: "symbols" },
        { emoji: "✅", name: "check", category: "symbols" },
        { emoji: "🚀", name: "rocket", category: "symbols" }
    ]

    function filteredEmojis() {
        var result = []
        var query = searchText.toLowerCase()

        for (var i = 0; i < emojiList.length; i++) {
            var item = emojiList[i]

            if (selectedCategory !== "all" &&
                    item.category !== selectedCategory)
                continue

            if (query !== "" &&
                    item.name.toLowerCase().indexOf(query) < 0 &&
                    item.emoji.indexOf(query) < 0)
                continue

            result.push(item)
        }

        return result
    }

    Column {
        anchors.fill: parent
        anchors.margins: units.gu(1)
        spacing: units.gu(0.8)

        Row {
            width: parent.width
            height: units.gu(4)

            Text {
                width: parent.width - units.gu(5)
                anchors.verticalCenter: parent.verticalCenter
                text: "React to message"
                color: "#ffffff"
                font.bold: true
                font.pixelSize: units.gu(1.8)
            }

            Text {
                width: units.gu(5)
                height: parent.height
                text: "×"
                color: "#a9bac6"
                font.pixelSize: units.gu(3)
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter

                MouseArea {
                    anchors.fill: parent
                    onClicked: picker.dismissed()
                }
            }
        }

        Rectangle {
            width: parent.width
            height: units.gu(4)
            radius: units.gu(1)
            color: "#263744"

            TextInput {
                id: searchInput
                anchors.fill: parent
                anchors.margins: units.gu(1)

                color: "#ffffff"
                font.pixelSize: units.gu(1.7)
                verticalAlignment: TextInput.AlignVCenter
                clip: true

                onTextChanged: picker.searchText = text

                Text {
                    anchors.fill: parent
                    text: "Search emoji"
                    color: "#91a5b3"
                    font.pixelSize: units.gu(1.7)
                    verticalAlignment: Text.AlignVCenter
                    visible: searchInput.text.length === 0
                }
            }
        }

        Flickable {
            width: parent.width
            height: units.gu(4.5)
            contentWidth: categoryRow.width
            contentHeight: height
            clip: true

            Row {
                id: categoryRow
                height: parent.height
                spacing: units.gu(0.5)

                Repeater {
                    model: picker.categories

                    delegate: Rectangle {
                        width: units.gu(5)
                        height: units.gu(4)
                        radius: units.gu(1)
                        color: picker.selectedCategory === modelData.key
                               ? "#315f78" : "#263744"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.icon
                            font.pixelSize: units.gu(2.3)
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: picker.selectedCategory = modelData.key
                        }
                    }
                }
            }
        }

        Flickable {
            id: emojiScroll
            width: parent.width
            height: parent.height - units.gu(14)
            contentWidth: width
            contentHeight: emojiFlow.height
            clip: true

            Flow {
                id: emojiFlow
                width: emojiScroll.width
                spacing: units.gu(0.3)

                Repeater {
                    model: picker.filteredEmojis()

                    delegate: Rectangle {
                        width: (emojiFlow.width - units.gu(1.2)) / 7
                        height: units.gu(4.5)
                        radius: units.gu(0.8)
                        color: emojiMouse.pressed ? "#526e82" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: modelData.emoji
                            font.pixelSize: units.gu(2.7)
                        }

                        MouseArea {
                            id: emojiMouse
                            anchors.fill: parent

                            onClicked: picker.reactionSelected(modelData.emoji)
                        }
                    }
                }
            }
        }
    }
}
