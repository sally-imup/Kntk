#!/data/data/com.termux/files/usr/bin/bash
# termux-vm-start.sh
#
# Dijalankan otomatis saat sesi pertama app dibuka (lihat TermuxActivity).
# Perilaku seperti VM:
#   - Ubuntu (proot-distro) langsung dijalankan.
#   - Jika di dalam Ubuntu ada window manager (XFCE, LXQt, Openbox, dll.),
#     desktop langsung tampil di dalam app ini (X server embedded dari Termux:X11).
#   - Jika tidak ada window manager, langsung masuk shell Ubuntu.
#
# Mematikan auto-start: buat file kosong ~/.termux/no-vm
# Ganti distro:          TERMUX_VM_DISTRO=debian (default: ubuntu)

DISTRO="${TERMUX_VM_DISTRO:-ubuntu}"
X_DISPLAY=":0"
ROOTFS="$PREFIX/var/lib/proot-distro/installed-rootfs/$DISTRO"
GUEST_LOGIN=(proot-distro login "$DISTRO" --shared-tmp)
APP_PACKAGE="com.termux"

# Urutan prioritas window manager yang dicari di dalam Ubuntu.
WM_CANDIDATES="touchwm startxfce4 startlxqt startplasma-x11 startlxde mate-session openbox-session fluxbox icewm-session i3 awesome"

# Mode manual: langsung ke shell Termux biasa.
if [ -f "$HOME/.termux/no-vm" ]; then
    exec "$SHELL" -l
fi

# 0. Symlink ke rootfs agar bisa diakses langsung dari shell Termux: ~/ubuntu-root
ln -sfn "$ROOTFS" "$HOME/ubuntu-root" 2>/dev/null

# 1. Pastikan proot-distro tersedia.
if ! command -v proot-distro >/dev/null 2>&1; then
    echo "[VM] Memasang proot-distro..."
    pkg install -y proot-distro || { echo "[VM] Gagal memasang proot-distro."; exec "$SHELL" -l; }
fi

# 2. Pasang distro (hanya sekali).
if [ ! -d "$ROOTFS" ]; then
    echo "[VM] $DISTRO belum terpasang, mengunduh (hanya sekali)..."
    proot-distro install "$DISTRO" || { echo "[VM] Gagal memasang $DISTRO."; exec "$SHELL" -l; }
fi

# 3. Pasang touchwm (WM khusus layar sentuh) ke dalam guest, compile ulang bila sumbernya berubah.
TOUCHWM_SRC="$HOME/.termux/vm/touchwm.c"
GUEST_BIN="$ROOTFS/usr/local/bin/touchwm"
GUEST_SRC="$ROOTFS/usr/local/src/touchwm.c"
if [ -f "$TOUCHWM_SRC" ] && { [ ! -x "$GUEST_BIN" ] || ! cmp -s "$TOUCHWM_SRC" "$GUEST_SRC" 2>/dev/null; }; then
    echo "[VM] Memasang touchwm (window manager layar sentuh)..."
    mkdir -p "$ROOTFS/usr/local/src"
    cp "$TOUCHWM_SRC" "$GUEST_SRC"
    "${GUEST_LOGIN[@]}" -- sh -c "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc libc6-dev libx11-dev xfonts-base xfonts-100dpi xterm >/dev/null && gcc -O2 -Wall -o /usr/local/bin/touchwm /usr/local/src/touchwm.c -lX11" \
        || echo "[VM] Gagal memasang touchwm, memakai WM lain jika ada."
fi

# 3b. Cari window manager di dalam guest.
GUEST_FIND="for w in $WM_CANDIDATES; do if command -v \"\$w\" >/dev/null 2>&1; then echo \"\$w\"; exit 0; fi; done; exit 1"
WM="$("${GUEST_LOGIN[@]}" -- sh -c "$GUEST_FIND" 2>/dev/null | tail -n 1)"

if [ -z "$WM" ]; then
    echo "[VM] Tidak ada window manager di $DISTRO. Masuk ke shell $DISTRO."
    echo "[VM] Tip desktop XFCE: apt update && apt install -y xfce4 dbus-x11"
    exec "${GUEST_LOGIN[@]}"
fi

# 4. Ada window manager: jalankan X server embedded lalu buka desktop di app ini.
echo "[VM] Window manager ditemukan: $WM. Membuka desktop..."

X_APK="$(pm path "$APP_PACKAGE" 2>/dev/null | head -n 1 | sed 's/^package://')"
if [ -z "$X_APK" ]; then
    echo "[VM] APK $APP_PACKAGE tidak ditemukan, desktop dilewati. Masuk ke shell $DISTRO."
    exec "${GUEST_LOGIN[@]}"
fi

pkill -f "com.termux.x11.CmdEntryPoint" 2>/dev/null
CLASSPATH="$X_APK" app_process -Xnoimage-dex2oat / --nice-name="termux-x11 $APP_PACKAGE $X_DISPLAY" \
    com.termux.x11.CmdEntryPoint "$X_DISPLAY" >/dev/null 2>&1 &
sleep 2

# Tampilkan activity desktop (MainActivity dari modul lorie) di depan.
am start --user 0 -n "$APP_PACKAGE/com.termux.x11.MainActivity" >/dev/null 2>&1

# Jalankan window manager di dalam guest. Setelah ditutup, kembali ke shell Ubuntu.
GUEST_START="if command -v dbus-run-session >/dev/null 2>&1; then exec dbus-run-session -- \"\$1\"; else exec \"\$1\"; fi"
"${GUEST_LOGIN[@]}" -- env DISPLAY="$X_DISPLAY" XDG_RUNTIME_DIR=/tmp sh -c "$GUEST_START" sh "$WM"

pkill -f "com.termux.x11.CmdEntryPoint" 2>/dev/null
echo "[VM] Desktop ditutup. Kembali ke shell $DISTRO."
exec "${GUEST_LOGIN[@]}"
