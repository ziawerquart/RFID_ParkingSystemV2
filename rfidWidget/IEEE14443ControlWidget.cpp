#include "IEEE14443ControlWidget.h"
#include "ui_IEEE14443ControlWidget.h"
//#include "IEEE1443PackageWidget.h"
//#include <IEEE1443Package.h>
#include<rfidWidget/IEEE1443Package.h>
#include <QMessageBox>
#include <QScrollBar>
#include <QDebug>
//#include <ioportManager.h>
#include<rfidWidget/ioportManager.h>

IEEE14443ControlWidget::IEEE14443ControlWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::IEEE14443ControlWidget),
    commPort(NULL),
    recvStatus(0)
{
    ui->setupUi(this);
    ui->dataEdit->setOverwriteMode(true);
    ui->dataEdit->setAddressArea(false);

    QByteArray defAuthKey(6, 0xFF);
    ui->authKeyEdit->setOverwriteMode(true);
    ui->authKeyEdit->setAddressArea(false);
    ui->authKeyEdit->setAsciiArea(false);
    ui->authKeyEdit->setHighlighting(false);
    ui->authKeyEdit->setBytesperLine(6);
    ui->authKeyEdit->setData(defAuthKey);

    connect(this, SIGNAL(recvPackage(QByteArray)), this, SLOT(onRecvedPackage(QByteArray)));
//    connect(ui->statusList->verticalScrollBar(), SIGNAL(rangeChanged(int,int)), this, SLOT(onStatusListScrollRangeChanced(int,int)));
    resetStatus();
}

IEEE14443ControlWidget::~IEEE14443ControlWidget()
{
    delete ui;
}

bool IEEE14443ControlWidget::start(const QString &port)
{
    if(commPort != NULL)
        return false;
    //commPort = new QextSerialPort(port,  QextSerialPort::EventDriven);
    commPort = new Posix_QextSerialPort(port,  QextSerialBase::Polling);
    commPort->setBaudRate(BAUD19200);
    commPort->setFlowControl(FLOW_OFF);
    commPort->setParity(PAR_NONE);
    commPort->setDataBits(DATA_8);
    commPort->setStopBits(STOP_1);

    if (commPort->open(QIODevice::ReadWrite) == true) {
        //connect(commPort, SIGNAL(readyRead()), this, SLOT(onPortDataReady()));
        readTimer = new QTimer(this);   //设置读取计时器
        readTimer->start(100);  //设置延时为100ms
        connect(readTimer,SIGNAL(timeout()),this,SLOT(onPortDataReady()));
        return true;
    }
    else {
        qDebug() << "device failed to open:" << commPort->errorString();
        delete commPort;
        commPort = NULL;
        return false;
    }
}

bool IEEE14443ControlWidget::stop()
{
    if(commPort != NULL)
    {
        commPort->close();
        delete commPort;
    }
    commPort = NULL;
    return true;
}

bool IEEE14443ControlWidget::sendData(const QByteArray &data)
{
//    IEEE1443PackageWidget *w = new IEEE1443PackageWidget(tr("Send"), data, this);
//    ui->statusListLayout->addWidget(w);
//    w->show();
    if(commPort)
    {
        //qDebug()<<"send data = "<<data.toHex();
        //qDebug()<<"rawPackage = "<<IEEE1443Package(data).toRawPackage().toHex();
        commPort->write(IEEE1443Package(data).toRawPackage());
    }
    return true;
}

void IEEE14443ControlWidget::resetBlockList(int min, int max, int secSize)
{
    ui->blockNumberBox->clear();
    int i;
    for(i = 0; i <= max; i++)
    {
        if((i % secSize) == (secSize - 1))
            continue;
        ui->blockNumberBox->addItem(QString::number(i));
    }
}

void IEEE14443ControlWidget::resetStatus()
{
    ui->s50CardBtn->setChecked(false);
    ui->s70CardBtn->setChecked(false);
    ui->selCardIdEdit->setText("");
    QByteArray empty(16, 0x00);
    ui->dataEdit->setData(empty);
    resetBlockList(1, 63, 4);
    ui->resultLabel->setText("");
}

// 串口接收数据函数, 通常不需要修改
void IEEE14443ControlWidget::onPortDataReady()
{
    QByteArray bytes;
    int a = commPort->bytesAvailable();
    bytes.resize(a);
    char *p = bytes.data();
    int len = bytes.size();
    commPort->read(p, len);
    while(len--)
    {
        switch(recvStatus)
        {
        case 0:// recv sync code
            if(*p == 0x02)
            {
                recvStatus = 1;
                lastRecvPackage.clear();
                lastRecvPackage.append(*p);
            }
            p++;
            break;
        case 1:// normal recv
            if(*p == 0x10)
                recvStatus = 2;
            else if(*p == 0x03)
            {
                // recved a end code
                lastRecvPackage.append(*p);
                emit recvPackage(lastRecvPackage);
                recvStatus = 0;
            }
            else // just append the data
                lastRecvPackage.append(*p);
            p++;
            break;
        case 2:// recved a escape code last time, so just append data to the package
            lastRecvPackage.append(*p++);
            recvStatus = 1;
            break;
        }
    }
}

// 接收到完整数据包的处理函数
void IEEE14443ControlWidget::onRecvedPackage(QByteArray pkg)
{
//    IEEE1443PackageWidget *w = new IEEE1443PackageWidget(tr("Recv"), pkg, this);
//    ui->statusListLayout->addWidget(w);
//    w->show();
    IEEE1443Package p(pkg);
    //qDebug()<<"the recieve pkg"<<pkg.toHex();
    QByteArray d = p.data();
    int status = d.at(0);
    d = d.mid(1);
    QString resultTipText;
    switch(p.command())
    {
    case IEEE1443Package::SearchCard:
        resultTipText = tr("Search Card ");
        if(status == 0)
            resultTipText += tr("Succeed");
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::AntiColl:
        resultTipText = tr("AntiColl ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            resultTipText += tr(", Card Id is %1").arg(QString(d.toHex()));
            ui->selCardIdEdit->setText(d.toHex());
        }
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::SelectCard:
        resultTipText = tr("Select Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            resultTipText += tr(", Type is %1").arg(d.at(0) == 0x08 ? "S50" : "S70");
            if(d.at(0) == 0x08) // S50
            {
                ui->s50CardBtn->setChecked(true);
                ui->s70CardBtn->setChecked(false);
                resetBlockList(1, 63, 4);
            }
            else
            {
                ui->s50CardBtn->setChecked(false);
                ui->s70CardBtn->setChecked(true);
                resetBlockList(1, 255, 4);
            }
        }
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::Authentication:
        resultTipText = tr("Authentication ");
        if(status == 0)
            resultTipText += tr("Succeed");
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::ReadCard:
        resultTipText = tr("Read Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            // 读卡指令的响应, 可以获得卡内数据
            // 读取正常
            ui->dataEdit->setData(d);
        }
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::WriteCard:
        resultTipText = tr("Write Card ");
        if(status == 0)
            resultTipText += tr("Succeed");
        else
            resultTipText += tr("Failure");
        break;
    }
    ui->resultLabel->setText(resultTipText);
}

void IEEE14443ControlWidget::onStatusListScrollRangeChanced(int min, int max)
{
//    ui->statusList->verticalScrollBar()->setValue(max);
}

void IEEE14443ControlWidget::on_clearDisplayBtn_clicked()
{
//    while(ui->statusListLayout->count())
//    {
//        QLayoutItem *item = ui->statusListLayout->itemAt(0);
//        ui->statusListLayout->removeItem(item);
//        if(item->widget())
//            delete item->widget();
//    }
    ui->resultLabel->setText("");
}

void IEEE14443ControlWidget::on_searchCardBtn_clicked()
{


        lastSendPackage = IEEE1443Package(0, 0x46, 0x52).toPurePackage();
        sendData(lastSendPackage);
        // 如果重新寻卡,则重置卡信息
        resetStatus();

}

void IEEE14443ControlWidget::on_getIdBtn_clicked()
{
    lastSendPackage = IEEE1443Package(0, 0x47, 0x04).toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::on_selCardBtn_clicked()
{
    if(ui->selCardIdEdit->text().length() != 8)
    {
        QMessageBox::warning(this, tr("Warning"), tr("card id error"));
        return;
    }
    QByteArray cardId = QByteArray::fromHex(ui->selCardIdEdit->text().toAscii());
    IEEE1443Package pkg(0, 0x48, cardId);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::on_authCheckBtn_clicked()
{
    if(ui->selCardIdEdit->text().length() != 8)
    {
        QMessageBox::warning(this, tr("Warning"), tr("card id error"));
        return;
    }
//    if(ui->authKeyEdit->text().length() != 12)
//    {
//        QMessageBox::warning(this, tr("Warning"), tr("auth key error"));
//        return;
//    }
    QByteArray authInfo;
    authInfo.append(0x60);      // Always use Type A
    authInfo.append((char)(ui->blockNumberBox->currentText().toLong()));
    authInfo.append(ui->authKeyEdit->data());
    if(authInfo.size() != 8)
    {
        QMessageBox::warning(this, tr("Warning"), tr("auth key error"));
        return;
    }
    IEEE1443Package pkg(0, 0x4A, authInfo);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::on_readBtn_clicked()
{
    IEEE1443Package pkg(0, 0x4B, (char)ui->blockNumberBox->currentText().toLong());
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::on_writeBtn_clicked()
{
    QByteArray writeInfo;
    writeInfo.append((char)ui->blockNumberBox->currentText().toLong());
    writeInfo.append(ui->dataEdit->data());
    if(writeInfo.size() != 17)
    {
        QMessageBox::warning(this, tr("Warning"), tr("write data error"));
        return;
    }
    IEEE1443Package pkg(0, IEEE1443Package::WriteCard, writeInfo);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::on_pushButton_clicked()
{
    if(ui->pushButton->text() == tr("BegainSearch"))
    {
        //qDebug()<<"set mode success";
        IOPortManager::setMode(Mode13_56M1);
        this->start("/dev/ttyS0");
        ui->pushButton->setText(tr("Stop"));
    }
    else
    {
        this->stop();
        ui->pushButton->setText(tr("BegainSearch"));
    }
}

void IEEE14443ControlWidget::showEvent(QShowEvent *)
{
    if(ui->pushButton->text() == tr("BegainSearch"))
    {
        //qDebug()<<"set mode success";
        IOPortManager::setMode(Mode13_56M1);
        this->start("/dev/ttyS0");
        ui->pushButton->setText(tr("Stop"));
    }
}

void IEEE14443ControlWidget::hideEvent(QHideEvent *)
{
    if(ui->pushButton->text() != tr("BegainSearch"))
    {
        this->stop();
        ui->pushButton->setText(tr("BegainSearch"));
    }
}
static IEEE14443ControlWidget *ieee14443ControlWidget;
void IEEE14443ControlWidget::showOut()
{
    if(ieee14443ControlWidget == NULL)
        ieee14443ControlWidget = new IEEE14443ControlWidget();
    //ieee14443ControlWidget->showFullScreen();
    ieee14443ControlWidget->show();
}
