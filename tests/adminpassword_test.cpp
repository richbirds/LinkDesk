#include "adminpassword.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
class AdminPasswordTest : public QObject {
    Q_OBJECT
private slots:
    void defaultAndChangePersistWithoutPlaintext() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("admin.ini");
        QSettings settings(path,QSettings::IniFormat); AdminPassword::initialize(settings);
        QVERIFY(AdminPassword::verify(settings,"888888")); QVERIFY(!AdminPassword::verify(settings,"wrong"));
        const auto before=settings.value("administrator/credential").toString(); QVERIFY(!before.contains("888888")); QString error;
        QVERIFY(!AdminPassword::change(settings,"wrong","new-secret",&error)); QCOMPARE(settings.value("administrator/credential").toString(),before);
        QVERIFY(!AdminPassword::change(settings,"888888","short",&error)); QVERIFY(AdminPassword::verify(settings,"888888"));
        QVERIFY(AdminPassword::change(settings,"888888","new-secret",&error));
        QVERIFY(!AdminPassword::verify(settings,"888888")); QVERIFY(AdminPassword::verify(settings,"new-secret"));
        QVERIFY(!settings.value("administrator/credential").toString().contains("new-secret"));
        QSettings reopened(path,QSettings::IniFormat); QVERIFY(AdminPassword::verify(reopened,"new-secret")); AdminPassword::initialize(reopened); QVERIFY(AdminPassword::verify(reopened,"new-secret"));
    }
    void saltsDifferAndMalformedCredentialFailsClosed() {
        QTemporaryDir dir; QSettings one(dir.filePath("one.ini"),QSettings::IniFormat),two(dir.filePath("two.ini"),QSettings::IniFormat);
        AdminPassword::initialize(one); AdminPassword::initialize(two);
        QVERIFY(one.value("administrator/credential")!=two.value("administrator/credential"));
        one.setValue("administrator/credential","corrupted"); QVERIFY(!AdminPassword::verify(one,"888888"));
        QString error; QVERIFY(!AdminPassword::change(one,"888888","new-secret",&error));
    }
};
QTEST_GUILESS_MAIN(AdminPasswordTest)
#include "adminpassword_test.moc"
