#from distutils.core import setup, Extension
from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext
import subprocess

with open("README.md", "r") as fh:
    long_description = fh.read()

backend = Extension('multicam.backend',
    define_macros = [('HAVE_JPEG',), ('NPY_NO_DEPRECATED_API','NPY_1_7_API_VERSION')],
    include_dirs  = ['libyuv/include'],
    libraries     = [':libyuv.a', ':libjpeg.so.8', 'stdc++'],
    library_dirs  = ['libyuv/out'],
    sources       = ['src/multicam.c', 'src/v4l2.c'],
    extra_compile_args = [],
    extra_link_args    = [],
)


class Buildlibyuv(build_ext):
    def run(self):
        subprocess.call("./build_libyuv.sh")
        build_ext.run(self)

setup(
    name='multicam',
    version='1.0.3',
    ext_modules=[backend],
    packages=find_packages(),
    install_requires=[
          'numpy',
      ],
    author="Søren Rasmussen",
    author_email="soren.rasmussen@alexandra.dk",
    description="Syncronous reading from multiple webcams",
    long_description=long_description,
    long_description_content_type="text/markdown",
    cmdclass={'build_ext': Buildlibyuv},
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: POSIX :: Linux"
    ],
    python_requires='>=3.6'
)
