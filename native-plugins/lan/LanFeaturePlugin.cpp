#include "hotaru-native-plugin.h"

class LanFeaturePlugin final : public QObject, public HotaruLanPackage {
    Q_OBJECT
    Q_INTERFACES(HotaruLanPackage)
#if defined(HOTARU_LAN_CLIPBOARD_MODULE)
    Q_PLUGIN_METADATA(IID HOTARU_LAN_PACKAGE_IID FILE "published-clipboard.json")
public:
    QString id() const override { return QLatin1String(kHotaruLanClipboardId); }
    int feature() const override { return kHotaruFeatureClipboard; }
#else
    Q_PLUGIN_METADATA(IID HOTARU_LAN_PACKAGE_IID FILE "published-files.json")
public:
    QString id() const override { return QLatin1String(kHotaruLanFilesId); }
    int feature() const override { return kHotaruFeatureFiles; }
#endif
};

#include "LanFeaturePlugin.moc"
