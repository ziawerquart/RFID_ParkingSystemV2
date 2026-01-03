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
    Ui::IEEE14443ControlWidget *ui;
    //QextSerialPort *commPort;
    Posix_QextSerialPort *commPort;
    QTimer *autoSearchTimer;//自动寻卡-定时器
    QTimer *replyTimeoutTimer;//等待回包-定时器
    QTimer *readTimer;
    QByteArray lastSendPackage;
    int recvStatus;
    QByteArray lastRecvPackage;
    bool waitingReply;//自动寻卡——是否等回复
    int pendingCommand;//自动寻卡——等待回包的命令
    int pendingRetries;
    int maxReplyRetries;
    int replyTimeoutMs;
    QHash<QString, QDateTime> recentReplyTimestamps;
    bool autoSearchInProgress;//自动寻卡——完整流程中
    QString currentCardId;//自动寻卡——当前识别到的ID
    bool tagAuthenticated;//自动寻卡——是否认证成功
    QByteArray lastBlock1;//缓存块1、2数据
    QByteArray lastBlock2;
    int pendingReadBlock;//自动寻卡——指示当前等回包的所哪个块的读操作
    int pendingWriteBlock;
    TagInfo pendingWriteInfo;
    TagInfo currentInfo;
    QMap<QString, QDateTime> entryTimeMap;
    QMap<QString, QDateTime> lastEntryTimeMap;
    QMap<QString, QDateTime> lastExitTimeMap;
    QMap<QString, TagInfo> activeInfoMap;
    bool requiresInitialization;
    bool refreshAfterWrite;
    bool registrationPaused;
    bool registrationFlowActive;
    bool registrationVerificationPending;
    bool registrationWritePending;
    QString registrationPendingStatusText;
    TagInfo registrationPendingInfo;
    bool registrationAwaitingRemoval;
    QString registrationAwaitingCardId;
    bool rechargePaused;
    bool rechargeFlowActive;
    bool rechargeVerificationPending;
    bool rechargeWritePending;
    QString rechargePendingStatusText;
    TagInfo rechargePendingInfo;
    int rechargeExpectedBalance;
    bool rechargeAwaitingRemoval;
    QString rechargeAwaitingCardId;
    int pendingExitFee;
    bool parkingFlowPaused;
    ParkingFlowState parkingFlowState;
    bool parkingExitWritePending;
    int lastExitFee;


private:
    bool sendData(const QByteArray &data);
    void resetBlockList(int min, int max, int secSize);
    void resetStatus();
    void startAutoSearch();
    void stopAutoSearch();
    void pauseForRegistration();
    void resumeAfterRegistration();
    void pauseForRecharge(int feeRequired);
    void resumeAfterRecharge();
    void requestSearch();
    void requestAntiColl();
    void requestSelect(const QByteArray &cardId);
    void requestAuth(quint8 blockNumber);
    void requestRead(quint8 blockNumber);
    void requestWrite(quint8 blockNumber, const QByteArray &data);
    void handleTagInfo();
    bool decodeTagInfo(const QByteArray &b1, const QByteArray &b2, TagInfo &info);
    void encodeTagInfo(const TagInfo &info, QByteArray &b1, QByteArray &b2);
    void updateInfoDisplay(const TagInfo &info);
    void updateInfoPanel(const TagInfo &info, const QDateTime &entryTime, const QDateTime &exitTime);
    void updateParkingTable();
    TagInfo defaultTagInfo() const;
    void ensureInitialized();
    void handleInvalidCard();
    bool showRegistrationDialog(TagInfo &info);
    void startRegistrationFlow();
    bool showRechargeDialog(int &amount);
    void startRechargeFlow(int feeRequired);
    void handleParkingFlow();
    int calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const;
    void writeUpdatedInfo(const TagInfo &info);
    void startReplyTimeout(quint8 command);
    void handleReplyTimeoutFailure(int command);
    bool isDuplicateResponse(const IEEE1443Package &pkg);
    void pruneRecentReplies();

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
    void onAutoSearchTimeout();//定时寻卡
    void onReplyTimeout();//等待回包超时
};

#endif // IEEE14443CONTROLWIDGET_H
