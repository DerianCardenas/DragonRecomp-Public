import QtQuick
import QtQuick.Layouts

Item {
    id: root
    property string title: ""
    property string code: ""
    property string meta: ""
    property color consoleColor: Theme.nes
    property bool selected: false
    signal clicked()

    width: 192
    height: 304

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Rectangle {
            id: coverBox
            Layout.fillWidth: true
            Layout.preferredHeight: 256
            radius: 10
            color: ma.containsMouse ? Theme.bgCardHover : Theme.bgCard
            border.width: (ma.containsMouse || root.selected) ? 2 : 1
            border.color: (ma.containsMouse || root.selected) ? Theme.accent : Theme.border

            Behavior on border.color { ColorAnimation { duration: 120 } }
            Behavior on color { ColorAnimation { duration: 120 } }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 0

                Rectangle {
                    width: 44
                    height: 6
                    radius: 2
                    color: root.consoleColor
                }

                Item { Layout.fillHeight: true }

                Text {
                    text: root.code
                    color: Theme.textPrimary
                    font.family: "JetBrains Mono"
                    font.pixelSize: 34
                    font.bold: true
                }

                Item { Layout.fillHeight: true }

                Text {
                    text: root.meta
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 0.8
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.title
            color: (ma.containsMouse || root.selected) ? Theme.accent : Theme.textPrimary
            font.pixelSize: 15
            font.bold: true
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.WordWrap
        }
    }
}
