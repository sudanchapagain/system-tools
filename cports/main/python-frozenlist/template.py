pkgname = "python-frozenlist"
pkgver = "1.7.0"
pkgrel = 0
build_style = "python_pep517"
hostmakedepends = [
    "python-build",
    # "python-cython",
    "python-installer",
    "python-setuptools",
    "python-wheel",
]
makedepends = ["python-devel","python-setuptools"]
depends = ["python"]
pkgdesc = "List-like structure that can be freezed"
license = "Apache-2.0"
url = "https://frozenlist.aio-libs.org"
source = f"$(PYPI_SITE)/f/frozenlist/frozenlist-{pkgver}.tar.gz"
sha256 = "2e310d81923c2437ea8670467121cc3e9b0f76d3043cc1d2331d56c7fb7a3a8f"
# pytest-cov is not yet packaged
options = ["!check"]


def post_install(self):
    self.install_license("LICENSE")
