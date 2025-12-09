pkgname = "python-aiofiles"
pkgver = "24.1.0"
pkgrel = 0
build_style = "python_pep517"
hostmakedepends = [
    "python-build",
    "python-hatchling",
    "python-installer",
    "python-setuptools",
]
checkdepends = ["python-pytest", "python-pytest-asyncio"]
pkgdesc = "File support for asyncio"
license = "Apache-2.0"
url = "https://github.com/Tinche/aiofiles"
source = f"$(PYPI_SITE)/a/aiofiles/aiofiles-{pkgver}.tar.gz"
sha256 = "22a075c9e5a3810f0c2e48f3008c94d68c65d763b9b03857924c99e57355166c"


def post_install(self):
    self.install_license("LICENSE")
