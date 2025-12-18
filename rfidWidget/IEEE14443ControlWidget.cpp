#include "IEEE14443ControlWidget.h"
#include "ui_IEEE14443ControlWidget.h"
//#include "IEEE1443PackageWidget.h"
//#include <IEEE1443Package.h>
#include<rfidWidget/IEEE1443Package.h>
#include <QMessageBox>
#include <QScrollBar>
#include <QDebug>
#include <QDateTime>
//#include <ioportManager.h>
#include<rfidWidget/ioportManager.h>

static const char kTagSignature1 = 'P';
static const char kTagSignature2 = 'K';
static const int kUserBlock1 = 1;
static const int kUserBlock2 = 2;
static const int kHourFee = 5;

IEEE14443ControlWidget::IEEE14443ControlWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::IEEE14443ControlWidget),
    commPort(NULL),
    autoSearchTimer(NULL),
    recvStatus(0),
    waitingReply(false),
    tagAuthenticated(false),
    pendingReadBlock(-1),
    pendingWriteBlock(-1),
    requiresInitialization(false),
    refreshAfterWrite(false),
    registrationPaused(false)
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
    autoSearchTimer = new QTimer(this);
    autoSearchTimer->setInterval(500);
    connect(autoSearchTimer, SIGNAL(timeout()), this, SLOT(onAutoSearchTimeout()));
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
        startAutoSearch();
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
    stopAutoSearch();
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
        QByteArray rawPackage = IEEE1443Package(data).toRawPackage();
        qDebug() << QString("[EMU] TX %1").arg(QString(rawPackage.toHex()));
        commPort->write(rawPackage);
        waitingReply = true;
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
    ui->balanceEdit->setText("");
    ui->parkingStatusLabel->setText("");
    waitingReply = false;
    tagAuthenticated = false;
    currentCardId.clear();
    lastBlock1.clear();
    lastBlock2.clear();
    pendingReadBlock = -1;
    pendingWriteBlock = -1;
    currentInfo = TagInfo();
    pendingWriteInfo = TagInfo();
    requiresInitialization = false;
    refreshAfterWrite = false;
    registrationPaused = false;
    lastEntryTimeMap.clear();
    lastExitTimeMap.clear();
    entryTimeMap.clear();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
}

void IEEE14443ControlWidget::startAutoSearch()
{
    if(registrationPaused)
        return;
    if(autoSearchTimer && !autoSearchTimer->isActive())
        autoSearchTimer->start();
}

void IEEE14443ControlWidget::stopAutoSearch()
{
    if(autoSearchTimer)
        autoSearchTimer->stop();
}

void IEEE14443ControlWidget::pauseForRegistration()
{
    registrationPaused = true;
    stopAutoSearch();
}

void IEEE14443ControlWidget::resumeAfterRegistration()
{
    if(!registrationPaused)
        return;
    registrationPaused = false;
    startAutoSearch();
}

void IEEE14443ControlWidget::requestSearch()
{
    if(waitingReply)
        return;
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::SearchCard, 0x52).toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::requestAntiColl()
{
    if(waitingReply)
        return;
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::AntiColl, 0x04).toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::requestSelect(const QByteArray &cardId)
{
    if(waitingReply)
        return;
    IEEE1443Package pkg(0, IEEE1443Package::SelectCard, cardId);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::requestAuth(quint8 blockNumber)
{
    QByteArray authInfo;
    authInfo.append(0x60);
    authInfo.append((char)blockNumber);
    authInfo.append(ui->authKeyEdit->data());
    if(authInfo.size() != 8)
    {
        QMessageBox::warning(this, tr("Warning"), tr("auth key error"));
        return;
    }
    IEEE1443Package pkg(0, IEEE1443Package::Authentication, authInfo);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

void IEEE14443ControlWidget::requestRead(quint8 blockNumber)
{
    pendingReadBlock = blockNumber;
    IEEE1443Package pkg(0, 0x4B, (char)blockNumber);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

//写函数
void IEEE14443ControlWidget::requestWrite(quint8 blockNumber, const QByteArray &data)
{
    if(data.size() != 16)
    {
        QMessageBox::warning(this, tr("Warning"), tr("write data error"));
        return;
    }
    pendingWriteBlock = blockNumber;
    QByteArray writeInfo;
    writeInfo.append((char)blockNumber);//块号
    writeInfo.append(data);//信息
    IEEE1443Package pkg(0, IEEE1443Package::WriteCard, writeInfo);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}

static char vehicleCodeFromText(const QString &text)
{
    if(text == "Sedan")
        return 1;
    if(text == "SUV")
        return 2;
    if(text == "Truck")
        return 3;
    if(text == "Electric")
        return 4;
    return 0;
}

static QString vehicleTextFromCode(char c)
{
    switch((int)(unsigned char)c)
    {
    case 1:
        return "Sedan";
    case 2:
        return "SUV";
    case 3:
        return "Truck";
    case 4:
        return "Electric";
    default:
        return "Other";
    }
}

bool IEEE14443ControlWidget::decodeTagInfo(const QByteArray &b1, const QByteArray &b2, TagInfo &info)
{
    info.valid = false;
    if(b1.size() != 16 || b2.size() != 16)
        return false;
    if((b1.at(0) != kTagSignature1) || (b1.at(1) != kTagSignature2))
        return false;
    info.owner = QString::fromLatin1(b1.constData() + 4, 12).trimmed();
    info.vehicleType = vehicleTextFromCode(b1.at(3));
    int bal = 0;
    bal |= (quint8)b2.at(0);
    bal |= ((quint8)b2.at(1)) << 8;
    bal |= ((quint8)b2.at(2)) << 16;
    bal |= ((quint8)b2.at(3)) << 24;
    info.balance = bal;
    info.valid = true;
    return true;
}

//turn tageinfo object to 2 16byte block
void IEEE14443ControlWidget::encodeTagInfo(const TagInfo &info, QByteArray &b1, QByteArray &b2)
{
    b1 = QByteArray(16, 0x00);
    b2 = QByteArray(16, 0x00);
    b1[0] = kTagSignature1;//P
    b1[1] = kTagSignature2;//K
    b1[2] = 0x01;
    b1[3] = vehicleCodeFromText(info.vehicleType);
    QByteArray nameBytes = info.owner.left(12).toLatin1();
    int i;
    for(i = 0; i < nameBytes.size() && i < 12; ++i)
        b1[4 + i] = nameBytes.at(i);
    b2[0] = (char)(info.balance & 0xFF);
    b2[1] = (char)((info.balance >> 8) & 0xFF);
    b2[2] = (char)((info.balance >> 16) & 0xFF);
    b2[3] = (char)((info.balance >> 24) & 0xFF);
}

void IEEE14443ControlWidget::updateInfoDisplay(const TagInfo &info)
{
    ui->ownerNameEdit->setText(info.owner);
    int idx = ui->vehicleTypeBox->findText(info.vehicleType);
    if(idx < 0)
        idx = ui->vehicleTypeBox->findText("Other");
    if(idx < 0)
        idx = 0;
    ui->vehicleTypeBox->setCurrentIndex(idx);
    ui->balanceEdit->setText(QString::number(info.balance));
    currentInfo = info;
}

void IEEE14443ControlWidget::updateInfoPanel(const TagInfo &info, const QDateTime &entryTime, const QDateTime &exitTime)
{
    ui->infoOwnerValue->setText(info.valid ? info.owner : tr("N/A"));
    ui->infoVehicleValue->setText(info.valid ? info.vehicleType : tr("N/A"));
    ui->infoEntryTimeValue->setText(entryTime.isValid() ? entryTime.toString("hh:mm:ss") : tr("--"));
    ui->infoExitTimeValue->setText(exitTime.isValid() ? exitTime.toString("hh:mm:ss") : tr("--"));
    if(info.valid)
        ui->infoBalanceValue->setText(QString::number(info.balance));
    else
        ui->infoBalanceValue->setText(tr("--"));
}

IEEE14443ControlWidget::TagInfo IEEE14443ControlWidget::defaultTagInfo() const
{
    TagInfo info;
    info.owner = ui->ownerNameEdit->text();
    if(info.owner.isEmpty())
        info.owner = tr("Unknown");
    info.vehicleType = ui->vehicleTypeBox->currentText();
    info.balance = ui->rechargeSpin->value();
    info.valid = true;
    return info;
}

void IEEE14443ControlWidget::ensureInitialized()
{
    TagInfo info;
    if(decodeTagInfo(lastBlock1, lastBlock2, info))
    {
        requiresInitialization = false;
        resumeAfterRegistration();
        updateInfoDisplay(info);
        QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ? entryTimeMap.value(currentCardId) : lastEntryTimeMap.value(currentCardId);
        updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
        handleParkingFlow();
        return;
    }
    if(!requiresInitialization)
        QMessageBox::information(this, tr("Initialization"), tr("Please fill user info and click Register to initialize the card."));
    entryTimeMap.remove(currentCardId);
    lastEntryTimeMap.remove(currentCardId);
    requiresInitialization = true;
    pauseForRegistration();
    currentInfo = TagInfo();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
    ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));
}

int IEEE14443ControlWidget::calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const
{
    int secs = (int)enterTime.secsTo(leaveTime);
    if(secs < 0)
        secs = 0;
    int minutes = secs / 60;
    int hours = (minutes + 59) / 60;
    return hours * kHourFee;
}

void IEEE14443ControlWidget::handleParkingFlow()
{
    if(currentCardId.isEmpty() || !currentInfo.valid)
        return;
    if(requiresInitialization)
    {
        ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));
        entryTimeMap.remove(currentCardId);
        lastEntryTimeMap.remove(currentCardId);
        return;
    }
    QDateTime now = QDateTime::currentDateTime();
    if(entryTimeMap.contains(currentCardId))
    {
        QDateTime enter = entryTimeMap.value(currentCardId);
        int fee = calculateFee(enter, now);
        if(currentInfo.balance < fee)
        {
            ui->parkingStatusLabel->setText(tr("Insufficient balance, fee %1").arg(fee));
            updateInfoPanel(currentInfo, enter, QDateTime());
            QMessageBox::warning(this, tr("Recharge"), tr("Balance is not enough, please recharge before leaving."));
            return;
        }
        entryTimeMap.remove(currentCardId);
        lastExitTimeMap.insert(currentCardId, now);
        lastEntryTimeMap.insert(currentCardId, enter);
        currentInfo.balance -= fee;
        ui->parkingStatusLabel->setText(tr("Stayed %1 min, fee %2").arg(enter.secsTo(now)/60).arg(fee));
        updateInfoDisplay(currentInfo);
        updateInfoPanel(currentInfo, QDateTime(), now);
        refreshAfterWrite = false;
        writeUpdatedInfo(currentInfo);
    }
    else
    {
        entryTimeMap.insert(currentCardId, now);
        lastEntryTimeMap.insert(currentCardId, now);
        ui->parkingStatusLabel->setText(tr("Entry time %1").arg(now.toString("hh:mm:ss")));
        updateInfoPanel(currentInfo, now, QDateTime());
    }
}

void IEEE14443ControlWidget::writeUpdatedInfo(const TagInfo &info)
{
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    QByteArray b1;
    QByteArray b2;
    encodeTagInfo(info, b1, b2);//把车主信息编写成两个块
    pendingWriteInfo = info;
    requestWrite(kUserBlock1, b1);//块号，data//写入块1

    lastBlock1 = b1;
    lastBlock2 = b2;
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
    qDebug() << QString("[EMU] RX cmd=0x%1 data=%2")
                .arg(p.command(), 2, 16, QChar('0'))
                .arg(QString(p.data().toHex()));
    QByteArray d = p.data();
    int status = d.at(0);
    d = d.mid(1);
    waitingReply = false;
    QString resultTipText;
    switch(p.command())
    {
    case IEEE1443Package::SearchCard:
        resultTipText = tr("Search Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            requestAntiColl();
        }
        else
            resultTipText += tr("Failure");
        break;
    case IEEE1443Package::AntiColl:
        resultTipText = tr("AntiColl ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            resultTipText += tr(", Card Id is %1").arg(QString(d.toHex()));
            currentCardId = d.toHex();
            ui->selCardIdEdit->setText(currentCardId);
            requestSelect(d);
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
            tagAuthenticated = false;
            //停车系统实现自动识别卡片功能，自动进行认证
            requestAuth(kUserBlock1);
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
        tagAuthenticated = (status == 0);
        if(tagAuthenticated)
            requestRead(kUserBlock1);//停车系统实现自动识别卡片功能，自动读块1用于判断是不是停车系统
        break;
    case IEEE1443Package::ReadCard:
        resultTipText = tr("Read Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            // 读卡指令的响应, 可以获得卡内数据
            // 读取正常
            ui->dataEdit->setData(d);

            //停车系统新增
            if(pendingReadBlock == kUserBlock1)
            {
                lastBlock1 = d;
                requestRead(kUserBlock2);//读完块1，自动读块2
            }
            else if(pendingReadBlock == kUserBlock2)
            {
                lastBlock2 = d;
                pendingReadBlock = -1;
                ensureInitialized();
            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingReadBlock = -1;
        }
        break;
    case IEEE1443Package::WriteCard:
        resultTipText = tr("Write Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            if(pendingWriteBlock == kUserBlock1)
            {
                pendingWriteBlock = -1;
                requestWrite(kUserBlock2, lastBlock2);
            }
            else if(pendingWriteBlock == kUserBlock2)
            {
                pendingWriteBlock = -1;
                updateInfoDisplay(pendingWriteInfo);
                if(refreshAfterWrite)
                {
                    refreshAfterWrite = false;
                    requestRead(kUserBlock1);
                }
                else
                {
                    QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ? entryTimeMap.value(currentCardId) : lastEntryTimeMap.value(currentCardId);
                    updateInfoPanel(pendingWriteInfo, entryDisplayTime, lastExitTimeMap.value(currentCardId));
                }
            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingWriteBlock = -1;
            refreshAfterWrite = false;
        }
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

void IEEE14443ControlWidget::on_registerBtn_clicked()
{
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    TagInfo info = defaultTagInfo();
    refreshAfterWrite = true;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("Registering user info"));
}

void IEEE14443ControlWidget::on_rechargeBtn_clicked()
{
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    if(requiresInitialization || !currentInfo.valid)
    {
        QMessageBox::information(this, tr("Initialization"), tr("Please register the card before recharging."));
        return;
    }
    TagInfo info = currentInfo;
    info.balance += ui->rechargeSpin->value();
    refreshAfterWrite = false;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("Recharged"));
}

void IEEE14443ControlWidget::onAutoSearchTimeout()
{
    if(registrationPaused || requiresInitialization)
        return;
    requestSearch();
}

void IEEE14443ControlWidget::on_searchCardBtn_clicked()
{


        resetStatus();
        requestSearch();

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
