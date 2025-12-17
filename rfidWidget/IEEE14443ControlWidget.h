#ifndef IEEE14443CONTROLWIDGET_H
#define IEEE14443CONTROLWIDGET_H

#include <QWidget>
//#include <qextserialport.h>
#include "posix_qextserialport.h"

namespace Ui {
    class IEEE14443ControlWidget;
}

class IEEE14443ControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit IEEE14443ControlWidget(QWidget *parent = 0);
    ~IEEE14443ControlWidget();
    static void showOut();

protected:
    void showEvent(QShowEvent *);
    void hideEvent(QHideEvent *);

public slots:
    bool start(const QString &port);
    bool stop();

signals:
    void recvPackage(QByteArray pkg);

private:
    Ui::IEEE14443ControlWidget *ui;
    //QextSerialPort *commPort;
    Posix_QextSerialPort *commPort;
    QTimer *readTimer;
    QByteArray lastSendPackage;
    int recvStatus;
    QByteArray lastRecvPackage;


private:
    bool sendData(const QByteArray &data);
    void resetBlockList(int min, int max, int secSize);
    void resetStatus();

private slots:
    void on_pushButton_clicked();
    void on_writeBtn_clicked();
    void on_readBtn_clicked();
    void on_authCheckBtn_clicked();
    void on_selCardBtn_clicked();
    void on_getIdBtn_clicked();
    void on_searchCardBtn_clicked();
    void on_clearDisplayBtn_clicked();
    void onPortDataReady();
    void onRecvedPackage(QByteArray pkg);
    void onStatusListScrollRangeChanced(int min, int max);
};

#endif // IEEE14443CONTROLWIDGET_H
