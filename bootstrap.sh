#!/bin/bash

autoreconf -vfi

if type -p colorgcc > /dev/null ; then
   export CC=colorgcc
fi

export CFLAGS=${CFLAGS-"-g -O0"}

if test "x$NOCONFIGURE" = "x"; then
    ./configure --enable-maintainer-mode --disable-processing "$@"
    make clean
fi
