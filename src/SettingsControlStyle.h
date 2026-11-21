#pragma once

#include "SettingsPalette.h"
#include "SettingsIcons.h"
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFocusFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>

class SettingsComboBox final : public QComboBox {
public:
    explicit SettingsComboBox(QWidget* parent) : QComboBox(parent)
    {
        setMaxVisibleItems(8);
        // 下拉列表自行配色，避免 Qlementine 为菜单式弹层另外调整视图尺寸。
        auto* list = new QListView;
        list->setStyle(QApplication::style());
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setView(list);
        list->window()->installEventFilter(this);
    }

    void showPopup() override
    {
        const auto colors = SettingsPalette::current();
        auto* list = view();
        const auto sheet = colors.expand(R"QSS(
QAbstractItemView {
    background: @group; color: @text; border: 1px solid @line; border-radius: 8px;
    padding: 3px; outline: none; selection-background-color: @selBg; selection-color: @text;
}
QAbstractItemView::item { min-height: 30px; padding: 0 8px; border-radius: 5px; }
QAbstractItemView::item:hover { background: @hover; }
QAbstractItemView::item:selected { background: @selBg; color: @text; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 20px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)QSS");
        if (list->styleSheet() != sheet)
            list->setStyleSheet(sheet);
        list->setMinimumWidth(qMin(width(), screen()->availableGeometry().width()));
        QComboBox::showPopup();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // 列表样式只覆盖列表本身，弹出窗口也需要裁去四个角。
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize) {
            if (auto* popup = qobject_cast<QWidget*>(watched); popup && popup == view()->window()) {
                QPainterPath shape;
                shape.addRoundedRect(QRectF(popup->rect()), 8, 8);
                popup->setMask(QRegion(shape.toFillPolygon().toPolygon()));
            }
        }
        return QComboBox::eventFilter(watched, event);
    }
};

class SettingsControlStyle final : public oclero::qlementine::QlementineStyle {
public:
    using QlementineStyle::QlementineStyle;
    using QlementineStyle::polish;

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr, QStyleHintReturn* result = nullptr) const override
    {
        // 保留 Qt 的列表定位、空间不足时的翻转和长列表滚动。
        if (hint == SH_ComboBox_Popup || hint == SH_ComboBox_PopupFrameStyle)
            return 0;
        return QlementineStyle::styleHint(hint, option, widget, result);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
                         SubControl part, const QWidget* widget = nullptr) const override
    {
        if (control == CC_ComboBox && part == SC_ComboBoxListBoxPopup && option)
            return option->rect.adjusted(0, -3, 0, 5);
        return QlementineStyle::subControlRect(control, option, part, widget);
    }

    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option = nullptr,
                       const QWidget* widget = nullptr) const override
    {
        const auto colors = SettingsPalette::current();
        switch (icon) {
        case SP_MessageBoxInformation: return settingsIcon(SettingsGlyph::Info, QColor(colors.accent));
        case SP_MessageBoxQuestion: return settingsIcon(SettingsGlyph::Question, QColor(colors.accent));
        case SP_MessageBoxWarning: return settingsIcon(SettingsGlyph::Warning, Theme::color("warn"));
        case SP_MessageBoxCritical: return settingsIcon(SettingsGlyph::Error, Theme::color("bad"));
        default: return QlementineStyle::standardIcon(icon, option, widget);
        }
    }

    void applyTheme(const SettingsPalette& colors)
    {
        using ControlTheme = oclero::qlementine::Theme;
        const auto base = Theme::current() == Theme::Mode::Dark ? ControlTheme::makeDark() : ControlTheme::makeLight();
        auto theme = base.toJson().object();
        theme.insert(QStringLiteral("useSystemFonts"), true);
        theme.insert(QStringLiteral("borderRadius"), 9);
        theme.insert(QStringLiteral("controlHeightMedium"), 32);
        theme.insert(QStringLiteral("focusBorderWidth"), 1);
        const struct { const char* key; const char* value; } entries[] = {
            {"backgroundColorMain1", colors.group}, {"backgroundColorMain2", colors.field},
            {"backgroundColorMain3", colors.hover}, {"backgroundColorMain4", colors.selected},
            {"borderColor", colors.line}, {"borderColorDisabled", colors.line},
            {"borderColorHovered", colors.accent}, {"focusColor", colors.accent},
            {"neutralColor", colors.field}, {"neutralColorDisabled", colors.field},
            {"neutralColorHovered", colors.hover}, {"neutralColorPressed", colors.selected},
            {"primaryColor", colors.primary}, {"primaryColorHovered", colors.accent},
            {"primaryColorPressed", colors.primary}, {"primaryColorForeground", colors.onPrimary},
            {"primaryColorForegroundHovered", colors.onPrimary}, {"primaryColorForegroundPressed", colors.onPrimary},
            {"secondaryColor", colors.text}, {"secondaryColorHovered", colors.text},
            {"secondaryColorPressed", colors.text}, {"secondaryColorDisabled", colors.muted}
        };
        for (const auto& entry : entries)
            theme.insert(QLatin1String(entry.key), QLatin1String(entry.value));
        setTheme(ControlTheme::fromJsonDoc(QJsonDocument(theme)).value_or(base));
    }

    void polish(QWidget* widget) override
    {
        QlementineStyle::polish(widget);
        // QFocusFrame 会移到控件的父层。保留局部样式，否则 Windows 会用原生方框绘制。
        for (auto* frame : widget->findChildren<QFocusFrame*>(QString(), Qt::FindDirectChildrenOnly))
            frame->setStyle(this);
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override
    {
        if (element == CE_FocusFrame) {
            const auto* frame = qobject_cast<const QFocusFrame*>(widget);
            const auto* combo = frame ? qobject_cast<const QComboBox*>(frame->widget()) : nullptr;
            if (combo && !combo->isEditable())
                return;
        }
        if (element == CE_PushButtonLabel) {
            if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
                button && button->text.isEmpty() && !button->icon.isNull()) {
                auto iconOnly = *button;
                iconOnly.features.setFlag(QStyleOptionButton::HasMenu, false);
                QlementineStyle::drawControl(element, &iconOnly, painter, widget);
                return;
            }
        }
        QlementineStyle::drawControl(element, option, painter, widget);
        if (element == CE_PushButton) {
            const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (button && !button->features.testFlag(QStyleOptionButton::Flat))
                drawOutline(painter, subElementRect(SE_PushButtonBevel, option, widget), theme().borderColor);
        }
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget) const override
    {
        QlementineStyle::drawComplexControl(control, option, painter, widget);
        if (control == CC_ComboBox) {
            const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
            const bool active = combo && !combo->editable && (option->state & (State_On | State_HasFocus));
            drawOutline(painter, option->rect, active ? theme().focusColor : theme().borderColor);
        } else if (control == CC_SpinBox) {
            drawOutline(painter, option->rect, theme().borderColor);
        }
    }

private:
    void drawOutline(QPainter* painter, const QRect& rect, const QColor& color) const
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(color);
        painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
                                 theme().borderRadius, theme().borderRadius);
        painter->restore();
    }
};
