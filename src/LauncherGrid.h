#pragma once
#include "core-api.h"
#include "PinnedApps.h"
#include <QIcon>
#include <QVariantAnimation>
#include <QTimer>
#include <QWidget>
class QPainter;

class LTOOL_CORE_API LauncherGrid : public QWidget {
    Q_OBJECT
public:
    struct Item { PinnedApp app; QIcon icon; bool available=true; bool group=false; QVector<PinnedApp> children; QVector<QIcon> covers; };
    explicit LauncherGrid(QWidget* parent=nullptr);
    void setItems(const QVector<Item>& items,int columns,int rows=4,int pages=1);
    void setGroupMode(bool enabled) {m_groupMode=enabled;}
    void beginExternalDrag(const Item& item,const QPoint& globalPos);
    bool mergePreview() const {return m_mergeReady;}
    int currentPage() const { return m_page; }
    int pageCount() const;
    void setPage(int page,bool allowEmpty=false);
    QStringList itemIds() const;
    QRect itemRect(int index) const;
    QRect cellRect(int row,int column) const;
    bool isDragging() const { return m_dragging; }
    void cancelDrag();
signals:
    void activated(const QString& id);
    void addRequested(int row,int column,bool folder);
    void createGroupRequested(int row,int column);
    void moveRequested(const QString& id,int row,int column);
    void mergeRequested(const QString& sourceId,const QString& targetId);
    void renameRequested(const QString& id);
    void deletePageRequested();
    void transferRequested(const QString& id,const QPoint& globalPos);
    void dragCanceled();
    void itemMenuRequested(const QString& id,const QPoint& globalPos);
    void pageChanged(int page,int count);
protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void hideEvent(QHideEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    QPoint cellAt(const QPoint& pos) const;
    int occupant(const QPoint& cell) const;
    int indexAt(const QPoint& pos) const;
    void arrange();
    void turnPage(int page);
    void loadIcons();
    void updateDrag(const QPoint& pos);
    void stopDrag();
    void drawItem(QPainter&,int,const QRectF&,bool floating=false);
    QVector<Item> m_items;
    QVariantAnimation m_animation;
    QVariantAnimation m_pageAnimation;
    QPixmap m_oldPage;
    qreal m_pageProgress=1;
    int m_pageDirection=1;
    QTimer m_pageTimer;
    QTimer m_mergeTimer,m_exitTimer;
    QPointF m_settling;
    int m_columns=8,m_rows=4,m_page=0,m_pages=1,m_extraPage=-1,m_dragPage=0,m_edgeDirection=0;
    int m_mergeTarget=-1;
    int m_pressed=-1,m_hovered=-1,m_selected=-1,m_dragIndex=-1,m_settleIndex=-1,m_wheelDelta=0;
    quint64 m_iconGeneration=0;
    QPoint m_pressPos,m_dragOffset,m_mousePos;
    QSize m_dragWindowSize;
    bool m_dragging=false;
    bool m_groupMode=false,m_mergeReady=false,m_external=false,m_snapshot=false;
};
