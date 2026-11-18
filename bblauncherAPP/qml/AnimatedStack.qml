import QtQuick
import Launcher 1.0

/*  Cross-fades and slides between lazily built pages.

    sources: array of urls relative to this file, e.g. ["tabs/HomeTab.qml", ...]
*/
Item {
    id: root

    property var sources: []
    property int currentIndex: -1
    property int slide: 18

    Repeater {
        id: pages
        model: root.sources

        delegate: Loader {
            id: page
            width: parent.width
            height: parent.height
            asynchronous: false
            visible: opacity > 0.001
            enabled: root.currentIndex === index
            opacity: 0
            x: 0

            Behavior on opacity {
                NumberAnimation { duration: Theme.durPage; easing: Theme.easeOut }
            }
            Behavior on x {
                NumberAnimation { duration: Theme.durPage; easing: Theme.easeOutQuint }
            }
        }
    }

    property int _lastIndex: -1

    function activate(index) {
        if (index < 0 || index >= root.sources.length)
            return

        const previous = root._lastIndex
        root._lastIndex = index

        for (let i = 0; i < pages.count; ++i) {
            const p = pages.itemAt(i)
            if (!p)
                continue

            if (i === index) {
                if (!p.source || p.source.toString().length === 0)
                    p.source = Qt.resolvedUrl(root.sources[i])
                p.x = previous < 0 ? 0 : (i > previous ? root.slide : -root.slide)
                p.opacity = 0
                const loader = p
                Qt.callLater(function () {
                    loader.x = 0
                    loader.opacity = 1
                })
            } else if (p.opacity > 0) {
                p.x = i < index ? -root.slide : root.slide
                p.opacity = 0
            }
        }
    }

    onCurrentIndexChanged: activate(currentIndex)

    Component.onCompleted: {
        if (currentIndex >= 0)
            activate(currentIndex)
    }
}
