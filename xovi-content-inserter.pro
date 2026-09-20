TEMPLATE = lib
TARGET = xovi-content-inserter
CONFIG += shared plugin no_plugin_name_prefix c++17

QT += core gui qml quick svg

XOVI_REPO = $$(XOVI_REPO)
isEmpty(XOVI_REPO): XOVI_REPO = $$clean_path($$PWD/../external/xovi)
XOVIGEN = $$XOVI_REPO/util/xovigen.py

xoviextension.target = $$PWD/xovi.cpp
xoviextension.commands = python3 $$XOVIGEN -o $$PWD/xovi.cpp -H $$PWD/xovi.h $$PWD/xovi-content-inserter.xovi
xoviextension.depends = $$PWD/xovi-content-inserter.xovi $$XOVIGEN

QMAKE_EXTRA_TARGETS += xoviextension
PRE_TARGETDEPS += $$PWD/xovi.cpp
QMAKE_CLEAN += xovi.cpp xovi.h

SOURCES += src/main.cpp $$PWD/xovi.cpp

QMAKE_CXXFLAGS += -fPIC
# xovigen emits generic global metadata-chain symbols. Bind references inside
# this extension to its own definitions so a previously loaded XOVI extension
# with the same generated symbol name cannot steal a broker signal's metadata.
QMAKE_LFLAGS += -Wl,-Bsymbolic
QMAKE_LFLAGS += -Wl,--version-script=$$PWD/xovi-content-inserter.map

DISTFILES += $$PWD/xovi-content-inserter.map
