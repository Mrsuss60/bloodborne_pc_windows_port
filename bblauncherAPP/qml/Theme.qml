pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ------------------------------------------------------------------ palette
    readonly property color bgDeep:    "#08080A"
    readonly property color bg:        "#0D0D11"
    readonly property color surface:   "#15151B"
    readonly property color surfaceHi: "#1B1B23"
    readonly property color surfaceAlt:"#101015"
    readonly property color border:    "#25252F"
    readonly property color borderHi:  "#3B3B49"
    readonly property color accent:    "#C21722"
    readonly property color accentHi:  "#E63B46"
    readonly property color accentGlow:"#66C21722"
    readonly property color accentSoft:"#2A1115"
    readonly property color gold:      "#D3B77B"
    readonly property color goldHi:    "#E9D3A2"
    readonly property color goldSoft:  "#2A2418"
    readonly property color text:      "#EDEDF2"
    readonly property color textMuted: "#9A9AA8"
    readonly property color textDim:   "#6C6C7A"
    readonly property color ok:        "#5BC98C"
    readonly property color okSoft:    "#12251B"
    readonly property color warn:      "#F0B429"
    readonly property color warnSoft:  "#2A2110"
    readonly property color err:       "#E5484D"
    readonly property color errSoft:   "#2A1113"
    readonly property color info:      "#6FA8FF"

    // ------------------------------------------------------------------ geometry
    readonly property int radiusSm: 6
    readonly property int radiusMd: 10
    readonly property int radiusLg: 16
    readonly property int gutter: 22       // page side padding
    readonly property int gap: 8           // between cards

    // ------------------------------------------------------------------ motion
    readonly property int durFast: 120
    readonly property int durMed: 200
    readonly property int durSlow: 320
    readonly property int durPage: 300

    readonly property int easeOut: Easing.OutCubic
    readonly property int easeOutQuint: Easing.OutQuint
    readonly property int easeInOut: Easing.InOutCubic
    readonly property int easeInQuad: Easing.InQuad

    // ------------------------------------------------------------------ type
    readonly property string fontUi: "Segoe UI"
    readonly property string fontDisplay: "Segoe UI"
    readonly property string fontMono: "Cascadia Mono"
    readonly property string fontSerif: "Georgia"
    readonly property string fontTitle: "Georgia"
    readonly property string fontNav: "Bahnschrift"

    readonly property int fsMicro: 10
    readonly property int fsSmall: 11
    readonly property int fsBody: 13
    readonly property int fsLarge: 15
    readonly property int fsTitle: 20

    // ------------------------------------------------------------------ helpers
    function toneColor(tone) {
        switch (tone) {
        case "ok":     return theme.ok
        case "warn":   return theme.warn
        case "err":    return theme.err
        case "accent": return theme.accentHi
        case "gold":   return theme.gold
        case "muted":  return theme.textMuted
        case "dim":    return theme.textDim
        default:       return theme.text
        }
    }

    function toneSoft(tone) {
        switch (tone) {
        case "ok":     return theme.okSoft
        case "warn":   return theme.warnSoft
        case "err":    return theme.errSoft
        case "accent": return theme.accentSoft
        case "gold":   return theme.goldSoft
        default:       return theme.surfaceHi
        }
    }
}
