#-------------------------------------------------
#
# Project created by QtCreator 2012-05-04T09:32:40
#
#-------------------------------------------------

QT       += core gui

TARGET = ex27_RFID_IEEE14443_Search
TEMPLATE = app


SOURCES += main.cpp\
        widget.cpp \
    rfidWidget/IEEE14443ControlWidget.cpp \
    rfidWidget/IEEE1443Package.cpp \
    rfidWidget/ioportmanager.cpp \
    rfidWidget/xbytearray.cpp \
    rfidWidget/qhexedit_p.cpp \
    rfidWidget/qhexedit.cpp \
    rfidWidget/commands.cpp \
    rfidWidget/qextserialbase.cpp \
    rfidWidget/posix_qextserialport.cpp

HEADERS  += widget.h \
    rfidWidget/IEEE14443ControlWidget.h \
    rfidWidget/IEEE1443Package.h \
    rfidWidget/ioportManager.h \
    rfidWidget/xbytearray.h \
    rfidWidget/qhexedit_p.h \
    rfidWidget/qhexedit.h \
    rfidWidget/commands.h \
    rfidWidget/qextserialbase.h \
    rfidWidget/posix_qextserialport.h

FORMS    += widget.ui \
    rfidWidget/IEEE14443ControlWidget.ui
