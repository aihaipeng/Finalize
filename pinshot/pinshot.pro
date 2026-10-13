QT       += core gui widgets concurrent
LIBS     += -limm32 -lgdi32 -ldwmapi -loleacc -lole32 -loleaut32
TARGET    = pinshot
TEMPLATE  = app
CONFIG   += c++17
SOURCES  += main.cpp
HEADERS  += onnxruntime_c_api.h onnxruntime_ep_c_api.h
DISTFILES += onnxruntime-LICENSE.txt
