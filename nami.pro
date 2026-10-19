QT       += core gui widgets concurrent svg network
LIBS     += -limm32 -lgdi32 -ldwmapi -loleacc -lole32 -loleaut32
TARGET    = nami
TEMPLATE  = app
CONFIG   += c++20
# Vendored zxing-cpp 3.1.1, readers only. Its core is C++20; the public API is C++17.
ZXING_ROOT = third_party/zxing-cpp
INCLUDEPATH += $$ZXING_ROOT/core/src $$ZXING_ROOT/core/include
# The vendored core/include/Version.h supplies ZXING_READERS and per-format switches.
DEFINES += ZXING_INTERNAL ZUECI_EMBED_NO_TO_ECI
ZXING_SOURCES = $$files($$ZXING_ROOT/core/src/*.cpp, true) $$files($$ZXING_ROOT/core/src/*.c, true)
ZXING_SOURCES -= $$files($$ZXING_ROOT/core/src/libzint/*.cpp, true) $$files($$ZXING_ROOT/core/src/libzint/*.c, true)
ZXING_SOURCES -= $$files($$ZXING_ROOT/core/src/*Writer.cpp, true) $$files($$ZXING_ROOT/core/src/*Encoder.cpp, true) $$files($$ZXING_ROOT/core/src/*Token.cpp, true)
ZXING_SOURCES -= $$ZXING_ROOT/core/src/qrcode/QRMaskUtil.cpp $$ZXING_ROOT/core/src/qrcode/QRMatrixUtil.cpp \
                 $$ZXING_ROOT/core/src/datamatrix/DMSymbolInfo.cpp $$ZXING_ROOT/core/src/TextEncoder.cpp \
                 $$ZXING_ROOT/core/src/MultiFormatWriter.cpp $$ZXING_ROOT/core/src/ZXingC.cpp
INCLUDEPATH += $$PWD $$PWD/src

SOURCES  += $$ZXING_SOURCES
HEADERS  += onnxruntime_c_api.h onnxruntime_ep_c_api.h

include(src/shared/shared.pri)
include(src/platform/platform.pri)
include(src/annotate/annotate.pri)
include(src/text/engines/engines.pri)
include(src/text/cards/cards.pri)
include(src/capture/capture.pri)
include(src/pin/pin.pri)
include(src/longshot/longshot.pri)
include(src/app/app.pri)
include(src/tests/tests.pri)
exists($$PWD/icons/icons.qrc) {
    RESOURCES += icons/icons.qrc
}
TRANSLATIONS += translations/nami_zh_CN.ts translations/nami_en_US.ts
TR_EXCLUDE += *_tests.cpp
CONFIG += lrelease
LRELEASE_DIR = .
# qmake's lrelease feature writes beside the build target; copy only the
# compiled QM packs into the runtime language folder.
QMAKE_POST_LINK = cmd /c "if not exist \"$$PWD\\release\\language\" mkdir \"$$PWD\\release\\language\" & copy /Y \"$$PWD\\release\\nami_*.qm\" \"$$PWD\\release\\language\\\""
DISTFILES += onnxruntime-LICENSE.txt third_party/zxing-cpp/LICENSE third_party/zxing-cpp/README.md
