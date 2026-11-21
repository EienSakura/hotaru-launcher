#pragma once

#include "SettingsPalette.h"

#include <QPainter>
#include <QPainterPath>

namespace SettingsFrame {

inline constexpr int kBorder = 1;

// 在同一个透明表面绘制四角和分区，保留分数 DPI 下沿圆弧的透明度。
inline void paint(QPainter& painter, const QRect& rect, int radius, int titleBottom,
                  const QRect& rail = {}, const QRect& footer = {})
{
    const auto colors = SettingsPalette::current();
    const auto roundRect = [](const QRectF& area, int corner) {
        QPainterPath path;
        path.addRoundedRect(area, corner, corner);
        return path;
    };
    const auto inside = rect.adjusted(kBorder, kBorder, -kBorder, -kBorder);
    const auto innerPath = roundRect(inside, qMax(0, radius - kBorder));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillPath(roundRect(rect, radius), QColor(colors.line));
    const auto fillBand = [&](const QRect& band, const QColor& fill) {
        if (band.isEmpty())
            return;
        painter.save();
        painter.setClipRect(band, Qt::IntersectClip);
        painter.fillPath(innerPath, fill);
        painter.restore();
    };
    fillBand(QRect(inside.left(), inside.top(), inside.width(), titleBottom - inside.top()),
             QColor(colors.group));
    const int bodyLeft = rail.isEmpty() ? inside.left() : rail.right() + 1;
    const int bodyBottom = footer.isEmpty() ? inside.bottom() + 1 : footer.top();
    fillBand(QRect(bodyLeft, titleBottom, inside.right() + 1 - bodyLeft, bodyBottom - titleBottom),
             QColor(colors.window));
    fillBand(rail, QColor(colors.rail));
    fillBand(footer, QColor(colors.group));
    painter.fillRect(inside.left(), titleBottom - kBorder, inside.width(), kBorder, QColor(colors.line));
    if (!rail.isEmpty())
        painter.fillRect(rail.right(), rail.top(), kBorder, rail.height(), QColor(colors.line));
    if (!footer.isEmpty())
        painter.fillRect(footer.left(), footer.top(), footer.width(), kBorder, QColor(colors.line));
}

} // namespace SettingsFrame
