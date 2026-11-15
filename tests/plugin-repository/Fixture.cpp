#include "hotaru-native-plugin.h"
#include <QWidget>

class Fixture final : public QObject, public HotaruNativePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HOTARU_NATIVE_PLUGIN_IID FILE "repository-fixture.json")
    Q_INTERFACES(HotaruNativePlugin)
public:
    bool start(const QString&) override { return true; }
    void stop() override {}
    QString state(const QString&) const override { return QStringLiteral("已启用"); }
    QWidget* createPage(const QString&, QWidget* parent) override { return new QWidget(parent); }
    void setFileOfferNotifier(std::function<void(QString)>) override {}
};
#include "Fixture.moc"
