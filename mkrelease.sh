#!/bin/sh
#
# Script for generating a release archive
#

OPENALDIR='openal-soft-1.18.2'

make -C $OPENALDIR -f makefile.amigaos4 clean
make -C $OPENALDIR -f makefile.amigaos4

DESTDIR='tmp'
RELEASE='r2'

rm -rf ${DESTDIR}
mkdir -p ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
mkdir -p ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/lib/pkgconfig

cp -p README ${DESTDIR}/${OPENALDIR}
cp -p ${OPENALDIR}/COPYING ${DESTDIR}/${OPENALDIR}
cp -p ${OPENALDIR}/include/AL/al.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/include/AL/alc.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/include/AL/alext.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/include/AL/efx.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/include/AL/efx-creative.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/include/AL/efx-presets.h ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/include/AL
cp -p ${OPENALDIR}/libopenal.a ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/lib
cp -p ${OPENALDIR}/libopenal.so ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/lib
cp -p ${OPENALDIR}/openal.pc ${DESTDIR}/${OPENALDIR}/SDK/local/newlib/lib/pkgconfig

rm -f ${OPENALDIR}${RELEASE}.7z
7za u ${OPENALDIR}${RELEASE}.7z ./${DESTDIR}/${OPENALDIR}

rm -rf ${DESTDIR}

echo "${OPENALDIR}${RELEASE}.7z created"

