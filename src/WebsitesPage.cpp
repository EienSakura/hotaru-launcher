#include "WebsitesPage.h"
#include "SettingsIcons.h"
#include "SettingsControlStyle.h"

#include "Theme.h"
#include "Utils.h"
#include "WebShortcuts.h"

#include <QDialog>
#include <QEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFrame>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QMouseEvent>
#include <QUrl>
#include <functional>

namespace {

QStringList splitKeys(const QString& text)
{
    static const QRegularExpression sep(QStringLiteral("[,，;；\\s]+"));
    QStringList out;
    const QStringList parts = text.split(sep, Qt::SkipEmptyParts);
    for (const QString& p : parts)
        out << p.toLower();
    return out;
}

class QuickSearchDropCard final : public QFrame {
public:
    explicit QuickSearchDropCard(QWidget* parent = nullptr) : QFrame(parent)
    {
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumWidth(300);
        setMinimumHeight(72);
        setObjectName(QStringLiteral("settingsDrop"));
        Theme::bindSettings(this, "QFrame#settingsDrop{border:1px dashed @line;border-radius:10px;background:@group;}"
                                  "QFrame#settingsDrop:hover{border-color:@accent;background:@hover;}");
        m_dragTimer.setInterval(90);
        connect(&m_dragTimer, &QTimer::timeout, this, [this] {
            m_dragPhase = (m_dragPhase + 2) % 20;
            update();
        });
    }
    std::function<void()> clicked;
    std::function<void(const QList<QUrl>&)> dropped;
protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && clicked) clicked();
        QFrame::mousePressEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!event->mimeData()->hasUrls()) { event->ignore(); return; }
        setDragFeedback(true);
        event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->mimeData()->hasUrls()) { setDragFeedback(true); event->acceptProposedAction(); }
        else event->ignore();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        setDragFeedback(false);
        event->accept();
    }
    void dropEvent(QDropEvent* event) override
    {
        if (event->mimeData()->hasUrls() && dropped) { dropped(event->mimeData()->urls()); event->acceptProposedAction(); }
        else event->ignore();
        setDragFeedback(false);
    }
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);
        if (!m_dragActive)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(32);
        const QRectF area = rect().adjusted(2, 2, -2, -2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(area, 9, 9);
        QPen pen(accent, 2, Qt::DashLine);
        pen.setDashOffset(-m_dragPhase);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        painter.drawRoundedRect(area, 9, 9);
    }
private:
    void setDragFeedback(bool active)
    {
        if (m_dragActive == active)
            return;
        m_dragActive = active;
        if (active) m_dragTimer.start(); else m_dragTimer.stop();
        update();
    }

    QTimer m_dragTimer;
    int m_dragPhase = 0;
    bool m_dragActive = false;
};

bool entryDialog(QWidget* parent, WebEntry* e)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(e->name.isEmpty() ? QStringLiteral("新增快捷搜索")
                                         : QStringLiteral("编辑快捷搜索"));
    dlg.setObjectName(QStringLiteral("quickSearchDialog"));
    dlg.setMinimumWidth(620);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(24, 22, 24, 20);
    lay->setSpacing(12);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(e->name, &dlg);
    auto* keys = new QLineEdit(e->keys.join(QStringLiteral(", ")), &dlg);
    auto* type = new SettingsComboBox(&dlg);
    type->addItem(QStringLiteral("网页"), QStringLiteral("web"));
    type->addItem(QStringLiteral("应用或脚本"), QStringLiteral("app"));
    type->addItem(QStringLiteral("磁盘文件夹"), QStringLiteral("folder"));
    const int typeIndex = type->findData(e->targetType.isEmpty() ? QStringLiteral("web") : e->targetType);
    type->setCurrentIndex(typeIndex < 0 ? 0 : typeIndex);
    auto* target = new QLineEdit(e->target.isEmpty() ? e->url : e->target, &dlg);
    name->setObjectName(QStringLiteral("quickEdit")); keys->setObjectName(QStringLiteral("quickEdit")); target->setObjectName(QStringLiteral("quickEdit")); type->setObjectName(QStringLiteral("quickType"));
    name->setMinimumHeight(38); keys->setMinimumHeight(38); target->setMinimumHeight(38);
    auto* browse = new QPushButton(QStringLiteral("选择…"), &dlg);
    auto* targetRow = new QWidget(&dlg); auto* targetLay = new QHBoxLayout(targetRow);
    targetLay->setContentsMargins(0,0,0,0); targetLay->addWidget(target,1); targetLay->addWidget(browse);
    form->addRow(QStringLiteral("名称"), name);
    form->addRow(QStringLiteral("关键字"), keys);
    form->addRow(QStringLiteral("目标类型"), type);
    form->addRow(QStringLiteral("目标"), targetRow);
    lay->addLayout(form);

    auto* hint = new QLabel(
        QStringLiteral("搜索框只匹配第一个完整词，回车后直接打开目标。应用和文件夹不会修改源文件。"),
        &dlg);
    hint->setObjectName(QStringLiteral("modalHint"));
    lay->addWidget(hint);

    auto* drop = new QuickSearchDropCard(&dlg);
    drop->setMinimumHeight(58);
    auto* dropLay = new QHBoxLayout(drop);
    dropLay->setContentsMargins(14, 8, 14, 8);
    auto* dropText = new QLabel(QStringLiteral("拖入网页、程序、脚本或磁盘文件夹 · 拖入后自动填写目标"), drop);
    dropText->setObjectName(QStringLiteral("modalHint"));
    dropText->setWordWrap(true);
    dropLay->addWidget(dropText, 1);
    drop->dropped = [type, target, name, keys, dropText](const QList<QUrl>& urls) {
        if (urls.size() != 1) {
            dropText->setText(QStringLiteral("一次只能拖入一个目标"));
            return;
        }
        const QUrl url = urls.first();
        QString targetType;
        QString value;
        QString displayName;
        if (url.isLocalFile()) {
            value = url.toLocalFile();
            const QFileInfo info(value);
            if (info.isDir())
                targetType = QStringLiteral("folder");
            else if (QStringList{QStringLiteral("exe"), QStringLiteral("lnk"), QStringLiteral("bat"), QStringLiteral("cmd")}
                         .contains(info.suffix().toLower()))
                targetType = QStringLiteral("app");
            else {
                dropText->setText(QStringLiteral("不支持的文件类型，请拖入 exe、lnk、bat、cmd 或文件夹"));
                return;
            }
            displayName = info.completeBaseName();
        } else if (url.scheme().startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
            targetType = QStringLiteral("web");
            value = url.toString();
            displayName = url.host();
        } else {
            dropText->setText(QStringLiteral("请拖入网页链接或本地程序、脚本、文件夹"));
            return;
        }
        type->setCurrentIndex(type->findData(targetType));
        target->setText(value);
        if (name->text().trimmed().isEmpty() || name->text() == name->placeholderText())
            name->setText(displayName);
        if (keys->text().trimmed().isEmpty())
            keys->setText(displayName.toLower().replace(QRegularExpression(QStringLiteral("[^a-z0-9一-龥]+")), QString()));
        dropText->setText(QStringLiteral("已填入：%1").arg(value));
    };
    lay->addWidget(drop);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(btns);
    auto* ok = btns->button(QDialogButtonBox::Ok); auto* cancel = btns->button(QDialogButtonBox::Cancel);
    ok->setText(QStringLiteral("保存")); cancel->setText(QStringLiteral("取消"));
    ok->setObjectName(QStringLiteral("quickPrimary")); cancel->setObjectName(QStringLiteral("quickSecondary"));
    auto updateTargetUi = [type,target,browse] {
        const bool web = type->currentData().toString() == QLatin1String("web");
        browse->setVisible(!web); target->setPlaceholderText(web ? QStringLiteral("https://example.com") : QStringLiteral("本地路径"));
    };
    QObject::connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, updateTargetUi);
    QObject::connect(browse, &QPushButton::clicked, &dlg, [&] {
        if (type->currentData().toString() == QLatin1String("folder")) {
            const QString p=QFileDialog::getExistingDirectory(&dlg, QStringLiteral("选择磁盘文件夹")); if (!p.isEmpty()) target->setText(p);
        } else {
            const QString p=QFileDialog::getOpenFileName(&dlg, QStringLiteral("选择程序或脚本"), QDir::rootPath(), QStringLiteral("程序和脚本 (*.exe *.lnk *.bat *.cmd);;所有文件 (*.*)")); if (!p.isEmpty()) target->setText(p);
        }
    });
    updateTargetUi();
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    Theme::bindModal(&dlg);
    if (dlg.exec() != QDialog::Accepted)
        return false;

    e->keys = splitKeys(keys->text());
    e->name = name->text().trimmed();
    e->targetType = type->currentData().toString();
    e->target = target->text().trimmed();
    e->url = e->targetType == QLatin1String("web") ? WebShortcuts::normalizeUrl(e->target) : QString();
    if (e->targetType == QLatin1String("web")) e->target = e->url;
    e->searchUrl.clear();
    return !e->name.isEmpty() && !e->target.isEmpty() && !e->keys.isEmpty();
}

// 结构化网站行组件
class WebRowWidget : public QWidget {
public:
    WebRowWidget(const WebEntry& e, int index, WebsitesPage* page, QWidget* parent = nullptr)
        : QWidget(parent)
    {
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(12, 8, 12, 8);
        lay->setSpacing(12);

        const bool folder = e.targetType == QLatin1String("folder");
        const bool app = e.targetType == QLatin1String("app");
        const QString target = e.target.isEmpty() ? e.url : e.target;
        lay->addWidget(new SettingsIconTile(folder ? SettingsGlyph::Folder
            : app ? SettingsGlyph::Terminal : SettingsGlyph::Search, this));

        auto* labels = new QVBoxLayout;
        labels->setSpacing(3);
        auto* name = new QLabel(e.name, this);
        name->setObjectName(QStringLiteral("settingsRowTitle"));
        name->setTextFormat(Qt::PlainText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name->setToolTip(e.name);
        labels->addWidget(name);
        const QString type = folder ? QStringLiteral("磁盘文件夹")
            : app ? QStringLiteral("应用或脚本") : QStringLiteral("网页");
        auto* description = new QLabel(QStringLiteral("%1 · %2").arg(type, target), this);
        description->setObjectName(QStringLiteral("muted"));
        description->setTextFormat(Qt::PlainText);
        description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        description->setToolTip(target);
        labels->addWidget(description);
        if (!e.searchUrl.isEmpty() && !folder && !app) {
            auto* compatibility = new QLabel(QStringLiteral("兼容模板 · %1").arg(e.searchUrl), this);
            compatibility->setObjectName(QStringLiteral("muted"));
            compatibility->setTextFormat(Qt::PlainText);
            compatibility->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            compatibility->setToolTip(e.searchUrl);
            labels->addWidget(compatibility);
        }
        lay->addLayout(labels, 1);

        const QString keys = e.keys.join(QStringLiteral(" · "));
        auto* keyBadge = new QLabel(keys, this);
        keyBadge->setObjectName(QStringLiteral("settingsBadge"));
        keyBadge->setTextFormat(Qt::PlainText);
        keyBadge->setMaximumWidth(140);
        keyBadge->setToolTip(keys);
        keyBadge->setVisible(!keys.isEmpty());
        lay->addWidget(keyBadge, 0, Qt::AlignVCenter);
        auto* edit = new QPushButton(QStringLiteral("编辑"), this);
        edit->setAccessibleName(QStringLiteral("编辑 %1").arg(e.name));
        connect(edit, &QPushButton::clicked, this, [page, index] { page->editEntryAt(index); });
        lay->addWidget(edit);
        auto* more = new QPushButton(this);
        more->setObjectName(QStringLiteral("quickMore"));
        more->setIcon(settingsIcon(SettingsGlyph::More, QColor(SettingsPalette::current().text)));
        more->setIconSize(QSize(16, 16));
        more->setFixedWidth(34);
        more->setAccessibleName(QStringLiteral("%1的更多操作").arg(e.name));
        auto* menu = new QMenu(more);
        Theme::bindMenu(menu);
        menu->addAction(QStringLiteral("删除"), page, [page, index] { page->removeEntryAt(index); });
        more->setMenu(menu);
        lay->addWidget(more);
    }
};

} // namespace

WebsitesPage::WebsitesPage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(30, 20, 30, 18);
    root->setSpacing(10);

    // 顶栏：标题说明与右上角新增按钮
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(10);

    auto* titleCol = new QVBoxLayout;
    titleCol->setSpacing(3);
    auto* heading = new QLabel(QStringLiteral("快捷搜索"), this);
    heading->setObjectName(QStringLiteral("pageTitle"));
    auto* tip = new QLabel(QStringLiteral("输入关键字后回车，直接打开网页、应用或磁盘文件夹；只匹配第一个完整词。"),
                           this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    titleCol->addWidget(heading);
    titleCol->addWidget(tip);
    topRow->addLayout(titleCol, 1);

    auto* addButton = new QPushButton(QStringLiteral("新增快捷搜索…"), this);
    addButton->setObjectName(QStringLiteral("quickAddButton"));
    addButton->setCursor(Qt::PointingHandCursor);
    addButton->setMinimumHeight(30);
    addButton->setMinimumWidth(124);
    addButton->setProperty("settingsPrimary", true);
    addButton->setIcon(settingsIcon(SettingsGlyph::Plus, QColor(SettingsPalette::current().onPrimary)));
    connect(addButton, &QPushButton::clicked, this, &WebsitesPage::addEntry);
    topRow->addWidget(addButton, 0, Qt::AlignVCenter);
    root->addLayout(topRow);

    auto* listTitle = new QLabel(QStringLiteral("搜索入口"), this);
    listTitle->setObjectName(QStringLiteral("settingsRowTitle"));
    root->addWidget(listTitle);

    // 列表主体
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("webList"));
    m_list->setSpacing(0);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_list, 1);
    root->addStretch(1);

    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (it && it->data(Qt::UserRole).isValid())
            editEntryAt(it->data(Qt::UserRole).toInt());
    });

    rebuild();
}

void WebsitesPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::ThemeChange)
        return;
    const auto colors = SettingsPalette::current();
    if (auto* add = findChild<QPushButton*>(QStringLiteral("quickAddButton")))
        add->setIcon(settingsIcon(SettingsGlyph::Plus, QColor(colors.onPrimary)));
    const auto moreIcon = settingsIcon(SettingsGlyph::More, QColor(colors.text));
    for (auto* more : findChildren<QPushButton*>(QStringLiteral("quickMore")))
        more->setIcon(moreIcon);
}

void WebsitesPage::rebuild()
{
    m_list->clear();
    const auto webs = WebShortcuts::loadWebs();
    int height = 2;
    if (webs.isEmpty()) {
        auto* placeholder = new QListWidgetItem(QStringLiteral("暂无快捷搜索。可以添加网页、应用、脚本或文件夹。"), m_list);
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setSizeHint(QSize(0, 44));
    }
    for (int i = 0; i < webs.size(); ++i) {
        const WebEntry& e = webs[i];
        const int rowHeight = e.searchUrl.isEmpty() ? 58 : 74;
        height += rowHeight;
        auto* it = new QListWidgetItem(m_list);
        it->setData(Qt::UserRole, i);
        it->setSizeHint(QSize(0, rowHeight));
        auto* w = new WebRowWidget(e, i, this, m_list);
        m_list->setItemWidget(it, w);
    }
    m_list->setMaximumHeight(webs.isEmpty() ? 48 : height);
}

void WebsitesPage::editEntryNamed(const QString& name)
{
    const auto entries = WebShortcuts::loadWebs();
    for (int i = 0; i < entries.size(); ++i)
        if (entries[i].name.compare(name, Qt::CaseInsensitive) == 0) {
            editEntryAt(i);
            return;
        }
}

void WebsitesPage::addEntry()
{
    WebEntry e;
    if (!entryDialog(this, &e))
        return;
    auto webs = WebShortcuts::loadWebs();
    for (const auto& old : webs)
        for (const auto& key : e.keys)
            if (old.keys.contains(key)) {
                QMessageBox::warning(this, QStringLiteral("关键字重复"), QStringLiteral("关键字“%1”已经被“%2”使用。").arg(key, old.name));
                return;
            }
    webs.append(e);
    WebShortcuts::saveWebs(webs);
    Utils::log(QString("webs: 新增 %1 (%2)").arg(e.name, e.keys.join(",")));
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void WebsitesPage::addEntryFromDrop(const QString& target, const QString& targetType)
{
    WebEntry e;
    e.targetType = targetType;
    e.target = target;
    e.url = targetType == QLatin1String("web") ? WebShortcuts::normalizeUrl(target) : QString();
    if (targetType == QLatin1String("web")) e.target = e.url;
    e.name = targetType == QLatin1String("web") ? QUrl(target).host() : QFileInfo(target).completeBaseName();
    if (e.name.isEmpty()) e.name = target;
    e.keys = { e.name.toLower().replace(QRegularExpression(QStringLiteral("[^a-z0-9一-龥]+")), QString()) };
    if (e.keys.first().isEmpty()) e.keys = { QStringLiteral("shortcut") };
    if (!entryDialog(this, &e)) return;
    auto entries = WebShortcuts::loadWebs();
    for (const auto& old : entries)
        for (const auto& key : e.keys)
            if (old.keys.contains(key)) {
                QMessageBox::warning(this, QStringLiteral("关键字重复"), QStringLiteral("关键字“%1”已经被“%2”使用。").arg(key, old.name));
                return;
            }
    entries.append(e);
    WebShortcuts::saveWebs(entries);
    rebuild();
}

void WebsitesPage::editEntryAt(int index)
{
    auto webs = WebShortcuts::loadWebs();
    if (index < 0 || index >= webs.size())
        return;
    WebEntry e = webs[index];
    if (!entryDialog(this, &e))
        return;
    for (int i = 0; i < webs.size(); ++i)
        if (i != index)
            for (const auto& key : e.keys)
                if (webs[i].keys.contains(key)) {
                    QMessageBox::warning(this, QStringLiteral("关键字重复"), QStringLiteral("关键字“%1”已经被“%2”使用。").arg(key, webs[i].name));
                    return;
                }
    webs[index] = e;
    WebShortcuts::saveWebs(webs);
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void WebsitesPage::removeEntryAt(int index)
{
    auto webs = WebShortcuts::loadWebs();
    if (index < 0 || index >= webs.size())
        return;
    if (QMessageBox::question(this, QStringLiteral("删除快捷搜索"),
                              QStringLiteral("确定删掉 %1？").arg(webs[index].name))
        != QMessageBox::Yes)
        return;
    webs.removeAt(index);
    WebShortcuts::saveWebs(webs);
    QTimer::singleShot(0, this, [this] { rebuild(); });
}
