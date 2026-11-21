#pragma once

#include "SettingsPalette.h"
#include <QIcon>
#include <QPainter>
#include <QSvgRenderer>
#include <QWidget>

enum class SettingsGlyph {
    Search, Plug, Gear, Info, Sun, Moon, Palette, Grid, Panels, Terminal, Folder,
    Check, Plus, More, Upload, Minimize, Maximize, Restore, Close, Question, Warning, Error, Count
};

// 24 像素坐标和 1.65 像素描边沿用已确认的设置设计稿。
inline QIcon settingsIcon(SettingsGlyph glyph, const QColor& color)
{
    static constexpr const char* paths[] = {
        R"svg(<circle cx="10.5" cy="10.5" r="5.8"/><path d="m15 15 4 4"/>)svg",
        R"svg(<path d="M8 3v5m8-5v5M6 8h12v3a6 6 0 0 1-12 0V8Zm6 9v4"/>)svg",
        R"svg(<circle cx="12" cy="12" r="3"/><path d="M12 2v3m0 14v3M2 12h3m14 0h3M5 5l2 2m10 10 2 2M5 19l2-2M17 7l2-2"/><circle cx="12" cy="12" r="7"/>)svg",
        R"svg(<circle cx="12" cy="12" r="9"/><path d="M12 11v5m0-8h.01"/>)svg",
        R"svg(<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/>)svg",
        R"svg(<path d="M20 15A8.5 8.5 0 0 1 9 4a8.5 8.5 0 1 0 11 11Z"/>)svg",
        R"svg(<path d="M12 3a9 9 0 1 0 0 18h1.3a1.6 1.6 0 0 0 1.2-2.7 1.4 1.4 0 0 1 1.1-2.3H18c2.1 0 3-1.8 3-4a9 9 0 0 0-9-9Z"/><path d="M8 8h.01m5-1h.01m4 3h.01M7 13h.01"/>)svg",
        R"svg(<rect x="4" y="4" width="6" height="6" rx="1"/><rect x="14" y="4" width="6" height="6" rx="1"/><rect x="4" y="14" width="6" height="6" rx="1"/><rect x="14" y="14" width="6" height="6" rx="1"/>)svg",
        R"svg(<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M9 5v14"/>)svg",
        R"svg(<rect x="3" y="5" width="18" height="14" rx="2"/><path d="m7 10 3 2-3 2m6 0h4"/>)svg",
        R"svg(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/>)svg",
        R"svg(<path d="m5 12 4 4L19 6"/>)svg",
        R"svg(<path d="M12 5v14M5 12h14"/>)svg",
        R"svg(<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>)svg",
        R"svg(<path d="M12 16V4m-4 4 4-4 4 4M4 16v4h16v-4"/>)svg",
        R"svg(<path d="M5 15h14"/>)svg",
        R"svg(<rect x="6" y="6" width="12" height="12"/>)svg",
        R"svg(<rect x="5" y="9" width="10" height="10"/><path d="M9 6h10v10"/>)svg",
        R"svg(<path d="m6 6 12 12M18 6 6 18"/>)svg",
        R"svg(<circle cx="12" cy="12" r="9"/><path d="M9.5 9a2.5 2.5 0 1 1 4.2 1.8c-1.1 1-1.7 1.5-1.7 3m0 3h.01"/>)svg",
        R"svg(<path d="m10.3 4.6-7.1 12.5a2 2 0 0 0 1.7 3h14.2a2 2 0 0 0 1.7-3L13.7 4.6a2 2 0 0 0-3.4 0ZM12 9v4m0 3h.01"/>)svg",
        R"svg(<circle cx="12" cy="12" r="9"/><path d="m9 9 6 6m0-6-6 6"/>)svg"
    };
    static_assert(sizeof(paths) / sizeof(*paths) == static_cast<int>(SettingsGlyph::Count));
    const QByteArray svg = QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<g fill=\"none\" stroke-width=\"1.65\" stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke=\"")
        + color.name().toLatin1() + QByteArrayLiteral("\">") + paths[static_cast<int>(glyph)]
        + QByteArrayLiteral("</g></svg>");
    QSvgRenderer renderer(svg);
    QPixmap pixmap(72, 72);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();
    return QIcon(pixmap);
}

class SettingsIconTile final : public QWidget {
public:
    SettingsIconTile(SettingsGlyph icon, QWidget* parent) : QWidget(parent), m_icon(icon)
    {
        setFixedSize(32, 32);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto colors = SettingsPalette::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(colors.field));
        p.drawRoundedRect(rect(), 9, 9);
        settingsIcon(m_icon, QColor(colors.accent)).paint(&p, QRect(7, 7, 18, 18));
    }

private:
    SettingsGlyph m_icon;
};
