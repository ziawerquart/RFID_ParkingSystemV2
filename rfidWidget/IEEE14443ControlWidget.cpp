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

//块1前两字节签名，用来判断这张卡是不是“停车系统卡”
static const char kTagSignature1 = 'P';
static const char kTagSignature2 = 'K';
//使用块1和块2写入
static const int kUserBlock1 = 1;
static const int kUserBlock2 = 2;
//计费单价
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
    registrationPaused(false),
    registrationFlowActive(false),
    registrationVerificationPending(false),
    registrationWritePending(false),
    registrationPendingStatusText(),
    registrationAwaitingRemoval(false),
    registrationAwaitingCardId(),
    rechargePaused(false),
    pendingExitFee(0),
    parkingFlowPaused(false),
    parkingFlowState(ParkingFlowIdle),
    parkingExitWritePending(false),
    lastExitFee(0)
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

    //收到包信号和处理包信息号
    connect(this, SIGNAL(recvPackage(QByteArray)), this, SLOT(onRecvedPackage(QByteArray)));
//  connect(ui->statusList->verticalScrollBar(), SIGNAL(rangeChanged(int,int)), this, SLOT(onStatusListScrollRangeChanced(int,int)));

    //设置自动寻卡定时器，实现自动刷卡
    autoSearchTimer = new QTimer(this);
    autoSearchTimer->setInterval(500);
    //连接信号到槽函数
    connect(autoSearchTimer, SIGNAL(timeout()), this, SLOT(onAutoSearchTimeout()));
    resetStatus();
}

IEEE14443ControlWidget::~IEEE14443ControlWidget()
{
    delete ui;
}

//开始逻辑
bool IEEE14443ControlWidget::start(const QString &port)
{
    //设置串口参数
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
        //设置读取计时器——用于读取包
        //connect(commPort, SIGNAL(readyRead()), this, SLOT(onPortDataReady()));
        readTimer = new QTimer(this);   //初始化计时器
        readTimer->start(100);  //设置延时为100ms
        connect(readTimer,SIGNAL(timeout()),this,SLOT(onPortDataReady()));

        //新增——开始自动读取
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
//发数据
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
        qDebug() << QString("send %1").arg(QString(rawPackage.toHex()));
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
    registrationFlowActive = false;
    registrationVerificationPending = false;
    registrationWritePending = false;
    registrationPendingStatusText.clear();
    registrationAwaitingRemoval = false;
    registrationAwaitingCardId.clear();
    rechargePaused = false;
    pendingExitFee = 0;
    parkingFlowPaused = false;
    parkingFlowState = ParkingFlowIdle;
    parkingExitWritePending = false;
    lastExitFee = 0;
    lastEntryTimeMap.clear();
    lastExitTimeMap.clear();
    entryTimeMap.clear();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
}

//自动寻卡控制========================================================
//开始自动寻卡
void IEEE14443ControlWidget::startAutoSearch()
{
    //检查是否需要暂停自动寻卡
    if(registrationPaused || rechargePaused || parkingFlowPaused)//当因注册或收费需要暂停时，不自动寻卡
        return;
    // 检查定时器是否有效且未激活
    if(autoSearchTimer && !autoSearchTimer->isActive())
        autoSearchTimer->start();//启动定时器
}
//停止自动寻卡
void IEEE14443ControlWidget::stopAutoSearch()
{
    if(autoSearchTimer)
        autoSearchTimer->stop();
}
//用户注册时暂停寻卡
void IEEE14443ControlWidget::pauseForRegistration()
{
    registrationPaused = true;
    stopAutoSearch();
}
//用户注册结束继续寻卡
void IEEE14443ControlWidget::resumeAfterRegistration()
{
    if(!registrationPaused)
        return;
    registrationPaused = false;
    startAutoSearch();
}
//用户充值时暂停寻卡
void IEEE14443ControlWidget::pauseForRecharge(int feeRequired)
{
    rechargePaused = true;
    pendingExitFee = feeRequired;
    stopAutoSearch();
}
//用户充值结束继续寻卡
void IEEE14443ControlWidget::resumeAfterRecharge()
{
    if(!rechargePaused)
        return;
    rechargePaused = false;
    pendingExitFee = 0;
    startAutoSearch();
}

//发不同种类的包给卡片==================================================
//请求寻卡
void IEEE14443ControlWidget::requestSearch()
{
    if(waitingReply)
        return;
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::SearchCard, 0x52).toPurePackage();
    sendData(lastSendPackage);
}
//请求防冲突
void IEEE14443ControlWidget::requestAntiColl()
{
    if(waitingReply)
        return;
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::AntiColl, 0x04).toPurePackage();
    sendData(lastSendPackage);
}
//请求选卡
void IEEE14443ControlWidget::requestSelect(const QByteArray &cardId)
{
    if(waitingReply)
        return;
    IEEE1443Package pkg(0, IEEE1443Package::SelectCard, cardId);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}
//请求认证
void IEEE14443ControlWidget::requestAuth(quint8 blockNumber)
{
    if(waitingReply)
        return;
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
//请求读卡
void IEEE14443ControlWidget::requestRead(quint8 blockNumber)
{
    if(waitingReply)
        return;
    pendingReadBlock = blockNumber;
    IEEE1443Package pkg(0, 0x4B, (char)blockNumber);
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}
//请求写卡
void IEEE14443ControlWidget::requestWrite(quint8 blockNumber, const QByteArray &data)
{
    if(waitingReply)
        return;
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

//车辆信息<->数字
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
//info<->16Byte
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
//更新信息展示区:车主信息
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
//更新信息展示区:进出场时间
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
//生成待写入info
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
//检测是否初始化
void IEEE14443ControlWidget::ensureInitialized()
{
    //尝试解析
    TagInfo info;
    if(decodeTagInfo(lastBlock1, lastBlock2, info))//若解析成功
    {
        //不需要初始化
        requiresInitialization = false;
        if(registrationVerificationPending)//注册流程验证
        {
            registrationVerificationPending = false;
            registrationFlowActive = false;
            updateInfoDisplay(info);
            updateInfoPanel(info, QDateTime(), QDateTime());
            ui->parkingStatusLabel->setText(tr("写入成功，请立即收卡"));
            registrationAwaitingRemoval = true;
            registrationAwaitingCardId = currentCardId;
            resumeAfterRegistration();//继续自动寻卡
            return;
        }

        resumeAfterRegistration();//继续自动寻卡

        if(registrationAwaitingRemoval && registrationAwaitingCardId == currentCardId)
        {
            ui->parkingStatusLabel->setText(tr("写入成功，请立即收卡"));
            return;
        }
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();

        //更新展示信息
        updateInfoDisplay(info);
        QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ?
                                     entryTimeMap.value(currentCardId) :
                                     lastEntryTimeMap.value(currentCardId);
        updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
        //进行出入场逻辑（注册流程不进行出入场判断）
        if(!registrationFlowActive)
            handleParkingFlow();
        return;
    }
    handleInvalidCard();
}

//处理无效卡片，进入注册流程
void IEEE14443ControlWidget::handleInvalidCard()
{
    //清理停车状态
    entryTimeMap.remove(currentCardId);
    lastEntryTimeMap.remove(currentCardId);

    //注册写卡后验证失败
    if(registrationVerificationPending)
    {
        registrationVerificationPending = false;
        registrationFlowActive = false;
        requiresInitialization = false;
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();
        ui->parkingStatusLabel->setText(tr("写入失败，请重新刷卡"));
        QMessageBox::warning(this, tr("注册失败"), tr("写入失败，请重新刷卡"));
        resumeAfterRegistration();
        return;
    }

    //只在第一次进入未初始化状态时提示要初始化
    if(!requiresInitialization)
        QMessageBox::information(this, tr("未注册卡片"), tr("请先注册，不要收卡"));
    requiresInitialization = true;
    pauseForRegistration();
    currentInfo = TagInfo();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
    ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));

    if(!registrationFlowActive)
        startRegistrationFlow();
}

//弹出注册对话框，采集用户信息
bool IEEE14443ControlWidget::showRegistrationDialog(TagInfo &info)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("注册新卡"));
    QFormLayout form(&dialog);

    QLineEdit *nameEdit = new QLineEdit(&dialog);
    nameEdit->setMaxLength(12);
    nameEdit->setText(ui->ownerNameEdit->text());
    form.addRow(tr("姓名"), nameEdit);

    QComboBox *vehicleBox = new QComboBox(&dialog);
    vehicleBox->addItems(QStringList() << "Sedan" << "SUV" << "Truck" << "Electric" << "Other");
    int currentVehicleIndex = vehicleBox->findText(ui->vehicleTypeBox->currentText());
    vehicleBox->setCurrentIndex(currentVehicleIndex < 0 ? 0 : currentVehicleIndex);
    form.addRow(tr("车型"), vehicleBox);

    QSpinBox *balanceSpin = new QSpinBox(&dialog);
    balanceSpin->setRange(0, 100000);
    balanceSpin->setValue(ui->rechargeSpin->value());
    form.addRow(tr("初始余额"), balanceSpin);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    form.addRow(&buttons);
    connect(&buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
    connect(&buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

    if(dialog.exec() != QDialog::Accepted)
        return false;

    info.owner = nameEdit->text().isEmpty() ? tr("Unknown") : nameEdit->text();
    info.vehicleType = vehicleBox->currentText();
    info.balance = balanceSpin->value();
    info.valid = true;
    return true;
}

//开始注册流程：暂停寻卡并写入数据
void IEEE14443ControlWidget::startRegistrationFlow()
{
    registrationFlowActive = true;
    pauseForRegistration();
    TagInfo info;
    if(!showRegistrationDialog(info))
    {
        registrationFlowActive = false;
        requiresInitialization = false;
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();
        resumeAfterRegistration();
        ui->parkingStatusLabel->setText(tr("注册已取消"));
        return;
    }
    if(waitingReply)
    {
        registrationWritePending = true;
        registrationPendingInfo = info;
        registrationPendingStatusText = tr("正在注册...");
        ui->parkingStatusLabel->setText(tr("等待当前操作完成..."));
        return;
    }
    registrationVerificationPending = true;
    refreshAfterWrite = true;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("正在注册..."));
}
//计算停车费用
int IEEE14443ControlWidget::calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const
{
    int secs = (int)enterTime.secsTo(leaveTime);
    if(secs < 0)
        secs = 0;
    int minutes = secs / 60;
    int hours = (minutes + 59) / 60;
    //return hours * kHourFee;
    return secs;//测试用
}
//判断进出场
void IEEE14443ControlWidget::handleParkingFlow()
{
    if(registrationFlowActive)
        return;
    //检查当前卡信息
    if(currentCardId.isEmpty() || !currentInfo.valid)
        return;
    //判断是否初始化
    if(requiresInitialization)
    {
        ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));
        entryTimeMap.remove(currentCardId);
        lastEntryTimeMap.remove(currentCardId);
        return;
    }
    //记录当前时间
    QDateTime now = QDateTime::currentDateTime();
    //若entryTimeMap包括当前卡号——出场
    if(entryTimeMap.contains(currentCardId))
    {
        if(parkingFlowState == ParkingFlowIdle)
        {
            parkingFlowState = ParkingFlowExit;
            parkingFlowPaused = true;
            stopAutoSearch();
            ui->parkingStatusLabel->setText(tr("正在出场中，不要收卡"));
        }
        //获取入场时间
        QDateTime enter = entryTimeMap.value(currentCardId);
        //算钱
        int fee = calculateFee(enter, now);
        //钱不够，提醒
        if(currentInfo.balance < fee)
        {
            //ui提醒
            ui->parkingStatusLabel->setText(tr("余额不足，请先充值"));
            updateInfoPanel(currentInfo, enter, QDateTime());
            //暂停自动寻卡
            pauseForRecharge(fee);
            QMessageBox::warning(this, tr("出场"), tr("余额不足，请先充值"));
            return;
        }

        //移除入场时间信息
        entryTimeMap.remove(currentCardId);
        //更新最近出场信息
        lastExitTimeMap.insert(currentCardId, now);
        //更新最近入场信息
        lastEntryTimeMap.insert(currentCardId, enter);
        //扣费
        currentInfo.balance -= fee;
        lastExitFee = fee;
        ui->parkingStatusLabel->setText(tr("正在出场中，不要收卡"));
        updateInfoDisplay(currentInfo);
        updateInfoPanel(currentInfo, QDateTime(), now);

        //不需要读回
        refreshAfterWrite = false;

        //写入当前信息
        parkingExitWritePending = true;
        writeUpdatedInfo(currentInfo);
    }
    else//入场
    {
        if(parkingFlowState == ParkingFlowIdle)
        {
            parkingFlowState = ParkingFlowEntry;
            parkingFlowPaused = true;
            stopAutoSearch();
            ui->parkingStatusLabel->setText(tr("正在入场中，不要收卡"));
        }
        entryTimeMap.insert(currentCardId, now);
        lastEntryTimeMap.insert(currentCardId, now);
        pendingExitFee = 0;
        updateInfoPanel(currentInfo, now, QDateTime());
        QMessageBox::information(this, tr("入场"), tr("入场成功，请收卡"));
        ui->parkingStatusLabel->setText(tr(""));
        parkingFlowState = ParkingFlowIdle;
        parkingFlowPaused = false;
        startAutoSearch();
    }
}
//把占两个块的车主信息写进卡里
void IEEE14443ControlWidget::writeUpdatedInfo(const TagInfo &info)
{
    //检查是否认证
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    //把车主信息编写成块
    QByteArray b1;
    QByteArray b2;
    encodeTagInfo(info, b1, b2);
    //生成待写入信息
    pendingWriteInfo = info;
    //写入b1
    requestWrite(kUserBlock1, b1);

    //保存待写入info的副本
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
    qDebug() << QString("recieve cmd=0x%1 data=%2")
                .arg(p.command(), 2, 16, QChar('0'))
                .arg(QString(p.data().toHex()));
    QByteArray d = p.data();
    if(d.isEmpty())
    {
        qDebug() << "empty payload for cmd" << p.command();
        waitingReply = false;
        return;
    }
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
        {
            resultTipText += tr("Failure");
            if(registrationAwaitingRemoval)
            {
                registrationAwaitingRemoval = false;
                registrationAwaitingCardId.clear();
            }
        }
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

            if(pendingReadBlock == kUserBlock1)//待读块1
            {
                lastBlock1 = d;
                requestRead(kUserBlock2);//读完块1，自动读块2
            }
            else if(pendingReadBlock == kUserBlock2)//待读块2
            {
                lastBlock2 = d;
                pendingReadBlock = -1;
                //确认初始化
                ensureInitialized();
            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingReadBlock = -1;
            if(!registrationFlowActive && !requiresInitialization && !currentCardId.isEmpty())
            {
                if(entryTimeMap.contains(currentCardId))
                    QMessageBox::warning(this, tr("出场"), tr("出场失败，请重新刷卡"));
                else
                    QMessageBox::warning(this, tr("入场"), tr("入场失败，请重新刷卡"));
                ui->parkingStatusLabel->setText(entryTimeMap.contains(currentCardId)
                                                ? tr("出场失败，请重新刷卡")
                                                : tr("入场失败，请重新刷卡"));
                parkingFlowState = ParkingFlowIdle;
                parkingFlowPaused = false;
                parkingExitWritePending = false;
                startAutoSearch();
            }
        }
        break;
    case IEEE1443Package::WriteCard:
        resultTipText = tr("Write Card ");
        if(status == 0)//写成功
        {
            resultTipText += tr("Succeed");
            if(pendingWriteBlock == kUserBlock1)//如果待写入b2
            {
                pendingWriteBlock = -1;
                requestWrite(kUserBlock2, lastBlock2);//继续写b2
            }
            else if(pendingWriteBlock == kUserBlock2)//如果b2已写入
            {
                pendingWriteBlock = -1;
                updateInfoDisplay(pendingWriteInfo);//更新显示
                if(refreshAfterWrite)//写完后是否刷新——用于注册后显示车主信息
                {
                    refreshAfterWrite = false;
                    requestRead(kUserBlock1);//马上读块1
                }
                else//写完后不用刷新，直接更新展示信息——用于充值/扣费逻辑
                {
                    QDateTime entryDisplayTime =
                            entryTimeMap.contains(currentCardId) ?//仍然在场？
                            entryTimeMap.value(currentCardId) ://当前入场时间
                            lastEntryTimeMap.value(currentCardId);//最近入场时间
                    updateInfoPanel(pendingWriteInfo, entryDisplayTime, lastExitTimeMap.value(currentCardId));
                }

                if(rechargePaused)
                {
                    currentInfo = pendingWriteInfo;//同步到系统当前信息
                    if(currentInfo.balance >= pendingExitFee)//钱够，放行
                    {
                        resumeAfterRecharge();//继续寻卡
                        handleParkingFlow();//继续放行
                    }
                    else//出场时钱不够，待充值，保持rechargePaused状态，待充值
                    {
                        QMessageBox::warning(this, tr("Recharge"), tr("Balance is still below required fee %1").arg(pendingExitFee));
                    }
                }

                if(parkingExitWritePending && parkingFlowState == ParkingFlowExit && !rechargePaused)
                {
                    parkingExitWritePending = false;
                    QMessageBox::information(this, tr("出场"), tr("出场成功，请收卡，费用为%1").arg(lastExitFee));
                    ui->parkingStatusLabel->setText(tr(""));
                    parkingFlowState = ParkingFlowIdle;
                    parkingFlowPaused = false;
                    lastExitFee = 0;
                    startAutoSearch();
                }

            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingWriteBlock = -1;
            refreshAfterWrite = false;
            if(parkingExitWritePending && parkingFlowState == ParkingFlowExit)
            {
                parkingExitWritePending = false;
                QMessageBox::warning(this, tr("出场"), tr("出场失败，请重新刷卡"));
                ui->parkingStatusLabel->setText(tr("出场失败，请重新刷卡"));
                parkingFlowState = ParkingFlowIdle;
                parkingFlowPaused = false;
                lastExitFee = 0;
                startAutoSearch();
            }
        }
        break;
    }

    if(registrationWritePending && !waitingReply)
    {
        registrationWritePending = false;
        registrationVerificationPending = true;
        refreshAfterWrite = true;
        writeUpdatedInfo(registrationPendingInfo);
        ui->parkingStatusLabel->setText(registrationPendingStatusText);
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
//充值按钮
void IEEE14443ControlWidget::on_rechargeBtn_clicked()
{
    //是否认证
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    //是否注册
    if(requiresInitialization || !currentInfo.valid)
    {
        QMessageBox::information(this, tr("Initialization"), tr("Please register the card before recharging."));
        return;
    }

    //读取余额和输入金额
    TagInfo info = currentInfo;
    int rechargeAmount = ui->rechargeSpin->value();

    //若钱不够，导致处于“待充值出场”状态，强制至少充值到覆盖 pendingExitFee
    if(rechargePaused && (currentInfo.balance + rechargeAmount < pendingExitFee))
    {
        QMessageBox::warning(this, tr("Recharge"), tr("Please recharge at least %1 to cover the pending fee.").arg(pendingExitFee));
        return;
    }

    //计算充值后的新余额
    info.balance += rechargeAmount;
    //写新余额
    refreshAfterWrite = false;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("Recharged"));
}

//处理timeout信号
void IEEE14443ControlWidget::onAutoSearchTimeout()
{
    if(registrationPaused || rechargePaused || requiresInitialization || parkingFlowPaused)
        return;
    requestSearch();//每过一小段时间，就请求寻卡
}

//寻卡按钮
void IEEE14443ControlWidget::on_searchCardBtn_clicked()
{


        resetStatus();
        requestSearch();

}
//getID按钮
void IEEE14443ControlWidget::on_getIdBtn_clicked()
{
    lastSendPackage = IEEE1443Package(0, 0x47, 0x04).toPurePackage();
    sendData(lastSendPackage);
}
//selectID按钮
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
//认证按钮
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
//读按钮
void IEEE14443ControlWidget::on_readBtn_clicked()
{
    IEEE1443Package pkg(0, 0x4B, (char)ui->blockNumberBox->currentText().toLong());
    lastSendPackage = pkg.toPurePackage();
    sendData(lastSendPackage);
}
//写按钮
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
