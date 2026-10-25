#!/bin/sh
# Build a portable AppImage from the built tree.
#
# Needs: make, curl, mksquashfs (squashfs-tools). No root.
#   make && tools/make-appimage.sh
#
# The result is build/TaskManagerOK-x86_64.AppImage: one file that runs on a
# stock desktop Linux without installing anything.
set -e

APP=taskmanagerok
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/appimage
APPDIR=$BUILD/AppDir
# Outside $BUILD and outside the tree: the script wipes $BUILD on every run and
# `make clean` wipes it too, and this is a 14 MB download that does not change
# between builds.
TOOL=${XDG_CACHE_HOME:-$HOME/.cache}/appimagetool

# Libraries the target system must provide; bundling these breaks more often
# than it helps (glibc, the X/Wayland client stack, GL, D-Bus, kernel glue).
EXCLUDE='^(ld-linux|libc\.|libm\.|libdl\.|libpthread\.|librt\.|libnsl\.|libutil\.|libresolv\.|libcrypt\.|libgcc_s|libstdc\+\+|libX|libxcb|libdrm|libGL|libEGL|libglapi|libwayland|libdbus|libsystemd|libselinux|libmount|libblkid|libcap|libaudit|libz\.|libbz2|liblzma|libffi|libexpat|libgpg-error|libgcrypt|libpcre|libselinux)'

[ -x "$ROOT/$APP" ] || { echo "run make first"; exit 1; }

rm -rf "$BUILD"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/locale"

cp "$ROOT/$APP" "$APPDIR/usr/bin/"
cp "$ROOT/data/$APP.desktop" "$APPDIR/"
cp "$ROOT/data/$APP.svg" "$APPDIR/"
[ -d "$ROOT/locale" ] && cp -r "$ROOT/locale/." "$APPDIR/usr/share/locale/"

# Copy the dependency closure until it stops growing.
todo=$(ldd "$APPDIR/usr/bin/$APP" | awk '/=> \// {print $3}')
count=0
while [ -n "$todo" ] && [ "$count" -lt 10 ]; do
  next=""
  for lib in $todo; do
    base=$(basename "$lib")
    if echo "$base" | grep -Eq "$EXCLUDE"; then
      continue
    fi
    if [ ! -e "$APPDIR/usr/lib/$base" ]; then
      cp -L "$lib" "$APPDIR/usr/lib/$base"
      next="$next $(ldd "$lib" | awk '/=> \// {print $3}')"
    fi
  done
  todo=$(echo "$next" | tr ' ' '\n' | sort -u | grep -v '^$' | tr '\n' ' ')
  count=$((count + 1))
done

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$here/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$here/usr/bin/taskmanagerok" "$@"
RUN
chmod +x "$APPDIR/AppRun"

echo "bundled $(ls "$APPDIR/usr/lib" | wc -l) libraries:"
ls "$APPDIR/usr/lib" | sed 's/^/  /'

[ -x "$TOOL" ] || {
  mkdir -p "$(dirname "$TOOL")"
  echo "downloading appimagetool"
  curl -fSL --retry 3 -o "$TOOL.part" \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$TOOL.part"
  mv "$TOOL.part" "$TOOL"
}

cd "$BUILD"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" -n "$APPDIR" "$BUILD/TaskManagerOK-x86_64.AppImage"
ln -sf build/appimage/TaskManagerOK-x86_64.AppImage "$ROOT/TaskManagerOK.AppImage"
echo
echo "built: $BUILD/TaskManagerOK-x86_64.AppImage"
