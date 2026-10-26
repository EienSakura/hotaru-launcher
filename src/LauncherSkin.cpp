#include "LauncherSkin.h"
#include <QLabel>
#include <QMovie>
#include <QPainter>

namespace {
class DecorationLabel : public QLabel {
public:
    DecorationLabel(const QRect& source,QWidget* parent):QLabel(parent),m_source(source) {}
protected:
    void paintEvent(QPaintEvent*) override {
        const QImage image=movie()?movie()->currentImage():pixmap().toImage();
        if (image.isNull()) return;
        QPainter painter(this);painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect(),image,m_source.isEmpty()?image.rect():m_source);
    }
private:
    QRect m_source;
};
// 中间区拉伸，四角维持原尺寸，搜索区和内容区使用同一坐标变换。
int stretchCoordinate(int value, int original, int target, int first, int last)
{
    if (value <= first) return value;
    if (value >= original - last) return target - (original - value);
    return first + qRound(qreal(value - first) * (target - first - last) / (original - first - last));
}

bool searchDecoration(const Theme::Decoration& decoration)
{
    // 仅搜索条收起状态隐藏底部和内容区装饰；结果展开后恢复整套装饰。
    return decoration.anchor.startsWith(QLatin1String("top"))
        && decoration.reference != QLatin1String("content");
}
}

LauncherSkin::LauncherSkin(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("hotaruSkin"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    if (parent) {
        m_contentDecorations=new QWidget(parent);
        m_contentDecorations->setObjectName(QStringLiteral("hotaruContentDecorations"));
        m_contentDecorations->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_contentDecorations->setAttribute(Qt::WA_NoSystemBackground);
    }
    reload();
}

LauncherSkin::~LauncherSkin()
{
    delete m_contentDecorations.data();
}

void LauncherSkin::reload()
{
    for (auto* label : m_decorations) {
        if (auto* movie = label->movie()) {movie->stop();delete movie;}
        delete label;
    }
    m_decorations.clear();
    m_surface = Theme::currentPackage().surface;
    m_frame = m_surface.enabled() ? QPixmap(Theme::assetPath(m_surface.frame)) : QPixmap();
    for (const auto& decoration : m_surface.decorations) {
        auto* label = new DecorationLabel(decoration.sourceRect,
            decoration.allowContentOverlap && m_contentDecorations ? m_contentDecorations.data() : this);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setStyleSheet(QStringLiteral("background:transparent;border:none;"));
        const QString path = Theme::assetPath(decoration.image);
        if (path.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
            // 动画仍归皮肤所有，覆盖层仅负责绘制，切换时统一停止释放。
            auto* movie = new QMovie(path, QByteArray(), this);
            movie->setCacheMode(QMovie::CacheAll);
            label->setMovie(movie);
            movie->start();
        } else {
            label->setPixmap(QPixmap(path));
        }
        m_decorations << label;
        label->show();
    }
    setSurfaceVisible(m_surfaceVisible);
    resizeEvent(nullptr);
    update();
}

void LauncherSkin::setSurfaceVisible(bool visible)
{
    m_surfaceVisible = visible;
    setVisible(active() && visible);
    if (m_contentDecorations) m_contentDecorations->setVisible(active() && visible);
    for (int i = 0; i < m_decorations.size(); ++i) {
        const bool show = visible && (!m_compact || m_hasResults || searchDecoration(m_surface.decorations[i]));
        auto* label = m_decorations[i];
        label->setVisible(show);
        if (auto* movie = label->movie()) movie->setPaused(!show);
    }
}

void LauncherSkin::setCompact(bool compact, bool hasResults)
{
    if (m_compact == compact && m_hasResults == hasResults) return;
    m_compact = compact;
    m_hasResults = hasResults;
    setSurfaceVisible(m_surfaceVisible);
    resizeEvent(nullptr);
    update();
}

QRect LauncherSkin::compactSearchRect() const
{
    QRect area = searchRect();
    // 垂直收拢只移除中间网格，搜索区沿用原始顶部坐标和高度。
    area.moveTop(m_surface.searchRect.top());
    return area;
}

QRect LauncherSkin::compactContentRect() const
{
    const QRect mapped = contentRect();
    return QRect(mapped.x(), qMax(m_surface.contentRect.top(), compactSearchRect().bottom()+13),
                 mapped.width(), 0);
}

int LauncherSkin::compactFrameHeight() const
{
    // 搜索条只保留原图顶部；角色高度不能把中段和底框重新带出来。
    return qMax(m_surface.frameMargins.top(),
        m_surface.searchRect.bottom()+1-m_surface.framePadding.top());
}

int LauncherSkin::compactHeight(int resultHeight) const
{
    const auto& p = m_surface.framePadding;
    const auto& m = m_surface.frameMargins;
    int bottom = resultHeight > 0 ? compactContentRect().top()+resultHeight
                                  : compactSearchRect().bottom()+1;
    for (const auto& decoration : m_surface.decorations) {
        if (!searchDecoration(decoration)) continue;
        const int top = decoration.offset.y() + (decoration.reference==QLatin1String("frame") ? p.top() : 0);
        bottom = qMax(bottom, top+decoration.size.height());
    }
    if (resultHeight <= 0) return qMax(p.top()+compactFrameHeight(), bottom);
    // 只有结果区展开时才需要中段和底边。
    return qMax(p.top()+m.top(), bottom)+8+m.bottom()+p.bottom();
}

QRect LauncherSkin::mapDesignRect(const QRect& rect) const
{
    if (!active()) return rect;
    const auto& p = m_surface.framePadding;
    const auto& m = m_surface.frameMargins;
    const auto x = [&](int value) { return stretchCoordinate(value, m_surface.canvasSize.width(), width(),
                                                            p.left() + m.left(), p.right() + m.right()); };
    const auto y = [&](int value) { return stretchCoordinate(value, m_surface.canvasSize.height(), height(),
                                                            p.top() + m.top(), p.bottom() + m.bottom()); };
    return QRect(x(rect.x()), y(rect.y()), x(rect.x() + rect.width()) - x(rect.x()),
                 y(rect.y() + rect.height()) - y(rect.y()));
}

QRect LauncherSkin::searchRect() const
{
    QRect area=mapDesignRect(m_surface.searchRect);
    area.setHeight(m_surface.searchRect.height());
    return area;
}

QRect LauncherSkin::footerRect() const
{
    if (!m_surface.footerRect.isEmpty()) return mapDesignRect(m_surface.footerRect);
    // 旧包没有页脚约定时使用独立的窗口内安全区，不再把齿轮压在内容下边缘。
    const QRect content=contentRect();
    return QRect(content.x(),qMin(content.bottom()+5,height()-50),content.width(),40);
}

QRect LauncherSkin::settingsRect() const
{
    if (!m_surface.settingsRect.isEmpty()) {
        const QRect mapped=mapDesignRect(m_surface.settingsRect);
        const int side=m_surface.settingsRect.width();
        return QRect(mapped.right()-side+1,mapped.bottom()-side+1,side,side);
    }
    const QRect footer=footerRect();
    return QRect(footer.right()-39,footer.bottom()-39,40,40);
}

QRect LauncherSkin::decorationRect(int index) const
{
    const auto& d=m_surface.decorations[index];
    QRect reference=rect();
    if (d.reference==QLatin1String("frame")) reference=rect().marginsRemoved(m_surface.framePadding);
    if (d.reference==QLatin1String("content")) reference=contentRect();
    QPoint pos=reference.topLeft()+d.offset;
    if (d.anchor.endsWith(QLatin1String("right"))) pos.rx()+=reference.width()-d.size.width();
    if (d.anchor.startsWith(QLatin1String("bottom"))) pos.ry()+=reference.height()-d.size.height();
    return QRect(pos,d.size);
}

void LauncherSkin::resizeEvent(QResizeEvent*)
{
    if (m_contentDecorations) {
        m_contentDecorations->setGeometry(geometry());
        m_contentDecorations->raise();
    }
    for (int i = 0; i < m_decorations.size(); ++i) {
        m_decorations[i]->setGeometry(decorationRect(i));
    }
}

void LauncherSkin::paintEvent(QPaintEvent*)
{
    if (m_frame.isNull()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect dest = rect().marginsRemoved(m_surface.framePadding);
    const auto& m = m_surface.frameMargins;
    const int sx[] = {0, m.left(), m_frame.width() - m.right(), m_frame.width()};
    const int sy[] = {0, m.top(), m_frame.height() - m.bottom(), m_frame.height()};
    const int dx[] = {dest.x(), dest.x() + m.left(), dest.x() + dest.width() - m.right(), dest.x() + dest.width()};
    if (m_compact && !m_hasResults) {
        const int topHeight = qMin(compactFrameHeight(), m_frame.height());
        for (int x = 0; x < 3; ++x)
            painter.drawPixmap(QRect(dx[x],dest.y(),dx[x+1]-dx[x],topHeight), m_frame,
                               QRect(sx[x],0,sx[x+1]-sx[x],topHeight));
        return;
    }
    const int dy[] = {dest.y(), dest.y() + m.top(), dest.y() + dest.height() - m.bottom(), dest.y() + dest.height()};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            painter.drawPixmap(QRect(dx[x], dy[y], dx[x+1]-dx[x], dy[y+1]-dy[y]), m_frame,
                               QRect(sx[x], sy[y], sx[x+1]-sx[x], sy[y+1]-sy[y]));
}
