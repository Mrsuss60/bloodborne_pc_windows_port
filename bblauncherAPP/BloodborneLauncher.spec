# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['bblauncher_app.py'],
    pathex=['.', '../scripts'],
    binaries=[],
    datas=[('icon.ico', '.'), ('resources', 'resources'), ('qml', 'qml')],
    hiddenimports=['bb_guard', 'game_check', 'prepare', 'PySide6.QtQml',
                   'PySide6.QtQuick', 'PySide6.QtQuickControls2'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['PySide6.QtWebEngineCore', 'PySide6.QtWebEngineWidgets', 'PySide6.Qt3DCore',
              'PySide6.QtMultimedia', 'PySide6.QtVirtualKeyboard', 'PySide6.QtPdf',
              'PySide6.QtPdfWidgets', 'PySide6.QtSvg', 'PySide6.QtOpenGLWidgets',
              'PySide6.QtSql', 'PySide6.QtTest', 'PySide6.QtXml',
              'PySide6.QtPrintSupport', 'PySide6.QtDesigner', 'PySide6.QtHelp',
              'PySide6.QtUiTools', 'PySide6.QtQuick3D', 'PySide6.QtQuickDialogs2',
              'PySide6.QtQuickParticles', 'tkinter', 'unittest', 'pydoc', 'doctest',
              'email', 'http'],
    noarchive=False,
    optimize=2,
)

# PySide6's hook collects every Qt6*.dll / plugin / QML module / translation
# in the package directory, including lots of things the launcher never uses.
# Trim aggressively but surgically: keep anything we cannot prove unused.
_UNNEEDED_QT_DLL_PREFIXES = (
    "Qt63D", "Qt6Charts", "Qt6DataVisualization", "Qt6Graphs",
    "Qt6Location", "Qt6Multimedia", "Qt6Pdf", "Qt6Positioning",
    "Qt6Bluetooth", "Qt6Nfc", "Qt6Sensors", "Qt6SerialBus", "Qt6SerialPort",
    "Qt6SpatialAudio", "Qt6TextToSpeech", "Qt6WebChannel", "Qt6WebEngine",
    "Qt6HttpServer", "Qt6Scxml", "Qt6StateMachine", "Qt6RemoteObjects",
    "Qt6Sql", "Qt6Test", "Qt6Svg", "Qt6VirtualKeyboard",
    "Qt6Quick3D", "Qt6QuickControls2Imagine", "Qt6QuickControls2Material",
    "Qt6QuickControls2Universal", "Qt6QuickControls2Fusion",
    "Qt6QuickDialogs2", "Qt6QuickParticles", "Qt6Quick3DXr",
    "Qt6ShaderTools", "Qt6QuickEffects", "Qt6QuickShapes",
    "Qt6QuickVectorImage", "Qt6LabsPlatform", "Qt6WebSockets",
    "Qt6QuickTest", "Qt6QmlXmlListModel", "Qt6QuickTimeline",
    "Qt6WebView", "Qt6OpenGLWidgets",
)

# Directory names (anywhere under PySide6\qml) that are never needed.
_UNNEEDED_QML_DIRS = {
    "QtQuick3D", "Qt3D", "QtGraphs", "QtCharts", "QtDataVisualization",
    "QtWebEngine", "QtTextToSpeech", "QtTest", "QtSensors", "QtLocation",
    "QtMultimedia", "QtWebSockets", "QtPositioning", "QtWebChannel",
    "QtWebView", "QtScxml", "QtRemoteObjects", "Qt5Compat",
    "VirtualKeyboard", "FluentWinUI3", "Windows", "Material", "Imagine",
    "Universal", "Fusion", "designer",
    "NativeStyle", "Dialogs", "Pdf", "Particles", "VectorImage", "Timeline",
    "tooling", "Shapes", "Effects", "LocalStorage", "Scene2D", "Scene3D",
}

_UNNEEDED_PLUGINS_FILES = {
    "qdirect2d.dll", "qpdf.dll", "qsvg.dll", "qgif.dll", "qicns.dll",
    "qtga.dll", "qwbmp.dll",
}

_UNNEEDED_IMAGEFORMATS = {"qjpeg", "qwebp", "qtiff", "qpdf", "qsvg", "qgif", "qicns", "qtga", "qwbmp"}


def _keep_entry(entry):
    name = entry[0]
    base = name.rsplit("\\", 1)[-1]
    low = base.lower()

    # Only trim PySide6's own files; leave our icon/qml/resources alone.
    if not (name.startswith("PySide6\\") or name.startswith("PySide6/")):
        return True

    # Debug artefacts.
    if low.endswith((".lib", ".pdb", ".exp", ".ilk")):
        return False

    # Software OpenGL fallback (users are assumed to have a GPU).
    if base == "opengl32sw.dll":
        return False

    # Unused top-level Qt6 DLLs.
    if base.startswith(_UNNEEDED_QT_DLL_PREFIXES):
        return False

    # Translation catalogues (English fallback is fine).
    if "\\translations\\" in name or "/translations/" in name:
        return False

    # Unused PySide6/qml sub-trees (any depth).
    parts = name.replace("/", "\\").split("\\")
    if len(parts) >= 3 and parts[1].lower() == "qml":
        for p in parts[2:]:
            if p in _UNNEEDED_QML_DIRS:
                return False

    # Unused plugins.
    if "\\plugins\\" in name or "/plugins/" in name:
        plug_dir = parts[2].lower() if len(parts) >= 4 else ""
        if plug_dir == "qmltooling":
            return False
        if plug_dir == "imageformats" and base[:-4].lower() in _UNNEEDED_IMAGEFORMATS:
            return False
        if low in _UNNEEDED_PLUGINS_FILES:
            return False

    return True


a.binaries = [b for b in a.binaries if _keep_entry(b)]
a.datas = [d for d in a.datas if _keep_entry(d)]

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='BloodborneLauncher',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='icon.ico',
    contents_directory='_BBLauncherFiles',
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='BloodborneLauncher',
)
