import QtQuick 2.7
import Lomiri.Components 1.3
import "emoji-data.js" as EmojiData

Rectangle {
    id: picker

    signal reactionSelected(string reactionKey)
    signal dismissed()

    property string pickerTitle: "React to message"
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
        { key: "people", icon: "🧑", label: "People" },
        { key: "hands", icon: "👋", label: "Hands" },
        { key: "hearts", icon: "❤️", label: "Hearts" },
        { key: "animals", icon: "🐱", label: "Animals" },
        { key: "nature", icon: "🌿", label: "Nature" },
        { key: "food", icon: "🍕", label: "Food" },
        { key: "travel", icon: "🚀", label: "Travel" },
        { key: "activities", icon: "⚽", label: "Activities" },
        { key: "objects", icon: "💡", label: "Objects" },
        { key: "symbols", icon: "🔣", label: "Symbols" },
        { key: "flags", icon: "🏳️", label: "Flags" }
    ]

    property var emojiList: EmojiData.emojiList

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
                text: picker.pickerTitle
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

        GridView {
            id: emojiScroll

            width: parent.width
            height: parent.height - units.gu(14)

            cellWidth: width / 7
            cellHeight: units.gu(4.5)

            clip: true
            model: picker.filteredEmojis()

            delegate: Rectangle {
                width: emojiScroll.cellWidth
                height: emojiScroll.cellHeight

                radius: units.gu(0.8)
                color: emojiMouse.pressed
                       ? "#526e82" : "transparent"

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
