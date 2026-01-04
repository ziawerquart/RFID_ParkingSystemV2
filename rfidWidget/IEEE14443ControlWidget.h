#ifndef IEEE14443CONTROLWIDGET_H
#define IEEE14443CONTROLWIDGET_H

#include <QWidget>
//#include <qextserialport.h>
#include "posix_qextserialport.h"
#include <QMap>
#include <QDateTime>
#include <QTimer>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QHash>
#include <QTableWidgetItem>

namespace Ui {
    class IEEE14443ControlWidget;
}

class IEEE1443Package;

class IEEE14443ControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit IEEE14443ControlWidget(QWidget *parent = 0);
    ~IEEE14443ControlWidget();
    static void showOut();

    //停车用户信息结构体
    struct TagInfo
    {
        QString owner;//12字节——块1
        QString vehicleType;//1字节——块1
        int balance;//4字节——块2
        bool valid;//由块1开头签名判断是否为“停车系统格式卡”
        TagInfo() : balance(0), valid(false) {}
    };
    enum ParkingFlowState
    {
        ParkingFlowIdle = 0,
        ParkingFlowEntry,
        ParkingFlowExit
    };

protected:
    void showEvent(QShowEvent *);
    void hideEvent(QHideEvent *);

public slots:
    bool start(const QString &port);
    bool stop();

signals:
    void recvPackage(QByteArray pkg);

private:
    // === UI与串口通信 ===
    Ui::IEEE14443ControlWidget *ui;
    //QextSerialPort *commPort;
    Posix_QextSerialPort *commPort;
    QTimer *autoSearchTimer;//自动寻卡-定时器
    QTimer *replyTimeoutTimer;//等待回包-定时器
    QTimer *readTimer;//轮询读取串口数据

    // === 通信包与状态管理 ===
    QByteArray lastSendPackage;//最近发送包
    QByteArray lastRecvPackage;//最近接收包
    int recvStatus;//接收状态
    bool waitingReply;//是否等待回包
    int pendingCommand;//等待回包的命令
    int pendingRetries;//当前重试次数
    int maxReplyRetries;//最大重试次数
    int replyTimeoutMs;//回包超时毫秒
    QHash<QString, QDateTime> recentReplyTimestamps;//用于重复包过滤

    // === 寻卡/认证与块数据 ===
    bool autoSearchInProgress;//自动寻卡流程中
    QString currentCardId;//当前识别到的ID
    bool tagAuthenticated;//是否认证成功
    QByteArray lastBlock1;//缓存块1数据
    QByteArray lastBlock2;//缓存块2数据
    int pendingReadBlock;//等待回包的读块编号
    int pendingWriteBlock;//等待回包的写块编号
    TagInfo pendingWriteInfo;//待写入的卡信息
    TagInfo currentInfo;//当前卡信息
    QByteArray authKeyData;//认证Key数据

    // === 停车记录与费用 ===
    QMap<QString, QDateTime> entryTimeMap;//当前入场时间
    QMap<QString, QDateTime> lastEntryTimeMap;//历史入场时间
    QMap<QString, QDateTime> lastExitTimeMap;//历史出场时间
    QMap<QString, TagInfo> activeInfoMap;//当前在场信息
    int pendingExitFee;//等待结算的费用
    int lastExitFee;//上次结算费用
    bool parkingFlowPaused;//停车流程暂停
    ParkingFlowState parkingFlowState;//停车流程状态
    bool parkingExitWritePending;//出场写卡待完成

    // === 注册流程 ===
    bool requiresInitialization;//是否需要初始化
    bool refreshAfterWrite;//写卡后是否刷新
    bool registrationPaused;//注册流程暂停
    bool registrationFlowActive;//注册流程激活
    bool registrationVerificationPending;//注册等待校验
    bool registrationWritePending;//注册等待写入
    QString registrationPendingStatusText;//注册提示文案
    TagInfo registrationPendingInfo;//注册信息
    bool registrationAwaitingRemoval;//注册等待移卡
    QString registrationAwaitingCardId;//注册等待的卡号

    // === 充值流程 ===
    bool rechargePaused;//充值流程暂停
    bool rechargeFlowActive;//充值流程激活
    bool rechargeVerificationPending;//充值等待校验
    bool rechargeWritePending;//充值等待写入
    QString rechargePendingStatusText;//充值提示文案
    TagInfo rechargePendingInfo;//充值信息
    int rechargeExpectedBalance;//充值后预期余额
    bool rechargeAwaitingRemoval;//充值等待移卡
    QString rechargeAwaitingCardId;//充值等待的卡号


private:
    // === 通信与状态 ===
    bool sendData(const QByteArray &data);
    void resetStatus();
    void startReplyTimeout(quint8 command);
    void handleReplyTimeoutFailure(int command);
    bool isDuplicateResponse(const IEEE1443Package &pkg);
    void pruneRecentReplies();

    // === 寻卡/协议指令 ===
    void startAutoSearch();
    void stopAutoSearch();
    void requestSearch();
    void requestAntiColl();
    void requestSelect(const QByteArray &cardId);
    void requestAuth(quint8 blockNumber);
    void requestRead(quint8 blockNumber);
    void requestWrite(quint8 blockNumber, const QByteArray &data);

    // === 业务流程控制 ===
    void pauseForRegistration();
    void resumeAfterRegistration();
    void pauseForRecharge(int feeRequired);
    void resumeAfterRecharge();
    void handleParkingFlow();
    int calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const;
    void writeUpdatedInfo(const TagInfo &info);
    void ensureInitialized();
    void handleInvalidCard();

    // === 卡信息解析与UI更新 ===
    void handleTagInfo();
    bool decodeTagInfo(const QByteArray &b1, const QByteArray &b2, TagInfo &info);
    void encodeTagInfo(const TagInfo &info, QByteArray &b1, QByteArray &b2);
    void updateInfoDisplay(const TagInfo &info);
    void updateInfoPanel(const TagInfo &info, const QDateTime &entryTime, const QDateTime &exitTime);
    void updateParkingTable();
    TagInfo defaultTagInfo() const;

    // === 注册/充值弹窗 ===
    bool showRegistrationDialog(TagInfo &info);
    void startRegistrationFlow();
    bool showRechargeDialog(int &amount);
    void startRechargeFlow(int feeRequired);

private slots:
    void onPortDataReady();
    void onRecvedPackage(QByteArray pkg);
    void onStatusListScrollRangeChanced(int min, int max);
    void onAutoSearchTimeout();//定时寻卡
    void onReplyTimeout();//等待回包超时
};

#endif // IEEE14443CONTROLWIDGET_H
