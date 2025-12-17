#ifndef IEEE1443PACKAGE_H
#define IEEE1443PACKAGE_H
#include <QByteArray>
#include <QString>

#define IEEE1443_ESCAPE_CHAR    0x10
#define IEEE1443_START_CODE     0x02
#define IEEE1443_STOP_CODE      0x03

class IEEE1443Package {
    bool _valid;
    quint8 _ssync;              // 同步头 = 0x02
    quint16 _addr;              // 模块地址
    quint8 _len;                // 长度, = cmd + data + chksum
    quint8 _cmd;                // 命令字
    QByteArray _data;           // 数据域
    quint8 _chksum;             // 校验和
    quint8 _esync;              // 同步尾 = 0x03

public:
    enum IEEE1443Command {
        SearchCard = 0x46,
        AntiColl,
        SelectCard,
        Authentication = 0x4A,
        ReadCard,
        WriteCard,
        MaxCmd
    };
    IEEE1443Package():_valid(false)              {}
    IEEE1443Package(const QByteArray &rawPkg);
    IEEE1443Package(quint16 addr, quint8 cmd);
    IEEE1443Package(quint16 addr, quint8 cmd, const QByteArray &data);
    IEEE1443Package(quint16 addr, quint8 cmd, quint8 data);
    IEEE1443Package(quint16 addr, quint8 cmd, quint8 data1, quint8 data2);

    bool isValid() {
        return _valid;
    }
    bool isSendPackage() {
        if(!_valid)
            return false;
        return (_len == (_data.size() + 3));
    }
    bool isRecvPackage() {
        if(!_valid)
            return false;
        return (_len == (_data.size() + 2));
    }

    quint8 ssync() const {
        return _ssync;
    }
    quint16 address() const {
        return _addr;
    }
    void setAddress(quint16 addr) {
        _addr = addr;
    }
    quint8 length() const {
        return _len;
    }
    quint8 dataLen() const {
        return _data.size();
    }
    quint8 command() const {
        return _cmd;
    }
    const QByteArray &data() const {
        return _data;
    }
    void setData(const QByteArray &data) {
        if(!isSendPackage())
            return;
        *this = IEEE1443Package(_addr, _cmd, data);
    }
    QByteArray &data() {
        return _data;
    }
    quint8 checkSum() const {
        return _chksum;
    }

    QByteArray toPurePackage() const;
    QByteArray toRawPackage() const;
    static QByteArray getRawPackage(const quint8 *data, int len);
    static QByteArray getRawPackage(const QByteArray &data);
    static QByteArray getRawPackage(quint8 data) {
        return getRawPackage(&data, 1);
    }
    static QByteArray getRawPackage(quint16 data) {
        return getRawPackage((quint8 *)&data, 2);
    }
    static QString getRawString(const quint8 *data, int len);
    static QString getRawString(const QByteArray &data);
    static QString getRawString(quint8 data) {
        return getRawString(&data, 1);
    }
    static QString getRawString(quint16 data) {
        return getRawString((quint8 *)&data, 2);
    }
};

#endif // IEEE1443PACKAGE_H
