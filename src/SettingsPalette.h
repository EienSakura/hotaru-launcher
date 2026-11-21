#pragma once

#include "Theme.h"
#include <QHash>
#include <QRegularExpression>

// 设置窗的设计稿配色。标准弹窗和启动器菜单继续各自解析主题角色。
struct SettingsPalette {
    const char* window;
    const char* rail;
    const char* group;
    const char* text;
    const char* muted;
    const char* line;
    const char* field;
    const char* hover;
    const char* selected;
    const char* accent;
    const char* primary;
    const char* onPrimary;

    static SettingsPalette current()
    {
        if (Theme::current() == Theme::Mode::Dark)
            return {"#202329", "#1b1e23", "#282c33", "#f1f3f7", "#b5becd", "#3d4653",
                    "#303641", "#343d4b", "#303f5a", "#b5cdff", "#b1caff", "#192a49"};
        return {"#f8f9fb", "#f0f2f6", "#ffffff", "#222630", "#535d6e", "#dce0e8",
                "#f5f6f9", "#edf0f7", "#e5edfd", "#2b56af", "#325ec0", "#ffffff"};
    }

    QString expand(const char* stylesheet) const
    {
        const QHash<QString, QString> colors = {
            {"window", window}, {"rail", rail}, {"group", group}, {"text", text},
            {"muted", muted}, {"line", line}, {"field", field}, {"hover", hover},
            {"selBg", selected}, {"accent", accent}, {"primary", primary},
            {"onPrimary", onPrimary}, {"button", field}, {"border", line}, {"borderHi", line},
            {"pressed", selected}, {"groupText", text}, {"edit", field}, {"editText", text}
        };
        const QString source = QString::fromLatin1(stylesheet);
        static const QRegularExpression token(QStringLiteral("@([a-zA-Z][a-zA-Z0-9]*)"));
        QString qss;
        auto matches = token.globalMatch(source);
        qsizetype cursor = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            qss += QStringView(source).mid(cursor, match.capturedStart() - cursor);
            qss += colors.value(match.captured(1), match.captured());
            cursor = match.capturedEnd();
        }
        qss += QStringView(source).mid(cursor);
        return Theme::expand(qss);
    }
};
