TEMPLATE = lib
CONFIG += staticlib

QT -= gui core

SOURCES += \
    unpack.c

HEADERS += \
    dmc_unrar.c

DEFINES +=

QMAKE_CFLAGS += -O3
QMAKE_CFLAGS_RELEASE -= -O2
QMAKE_CFLAGS_RELEASE += -g1
QMAKE_CFLAGS_DEBUG += -g
