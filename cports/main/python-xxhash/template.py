pkgname = "python-xxhash"
pkgver = "3.5.0"
pkgrel = 0
build_style = "python_pep517"
hostmakedepends = [
    "python-build",
    "python-installer",
    "python-setuptools"
]
makedepends = ["python-devel", "xxhash-devel"]
depends = ["python"]
pkgdesc = "Python binding for xxHash"
license = "BSD-2-Clause"
url = "https://github.com/ifduyue/python-xxhash"
source = f"$(PYPI_SITE)/x/xxhash/xxhash-{pkgver}.tar.gz"
sha256 = "84f2caddf951c9cbf8dc2e22a89d4ccf5d86391ac6418fe81e3c67d0cf60b45f"
# skip tests
options = ["!check"]

def post_install(self):
    self.install_license("LICENSE")
