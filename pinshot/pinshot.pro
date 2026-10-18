QT       += core gui widgets concurrent svg network
LIBS     += -limm32 -lgdi32 -ldwmapi -loleacc -lole32 -loleaut32
TARGET    = pinshot
TEMPLATE  = app
CONFIG   += c++17
SOURCES  += main.cpp screen_capture.cpp output.cpp test_support.cpp \
            pin_window.cpp widgets.cpp long_capture.cpp window_detection.cpp \
            pixel_inspector.cpp ocr_engine.cpp ocr_dialog.cpp capture_overlay.cpp \
            annotations.cpp controller.cpp translate_gateway.cpp smart_erase.cpp \
            pin_window_tests.cpp widgets_tests.cpp long_capture_tests.cpp \
            window_detection_tests.cpp ocr_engine_tests.cpp ocr_dialog_tests.cpp ocr_workflow_tests.cpp \
            capture_overlay_tests.cpp color_picker_tests.cpp annotation_tests.cpp smart_erase_tests.cpp
HEADERS  += screen_capture.h output.h test_support.h pin_window.h widgets.h \
            long_capture.h window_detection.h pixel_inspector.h ocr_engine.h \
            ocr_dialog.h capture_overlay.h controller.h \
            translate_gateway.h smart_erase.h \
            onnxruntime_c_api.h onnxruntime_ep_c_api.h
RESOURCES += icons/icons.qrc
DISTFILES += onnxruntime-LICENSE.txt
