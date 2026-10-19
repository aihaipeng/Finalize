QT       += core gui widgets concurrent svg network
LIBS     += -limm32 -lgdi32 -ldwmapi -loleacc -lole32 -loleaut32
TARGET    = pinshot
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
SOURCES  += $$ZXING_SOURCES \
            barcode_engine.cpp \
            main.cpp screen_capture.cpp output.cpp test_support.cpp \
            pin_window.cpp widgets.cpp long_capture.cpp window_detection.cpp \
            pixel_inspector.cpp ocr_engine.cpp ocr_dialog.cpp capture_overlay.cpp \
            annotations.cpp controller.cpp translate_gateway.cpp smart_erase.cpp \
            pin_window_tests.cpp widgets_tests.cpp long_capture_tests.cpp \
            window_detection_tests.cpp ocr_engine_tests.cpp ocr_dialog_tests.cpp ocr_workflow_tests.cpp \
            capture_overlay_tests.cpp color_picker_tests.cpp annotation_tests.cpp smart_erase_tests.cpp \
            barcode_tests.cpp round_refresh_tests.cpp
HEADERS  += screen_capture.h output.h test_support.h pin_window.h widgets.h \
            long_capture.h window_detection.h pixel_inspector.h ocr_engine.h \
            ocr_dialog.h barcode_engine.h capture_overlay.h controller.h \
            translate_gateway.h smart_erase.h \
            onnxruntime_c_api.h onnxruntime_ep_c_api.h
RESOURCES += icons/icons.qrc
DISTFILES += onnxruntime-LICENSE.txt third_party/zxing-cpp/LICENSE third_party/zxing-cpp/README.md
