import os
import shutil
import sys
import subprocess
from pathlib import Path

out = Path("out")
out.mkdir(parents=True, exist_ok=True)

search_dirs = [
    Path(os.environ.get("MINGW_BIN", r"C:\msys64\mingw64\bin")),
    Path(r"C:\msys64\mingw64\bin"),
]
if os.environ.get("W64DEVKIT_DIR"):
    search_dirs.append(Path(os.environ["W64DEVKIT_DIR"]) / "bin")
if os.environ.get("SDL3_DIR"):
    search_dirs.append(Path(os.environ["SDL3_DIR"]) / "bin")

for p in os.environ.get("PATH", "").split(os.pathsep):
    if p.strip():
        search_dirs.append(Path(p.strip()))

system_dlls = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'shell32.dll', 'ole32.dll',
    'advapi32.dll', 'ws2_32.dll', 'msvcrt.dll', 'ntdll.dll', 'dbghelp.dll',
    'winmm.dll', 'shlwapi.dll', 'imm32.dll', 'version.dll', 'oleaut32.dll',
    'setupapi.dll', 'comdlg32.dll', 'vulkan-1.dll', 'bcrypt.dll', 'crypt32.dll',
    'iphlpapi.dll', 'wldap32.dll', 'secur32.dll', 'userenv.dll', 'winspool.drv',
    'comctl32.dll', 'd3d11.dll', 'dxgi.dll', 'd3d9.dll', 'cfgmgr32.dll',
    'dnsapi.dll', 'dwrite.dll', 'msimg32.dll', 'rpcrt4.dll', 'usp10.dll',
    'ncrypt.dll'
}

def find_objdump():
    if os.environ.get("W64DEVKIT_DIR"):
        candidate = Path(os.environ["W64DEVKIT_DIR"]) / "bin" / "objdump.exe"
        if candidate.exists():
            return candidate
    for s in search_dirs:
        candidate = s / "objdump.exe"
        if candidate.exists():
            return candidate
    return None

objdump = find_objdump()

def get_imported_dlls(filepath):
    if not objdump:
        return []
    try:
        res = subprocess.run([str(objdump), '-p', str(filepath)], capture_output=True, text=True, check=False)
        dlls = []
        for line in res.stdout.splitlines():
            if 'DLL Name:' in line:
                dlls.append(line.split('DLL Name:')[1].strip())
        return dlls
    except Exception:
        return []

# Base set of known dependencies
base_dlls = [
    'SDL3.dll',
    'libxxhash.dll','libzstd.dll','zlib1.dll','liblzma-5.dll','libbz2-1.dll',
    'libiconv-2.dll','libintl-8.dll','libbrotlicommon.dll','libbrotlidec.dll',
    'libbrotlienc.dll','libglib-2.0-0.dll','libgmodule-2.0-0.dll','libgobject-2.0-0.dll',
    'libgio-2.0-0.dll','libpcre2-8-0.dll','libffi-8.dll','libwinpthread-1.dll',
    'libgcc_s_seh-1.dll','libstdc++-6.dll','libspeex-1.dll','libtheoradec-1.dll',
    'libtheoradec-2.dll','libtheoraenc-1.dll','libtheoraenc-2.dll','libvorbis-0.dll',
    'libvorbisenc-2.dll','libvorbisfile-3.dll','libogg-0.dll','libopus-0.dll',
    'libvpx-1.dll','libdav1d-7.dll','libva.dll','libmfx-1.dll','libva_win32.dll',
    'libgsm.dll','libmp3lame-0.dll','libopencore-amrnb-0.dll','libopencore-amrwb-0.dll',
    'libopenjp2-7.dll','librav1e.dll','libsharpyuv-0.dll','libwebp-7.dll','libwebpmux-3.dll',
    'libsnappy.dll','libsoxr.dll','libtwolame-0.dll','libx264-166.dll','libx264-165.dll',
    'libx265.dll','libx265-216.dll','libxvidcore-4.dll','xvidcore.dll','libxml2-16.dll',
    'libbluray-2.dll','libbluray-3.dll','libgmp-10.dll','libgnutls-30.dll','libhogweed-6.dll',
    'libnettle-8.dll','libp11-kit-0.dll','libtasn1-6.dll','libidn2-0.dll','libunistring-5.dll',
    'libfontconfig-1.dll','libfreetype-6.dll','libfribidi-0.dll','libharfbuzz-0.dll',
    'libgraphite2.dll','libthai-0.dll','libdatrie-1.dll','libpango-1.0-0.dll',
    'libpangocairo-1.0-0.dll','libpangoft2-1.0-0.dll','libpangowin32-1.0-0.dll',
    'libcairo-2.dll','libcairo-gobject-2.dll','libpixman-1-0.dll','libpng16-16.dll',
    'libdeflate.dll','libjpeg-8.dll','libjbig-0.dll','libLerc.dll','libtiff-6.dll',
    'libexpat-1.dll','liblcms2-2.dll','avcodec-62.dll','avformat-62.dll','avutil-60.dll',
    'swresample-6.dll','swscale-9.dll','libZydis.dll','libZycore.dll',
    'libaom.dll','libjxl.dll','libjxl_threads.dll','liblc3-1.dll','libgomp-1.dll',
    'libshaderc_shared.dll','libsrt.dll','libssh.dll','libvpl-2.dll','libzvbi-0.dll',
    'libSvtAv1Enc-4.dll','libgme.dll','libmodplug-1.dll','librsvg-2-2.dll','librtmp-1.dll',
    'libhwy.dll','libgdk_pixbuf-2.0-0.dll','libjxl_cms.dll','libcrypto-3-x64.dll'
]

staged = 0
copied_names = set(f.name.lower() for f in out.glob('*.dll'))

for d in base_dlls:
    if d.lower() in copied_names:
        continue
    for sdir in search_dirs:
        src = sdir / d
        if src.is_file():
            try:
                shutil.copy2(src, out / d)
                copied_names.add(d.lower())
                staged += 1
                break
            except OSError:
                pass

# Dynamic transitive closure stage using objdump
if objdump:
    changed = True
    while changed:
        changed = False
        scan_files = list(out.glob('*.exe')) + list(out.glob('*.dll'))
        for f in scan_files:
            for imp in get_imported_dlls(f):
                imp_lower = imp.lower()
                if imp_lower in system_dlls or imp_lower.startswith('api-ms-win'):
                    continue
                if imp_lower not in copied_names:
                    for sdir in search_dirs:
                        candidate = sdir / imp
                        if candidate.is_file():
                            try:
                                shutil.copy2(candidate, out / imp)
                                copied_names.add(imp_lower)
                                staged += 1
                                changed = True
                                break
                            except OSError:
                                pass

print(f"Runtime DLL staging complete ({staged} new DLLs staged).")
