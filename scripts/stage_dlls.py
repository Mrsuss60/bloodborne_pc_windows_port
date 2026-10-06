import os
import shutil
import sys
from pathlib import Path

out = Path("out")
out.mkdir(parents=True, exist_ok=True)

search_dirs = [
    Path(os.environ.get("MINGW_BIN", r"C:\msys64\mingw64\bin")),
    Path(r"C:\msys64\mingw64\bin"),
]
if os.environ.get("SDL3_DIR"):
    search_dirs.append(Path(os.environ["SDL3_DIR"]) / "bin")

for p in os.environ.get("PATH", "").split(os.pathsep):
    if p.strip():
        search_dirs.append(Path(p.strip()))

dlls = [
    'SDL3.dll',
    'libxxhash.dll','libzstd.dll','zlib1.dll','liblzma-5.dll','libbz2-1.dll',
    'libiconv-2.dll','libintl-8.dll','libbrotlicommon.dll','libbrotlidec.dll',
    'libbrotlienc.dll','libglib-2.0-0.dll','libgmodule-2.0-0.dll','libgobject-2.0-0.dll',
    'libgio-2.0-0.dll','libpcre2-8-0.dll','libffi-8.dll','libwinpthread-1.dll',
    'libgcc_s_seh-1.dll','libstdc++-6.dll','libspeex-1.dll','libtheoradec-1.dll',
    'libtheoraenc-1.dll','libvorbis-0.dll','libvorbisenc-2.dll','libvorbisfile-3.dll',
    'libogg-0.dll','libopus-0.dll','libvpx-1.dll','libdav1d-7.dll','libva.dll',
    'libmfx-1.dll','libva_win32.dll','libgsm.dll','libmp3lame-0.dll',
    'libopencore-amrnb-0.dll','libopencore-amrwb-0.dll','libopenjp2-7.dll','librav1e.dll',
    'libsharpyuv-0.dll','libwebp-7.dll','libwebpmux-3.dll','libsnappy.dll','libsoxr.dll',
    'libtwolame-0.dll','libx264-166.dll','libx265.dll','libxvidcore-4.dll',
    'libxml2-16.dll','libbluray-2.dll','libgmp-10.dll','libgnutls-30.dll',
    'libhogweed-6.dll','libnettle-8.dll','libp11-kit-0.dll','libtasn1-6.dll',
    'libidn2-0.dll','libunistring-5.dll','libfontconfig-1.dll','libfreetype-6.dll',
    'libfribidi-0.dll','libharfbuzz-0.dll','libgraphite2.dll','libthai-0.dll',
    'libdatrie-1.dll','libpango-1.0-0.dll','libpangocairo-1.0-0.dll','libpangoft2-1.0-0.dll',
    'libpangowin32-1.0-0.dll','libcairo-2.dll','libcairo-gobject-2.dll','libpixman-1-0.dll',
    'libpng16-16.dll','libdeflate.dll','libjpeg-8.dll','libjbig-0.dll','libLerc.dll',
    'libtiff-6.dll','libexpat-1.dll','liblcms2-2.dll','avcodec-62.dll','avformat-62.dll',
    'avutil-60.dll','swresample-6.dll','swscale-9.dll','libZydis.dll','libZycore.dll'
]

staged = 0
for d in dlls:
    dst = out / d
    if dst.exists():
        continue
    for sdir in search_dirs:
        src = sdir / d
        if src.is_file():
            try:
                shutil.copy2(src, dst)
                staged += 1
                break
            except OSError:
                pass

print(f"Runtime DLL staging complete ({staged} new DLLs staged).")
