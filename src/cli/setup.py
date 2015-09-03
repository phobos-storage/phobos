#!/usr/bin/python

# Copyright CEA/DAM 2015
# Author: Henri Doreau <henri.doreau@cea.fr>
#
# This file is part of the Phobos project

from distutils.core import setup, Extension

cdss_module = Extension('cdss',
                        sources=['phobos/cdss_module.c'],
                        include_dirs = ['../include',
                                        '/usr/include/glib-2.0',
                                        '/usr/lib64/glib-2.0/include'],
                        libraries = ['phobos_store'],
                        library_dirs = ['../store/.libs'])

ccfg_module = Extension('ccfg',
                        sources=['phobos/ccfg_module.c'],
                        include_dirs = ['../include',
                                        '/usr/include/glib-2.0',
                                        '/usr/lib64/glib-2.0/include'],
                        libraries = ['phobos_store'],
                        library_dirs = ['../store/.libs'])

setup(
    name = 'phobos',
    packages = ['phobos'],
    ext_package = 'phobos',
    ext_modules = [cdss_module, ccfg_module],
    scripts = ['scripts/phobos'],
    version = '0.0.1',
    description = 'Phobos control scripts and libraries',
    author = 'Henri Doreau',
    author_email = 'henri.doreau@cea.fr')
