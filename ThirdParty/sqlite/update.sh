#!/usr/bin/env bash

set -e
set -x
shopt -s dotglob

readonly name="sqlite"
readonly ownership="sqlite Upstream <kwrobot@kitware.com>"
readonly subtree="ThirdParty/$name/vtk$name"
readonly repo="https://gitlab.kitware.com/third-party/sqlite.git"
readonly tag="for/vtk-20250102-3.47.2" # see `manifest` below if the version number changes
readonly paths="
.gitattributes
CMakeLists.txt
vtk_sqlite_mangle.h
README.kitware.md
README.md
VERSION

ext/
src/
tool/
main.mk
"

extract_source () {
    git_archive
    pushd "$extractdir/$name-reduced"
    echo "3.47.2-vtk" > manifest
    echo "3.47.2-vtk" > manifest.uuid
    make -f main.mk TOP=$PWD BCC=cc target_source sqlite3.c
    rm -rvf ext src tool main.mk manifest manifest.uuid VERSION
    rm -rvf lemon keywordhash.h lempar.c mkkeywordhash mksourceid src-verify
    rm -rvf opcodes.* parse.* tsrc fts5.* fts5parse.*
    rm -rvf sqlite3ext.h sqlite3session.h tclsqlite3.c target_source
    popd
}

. "${BASH_SOURCE%/*}/../update-common.sh"
