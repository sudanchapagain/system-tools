pkgname = "keypunch"
pkgver = "6.3"
pkgrel = 0
build_style = "meson"
hostmakedepends = [
    "blueprint-compiler",
    "cargo-auditable",
    "gettext",
    "glib-devel",
    "meson",
    "pkgconf"
]
makedepends = [
    "libadwaita-devel",
    "rust-std",
]
pkgdesc = "Practice your typing skills"
license = "GPL-3.0-or-later"
url = "https://apps.gnome.org/Keypunch"
source = (
    f"https://github.com/bragefuglseth/keypunch/archive/refs/tags/v{pkgver}.tar.gz"
)
sha256 = "c58a6f3a7b4c7cc857c3126ca8ddb856d45c5275fe2b9f51a820d6d22fec8641"

