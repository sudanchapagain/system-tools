pkgname = "python-multidict"
pkgver = "6.6.4"
pkgrel = 0
build_style = "python_pep517"
hostmakedepends = [
    "python-build",
    "python-installer",
    "python-setuptools",
]
makedepends = ["python-devel"]
depends = ["python"]
pkgdesc = "Dict-like collection of key-value pairs"
license = "Apache-2.0"
url = "https://multidict.aio-libs.org"
source = f"https://github.com/aio-libs/multidict/releases/download/v{pkgver}/multidict-{pkgver}.tar.gz"
sha256 = "d2d4e4787672911b48350df02ed3fa3fffdc2f2e8ca06dd6afdf34189b76a9dd"
# checkdepends not yet packaged
options = ["!check"]


def post_install(self):
    self.install_license("LICENSE")
