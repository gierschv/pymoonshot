##
# Copyright (c) 2006-2008 Apple Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##

from distutils.core import setup, Extension
import sys
import commands

long_description = """
This package is a fork of PyKerberos package.

"""

setup (
    name = "moonshot",
    version = "0.1",
    description = "GSS-API high-level interface for Janet Moonshot",
    long_description=long_description,
    classifiers = [
        "License :: OSI Approved :: Apache Software License",
        "Programming Language :: Python :: 2",
        "Topic :: Software Development :: Libraries :: Python Modules",
        "Topic :: System :: Systems Administration :: Authentication/Directory"
        ],
    ext_modules = [
        Extension(
            "moonshot",
            extra_link_args = commands.getoutput("krb5-config --libs gssapi").split(),
            extra_compile_args = commands.getoutput("krb5-config --cflags gssapi").split(),
            sources = [
                "src/kerberos.c",
                "src/kerberosbasic.c",
                "src/kerberosgss.c",
                "src/kerberospw.c",
                "src/base64.c"
            ],
        ),
    ],
)
