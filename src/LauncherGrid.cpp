#include "LauncherGrid.h"
#include "LauncherTheme.h"
#include "Utils.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QWheelEvent>
#include <cmath>

LauncherGrid::LauncherGrid(QWidget* parent):QWidget(parent)
{
    setObjectName("hotaruGrid");setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);
    m_animation.setDuration(160);m_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant& value){m_settling=value.toPointF();update();});
    connect(&m_animation,&QVariantAnimation::finished,this,[this]{m_settleIndex=-1;update();});
    m_pageAnimation.setDuration(200);m_pageAnimation.setStartValue(0.);m_pageAnimation.setEndValue(1.);
    m_pageAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_pageAnimation,&QVariantAnimation::valueChanged,this,[this](const QVariant& v){m_pageProgress=v.toReal();update();});
    connect(&m_pageAnimation,&QVariantAnimation::finished,this,[this]{m_oldPage=QPixmap();update();});
    m_mergeTimer.setSingleShot(true);m_mergeTimer.setInterval(500);
    connect(&m_mergeTimer,&QTimer::timeout,this,[this]{if (m_dragging && m_mergeTarget>=0) {m_mergeReady=true;setCursor(Qt::ClosedHandCursor);update();}});
    m_exitTimer.setSingleShot(true);m_exitTimer.setInterval(350);
    connect(&m_exitTimer,&QTimer::timeout,this,[this] {
        if (!m_dragging || !m_groupMode || !parentWidget()
            || parentWidget()->rect().contains(mapToParent(m_mousePos))) return;
        const auto id=m_items[m_dragIndex].app.id;const auto global=mapToGlobal(m_mousePos);
        stopDrag();emit transferRequested(id,global);
    });
    m_pageTimer.setInterval(650);
    connect(&m_pageTimer,&QTimer::timeout,this,[this] {
        if (!m_dragging || !m_edgeDirection) return;
        const int page=m_page+m_edgeDirection;
        if (page<0 || page>9999/m_rows) return;
        if (page>=pageCount()) m_extraPage=page;
        turnPage(page);
    });
}
void LauncherGrid::setItems(const QVector<Item>& items,int columns,int rows,int pages)
{
    cancelDrag();m_animation.stop();m_settleIndex=-1;m_items=items;
    m_columns=qBound(m_groupMode ? 3 : 4,columns,8);m_selected=-1;
    m_rows=qBound(2,rows,6);m_pages=qMax(1,pages);m_extraPage=-1;m_page=qMin(m_page,pageCount()-1);
    arrange();loadIcons();
    emit pageChanged(m_page,pageCount());
}
int LauncherGrid::pageCount() const
{
    int count=m_pages;
    for (int i=0;i<m_items.size();++i) if (!m_external || i!=m_dragIndex) count=qMax(count,m_items[i].app.gridRow/m_rows+1);
    return qMax(count,m_extraPage+1);
}
void LauncherGrid::turnPage(int page)
{
    const int next=qBound(0,page,pageCount()-1);
    if (next!=m_page && isVisible()) {
        m_pageAnimation.stop();m_oldPage=QPixmap();m_pageProgress=1;
        m_snapshot=true;m_oldPage=grab();m_snapshot=false;
        m_pageDirection=next>m_page?1:-1;m_pageProgress=0;m_pageAnimation.start();
    }
    m_page=next;m_hovered=m_selected=-1;
    m_mergeTimer.stop();m_mergeTarget=-1;m_mergeReady=false;
    m_animation.stop();m_settleIndex=-1;update();emit pageChanged(m_page,pageCount());
}
void LauncherGrid::setPage(int page,bool allowEmpty)
{
    cancelDrag();
    if (allowEmpty && page==pageCount() && page<=9999/m_rows) m_extraPage=page;
    turnPage(page);
}
void LauncherGrid::loadIcons()
{
    const auto generation=++m_iconGeneration;
    const qreal dpr=devicePixelRatioF();
    for (int i=0;i<m_items.size();++i) {
        const auto& app=m_items[i].app;
        if (!app.enabled) continue;
        if (m_items[i].group) {
            m_items[i].covers.clear();
            int cover=0;
            for (const auto& child:m_items[i].children) {
                if (!child.enabled || cover>=4) continue;
                const int slot=cover++;
                m_items[i].covers.append(Utils::letterIcon(child.title,Theme::launcherColor("accent")));
                Utils::requestFileIcon(child.iconPath.isEmpty()?child.targetPath:child.iconPath,qCeil(24*dpr),this,
                    [this,i,slot,generation,dpr](const QImage& image) {
                        if (generation!=m_iconGeneration || i>=m_items.size() || slot>=m_items[i].covers.size() || image.isNull()) return;
                        auto pix=QPixmap::fromImage(image);pix.setDevicePixelRatio(dpr);m_items[i].covers[slot]=QIcon(pix);update();
                    });
            }
            continue;
        }
        Utils::requestFileIcon(app.iconPath.isEmpty()?app.targetPath:app.iconPath,qCeil(48*dpr),this,
            [this,i,generation,dpr](const QImage& image) {
                if (generation!=m_iconGeneration || i>=m_items.size() || image.isNull()) return;
                auto pix=QPixmap::fromImage(image);pix.setDevicePixelRatio(dpr);m_items[i].icon=QIcon(pix);update();
            });
    }
}
QStringList LauncherGrid::itemIds() const
{
    QStringList out;for (const auto& item:m_items) if (item.app.enabled && item.app.gridRow/m_rows==m_page) out<<item.app.id;return out;
}
QRect LauncherGrid::cellRect(int row,int column) const
{
    // 按共享边界分配余数，绘制和命中共用此矩形，避免误差累计到最后一列。
    const int availableWidth=qMax(m_columns,width()-16),availableHeight=qMax(m_rows,height()-16);
    const int localRow=row-m_page*m_rows;
    const int left=column*availableWidth/m_columns,right=(column+1)*availableWidth/m_columns;
    const int top=localRow*availableHeight/m_rows,bottom=(localRow+1)*availableHeight/m_rows;
    return {8+left,8+top,right-left,bottom-top};
}
QRect LauncherGrid::itemRect(int index) const
{
    if (index>=0 && index<m_items.size()) {
        const auto& app=m_items[index].app;return cellRect(app.gridRow,app.gridColumn);
    }
    return cellRect(index/m_columns,index%m_columns);
}
QPoint LauncherGrid::cellAt(const QPoint& pos) const
{
    if (!rect().contains(pos)) return {-1,-1};
    for (int row=m_page*m_rows;row<(m_page+1)*m_rows;++row)
        for (int column=0;column<m_columns;++column)
            if (cellRect(row,column).contains(pos)) return {column,row};
    return {-1,-1};
}
int LauncherGrid::occupant(const QPoint& cell) const
{
    for (int i=0;i<m_items.size();++i) if ((!m_external || i!=m_dragIndex) && m_items[i].app.gridRow==cell.y() && m_items[i].app.gridColumn==cell.x()) return i;
    return -1;
}
int LauncherGrid::indexAt(const QPoint& pos) const
{
    const int i=occupant(cellAt(pos));return i>=0 && m_items[i].app.enabled?i:-1;
}
void LauncherGrid::arrange()
{
    setMinimumSize(0,0);update();
}
void LauncherGrid::drawItem(QPainter& p,int index,const QRectF& area,bool floating)
{
    p.save();p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (floating) p.setOpacity(.92);
    if (floating || index==m_hovered || (hasFocus() && index==m_selected)) {
        p.setPen(Qt::NoPen);p.setBrush(Theme::launcherColor("rowSel"));p.drawRoundedRect(area.adjusted(3,2,-3,-2),7,7);
    }
    const int iconSize=qBound(16,int(area.height())-34,48);
    const int top=qMax(2,(int(area.height())-iconSize-28)/2);
    const QRect icon(int(area.center().x())-iconSize/2,int(area.y())+top,iconSize,iconSize);
    const auto& item=m_items[index];
    if (item.group) {
        p.setPen(Qt::NoPen);p.setBrush(Theme::launcherColor("rowSel"));p.drawRoundedRect(icon,10,10);
        const int small=(iconSize-10)/2;
        for (int i=0;i<item.covers.size() && i<4;++i)
            item.covers[i].paint(&p,QRect(icon.x()+4+(i%2)*(small+2),icon.y()+4+(i/2)*(small+2),small,small));
    } else item.icon.paint(&p,icon,Qt::AlignCenter,item.available?QIcon::Normal:QIcon::Disabled);
    p.setPen(Theme::launcherColor(item.available?"text":"warn"));
    QFont f=font();f.setPointSizeF(9.5);p.setFont(f);
    const QRectF text(area.x()+4,icon.bottom()+5,area.width()-8,20);
    p.drawText(text,Qt::AlignCenter,p.fontMetrics().elidedText(item.app.title,Qt::ElideRight,int(text.width())));
    p.restore();
}
void LauncherGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
    p.save();
    if (!m_oldPage.isNull()) p.translate(m_pageDirection*(1-m_pageProgress)*width(),0);
    if (m_dragging && !m_snapshot) {
        const QPoint target=cellAt(m_mousePos);const int occupied=occupant(target);
        const int first=m_page*m_rows;
        const int last=first+m_rows;
        for (int row=first;row<last;++row) for (int col=0;col<m_columns;++col) {
            const QRect area=cellRect(row,col).adjusted(3,2,-3,-2);
            const QPoint cell(col,row);const int resident=occupant(cell);
            QColor color=Theme::launcherColor("muted");color.setAlpha(90);
            p.setBrush(Qt::NoBrush);p.setPen(QPen(color,1,Qt::DashLine));p.drawRoundedRect(area,5,5);
            if (resident>=0 && !m_items[resident].app.enabled) {
                p.setPen(Theme::launcherColor("muted"));p.drawText(area,Qt::AlignCenter,QStringLiteral("隐藏项保留"));
            }
            if (cell==target) {
                QColor highlight=Theme::launcherColor(occupied<0 || occupied==m_dragIndex || m_mergeReady?"accent":"warn");
                p.setPen(QPen(highlight,2,Qt::DashLine));highlight.setAlpha(35);p.setBrush(highlight);p.drawRoundedRect(area,5,5);
            }
        }
    }
    for (int i=0;i<m_items.size();++i) {
        if (!m_items[i].app.enabled || m_items[i].app.gridRow/m_rows!=m_page || (m_dragging && i==m_dragIndex)) continue;
        QRectF area=itemRect(i);if (i==m_settleIndex) area.moveTopLeft(m_settling);
        drawItem(p,i,area);
    }
    if (m_mergeReady && m_mergeTarget>=0 && !m_snapshot) {
        p.setBrush(QColor(70,140,230,65));p.setPen(QPen(Theme::launcherColor("accent"),2));
        p.drawRoundedRect(itemRect(m_mergeTarget).adjusted(5,4,-5,-4),12,12);
        p.drawText(itemRect(m_mergeTarget).adjusted(0,0,0,-2),Qt::AlignBottom|Qt::AlignHCenter,QStringLiteral("松开加入文件夹"));
    }
    p.restore();
    if (!m_oldPage.isNull()) {
        p.save();p.setOpacity(1-m_pageProgress);p.drawPixmap(QPointF(-m_pageDirection*m_pageProgress*width(),0),m_oldPage);p.restore();
    }
    if (m_dragging && !m_snapshot) drawItem(p,m_dragIndex,QRectF(m_mousePos-m_dragOffset,itemRect(m_dragIndex).size()),true);
    if (m_dragging && !m_snapshot) {
        p.setPen(Theme::launcherColor("text"));p.drawText(QRect(0,height()-25,width(),22),Qt::AlignCenter,QStringLiteral("第 %1 页%2").arg(m_page+1).arg(m_page>=m_pages?QStringLiteral(" · 松开后创建"):QString()));
    }
    if (itemIds().isEmpty() && !m_dragging) {
        p.setPen(Theme::launcherColor("muted"));p.drawText(QRect(8,8,width()-16,100),Qt::AlignCenter,QStringLiteral("右键添加软件或文件夹"));
    }
}
void LauncherGrid::resizeEvent(QResizeEvent* e)
{
    if (m_dragging && window()->size()!=m_dragWindowSize) cancelDrag();
    if (!m_dragging) arrange();
}
void LauncherGrid::mousePressEvent(QMouseEvent* e)
{
    if (e->button()!=Qt::LeftButton) return;
    m_animation.stop();m_settleIndex=-1;setFocus();m_pressed=indexAt(e->pos());m_selected=m_pressed;m_pressPos=m_mousePos=e->pos();update();
}
void LauncherGrid::mouseMoveEvent(QMouseEvent* e)
{
    m_mousePos=e->pos();m_hovered=indexAt(e->pos());
    if (!m_dragging && m_pressed>=0 && (e->buttons()&Qt::LeftButton)
        && (e->pos()-m_pressPos).manhattanLength()>=QApplication::startDragDistance()) {
        m_dragging=true;m_dragPage=m_page;m_dragWindowSize=window()->size();m_dragIndex=m_pressed;m_dragOffset=m_pressPos-itemRect(m_pressed).topLeft();grabMouse();
    }
    if (m_dragging) {
        updateDrag(e->pos());
    } else {
        setCursor(m_hovered>=0?Qt::PointingHandCursor:Qt::ArrowCursor);
        setToolTip(m_hovered>=0?m_items[m_hovered].app.title+"\n"+m_items[m_hovered].app.targetPath:QString());
    }
    update();
}
void LauncherGrid::updateDrag(const QPoint& pos)
{
    m_mousePos=pos;
    const int direction=rect().contains(pos)?(pos.x()<20?-1:pos.x()>width()-20?1:0):0;
    if (direction!=m_edgeDirection) {m_edgeDirection=direction;if (direction) m_pageTimer.start();else m_pageTimer.stop();}
    const bool outsidePanel=parentWidget() && !parentWidget()->rect().contains(mapToParent(pos));
    if (m_groupMode && outsidePanel) {if (!m_exitTimer.isActive()) m_exitTimer.start();}
    else m_exitTimer.stop();
    const auto cell=cellAt(pos);const int occupied=occupant(cell);
    int candidate=-1;
    if (!m_groupMode && !m_items[m_dragIndex].group && occupied>=0 && occupied!=m_dragIndex && m_items[occupied].app.enabled) {
        const QRect area=itemRect(occupied);
        if (area.adjusted(area.width()/4,area.height()/4,-area.width()/4,-area.height()/4).contains(pos)) candidate=occupied;
    }
    if (candidate!=m_mergeTarget) {
        m_mergeTarget=candidate;m_mergeReady=false;m_mergeTimer.stop();if (candidate>=0) m_mergeTimer.start();
    }
    setCursor(cell.x()<0 || (occupied>=0 && occupied!=m_dragIndex && !m_mergeReady)?Qt::ForbiddenCursor:Qt::ClosedHandCursor);
    update();
}
void LauncherGrid::beginExternalDrag(const Item& item,const QPoint& globalPos)
{
    cancelDrag();m_animation.stop();m_external=true;m_items.append(item);m_dragIndex=m_items.size()-1;
    loadIcons();
    m_dragging=true;m_dragPage=m_page;m_dragWindowSize=window()->size();m_dragOffset=QPoint(cellRect(0,0).width()/2,cellRect(0,0).height()/2);
    grabMouse();updateDrag(mapFromGlobal(globalPos));
}
void LauncherGrid::stopDrag()
{
    m_dragging=false;m_pageTimer.stop();m_mergeTimer.stop();m_exitTimer.stop();
    m_mergeReady=false;m_mergeTarget=-1;m_edgeDirection=0;m_pressed=-1;releaseMouse();unsetCursor();
}
void LauncherGrid::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button()!=Qt::LeftButton) return;
    if (m_dragging) {
        updateDrag(e->pos());
        const auto cell=cellAt(e->pos());const int occupied=occupant(cell);
        if (m_mergeReady && occupied==m_mergeTarget && m_mergeTarget>=0) {
            const QString source=m_items[m_dragIndex].app.id,target=m_items[m_mergeTarget].app.id;
            const bool external=m_external;stopDrag();if (external) {m_items.removeLast();m_external=false;}
            emit mergeRequested(source,target);return;
        }
        const bool valid=cell.x()>=0 && (occupied<0 || occupied==m_dragIndex)
            && parentWidget() && parentWidget()->rect().contains(mapToParent(e->pos()));
        if (!valid) {cancelDrag();return;}
        const int index=m_dragIndex;const QString id=m_items[index].app.id;
        const bool changed=m_external || m_items[index].app.gridRow!=cell.y() || m_items[index].app.gridColumn!=cell.x();
        stopDrag();
        m_items[index].app.gridRow=cell.y();m_items[index].app.gridColumn=cell.x();
        m_pages=qMax(m_pages,m_page+1);m_extraPage=-1;
        m_settleIndex=index;m_animation.setStartValue(QPointF(e->pos()-m_dragOffset));
        m_animation.setEndValue(QPointF(itemRect(index).topLeft()));m_animation.start();
        m_external=false;
        if (changed) emit moveRequested(id,cell.y(),cell.x());
        emit pageChanged(m_page,pageCount());
    } else {
        const int clicked=m_pressed;m_pressed=-1;
        if (clicked>=0 && clicked==indexAt(e->pos())) emit activated(m_items[clicked].app.id);
    }
    update();
}
void LauncherGrid::cancelDrag()
{
    if (m_dragging) {
        stopDrag();
        if (m_external) {m_items.removeLast();m_external=false;}
        m_extraPage=-1;turnPage(m_dragPage);emit dragCanceled();
    }
    m_pressed=-1;update();
}
void LauncherGrid::leaveEvent(QEvent*) {m_hovered=-1;update();}
void LauncherGrid::hideEvent(QHideEvent*) {cancelDrag();}
bool LauncherGrid::event(QEvent* e)
{
    if (e->type()==QEvent::UngrabMouse && m_dragging) cancelDrag();
    if (e->type()==QEvent::DevicePixelRatioChange) loadIcons();
    return QWidget::event(e);
}
void LauncherGrid::keyPressEvent(QKeyEvent* e)
{
    if (e->key()==Qt::Key_Escape && m_dragging) {cancelDrag();return;}
    if (m_dragging) return;
    if (e->key()==Qt::Key_F2 && m_selected>=0) {emit renameRequested(m_items[m_selected].app.id);return;}
    if (e->key()==Qt::Key_PageDown || e->key()==Qt::Key_PageUp) {setPage(m_page+(e->key()==Qt::Key_PageDown?1:-1));return;}
    int delta=0;
    if (e->key()==Qt::Key_Left || e->key()==Qt::Key_Up) delta=-1;
    if (e->key()==Qt::Key_Right || e->key()==Qt::Key_Down) delta=1;
    if (delta) {
        for (int i=m_selected<0?0:m_selected+delta;i>=0 && i<m_items.size();i+=delta)
            if (m_items[i].app.enabled && m_items[i].app.gridRow/m_rows==m_page) {m_selected=i;break;}
        update();return;
    }
    if ((e->key()==Qt::Key_Return || e->key()==Qt::Key_Space) && m_selected>=0) {emit activated(m_items[m_selected].app.id);return;}
    QWidget::keyPressEvent(e);
}
void LauncherGrid::wheelEvent(QWheelEvent* e)
{
    if (m_dragging) {e->accept();return;}
    m_wheelDelta+=e->angleDelta().y();
    if (qAbs(m_wheelDelta)>=120) {setPage(m_page+(m_wheelDelta<0?1:-1));m_wheelDelta=0;}
    e->accept();
}
void LauncherGrid::contextMenuEvent(QContextMenuEvent* e)
{
    const auto cell=cellAt(e->pos());if (cell.x()<0) return;
    const int index=occupant(cell);
    if (index>=0) {if (m_items[index].app.enabled) emit itemMenuRequested(m_items[index].app.id,e->globalPos());return;}
    QMenu menu(this);Theme::bindMenu(&menu);
    auto* software=menu.addAction(QStringLiteral("添加程序或脚本…"));
    auto* diskFolder=menu.addAction(QStringLiteral("添加磁盘文件夹…"));
    QAction* panelFolder=nullptr;
    if (!m_groupMode) {
        menu.addSeparator();
        panelFolder=menu.addAction(QStringLiteral("新建面板文件夹"));
    }
    menu.addSeparator();
    auto* deletion=menu.addAction(QStringLiteral("删除当前页"));deletion->setEnabled(!m_groupMode && m_pages>1);
    auto* selected=menu.exec(e->globalPos());
    if (selected==deletion) emit deletePageRequested();
    else if (selected==panelFolder) emit createGroupRequested(cell.y(),cell.x());
    else if (selected==software) emit addRequested(cell.y(),cell.x(),false);
    else if (selected==diskFolder) emit addRequested(cell.y(),cell.x(),true);
}
