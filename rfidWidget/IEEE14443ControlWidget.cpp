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
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << "device failed to open:" << commPort->errorString();
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
    if(replyTimeoutTimer)
        replyTimeoutTimer->stop();
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;
    autoSearchInProgress = false;
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
        IEEE1443Package pkg(data);
        QByteArray rawPackage = pkg.toRawPackage();
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << QString("send %1").arg(QString(rawPackage.toHex()));
        commPort->write(rawPackage);
        waitingReply = true;
        if(pkg.isValid())
            pendingCommand = pkg.command();
        else
            pendingCommand = -1;
        pendingRetries = 0;
        if(pkg.isValid())
            startReplyTimeout(pkg.command());
    }
    return true;
}

void IEEE14443ControlWidget::resetStatus()
{
    ui->selCardIdEdit->setText("");
    ui->resultLabel->setText("");
    ui->balanceEdit->setText("");
    ui->parkingStatusLabel->setText("");
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;
    autoSearchInProgress = false;
    if(replyTimeoutTimer)
        replyTimeoutTimer->stop();
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
    rechargeFlowActive = false;
    rechargeVerificationPending = false;
    rechargeWritePending = false;
    rechargePendingStatusText.clear();
    rechargePendingInfo = TagInfo();
    rechargeExpectedBalance = 0;
    rechargeAwaitingRemoval = false;
    rechargeAwaitingCardId.clear();
    pendingExitFee = 0;
    parkingFlowPaused = false;
    parkingFlowState = ParkingFlowIdle;
    parkingExitWritePending = false;
    lastExitFee = 0;
    lastEntryTimeMap.clear();
    lastExitTimeMap.clear();
    entryTimeMap.clear();
    activeInfoMap.clear();
    recentReplyTimestamps.clear();
    updateInfoPanel(TagInfo(), QDateTime(), QDateTime());
    updateParkingTable();
}

void IEEE14443ControlWidget::startReplyTimeout(quint8 command)
{
    Q_UNUSED(command);
    if(!replyTimeoutTimer)
        return;
    replyTimeoutTimer->setInterval(replyTimeoutMs);
    replyTimeoutTimer->start();
}

void IEEE14443ControlWidget::handleReplyTimeoutFailure(int command)
{
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

    if(command == IEEE1443Package::ReadCard)
        pendingReadBlock = -1;
    if(command == IEEE1443Package::WriteCard)
        pendingWriteBlock = -1;
    refreshAfterWrite = false;
    ui->resultLabel->setText(tr("Command Timeout"));
}

bool IEEE14443ControlWidget::isDuplicateResponse(const IEEE1443Package &pkg)
{
    const int kDuplicateWindowMs = 800;
    QString signature = QString::number(pkg.command()) + ":" + QString(pkg.data().toHex());
    QDateTime now = QDateTime::currentDateTime();
    if(recentReplyTimestamps.contains(signature))
    {
        if(recentReplyTimestamps.value(signature).msecsTo(now) <= kDuplicateWindowMs)
            return true;
    }
    recentReplyTimestamps.insert(signature, now);
    pruneRecentReplies();
    return false;
}

void IEEE14443ControlWidget::pruneRecentReplies()
{
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
    if(waitingReply || autoSearchInProgress)
        return;
    lastSendPackage = IEEE1443Package(0, IEEE1443Package::SearchCard, 0x52).toPurePackage();
    sendData(lastSendPackage);
    autoSearchInProgress = true;
    if(replyTimeoutTimer)
        replyTimeoutTimer->start();
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
    authInfo.append(authKeyData);
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

void IEEE14443ControlWidget::updateParkingTable()
{
    if(!ui->parkingTable)
        return;
    ui->parkingTable->setRowCount(entryTimeMap.size());
    int row = 0;
    QMap<QString, QDateTime>::const_iterator it = entryTimeMap.constBegin();
    for(; it != entryTimeMap.constEnd(); ++it, ++row)
    {
        const QString cardId = it.key();
        const QDateTime entryTime = it.value();
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
//生成待写入info
IEEE14443ControlWidget::TagInfo IEEE14443ControlWidget::defaultTagInfo() const
{
    TagInfo info;
    info.owner = ui->ownerNameEdit->text();
    if(info.owner.isEmpty())
        info.owner = tr("Unknown");
    info.vehicleType = ui->vehicleTypeBox->currentText();
    info.balance = 0;
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
            ui->parkingStatusLabel->setText(tr("注册成功，请收卡"));
            registrationAwaitingRemoval = true;
            registrationAwaitingCardId = currentCardId;
            QMessageBox::information(this, tr("注册成功"), tr("注册成功，请收卡"));
            resumeAfterRegistration();//继续自动寻卡
            return;
        }

        if(rechargeVerificationPending)
        {
            rechargeVerificationPending = false;
            rechargeFlowActive = false;
            refreshAfterWrite = false;
            updateInfoDisplay(info);
            QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ?
                                         entryTimeMap.value(currentCardId) :
                                         lastEntryTimeMap.value(currentCardId);
            updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
            currentInfo = info;
            if(info.balance == rechargeExpectedBalance)
            {
                ui->parkingStatusLabel->setText(tr("充值成功，余额为%1").arg(info.balance));
                rechargeAwaitingRemoval = true;
                rechargeAwaitingCardId = currentCardId;
                QMessageBox::information(this, tr("充值成功"), tr("充值成功，余额为%1").arg(info.balance));
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
            else
            {
                QMessageBox::warning(this, tr("充值失败"), tr("请重新充值"));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
            }
            return;
        }

        resumeAfterRegistration();//继续自动寻卡

        if(registrationAwaitingRemoval && registrationAwaitingCardId == currentCardId)
        {
            ui->parkingStatusLabel->setText(tr("注册成功，请收卡"));
            return;
        }
        registrationAwaitingRemoval = false;
        registrationAwaitingCardId.clear();
        if(rechargeAwaitingRemoval && rechargeAwaitingCardId == currentCardId)
        {
            ui->parkingStatusLabel->setText(tr("充值成功，余额为%1").arg(info.balance));
            return;
        }
        rechargeAwaitingRemoval = false;
        rechargeAwaitingCardId.clear();

        //更新展示信息
        updateInfoDisplay(info);
        QDateTime entryDisplayTime = entryTimeMap.contains(currentCardId) ?
                                     entryTimeMap.value(currentCardId) :
                                     lastEntryTimeMap.value(currentCardId);
        updateInfoPanel(info, entryDisplayTime, lastExitTimeMap.value(currentCardId));
        if(entryTimeMap.contains(currentCardId))
        {
            activeInfoMap.insert(currentCardId, info);
            updateParkingTable();
        }
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
    activeInfoMap.remove(currentCardId);
    updateParkingTable();

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
    balanceSpin->setValue(0);
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

bool IEEE14443ControlWidget::showRechargeDialog(int &amount)
{
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

    if(dialog.exec() != QDialog::Accepted)
        return false;

    amount = amountSpin->value();
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

void IEEE14443ControlWidget::startRechargeFlow(int feeRequired)
{
    if(rechargeFlowActive)
        return;
    rechargeFlowActive = true;
    pauseForRecharge(feeRequired);

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

    int rechargeAmount = 0;
    if(!showRechargeDialog(rechargeAmount))
    {
        rechargeFlowActive = false;
        if(feeRequired == 0)
            resumeAfterRecharge();
        else
            ui->parkingStatusLabel->setText(tr("请充值后出场"));
        return;
    }

    if(rechargePaused && pendingExitFee > 0 && (currentInfo.balance + rechargeAmount < pendingExitFee))
    {
        QMessageBox::warning(this, tr("Recharge"), tr("请至少充值到覆盖待缴费用%1").arg(pendingExitFee));
        ui->parkingStatusLabel->setText(tr("请重新充值"));
        rechargeFlowActive = false;
        return;
    }

    TagInfo info = currentInfo;
    info.balance += rechargeAmount;
    rechargeExpectedBalance = info.balance;
    rechargePendingInfo = info;

    if(waitingReply)
    {
        rechargeWritePending = true;
        rechargePendingStatusText = tr("正在充值...");
        ui->parkingStatusLabel->setText(tr("等待当前操作完成..."));
        return;
    }

    rechargeVerificationPending = true;
    refreshAfterWrite = true;
    writeUpdatedInfo(info);
    ui->parkingStatusLabel->setText(tr("正在充值..."));
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
            ui->parkingStatusLabel->setText(tr("余额不足，请先充值，最低收费金额%1").arg(fee));
            updateInfoPanel(currentInfo, enter, QDateTime());
            //进入充值流程
            QMessageBox::warning(this, tr("出场"), tr("余额不足，请先充值，最低收费金额%1").arg(fee));
            startRechargeFlow(fee);
            return;
        }

        //移除入场时间信息
        entryTimeMap.remove(currentCardId);
        activeInfoMap.remove(currentCardId);
        updateParkingTable();
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
    if(!p.isValid())
        return;
    if(!waitingReply && pendingCommand < 0)
        return;
    if(waitingReply && pendingCommand >= 0 && p.command() != pendingCommand)
        return;
    if(isDuplicateResponse(p))
        return;
    if(replyTimeoutTimer && replyTimeoutTimer->isActive())
        replyTimeoutTimer->stop();
    //qDebug()<<"the recieve pkg"<<pkg.toHex();
    qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
             << QString("recieve cmd=0x%1 data=%2")
                .arg(p.command(), 2, 16, QChar('0'))
                .arg(QString(p.data().toHex()));
    QByteArray d = p.data();
    if(d.isEmpty())
    {
        qDebug() << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                 << "empty payload for cmd" << p.command();
        waitingReply = false;
        pendingCommand = -1;
        autoSearchInProgress = false;
        return;
    }
    int status = d.at(0);
    d = d.mid(1);
    waitingReply = false;
    pendingCommand = -1;
    pendingRetries = 0;
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
        tagAuthenticated = (status == 0);
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
            // 读卡指令的响应, 可以获得卡内数据
            // 读取正常
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
                autoSearchInProgress = false;
            }
        }
        else
        {
            resultTipText += tr("Failure");
            pendingReadBlock = -1;
            autoSearchInProgress = false;
            if(rechargeVerificationPending)
            {
                rechargeVerificationPending = false;
                rechargeFlowActive = false;
                refreshAfterWrite = false;
                QMessageBox::warning(this, tr("充值失败"), tr("请重新充值"));
                ui->parkingStatusLabel->setText(tr("请重新充值"));
                break;
            }
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
                    if(entryTimeMap.contains(currentCardId) && pendingWriteInfo.valid)
                    {
                        activeInfoMap.insert(currentCardId, pendingWriteInfo);
                        updateParkingTable();
                    }
                }

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

    if(registrationWritePending && !waitingReply)
    {
        registrationWritePending = false;
        registrationVerificationPending = true;
        refreshAfterWrite = true;
        writeUpdatedInfo(registrationPendingInfo);
        ui->parkingStatusLabel->setText(registrationPendingStatusText);
    }
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

void IEEE14443ControlWidget::onStatusListScrollRangeChanced(int min, int max)
{
//    ui->statusList->verticalScrollBar()->setValue(max);
}

//处理timeout信号
void IEEE14443ControlWidget::onAutoSearchTimeout()
{
    if(registrationPaused || rechargePaused || requiresInitialization || parkingFlowPaused)
        return;
    if(autoSearchInProgress)
        return;
    requestSearch();//每过一小段时间，就请求寻卡
}

void IEEE14443ControlWidget::onReplyTimeout()
{
    if(!waitingReply || pendingCommand < 0)
        return;
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
    int failedCommand = pendingCommand;
    waitingReply = false;
    pendingCommand = -1;
    autoSearchInProgress = false;
    handleReplyTimeoutFailure(failedCommand);
}

void IEEE14443ControlWidget::showEvent(QShowEvent *)
{
    if(!commPort)
    {
        //qDebug()<<"set mode success";
        IOPortManager::setMode(Mode13_56M1);
        this->start("/dev/ttyS0");
    }
}
void IEEE14443ControlWidget::hideEvent(QHideEvent *)
{
    if(commPort)
    {
        this->stop();
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
