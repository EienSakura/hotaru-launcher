#pragma once
#include "LauncherTheme.h"
#include "core-api.h"
#include <QPixmap>
#include <QPointer>
#include <QWidget>

class QLabel;

// 纯资源绘制层，鼠标事件交给下面的输入框、固定区和窗口拖动处理。
class LTOOL_CORE_API LauncherSkin : public QWidget {
    Q_OBJECT
public:
    explicit LauncherSkin(QWidget* parent);
    ~LauncherSkin() override;
    void reload();
    void setSurfaceVisible(bool visible);
    void setCompact(bool compact, bool hasResults = false);
    QRect compactSearchRect() const;
    QRect compactContentRect() const;
    int compactHeight(int resultHeight = 0) const;
    bool active() const { return m_surface.enabled(); }
    QRect mapDesignRect(const QRect& rect) const;
    QRect searchRect() const;
    QRect contentRect() const { return mapDesignRect(m_surface.contentRect); }
    QRect footerRect() const;
    QRect settingsRect() const;
    QRect decorationRect(int index) const;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    int compactFrameHeight() const;
    Theme::Surface m_surface;
    QPixmap m_frame;
    QVector<QLabel*> m_decorations;
    QPointer<QWidget> m_contentDecorations;
    bool m_surfaceVisible = true;
    bool m_compact = false;
    bool m_hasResults = false;
};
