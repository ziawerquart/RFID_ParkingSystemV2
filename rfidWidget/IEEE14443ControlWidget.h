#ifndef IEEE14443CONTROLWIDGET_H
#define IEEE14443CONTROLWIDGET_H

#include <QWidget>
//#include <qextserialport.h>
#include "posix_qextserialport.h"
#include <QMap>
#include <QDateTime>
#include <QTimer>

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

    //停车用户信息结构体
    struct TagInfo
    {
        QString owner;//12字节——块1
        QString vehicleType;//1字节——块1
        int balance;//4字节——块2
        bool valid;//由块1开头签名判断是否为“停车系统格式卡”
        TagInfo() : balance(0), valid(false) {}
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
    QTimer *autoSearchTimer;//自动寻卡定时器
    QTimer *readTimer;
    QByteArray lastSendPackage;
    int recvStatus;
    QByteArray lastRecvPackage;
    bool waitingReply;//是否停止等回复，用于防乱序
    QString currentCardId;//当前识别到的卡ID
    bool tagAuthenticated;//是否成功认证当前卡
    QByteArray lastBlock1;
    QByteArray lastBlock2;
    int pendingReadBlock;//待读，等回包
    int pendingWriteBlock;
    TagInfo pendingWriteInfo;//暂存要写入的车主信息
    TagInfo currentInfo;//当前卡解析出的车主信息
    QMap<QString, QDateTime> entryTimeMap;
    QMap<QString, QDateTime> lastEntryTimeMap;//出场时间
    QMap<QString, QDateTime> lastExitTimeMap;
    bool requiresInitialization;//是否是PK签名格式
    bool refreshAfterWrite;//写完后是否重新读确认
    bool registrationPaused;//注册过程中暂停寻卡


private:
    bool sendData(const QByteArray &data);
    void resetBlockList(int min, int max, int secSize);
    void resetStatus();
    void startAutoSearch();
    void stopAutoSearch();
    void pauseForRegistration();
    void resumeAfterRegistration();
    void requestSearch();//寻卡
    void requestAntiColl();//防冲突
    void requestSelect(const QByteArray &cardId);//选卡
    void requestAuth(quint8 blockNumber);//认证
    void requestRead(quint8 blockNumber);//读块
    void requestWrite(quint8 blockNumber, const QByteArray &data);//写块
    void handleTagInfo();
    bool decodeTagInfo(const QByteArray &b1, const QByteArray &b2, TagInfo &info);//业务对象 ↔ 两个16字节块
    void encodeTagInfo(const TagInfo &info, QByteArray &b1, QByteArray &b2);
    void updateInfoDisplay(const TagInfo &info);//更新界面显示
    void updateInfoPanel(const TagInfo &info, const QDateTime &entryTime, const QDateTime &exitTime);
    TagInfo defaultTagInfo() const;//用于注册
    void ensureInitialized();//判断是不是停车卡
    void handleParkingFlow();//进出场判断+扣费
    int calculateFee(const QDateTime &enterTime, const QDateTime &leaveTime) const;
    void writeUpdatedInfo(const TagInfo &info);//写卡写两次

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

    //停车系统逻辑
    void on_registerBtn_clicked();//注册
    void on_rechargeBtn_clicked();//充值
    void onAutoSearchTimeout();//定时寻卡
};

#endif // IEEE14443CONTROLWIDGET_H
