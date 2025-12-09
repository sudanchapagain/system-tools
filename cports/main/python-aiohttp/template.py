pkgname = "python-aiohttp"
pkgver = "3.12.15"
pkgrel = 0
build_style = "python_pep517"
hostmakedepends = [
    "python-build",
    "python-installer",
    "python-pkgconfig",
    "python-setuptools",
]
makedepends = ["python-devel"]
depends = ["python"]
pkgdesc = "Asynchronous HTTP framework"
license = "Apache-2.0"
url = "https://github.com/aio-libs/aiohttp"
source = f"$(PYPI_SITE)/a/aiohttp/aiohttp-{pkgver}.tar.gz"
sha256 = "4fc61385e9c98d72fcdf47e6dd81833f47b2f77c114c29cd64a361be57a763a2"
# checkdepends has not been packaged
options = ["!check"]


def post_install(self):
    makedepends = ["python-devel"]
    self.install_license("LICENSE.txt")
