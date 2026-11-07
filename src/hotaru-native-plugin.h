#pragma once

#include <QtPlugin>
#include <QString>

class QWidget;

// Hotaru 直属插件接口。与 ltool API v4 独立，插件只链接 Qt 和自己的运行库。
inline constexpr int kHotaruNativePluginApi = 1;
#define HOTARU_NATIVE_PLUGIN_IID "app.hotaru.NativePlugin/1.0"

class HotaruNativePlugin {
public:
    virtual ~HotaruNativePlugin() = default;
    virtual QString id() const = 0;
    virtual bool start(QString* error) = 0;
    virtual void stop() = 0;
    virtual QWidget* createPage(QWidget* parent) = 0;
};

Q_DECLARE_INTERFACE(HotaruNativePlugin, HOTARU_NATIVE_PLUGIN_IID)
