#pragma once

#include <QWidget>

class QListWidget;
class QPushButton;

// 配置 - 快捷搜索页：网页、应用和磁盘文件夹关键字规则（存 quick-search.json）
class WebsitesPage : public QWidget {
    Q_OBJECT
public:
    explicit WebsitesPage(QWidget* parent = nullptr);
    void editEntryNamed(const QString& name);

    void editEntryAt(int index);
    void removeEntryAt(int index);

private:
    void rebuild();
    void addEntry();
    void addEntryFromDrop(const QString& target, const QString& targetType);

    QListWidget* m_list = nullptr;
};
