#include "IEEE14443ControlWidget.h"
#include "ui_IEEE14443ControlWidget.h"
//#include "IEEE1443PackageWidget.h"
//#include <IEEE1443Package.h>
#include<rfidWidget/IEEE1443Package.h>
#include <QMessageBox>
#include <QScrollBar>
#include <QDebug>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QAbstractItemView>
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


// === 构造/析构与生命周期 ===
// 功能：构造函数：初始化界面、定时器与状态。
IEEE14443ControlWidget::IEEE14443ControlWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::IEEE14443ControlWidget),
    commPort(NULL),
    autoSearchTimer(NULL),
    replyTimeoutTimer(NULL),
    recvStatus(0),
    waitingReply(false),
    pendingCommand(-1),
    pendingRetries(0),
    maxReplyRetries(2),
    replyTimeoutMs(400),
    autoSearchInProgress(false),
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
    rechargeFlowActive(false),
    rechargeVerificationPending(false),
    rechargeWritePending(false),
    rechargePendingStatusText(),
    rechargeExpectedBalance(0),
    rechargeAwaitingRemoval(false),
    rechargeAwaitingCardId(),
    pendingExitFee(0),
    parkingFlowPaused(false),
    parkingFlowState(ParkingFlowIdle),
    parkingExitWritePending(false),
    lastExitFee(0),
    authKeyData(6, static_cast<char>(0xFF))
{
    ui->setupUi(this);
    if(ui->parkingTable)
    {
        ui->parkingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->parkingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->parkingTable->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->parkingTable->setAlternatingRowColors(true);
        QHeaderView *header = ui->parkingTable->horizontalHeader();
        if(header)
            header->setResizeMode(QHeaderView::Stretch);
    }

    //收到包信号和处理包信息号
    connect(this, SIGNAL(recvPackage(QByteArray)), this, SLOT(onRecvedPackage(QByteArray)));
//  connect(ui->statusList->verticalScrollBar(), SIGNAL(rangeChanged(int,int)), this, SLOT(onStatusListScrollRangeChanced(int,int)));

    //设置自动寻卡定时器，实现自动刷卡
    autoSearchTimer = new QTimer(this);
    autoSearchTimer->setInterval(500);
    //连接信号到槽函数
    connect(autoSearchTimer, SIGNAL(timeout()), this, SLOT(onAutoSearchTimeout()));
    //等待回包超时定时器
    replyTimeoutTimer = new QTimer(this);
    replyTimeoutTimer->setInterval(replyTimeoutMs);
    replyTimeoutTimer->setSingleShot(true);
    connect(replyTimeoutTimer, SIGNAL(timeout()), this, SLOT(onReplyTimeout()));
    resetStatus();
}

// 功能：析构函数：释放 UI 资源。
IEEE14443ControlWidget::~IEEE14443ControlWidget()
{
    delete ui;
}

// 功能：显示事件：打开串口并启动自动寻卡流程。
void IEEE14443ControlWidget::showEvent(QShowEvent *)
{
    if(!commPort)
    {
        //qDebug()<<"set mode success";
        IOPortManager::setMode(Mode13_56M1);
        this->start("/dev/ttyS0");
    }
}

// 功能：隐藏事件：停止串口与定时器，释放硬件占用。
void IEEE14443ControlWidget::hideEvent(QHideEvent *)
{
    if(commPort)
    {
        this->stop();
    }
}

// 功能：静态入口：按需创建并显示窗口实例。
static IEEE14443ControlWidget *ieee14443ControlWidget;
void IEEE14443ControlWidget::showOut()
{
    if(ieee14443ControlWidget == NULL)
        ieee14443ControlWidget = new IEEE14443ControlWidget();
    //ieee14443ControlWidget->showFullScreen();
    ieee14443ControlWidget->show();
}


// === 启停与通信 ===
// 功能：启动串口与读卡流程。
bool IEEE14443ControlWidget::start(const QString &port)
{
    //1.防止重复启动
    if(commPort != NULL)
        return false;
    //2.创建串口对象并设置参数
    //commPort = new QextSerialPort(port,  QextSerialPort::EventDriven);
    commPort = new Posix_QextSerialPort(port,  QextSerialBase::Polling);
    commPort->setBaudRate(BAUD19200);
    commPort->setFlowControl(FLOW_OFF);
    commPort->setParity(PAR_NONE);
    commPort->setDataBits(DATA_8);
    commPort->setStopBits(STOP_1);

    //3.1打开串口
    if (commPort->open(QIODevice::ReadWrite) == true) {

        //4.启动读卡计时器
        //connect(commPort, SIGNAL(readyRead()), this, SLOT(onPortDataReady()));
        readTimer = new QTimer(this);   //初始化计时器
        readTimer->start(100);  //设置延时为100ms
        connect(readTimer,SIGNAL(timeout()),this,SLOT(onPortDataReady()));

        //5.开始自动读卡
        startAutoSearch();
        return true;
    }
    else {//3.2若打开失败
        //4.记录错误日志
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << "device failed to open:" << commPort->errorString();
        //5.删除串口对象
        delete commPort;
        commPort = NULL;
        return false;
    }
}

// 功能：停止串口与自动寻卡流程。
bool IEEE14443ControlWidget::stop()
{
    //1.关闭串口、释放对象
    if(commPort != NULL)
    {
        commPort->close();
        delete commPort;
    }
    commPort = NULL;

    //2.停止超时计时器
    if(replyTimeoutTimer)
        replyTimeoutTimer->stop();

    //3.清理通讯状态
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;
    autoSearchInProgress = false;
    stopAutoSearch();

    return true;
}

// 功能：发送命令数据包并进入等待回包状态。
bool IEEE14443ControlWidget::sendData(const QByteArray &data)
{
    //1.检查串口存在
    if(commPort)
    {
        //2.纯数据包装为协议包
        IEEE1443Package pkg(data);
        QByteArray rawPackage = pkg.toRawPackage();
        
        //3.打印日志信息
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << QString("send %1").arg(QString(rawPackage.toHex()));

        //4.写入串口
        commPort->write(rawPackage);

        //5.设置等待回包状态
        waitingReply = true;
        //记住当前命令
        if(pkg.isValid())
            pendingCommand = pkg.command();
        else
            pendingCommand = -1;
        //重试次数清零
        pendingRetries = 0;
        //开启等待回包超时计时器
        if(pkg.isValid())
            startReplyTimeout(pkg.command());
    }
    return true;
}

// 功能：复位界面与内部状态到初始值。
void IEEE14443ControlWidget::resetStatus()
{
    //1.清空ui
    ui->selCardIdEdit->setText("");
    ui->resultLabel->setText("");
    ui->parkingStatusLabel->setText("");

    //2.清空通信状态
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;
    autoSearchInProgress = false;
    if(replyTimeoutTimer)
        replyTimeoutTimer->stop();

    //3.清空卡片信息缓存
    tagAuthenticated = false;
    currentCardId.clear();
    lastBlock1.clear();
    lastBlock2.clear();
    pendingReadBlock = -1;
    pendingWriteBlock = -1;
    currentInfo = TagInfo();
    pendingWriteInfo = TagInfo();

    //4.清空注册/充值流程状态
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
    rechargeFlowActive = false;
    rechargeVerificationPending = false;
    rechargeWritePending = false;
    rechargePendingStatusText.clear();
    rechargePendingInfo = TagInfo();
    rechargeExpectedBalance = 0;
    rechargeAwaitingRemoval = false;
    rechargeAwaitingCardId.clear();

    //5.清空停车业务状态
    pendingExitFee = 0;
    parkingFlowPaused = false;
    parkingFlowState = ParkingFlowIdle;
    parkingExitWritePending = false;
    lastExitFee = 0;
    lastEntryTimeMap.clear();
    lastExitTimeMap.clear();
    entryTimeMap.clear();
    activeInfoMap.clear();

    //6.清空重复包缓存
    recentReplyTimestamps.clear();

    //7.刷新ui
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
    updateParkingTable();
}


// === 自动寻卡与指令请求 ===
// 功能：启动自动寻卡定时器。
void IEEE14443ControlWidget::startAutoSearch()
{
    //1.检查是否需要暂停自动寻卡
    if(registrationPaused || rechargePaused || parkingFlowPaused)//当因注册或收费需要暂停时，不自动寻卡
        return;
    //2.检查定时器是否有效且未激活
    if(autoSearchTimer && !autoSearchTimer->isActive())
        autoSearchTimer->start();//启动定时器
}

// 功能：停止自动寻卡定时器。
void IEEE14443ControlWidget::stopAutoSearch()
{
    //1.停止定时器
    if(autoSearchTimer)
        autoSearchTimer->stop();
}

// 功能：请求寻卡指令。
void IEEE14443ControlWidget::requestSearch()
{
    //1.等待回复||已经在寻卡：不重复发送
    if(waitingReply || autoSearchInProgress)
        return;
    //2.构造存储并发送寻卡命令包
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::SearchCard, 0x52).toPurePackage();
    sendData(lastSendPackage);
    //3.标记正在寻卡
    autoSearchInProgress = true;
    //4.开始等待回包超时计时器
    if(replyTimeoutTimer)
        replyTimeoutTimer->start();
}

// 功能：请求防冲突指令。
void IEEE14443ControlWidget::requestAntiColl()
{
    //1.等待回复——不发新指令
    if(waitingReply)
        return;
    //2.构造存储并发送防冲突包
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::AntiColl, 0x04).toPurePackage();
    sendData(lastSendPackage);
}

// 功能：请求选择指定卡片。
void IEEE14443ControlWidget::requestSelect(const QByteArray &cardId)
{
    //1.若还在等待回包就不发
    if(waitingReply)
        return;
    //2.构造包
    IEEE1443Package pkg(0, IEEE1443Package::SelectCard, cardId);
    //3.记录此包信息，便于重发
    lastSendPackage = pkg.toPurePackage();
    //3.发包
    sendData(lastSendPackage);
}

// 功能：请求认证指定块。
void IEEE14443ControlWidget::requestAuth(quint8 blockNumber)
{
    //1.判断是否正在等待回包
    if(waitingReply)
        return;
    //2.构造认证信息
    QByteArray authInfo;
    authInfo.append(0x60);
    authInfo.append((char)blockNumber);
    authInfo.append(authKeyData);
    //认证信息错误检查
    if(authInfo.size() != 8)
    {
        QMessageBox::warning(this, tr("Warning"), tr("auth key error"));
        return;
    }
    //3.构造包
    IEEE1443Package pkg(0, IEEE1443Package::Authentication, authInfo);
    //4.存包
    lastSendPackage = pkg.toPurePackage();
    //5.发包
    sendData(lastSendPackage);
}

// 功能：请求读取指定块。
void IEEE14443ControlWidget::requestRead(quint8 blockNumber)
{
    //1.等回包
    if(waitingReply)
        return;
    //2.构造包
    pendingReadBlock = blockNumber;
    IEEE1443Package pkg(0, 0x4B, (char)blockNumber);
    //3.存包
    lastSendPackage = pkg.toPurePackage();
    //4.发包
    sendData(lastSendPackage);
}

// 功能：请求写入指定块。
void IEEE14443ControlWidget::requestWrite(quint8 blockNumber, const QByteArray &data)
{
    //1.等回包
    if(waitingReply)
        return;
    //2.安全检测
    if(data.size() != 16)
    {
        QMessageBox::warning(this, tr("Warning"), tr("write data error"));
        return;
    }
    //3.构造包
    pendingWriteBlock = blockNumber;
    QByteArray writeInfo;
    writeInfo.append((char)blockNumber);//块号
    writeInfo.append(data);//信息
    IEEE1443Package pkg(0, IEEE1443Package::WriteCard, writeInfo);
    //4.存包
    lastSendPackage = pkg.toPurePackage();
    //5.发包
    sendData(lastSendPackage);
}


// === 卡信息解析与UI更新 ===
// 功能：车辆类型文本转编码。
static char vehicleCodeFromText(const QString &text)
{
    //1.按车型文本映射编码
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

// 功能：车辆类型编码转文本。
static QString vehicleTextFromCode(char c)
{
    //1.按车型编码映射文本
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

// 功能：从块数据解析 TagInfo。
bool IEEE14443ControlWidget::decodeTagInfo(const QByteArray &b1, const QByteArray &b2, TagInfo &info)
{
    //1.初始化为无效信息
    info.valid = false;
    //2.检验块是否有效
    if(b1.size() != 16 || b2.size() != 16)
        return false;
    if((b1.at(0) != kTagSignature1) || (b1.at(1) != kTagSignature2))
        return false;
    //3.解析车主信息
    QByteArray ownerBytes = b1.mid(4, 12);
    int nullIndex = ownerBytes.indexOf('\0');
    if(nullIndex >= 0)
        ownerBytes.truncate(nullIndex);
    info.owner = QString::fromLatin1(ownerBytes).trimmed();
    info.vehicleType = vehicleTextFromCode(b1.at(3));
    //4.解析余额信息
    int bal = 0;
    bal |= (quint8)b2.at(0);
    bal |= ((quint8)b2.at(1)) << 8;
    bal |= ((quint8)b2.at(2)) << 16;
    bal |= ((quint8)b2.at(3)) << 24;
    info.balance = bal;
    //5.设为有效信息
    info.valid = true;
    return true;
}

// 功能：将 TagInfo 编码为块数据。
void IEEE14443ControlWidget::encodeTagInfo(const TagInfo &info, QByteArray &b1, QByteArray &b2)
{
    //1.初始化两个块
    b1 = QByteArray(16, 0x00);
    b2 = QByteArray(16, 0x00);
    //2.写入卡片签名与固定标志
    b1[0] = kTagSignature1;//P
    b1[1] = kTagSignature2;//K
    b1[2] = 0x01;
    //3.写入车型、车主信息
    b1[3] = vehicleCodeFromText(info.vehicleType);
    QByteArray nameBytes = info.owner.left(12).toLatin1();
    //4.写入余额信息
    int i;
    for(i = 0; i < nameBytes.size() && i < 12; ++i)
        b1[4 + i] = nameBytes.at(i);
    b2[0] = (char)(info.balance & 0xFF);
    b2[1] = (char)((info.balance >> 8) & 0xFF);
    b2[2] = (char)((info.balance >> 16) & 0xFF);
    b2[3] = (char)((info.balance >> 24) & 0xFF);
}

// 功能：更新面板的进出场时间与余额显示。
void IEEE14443ControlWidget::updateInfoPanel(const TagInfo &info, const QDateTime &entryTime, const QDateTime &exitTime)
{
    //1.刷新车主与车型信息
    ui->infoOwnerValue->setText(info.valid ? info.owner : tr("N/A"));
    ui->infoVehicleValue->setText(info.valid ? info.vehicleType : tr("N/A"));
    //2.刷新进出场时间信息
    ui->infoEntryTimeValue->setText(entryTime.isValid() ? entryTime.toString("hh:mm:ss") : tr("--"));
    ui->infoExitTimeValue->setText(exitTime.isValid() ? exitTime.toString("hh:mm:ss") : tr("--"));
    //3.刷新余额信息
    if(info.valid)
        ui->infoBalanceValue->setText(QString::number(info.balance));
    else
        ui->infoBalanceValue->setText(tr("--"));
}

// 功能：刷新停车记录表格。
void IEEE14443ControlWidget::updateParkingTable()
{
    //1.检查表格是否存在
    if(!ui->parkingTable)
        return;
    //2.设置行数
    ui->parkingTable->setRowCount(entryTimeMap.size());
    //3.遍历entryTimeMap 建立在场车辆表
    int row = 0;
    QMap<QString, QDateTime>::const_iterator it = entryTimeMap.constBegin();
    for(; it != entryTimeMap.constEnd(); ++it, ++row)
    {
        const QString cardId = it.key();
        const QDateTime entryTime = it.value();
        //从在场车辆表获取信息
        TagInfo info;
        if(activeInfoMap.contains(cardId))
            info = activeInfoMap.value(cardId);

        QTableWidgetItem *cardItem = new QTableWidgetItem(cardId);
        QTableWidgetItem *ownerItem = new QTableWidgetItem(info.valid ? info.owner : tr("--"));
        QTableWidgetItem *vehicleItem = new QTableWidgetItem(info.valid ? info.vehicleType : tr("--"));
        QTableWidgetItem *entryItem = new QTableWidgetItem(entryTime.isValid()
                                                           ? entryTime.toString("yyyy-MM-dd hh:mm:ss")
                                                           : tr("--"));
        QTableWidgetItem *balanceItem = new QTableWidgetItem(info.valid
                                                             ? QString::number(info.balance)
                                                             : tr("--"));
        ui->parkingTable->setItem(row, 0, cardItem);
        ui->parkingTable->setItem(row, 1, ownerItem);
        ui->parkingTable->setItem(row, 2, vehicleItem);
        ui->parkingTable->setItem(row, 3, entryItem);
        ui->parkingTable->setItem(row, 4, balanceItem);
    }
}


// === 业务流程处理（注册/充值/停车） ===

// 功能：注册开始时暂停自动寻卡流程。
void IEEE14443ControlWidget::pauseForRegistration()
{
    //1.标记注册暂停状态
    registrationPaused = true;
    //2.停止自动寻卡
    stopAutoSearch();
}

// 功能：注册结束后恢复自动寻卡流程。
void IEEE14443ControlWidget::resumeAfterRegistration()
{
    //1.判断是否处于注册暂停状态
    if(!registrationPaused)
        return;
    //2.清理状态并恢复自动寻卡
    registrationPaused = false;
    startAutoSearch();
}

// 功能：充值开始时暂停自动寻卡流程并提示。
void IEEE14443ControlWidget::pauseForRecharge(int feeRequired)
{
    //1.标记充值暂停状态
    rechargePaused = true;
    //2.记录待缴费用
    pendingExitFee = feeRequired;
    //3.停止自动寻卡
    stopAutoSearch();
}

// 功能：充值结束后恢复自动寻卡流程。
void IEEE14443ControlWidget::resumeAfterRecharge()
{
    //1.判断是否处于充值暂停状态
    if(!rechargePaused)
        return;
    //2.清理状态并恢复自动寻卡
    rechargePaused = false;
    startAutoSearch();
}

// 功能：检查卡片是否初始化并处理注册/充值校验。
void IEEE14443ControlWidget::ensureInitialized()
{
    //1.尝试解析
    TagInfo info;
    //1.1解析成功
    if(decodeTagInfo(lastBlock1, lastBlock2, info))
    {
        requiresInitialization = false;
        
        //2.1注册校验
        if(registrationVerificationPending)
        {
            //3.充值状态
            registrationVerificationPending = false;
            registrationFlowActive = false;
            //4，记录当前info
            currentInfo = info;
            //5.更新ui
            updateInfoPanel(info, QDateTime(), QDateTime());
            //6.提示信息
            ui->parkingStatusLabel->setText(tr("注册成功，请收卡"));
            //7.设置等待取卡状态
            registrationAwaitingRemoval = true;
            registrationAwaitingCardId = currentCardId;
            //7.弹出提示框
            QMessageBox::information(this, tr("注册成功"), tr("注册成功，请收卡"));
            //8.继续自动寻卡
            resumeAfterRegistration();

            return;
        }

        //2.2充值校验
        if(rechargeVerificationPending)
        {
            //3.重置流程状态
            rechargeVerificationPending = false;
            rechargeFlowActive = false;
            refreshAfterWrite = false;
            //2.记录当前info
            currentInfo = info;
            //3.读取入场时间
            QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ?
                                         entryTimeMap.value(currentCardId) :
                                         lastEntryTimeMap.value(currentCardId);
            //4.更新ui
            updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
            currentInfo = info;
            //5.1充值成功
            if(info.balance == rechargeExpectedBalance)
            {
                //6.ui提示
                ui->parkingStatusLabel->setText(tr("充值成功，余额为%1").arg(info.balance));
                //7，等待取卡
                rechargeAwaitingRemoval = true;
                rechargeAwaitingCardId = currentCardId;
                //8.提示信息
                QMessageBox::information(this, tr("充值成功"), tr("充值成功，余额为%1").arg(info.balance));
                //9。校验余额
                if(pendingExitFee > 0 && info.balance >= pendingExitFee)
                {
                    resumeAfterRecharge();
                    handleParkingFlow();
                }
                else if(pendingExitFee == 0)
                {
                    resumeAfterRecharge();
                }
            }
            else//5.2充值失败
            {
                QMessageBox::warning(this, tr("充值失败"), tr("请重新充值"));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
            }
            return;
        }
        
        //2.3常规解析成功后
        //3.继续自动寻卡 
        resumeAfterRegistration();

        //4.处理等待取卡
        //4.1注册时等待取卡
        if(registrationAwaitingRemoval && registrationAwaitingCardId == currentCardId)
        {
            ui->parkingStatusLabel->setText(tr("注册成功，请收卡"));
            return;
        }
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();
        //4.2充值时等待取卡
        if(rechargeAwaitingRemoval && rechargeAwaitingCardId == currentCardId)
        {
            ui->parkingStatusLabel->setText(tr("充值成功，余额为%1").arg(info.balance));
            return;
        }
        rechargeAwaitingRemoval = false;
        rechargeAwaitingCardId.clear();

        //5.更新展示信息
        currentInfo = info;
        //更新信息面板
        QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ?
                                     entryTimeMap.value(currentCardId) :
                                     lastEntryTimeMap.value(currentCardId);
        updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
        //更新停车表
        if(entryTimeMap.contains(currentCardId))
        {
            activeInfoMap.insert(currentCardId, info);
            updateParkingTable();
        }
       
        //6.进行出入场逻辑（注册流程不进行出入场判断）
        if(!registrationFlowActive)
            handleParkingFlow();
        return;
    }
    //1.2解析失败处理无效卡
    handleInvalidCard();
}

// 功能：处理非法或格式不正确的卡。
void IEEE14443ControlWidget::handleInvalidCard()
{
    //1.清理停车状态
    entryTimeMap.remove(currentCardId);
    lastEntryTimeMap.remove(currentCardId);
    activeInfoMap.remove(currentCardId);
    updateParkingTable();

    //2.注册写卡后验证失败
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

    //3.只在第一次进入未初始化状态时提示要初始化
    if(!requiresInitialization)
        QMessageBox::information(this, tr("未注册卡片"), tr("请先注册，不要收卡"));
    requiresInitialization = true;
    pauseForRegistration();
    currentInfo = TagInfo();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
    ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));

    //4.触发注册流程
    if(!registrationFlowActive)
        startRegistrationFlow();
}

// 功能：弹出注册对话框收集用户信息。
bool IEEE14443ControlWidget::showRegistrationDialog(TagInfo &info)
{
    //1.构建注册对话框
    QDialog dialog(this);
    dialog.setWindowTitle(tr("注册新卡"));
    QFormLayout form(&dialog);

    QLineEdit *nameEdit = new QLineEdit(&dialog);
    nameEdit->setMaxLength(12);
    nameEdit->setText(QString());
    form.addRow(tr("姓名"), nameEdit);

    QComboBox *vehicleBox = new QComboBox(&dialog);
    vehicleBox->addItems(QStringList() << "Sedan" << "SUV" << "Truck" << "Electric" << "Other");
    vehicleBox->setCurrentIndex(0);
    form.addRow(tr("车型"), vehicleBox);

    QSpinBox *balanceSpin = new QSpinBox(&dialog);
    balanceSpin->setRange(0, 100000);
    balanceSpin->setValue(0);
    form.addRow(tr("初始余额"), balanceSpin);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    form.addRow(&buttons);
    connect(&buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
    connect(&buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

    //2.确认或取消注册
    if(dialog.exec() != QDialog::Accepted)
        return false;

    //3.写入注册信息
    info.owner = nameEdit->text().isEmpty() ? tr("Unknown") : nameEdit->text();
    info.vehicleType = vehicleBox->currentText();
    info.balance = balanceSpin->value();
    info.valid = true;
    return true;
}

// 功能：启动注册流程并请求写卡。
void IEEE14443ControlWidget::startRegistrationFlow()
{
    //1.进入注册流程
    registrationFlowActive = true;
    pauseForRegistration();
    TagInfo info;
    if(!showRegistrationDialog(info))
    {
        //2.用户取消注册
        registrationFlowActive = false;
        requiresInitialization = false;
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();
        resumeAfterRegistration();
        ui->parkingStatusLabel->setText(tr("注册已取消"));
        return;
    }
    //3.等待通信时缓存写卡信息
    if(waitingReply)
    {
        registrationWritePending = true;
        registrationPendingInfo = info;
        registrationPendingStatusText = tr("正在注册...");
        ui->parkingStatusLabel->setText(tr("等待当前操作完成..."));
        return;
    }
    //4.直接写卡并等待读回校验
    registrationVerificationPending = true;
    refreshAfterWrite = true;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("正在注册..."));
}

// 功能：弹出充值对话框输入金额。
bool IEEE14443ControlWidget::showRechargeDialog(int &amount)
{
    //1.构建充值对话框
    QDialog dialog(this);
    dialog.setWindowTitle(tr("充值"));
    QFormLayout form(&dialog);

    QLabel *tipLabel = new QLabel(tr("请勿拿开卡"), &dialog);
    tipLabel->setWordWrap(true);
    form.addRow(tipLabel);

    QSpinBox *amountSpin = new QSpinBox(&dialog);
    amountSpin->setRange(1, 100000);
    amountSpin->setValue(1);
    form.addRow(tr("金额"), amountSpin);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    buttons.button(QDialogButtonBox::Ok)->setText(tr("确认"));
    buttons.button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    form.addRow(&buttons);
    connect(&buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
    connect(&buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

    //2.确认或取消充值
    if(dialog.exec() != QDialog::Accepted)
        return false;

    //3.记录充值金额
    amount = amountSpin->value();
    return true;
}

// 功能：启动充值流程并请求写卡。
void IEEE14443ControlWidget::startRechargeFlow(int feeRequired)
{
    //1.检查充值流程是否已启动
    if(rechargeFlowActive)
        return;
    rechargeFlowActive = true;
    pauseForRecharge(feeRequired);

    //2.检查认证和初始化状态
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        rechargeFlowActive = false;
        if(feeRequired == 0)
            resumeAfterRecharge();
        return;
    }
    if(requiresInitialization || !currentInfo.valid)
    {
        QMessageBox::information(this, tr("Initialization"), tr("Please register the card before recharging."));
        rechargeFlowActive = false;
        if(feeRequired == 0)
            resumeAfterRecharge();
        return;
    }

    //3.循环获取合法充值金额
    int rechargeAmount = 0;
    while(true)
    {
        if(!showRechargeDialog(rechargeAmount))
        {
            rechargeFlowActive = false;
            if(feeRequired == 0)
                resumeAfterRecharge();
            else
                ui->parkingStatusLabel->setText(tr("请充值后出场"));
            return;
        }

        //4.出场充值时确保金额足够
        if(rechargePaused && pendingExitFee > 0)
        {
            int requiredAmount = qMax(0, pendingExitFee - currentInfo.balance);
            if(requiredAmount > 0 && rechargeAmount <= requiredAmount)
            {
                QMessageBox::warning(this, tr("Recharge"), tr("充值金额需超过待缴收费%1").arg(requiredAmount));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
                continue;
            }
        }
        break;
    }

    //5.更新余额并准备写卡
    TagInfo info = currentInfo;
    info.balance += rechargeAmount;
    rechargeExpectedBalance = info.balance;
    rechargePendingInfo = info;

    //6.等待通信时缓存写卡信息
    if(waitingReply)
    {
        rechargeWritePending = true;
        rechargePendingStatusText = tr("正在充值...");
        ui->parkingStatusLabel->setText(tr("等待当前操作完成..."));
        return;
    }

    //7.写卡并等待读回校验
    rechargeVerificationPending = true;
    refreshAfterWrite = true;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("正在充值..."));
}

// 功能：计算停车费用。
int IEEE14443ControlWidget::calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const
{
    //1.计算停车时长（秒）
    int secs = (int)enterTime.secsTo(leaveTime);
    if(secs < 0)
        secs = 0;
    //2.换算为计费小时
    int minutes = secs / 60;
    int hours = (minutes + 59) / 60;
    //return hours * kHourFee;
    //3.返回费用
    return secs;//测试用
}

// 功能：处理停车进出场业务流程。
void IEEE14443ControlWidget::handleParkingFlow()
{
    //1.注册流程中不处理出入场
    if(registrationFlowActive)
        return;
    //2.检查当前卡信息
    if(currentCardId.isEmpty() || !currentInfo.valid)
        return;
    //3.判断是否初始化
    if(requiresInitialization)
    {
        ui->parkingStatusLabel->setText(tr("Card not initialized, please register"));
        entryTimeMap.remove(currentCardId);
        lastEntryTimeMap.remove(currentCardId);
        return;
    }
    //4.记录当前时间
    QDateTime now = QDateTime::currentDateTime();
    //5.若entryTimeMap包括当前卡号——出场
    if(entryTimeMap.contains(currentCardId))
    {
        if(parkingFlowState == ParkingFlowIdle)
        {
            parkingFlowState = ParkingFlowExit;
            parkingFlowPaused = true;
            stopAutoSearch();
            ui->parkingStatusLabel->setText(tr("正在出场中，不要收卡"));
        }
        //6.获取入场时间
        QDateTime enter = entryTimeMap.value(currentCardId);
        //7.计算费用
        int fee = pendingExitFee > 0 ? pendingExitFee : calculateFee(enter, now);
        //8.余额不足时进入充值流程
        if(currentInfo.balance < fee)
        {
            //ui提醒
            ui->parkingStatusLabel->setText(tr("余额不足，请先充值，待缴收费%1").arg(fee));
            updateInfoPanel(currentInfo, enter, QDateTime());
            //进入充值流程
            QMessageBox::warning(this, tr("出场"), tr("余额不足，请先充值，待缴收费%1").arg(fee));
            startRechargeFlow(fee);
            return;
        }

        //9.更新出场时间与扣费信息
        entryTimeMap.remove(currentCardId);
        activeInfoMap.remove(currentCardId);
        updateParkingTable();
        //更新最近出场信息
        lastExitTimeMap.insert(currentCardId, now);
        //更新最近入场信息
        lastEntryTimeMap.insert(currentCardId, enter);
        //扣费
        currentInfo.balance -= fee;
        pendingExitFee = 0;
        lastExitFee = fee;
        ui->parkingStatusLabel->setText(tr("正在出场中，不要收卡"));
        updateInfoPanel(currentInfo, QDateTime(), now);

        //10.出场写卡不读回
        refreshAfterWrite = false;

        //11.写入当前信息
        parkingExitWritePending = true;
        writeUpdatedInfo(currentInfo);
    }
    else//入场
    {
        //12.入场流程更新
        if(parkingFlowState == ParkingFlowIdle)
        {
            parkingFlowState = ParkingFlowEntry;
            parkingFlowPaused = true;
            stopAutoSearch();
            ui->parkingStatusLabel->setText(tr("正在入场中，不要收卡"));
        }
        entryTimeMap.insert(currentCardId, now);
        activeInfoMap.insert(currentCardId, currentInfo);
        updateParkingTable();
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

// 功能：写回更新后的卡信息。
void IEEE14443ControlWidget::writeUpdatedInfo(const TagInfo &info)
{
    //1.检查是否认证
    if(!tagAuthenticated)
    {
        QMessageBox::warning(this, tr("Warning"), tr("authenticate first"));
        return;
    }
    //2.把车主信息编写成块
    QByteArray b1;
    QByteArray b2;
    encodeTagInfo(info, b1, b2);
    //3.生成待写入信息
    pendingWriteInfo = info;
    //4.写入b1
    requestWrite(kUserBlock1, b1);

    //5.保存待写入info的副本
    lastBlock1 = b1;
    lastBlock2 = b2;
}

// === 超时与重复包处理 ===
// 功能：启动等待回包超时计时。
void IEEE14443ControlWidget::startReplyTimeout(quint8 command)
{
    //1.保留接口参数
    Q_UNUSED(command);
    //2.启动计时器
    if(!replyTimeoutTimer)
        return;
    replyTimeoutTimer->setInterval(replyTimeoutMs);
    replyTimeoutTimer->start();
}

// 功能：处理回包超时失败逻辑。
void IEEE14443ControlWidget::handleReplyTimeoutFailure(int command)
{
    //1.寻卡指令超时处理
    if(command == IEEE1443Package::SearchCard)
    {
        if(registrationAwaitingRemoval)
        {
            registrationAwaitingRemoval = false;
            registrationAwaitingCardId.clear();
        }
        if(rechargeAwaitingRemoval)
        {
            rechargeAwaitingRemoval = false;
            rechargeAwaitingCardId.clear();
        }
        currentCardId.clear();
        tagAuthenticated = false;
        ui->resultLabel->setText(tr("Search Card Failure"));
        return;
    }

    //2.其他指令超时处理
    if(command == IEEE1443Package::ReadCard)
        pendingReadBlock = -1;
    if(command == IEEE1443Package::WriteCard)
        pendingWriteBlock = -1;
    refreshAfterWrite = false;
    ui->resultLabel->setText(tr("Command Timeout"));
}

// 功能：判断是否为重复响应包。
bool IEEE14443ControlWidget::isDuplicateResponse(const IEEE1443Package &pkg)
{
    //1.生成去重签名
    const int kDuplicateWindowMs = 800;
    QString signature = QString::number(pkg.command()) + ":" + QString(pkg.data().toHex());
    QDateTime now = QDateTime::currentDateTime();
    //2.判断是否在去重窗口内重复
    if(recentReplyTimestamps.contains(signature))
    {
        if(recentReplyTimestamps.value(signature).msecsTo(now) <= kDuplicateWindowMs)
            return true;
    }
    //3.记录当前响应时间
    recentReplyTimestamps.insert(signature, now);
    pruneRecentReplies();
    return false;
}

// 功能：清理历史响应记录。
void IEEE14443ControlWidget::pruneRecentReplies()
{
    //1.清理超过保留窗口的响应记录
    const int kKeepWindowMs = 3000;
    QDateTime now = QDateTime::currentDateTime();
    QHash<QString, QDateTime>::iterator it = recentReplyTimestamps.begin();
    while(it != recentReplyTimestamps.end())
    {
        if(it.value().msecsTo(now) > kKeepWindowMs)
            it = recentReplyTimestamps.erase(it);
        else
            ++it;
    }
}

// === 信号槽（事件驱动） ===
// 功能：串口数据就绪事件处理。
void IEEE14443ControlWidget::onPortDataReady()
{
    //1.读取串口可用数据
    QByteArray bytes;
    int a = commPort->bytesAvailable();
    bytes.resize(a);
    char *p = bytes.data();
    int len = bytes.size();
    commPort->read(p, len);
    //2.按协议解析数据包
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

// 功能：处理接收到的数据包。
void IEEE14443ControlWidget::onRecvedPackage(QByteArray pkg)
{

    //1.前置检验
    IEEE1443Package p(pkg);
    if(!p.isValid())
        return;
    if(!waitingReply && pendingCommand < 0)//没有等待响应
        return;
    if(waitingReply && pendingCommand >= 0 && p.command() != pendingCommand)//等待响应但指令码不匹配
        return;
    if(isDuplicateResponse(p))//避免重复响应
        return;
    if(replyTimeoutTimer && replyTimeoutTimer->isActive())//停止超时计时器
        replyTimeoutTimer->stop();

    //2.打印日志，解析load
    qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
             << QString("recieve cmd=0x%1 data=%2")
                .arg(p.command(), 2, 16, QChar('0'))
                .arg(QString(p.data().toHex()));
    QByteArray d = p.data();
    if(d.isEmpty())
    {
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << "empty payload for cmd" << p.command();
                 
        //清理等待状态
        waitingReply = false;
        pendingCommand = -1;
        autoSearchInProgress = false;
        return;
    }
    //记录指令状态和信息
    int status = d.at(0);
    d = d.mid(1);
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;//当前重试发包次数
    QString resultTipText;

    //3.命令分发逻辑
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
            autoSearchInProgress = false;
            if(registrationAwaitingRemoval)
            {
                registrationAwaitingRemoval = false;
                registrationAwaitingCardId.clear();
            }
            // 新增：充值等待收卡
            if(rechargeAwaitingRemoval)
            {
                rechargeAwaitingRemoval = false;
                rechargeAwaitingCardId.clear();
            }

            // 无卡时清掉当前卡状态，避免“同卡再次放卡”被误判为没收卡
            currentCardId.clear();
            tagAuthenticated = false;
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
        {
            resultTipText += tr("Failure");
            autoSearchInProgress = false;
        }
        break;
    case IEEE1443Package::SelectCard:
        resultTipText = tr("Select Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            resultTipText += tr(", Type is %1").arg(d.at(0) == 0x08 ? "S50" : "S70");
            tagAuthenticated = false;
            //停车系统实现自动识别卡片功能，自动进行认证
            requestAuth(kUserBlock1);
        }
        else
        {
            resultTipText += tr("Failure");
            autoSearchInProgress = false;
        }
        break;
    case IEEE1443Package::Authentication:
        resultTipText = tr("Authentication ");
        if(status == 0)
            resultTipText += tr("Succeed");
        else
            resultTipText += tr("Failure");
        tagAuthenticated = (status == 0);//记录认证信息
        if(tagAuthenticated)
            requestRead(kUserBlock1);//停车系统实现自动识别卡片功能，自动读块1用于判断是不是停车系统
        else
            autoSearchInProgress = false;
        break;
    case IEEE1443Package::ReadCard:
        resultTipText = tr("Read Card ");
        if(status == 0)
        {
            resultTipText += tr("Succeed");
            //待读块1
            if(pendingReadBlock == kUserBlock1)
            {
                lastBlock1 = d;//保存块1
                requestRead(kUserBlock2);//读完块1，自动读块2
            }
            else if(pendingReadBlock == kUserBlock2)//待读块2
            {
                lastBlock2 = d;
                pendingReadBlock = -1;
                //根据读到的卡信息做相应处理
                ensureInitialized();
                autoSearchInProgress = false;
            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingReadBlock = -1;
            autoSearchInProgress = false;
            //若是充值校验
            if(rechargeVerificationPending)
            {
                rechargeVerificationPending = false;
                rechargeFlowActive = false;
                refreshAfterWrite = false;
                QMessageBox::warning(this, tr("充值失败"), tr("请重新充值"));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
                break;
            }
            //若不是注册流程且卡已识别——出入场逻辑
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
            //继续写下一块
            if(pendingWriteBlock == kUserBlock1)
            {
                pendingWriteBlock = -1;
                requestWrite(kUserBlock2, lastBlock2);
            }
            else if(pendingWriteBlock == kUserBlock2)//如果b2已写入
            {
                pendingWriteBlock = -1;
                currentInfo = pendingWriteInfo;//更新当前info

                //需要读回验证——注册、充值
                if(refreshAfterWrite)
                {
                    refreshAfterWrite = false;
                    requestRead(kUserBlock1);//马上读块1
                }
                else//不需要读回验证——出场
                {
                    //更新ui
                    QDateTime entryDisplayTime =
                            entryTimeMap.contains(currentCardId) ?//仍然在场？
                            entryTimeMap.value(currentCardId) ://当前入场时间
                            lastEntryTimeMap.value(currentCardId);//最近入场时间
                    updateInfoPanel(pendingWriteInfo, entryDisplayTime, lastExitTimeMap.value(currentCardId));

                    //更新停车标
                    if(entryTimeMap.contains(currentCardId) && pendingWriteInfo.valid)
                    {
                        activeInfoMap.insert(currentCardId, pendingWriteInfo);
                        updateParkingTable();
                    }
                }

                //充值——余额足够
                if(rechargePaused && !rechargeVerificationPending)
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

                //出场写卡成功
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
            if(rechargeVerificationPending || rechargeFlowActive)
            {
                rechargeVerificationPending = false;
                rechargeFlowActive = false;
                rechargeWritePending = false;
                QMessageBox::warning(this, tr("充值失败"), tr("请重新充值"));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
            }
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

    //4.写卡后自动触发二次写入逻辑
    //注册写卡
    if(registrationWritePending && !waitingReply)
    {
        registrationWritePending = false;
        registrationVerificationPending = true;
        refreshAfterWrite = true;
        writeUpdatedInfo(registrationPendingInfo);
        ui->parkingStatusLabel->setText(registrationPendingStatusText);
    }
    //充值写卡
    if(rechargeWritePending && !waitingReply)
    {
        rechargeWritePending = false;
        rechargeVerificationPending = true;
        refreshAfterWrite = true;
        rechargeExpectedBalance = rechargePendingInfo.balance;
        writeUpdatedInfo(rechargePendingInfo);
        ui->parkingStatusLabel->setText(rechargePendingStatusText);
    }
    ui->resultLabel->setText(resultTipText);
}

// 功能：状态列表滚动范围变化处理。
void IEEE14443ControlWidget::onStatusListScrollRangeChanced(int min, int max)
{
    //1.保留接口参数
    Q_UNUSED(min);
    Q_UNUSED(max);
//    ui->statusList->verticalScrollBar()->setValue(max);
}

// 功能：自动寻卡定时器超时处理。
void IEEE14443ControlWidget::onAutoSearchTimeout()
{
    //1.暂停状态不自动寻卡
    if(registrationPaused || rechargePaused || requiresInitialization || parkingFlowPaused)
        return;
    //2.已在寻卡则跳过
    if(autoSearchInProgress)
        return;
    //3.发送寻卡请求
    requestSearch();//每过一小段时间，就请求寻卡
}

// 功能：等待回包超时处理。
void IEEE14443ControlWidget::onReplyTimeout()
{
    //1.检查是否存在待回复指令
    if(!waitingReply || pendingCommand < 0)
        return;
    //2.进入重试流程
    if(pendingRetries < maxReplyRetries)
    {
        pendingRetries++;
        IEEE1443Package retryPackage(lastSendPackage);
        if(retryPackage.isValid() && commPort)
        {
            commPort->write(retryPackage.toRawPackage());
            startReplyTimeout(pendingCommand);
            return;
        }
    }
    //3.重试失败后的清理逻辑
    int failedCommand = pendingCommand;
    waitingReply = false;
    pendingCommand = -1;
    autoSearchInProgress = false;
    handleReplyTimeoutFailure(failedCommand);
}
