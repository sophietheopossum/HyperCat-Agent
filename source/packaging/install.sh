#!/usr/bin/env bash
# HyperCat installer — system-wide install of the test bundle to /opt.
#
# Installs the runtime libraries + fonts via the distro package manager, copies the three programs to
# /opt/hypercat, and adds a `hypercat` launcher to /usr/local/bin. Run it from inside the unpacked
# bundle directory:   ./install.sh           (asks for sudo)
#   Preview what it would install (no sudo, no changes):  ./install.sh --dry-run
#   Remove everything except your data:                   ./install.sh --uninstall
#
# Covers the big-4 glibc families: Debian/Ubuntu (apt), Fedora/RHEL/Rocky/Alma (dnf), Arch/Manjaro
# (pacman), openSUSE (zypper) — detected from /etc/os-release. An unknown distro still installs the
# programs; it just prints the runtime libraries to install by hand.
#
# This is a pre-release TEST build — see DISCLAIMER.txt. Your API key is supplied at runtime via
# OPENROUTER_API_KEY (env only); it is never stored.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="/opt/hypercat"
LAUNCHER="/usr/local/bin/hypercat"
DESKTOP="/usr/share/applications/hypercat.desktop"
ICON="/usr/share/icons/hicolor/128x128/apps/hypercat.png"

# ---- uninstall -------------------------------------------------------------
if [ "${1:-}" = "--uninstall" ]; then
  echo "Removing HyperCat (your data under ~/.local/share/hypercat is kept)..."
  sudo rm -rf "$PREFIX"
  sudo rm -f "$LAUNCHER" "$DESKTOP" "$ICON"
  echo "Done. To also delete your data:  rm -rf ~/.local/share/hypercat"
  exit 0
fi

DRYRUN=0
[ "${1:-}" = "--dry-run" ] && DRYRUN=1

# ---- sanity (skip the bundle check in a dry run — it's for previewing detection anywhere) ----
if [ "$DRYRUN" -eq 0 ]; then
  for bin in hypercat agentd hc_audio_helper hc_tool_launch; do
    [ -x "$HERE/$bin" ] || { echo "error: $bin missing from the bundle ($HERE)" >&2; exit 1; }
  done
fi

# Parse (do NOT source) /etc/os-release — sourcing would execute a root-writable file inside this sudo script.
osr() { [ -r /etc/os-release ] && grep -m1 "^$1=" /etc/os-release | cut -d= -f2- | tr -d '"'; }
ID="$(osr ID)"; VERSION_ID="$(osr VERSION_ID)"; ID_LIKE="$(osr ID_LIKE)"; PRETTY_NAME="$(osr PRETTY_NAME)"

# Resolve the package-manager FAMILY from ID, falling back to ID_LIKE for derivatives.
FAMILY=""
case "$ID" in
  ubuntu|debian|linuxmint|pop|elementary|raspbian|devuan|zorin|neon) FAMILY=debian ;;
  fedora|rhel|centos|rocky|almalinux|ol|amzn|scientific)             FAMILY=fedora ;;
  arch|manjaro|endeavouros|cachyos|garuda|arcolinux)                 FAMILY=arch ;;
  opensuse*|sles|sled|suse|tumbleweed)                               FAMILY=suse ;;
esac
if [ -z "$FAMILY" ]; then
  case " ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*)           FAMILY=debian ;;
    *" fedora "*|*" rhel "*|*" centos "*) FAMILY=fedora ;;
    *" arch "*)                           FAMILY=arch ;;
    *suse*)                               FAMILY=suse ;;
  esac
fi

# Pick the package-manager binary (dnf preferred over yum on the fedora family).
PMBIN=""
case "$FAMILY" in
  debian) PMBIN=apt-get ;;
  fedora) command -v dnf >/dev/null 2>&1 && PMBIN=dnf || PMBIN=yum ;;
  arch)   PMBIN=pacman ;;
  suse)   PMBIN=zypper ;;
esac
# A dry run only previews detection, so it does not require the manager to be installed.
if [ "$DRYRUN" -eq 0 ] && { [ -z "$FAMILY" ] || ! command -v "$PMBIN" >/dev/null 2>&1; }; then
  echo "warning: unrecognised distro (detected: ${PRETTY_NAME:-unknown}); cannot auto-install dependencies."
  FAMILY="" # skip the dep step; we still install the programs and print the manual list below
fi

# ---- runtime dependency set (per family; soname-providing packages) --------
# The app links a small soname set (libcurl.so.4, libcjson.so.1, libglfw.so.3, libopenal.so.1, GLVND
# GL/GLX, libX11.so.6, + libsecret-1.so.0 / libglib-2.0.so.0 / libgobject-2.0.so.0 when built with the OS
# keychain backend); the manager pulls the transitive deps (gnutls/krb5/brotli/...). Fonts are best-effort —
# the UI falls through JetBrains -> DejaVu -> ImGui's built-in, so a missing font never breaks anything.
# Names are best-known per family; the install loop tolerates a miss (logs + continues).
PKGS=()
case "$FAMILY" in
  debian)
    # the 24.04+/Debian-13+ t64 transition renamed some runtime libs to *t64 (libsecret-1-0 is NOT in that set)
    CURL_PKG=libcurl4; ASOUND_PKG=libasound2; GLIB_PKG=libglib2.0-0
    case "$ID:$VERSION_ID" in
      ubuntu:20.04|ubuntu:22.04|debian:11|debian:12) : ;;                                            # non-t64
      ubuntu:*|debian:*) CURL_PKG=libcurl4t64; ASOUND_PKG=libasound2t64; GLIB_PKG=libglib2.0-0t64 ;; # 24.04+/13+
    esac
    PKGS=(libglfw3 libopenal1 libcjson1 "$CURL_PKG" "$ASOUND_PKG"
          libopengl0 libglx0 libgl1 libx11-6
          libsecret-1-0 "$GLIB_PKG"
          fonts-jetbrains-mono fonts-noto-cjk fonts-dejavu-core)
    ;;
  fedora)
    PKGS=(glfw openal-soft cjson libcurl alsa-lib
          libglvnd-glx libglvnd-opengl libX11 libxcb
          libsecret glib2
          google-noto-sans-cjk-fonts dejavu-sans-mono-fonts)
    ;;
  arch)
    PKGS=(glfw openal cjson curl alsa-lib
          libglvnd libx11 libxcb
          libsecret glib2
          noto-fonts-cjk ttf-dejavu)
    ;;
  suse)
    PKGS=(libglfw3 libopenal1 libcjson1 libcurl4 libasound2
          libglvnd Mesa-libGL1 libX11-6 libxcb1
          libsecret-1-0 libglib-2_0-0 libgobject-2_0-0
          noto-sans-cjk-fonts dejavu-fonts)
    ;;
esac

# ---- dry run: print the detection + the package plan, change nothing -------
if [ "$DRYRUN" -eq 1 ]; then
  echo "HyperCat installer (dry run)"
  echo "  distro:  ${PRETTY_NAME:-unknown}  (ID=$ID VERSION_ID=$VERSION_ID ID_LIKE='${ID_LIKE}')"
  echo "  family:  ${FAMILY:-unknown}        package manager: ${PMBIN:-none}"
  if [ -n "$FAMILY" ]; then
    echo "  would install: ${PKGS[*]}"
  else
    echo "  no auto-install for this distro — install the runtime sonames by hand (see README)."
  fi
  exit 0
fi

# ---- disclaimer ------------------------------------------------------------
[ -r "$HERE/DISCLAIMER.txt" ] && { echo; cat "$HERE/DISCLAIMER.txt"; echo; }
read -r -p "Install HyperCat to ${PREFIX}? [y/N] " a; [ "${a:-}" = "y" ] || { echo "Aborted."; exit 0; }

# ---- install the runtime dependencies --------------------------------------
pm_update() { # refresh metadata where the manager needs it before install
  case "$FAMILY" in
    debian) sudo apt-get update ;;
    arch)   sudo pacman -Syu --noconfirm ;; # -Syu (not -Sy): avoid the Arch partial-upgrade footgun
    suse)   sudo zypper --non-interactive refresh ;;
    fedora) : ;; # dnf refreshes on demand
  esac
}
pm_install_one() { # $1 = one package; nonzero on failure (the caller tolerates it)
  case "$FAMILY" in
    debian) sudo apt-get install -y --no-install-recommends "$1" ;;
    fedora) sudo "$PMBIN" install -y --allowerasing "$1" ;; # swap libcurl-minimal->libcurl on minimal bases
    arch)   sudo pacman -S --needed --noconfirm "$1" ;;
    suse)   sudo zypper --non-interactive install --no-recommends "$1" ;;
  esac
}

if [ -n "$FAMILY" ]; then
  echo "Installing runtime dependencies via ${PMBIN} (sudo)..."
  pm_update || echo "  note: package metadata refresh failed — continuing."
  for p in "${PKGS[@]}"; do
    if ! pm_install_one "$p"; then
      echo "  note: '$p' could not be installed — continuing (it may be named differently or already present)."
    fi
  done
else
  echo
  echo "Install these runtime libraries with your package manager before running HyperCat:"
  echo "  libcurl.so.4, libcjson.so.1, libglfw.so.3, libopenal.so.1, the GLVND OpenGL/GLX libs,"
  echo "  libX11.so.6, and (optional) JetBrains Mono / Noto CJK / DejaVu fonts."
fi

# ---- place the programs ----------------------------------------------------
echo "Installing programs to ${PREFIX}..."
sudo mkdir -p "$PREFIX"
sudo install -m 0755 "$HERE/hypercat" "$HERE/agentd" "$HERE/hc_audio_helper" "$HERE/hc_tool_launch" "$PREFIX/"

# ---- launcher (warns about a missing key, then execs the host) -------------
echo "Installing launcher ${LAUNCHER}..."
sudo tee "$LAUNCHER" >/dev/null <<'LAUNCH'
#!/usr/bin/env bash
# HyperCat launcher.
if [ -z "${OPENROUTER_API_KEY:-}" ]; then
  echo "HyperCat: OPENROUTER_API_KEY is not set — the conductor and workers will be offline." >&2
  echo "  export OPENROUTER_API_KEY=sk-or-...   (add it to ~/.profile to persist)" >&2
fi
exec /opt/hypercat/hypercat "$@"
LAUNCH
sudo chmod 0755 "$LAUNCHER"

# ---- optional desktop entry + icon ----------------------------------------
if [ -r "$HERE/hypercat.png" ]; then
  sudo install -Dm 0644 "$HERE/hypercat.png" "$ICON"
  sudo tee "$DESKTOP" >/dev/null <<DESK
[Desktop Entry]
Type=Application
Name=HyperCat
Comment=Multi-agent workspace
Exec=${LAUNCHER}
Icon=hypercat
Terminal=false
Categories=Development;Utility;
DESK
fi

cat <<DONE

HyperCat installed.
  1. Set your API key:   export OPENROUTER_API_KEY="sk-or-..."
                         (add that line to ~/.profile to persist across logins)
  2. Run:                hypercat
  3. Your data lives in: ~/.local/share/hypercat
  4. Uninstall:          $HERE/install.sh --uninstall

This is a pre-release test build — see DISCLAIMER.txt and THIRD_PARTY.txt.
DONE
