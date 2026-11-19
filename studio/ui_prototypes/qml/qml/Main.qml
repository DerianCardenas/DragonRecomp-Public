import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    visible: true
    visibility: Window.FullScreen
    width: 1360
    height: 840
    title: "DragonRecomp Studio — Prototipo QML"
    color: Theme.bgBase

    property string currentView: "library"
    property string activeConsole: "nes"
    property int compileCores: 2
    property string compileOpt: "-O1"
    property int selectedIndex: 0

    ListModel {
        id: mockGames
        ListElement { consoleKey: "nes";  code: "SMB3"; title: "Super Mario Bros. 3"; meta: "4H 12M · 4.2 MB" }
        ListElement { consoleKey: "nes";  code: "CTRA"; title: "Contra";              meta: "1H 45M · 3.1 MB" }
        ListElement { consoleKey: "nes";  code: "ZLD1"; title: "The Legend of Zelda"; meta: "6H 05M · 3.4 MB" }
        ListElement { consoleKey: "snes"; code: "SMW";  title: "Super Mario World";   meta: "8H 30M · 5.8 MB" }
        ListElement { consoleKey: "gb";   code: "TTRS"; title: "Tetris (DX)";         meta: "2H 10M · 2.8 MB" }
        ListElement { consoleKey: "gba";  code: "PKFR"; title: "Pokemon FireRed";     meta: "9H 15M · 9.4 MB" }
    }

    function colorForConsole(key) {
        if (key === "nes")  return Theme.nes
        if (key === "snes") return Theme.snes
        if (key === "gb")   return Theme.gb
        return Theme.gba
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Left Navigation Sidebar (Flat Solid Surface) ──────────────────
        Rectangle {
            Layout.preferredWidth: 248
            Layout.fillHeight: true
            color: Theme.bgPanel

            // Right 1px crisp divider
            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: Theme.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 10

                // Brand Header
                RowLayout {
                    spacing: 12
                    Layout.bottomMargin: 18
                    Rectangle {
                        width: 34; height: 34; radius: 8
                        color: Theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: "DR"
                            color: Theme.accentInk
                            font.bold: true
                            font.pixelSize: 15
                        }
                    }
                    ColumnLayout {
                        spacing: 0
                        Text { text: "DRAGONRECOMP"; color: Theme.textPrimary; font.bold: true; font.pixelSize: 15; font.letterSpacing: 0.8 }
                        Text { text: "STUDIO · QML"; color: Theme.accent; font.bold: true; font.pixelSize: 11; font.letterSpacing: 1.2 }
                    }
                }

                // Primary Section Buttons
                Repeater {
                    model: [
                        { id: "library",    label: "Biblioteca" },
                        { id: "recompiler", label: "Recompilador" },
                        { id: "settings",   label: "Configuración" }
                    ]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 44
                        radius: 6
                        color: win.currentView === modelData.id ? Theme.accent : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            text: modelData.label
                            color: win.currentView === modelData.id ? Theme.accentInk : Theme.textSecondary
                            font.bold: true
                            font.pixelSize: 14
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: win.currentView = modelData.id
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; Layout.topMargin: 12; Layout.bottomMargin: 8 }

                Text {
                    text: "CONSOLAS"
                    color: Theme.textMuted
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 1.2
                    Layout.leftMargin: 6
                }

                Repeater {
                    model: [
                        { key: "nes",  name: "Nintendo NES" },
                        { key: "snes", name: "Super Nintendo" },
                        { key: "gb",   name: "Game Boy / Color" },
                        { key: "gba",  name: "Game Boy Advance" }
                    ]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 40
                        radius: 6
                        color: (win.currentView === "library" && win.activeConsole === modelData.key)
                               ? Theme.bgCard : "transparent"
                        border.width: (win.currentView === "library" && win.activeConsole === modelData.key) ? 1 : 0
                        border.color: win.colorForConsole(modelData.key)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            spacing: 10
                            Rectangle { width: 8; height: 8; radius: 2; color: win.colorForConsole(modelData.key) }
                            Text {
                                text: modelData.name
                                color: (win.activeConsole === modelData.key) ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: 13
                                font.bold: win.activeConsole === modelData.key
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                win.activeConsole = modelData.key
                                win.currentView = "library"
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Resource status pill at bottom of sidebar
                Rectangle {
                    Layout.fillWidth: true
                    height: 52
                    radius: 8
                    color: Theme.bgCard
                    border.width: 1
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 2
                        Text {
                            text: "MOTOR DE COMPILACIÓN"
                            color: Theme.textMuted
                            font.pixelSize: 10
                            font.bold: true
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Text {
                            text: win.compileCores + " Núcleos · " + win.compileOpt
                            color: Theme.accent
                            font.family: "JetBrains Mono"
                            font.pixelSize: 12
                            font.bold: true
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }
            }
        }

        // ── Right Main Content Area ───────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // LIBRARY VIEW
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 22
                visible: win.currentView === "library"

                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 4
                        Text { text: "Biblioteca Portable"; color: Theme.textPrimary; font.pixelSize: 28; font.bold: true }
                        Text { text: "Juegos recompilados listos para ejecutar de forma nativa."; color: Theme.textSecondary; font.pixelSize: 14 }
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        width: 210; height: 42; radius: 6; color: Theme.accent
                        Text { anchors.centerIn: parent; text: "+ Importar Recompilado"; color: Theme.accentInk; font.bold: true; font.pixelSize: 13 }
                    }
                }

                // Quick-Launch Flat Banner (No Gradients)
                Rectangle {
                    Layout.fillWidth: true
                    height: 92
                    radius: 10
                    color: Theme.bgPanel
                    border.width: 1
                    border.color: Theme.border
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 18
                        Rectangle { width: 6; Layout.fillHeight: true; radius: 3; color: win.colorForConsole(win.activeConsole) }
                        ColumnLayout {
                            spacing: 4
                            Text { text: "CONTINUAR JUGANDO"; color: Theme.accent; font.pixelSize: 11; font.bold: true; font.letterSpacing: 1.0 }
                            Text { text: "Super Mario Bros. 3"; color: Theme.textPrimary; font.pixelSize: 20; font.bold: true }
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            width: 170; height: 44; radius: 6; color: Theme.accent
                            Text { anchors.centerIn: parent; text: "▶ JUGAR AHORA"; color: Theme.accentInk; font.bold: true; font.pixelSize: 14 }
                        }
                    }
                }

                GridView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cellWidth: 212
                    cellHeight: 322
                    clip: true
                    model: mockGames
                    delegate: GameCard {
                        visible: model.consoleKey === win.activeConsole
                        title: model.title
                        code: model.code
                        meta: model.meta
                        consoleColor: win.colorForConsole(model.consoleKey)
                        selected: index === win.selectedIndex
                        onClicked: win.selectedIndex = index
                    }
                }
            }

            // RECOMPILER VIEW
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 22
                visible: win.currentView === "recompiler"

                Text { text: "Recompilador Estático"; color: Theme.textPrimary; font.pixelSize: 28; font.bold: true }
                Rectangle {
                    Layout.fillWidth: true
                    height: 220
                    radius: 12
                    color: Theme.bgPanel
                    border.width: 2
                    border.color: Theme.borderStrong
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 12
                        Text { text: "Arrastra tu ROM (.nes, .sfc, .gb, .gba)"; color: Theme.textPrimary; font.pixelSize: 20; font.bold: true; Layout.alignment: Qt.AlignHCenter }
                        Text { text: "Compilación optimizada con " + win.compileCores + " núcleos (" + win.compileOpt + ")"; color: Theme.textSecondary; font.pixelSize: 14; Layout.alignment: Qt.AlignHCenter }
                    }
                }
                Item { Layout.fillHeight: true }
            }

            // SETTINGS VIEW
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 20
                visible: win.currentView === "settings"

                Text { text: "Configuración"; color: Theme.textPrimary; font.pixelSize: 28; font.bold: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 720
                    height: 84
                    radius: 10
                    color: Theme.bgPanel
                    border.width: 1
                    border.color: Theme.border
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        ColumnLayout {
                            spacing: 4
                            Text { text: "Núcleos de compilación"; color: Theme.textPrimary; font.pixelSize: 15; font.bold: true }
                            Text { text: "Por defecto: 2 núcleos para mantener el sistema fluido."; color: Theme.textSecondary; font.pixelSize: 13 }
                        }
                        Item { Layout.fillWidth: true }
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                width: 38; height: 38; radius: 6; color: Theme.bgCard; border.color: Theme.border
                                Text { anchors.centerIn: parent; text: "-"; color: Theme.textPrimary; font.bold: true }
                                MouseArea { anchors.fill: parent; onClicked: win.compileCores = Math.max(1, win.compileCores - 1) }
                            }
                            Rectangle {
                                width: 52; height: 38; radius: 6; color: Theme.bgInput; border.color: Theme.accent
                                Text { anchors.centerIn: parent; text: win.compileCores; color: Theme.accent; font.bold: true; font.family: "JetBrains Mono" }
                            }
                            Rectangle {
                                width: 38; height: 38; radius: 6; color: Theme.bgCard; border.color: Theme.border
                                Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.bold: true }
                                MouseArea { anchors.fill: parent; onClicked: win.compileCores = Math.min(16, win.compileCores + 1) }
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
