pkgname = "doxx"
pkgver = "0.1.1"
pkgrel = 0
build_style = "cargo"
# hostmakedepends = ["cargo-auditable", "pkgconf"]
makedepends = [
    "rust-std",
]
pkgdesc = "Terminal-native document viewer for Word files"
license = "MIT"
url = "https://github.com/bgreenwell/doxx"
source = f"{url}/archive/v{pkgver}.tar.gz"
sha256 = "6923cefa432a08adacedeb105902d47858f0ceea51b00e21e8b10117d86ca9e6"
