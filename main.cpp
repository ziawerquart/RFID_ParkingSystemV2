#include <QtGui/QApplication>
#include "widget.h"
#include <QTextCodec>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QTextCodec *utf8Codec = QTextCodec::codecForName("UTF-8");
    if(utf8Codec)
    {
        QTextCodec::setCodecForLocale(utf8Codec);
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
        QTextCodec::setCodecForTr(utf8Codec);
#endif
    }
    Widget w;
    w.show();

    return a.exec();
}
