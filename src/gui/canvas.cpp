#include "canvas.hpp"
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QStyleOptionGraphicsItem>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <cmath>
#include <algorithm>

namespace placement::gui {
/** @brief 每空间块一个批量图元；无单元图元、无交互写入，主线程拥有。 */
class BlockItem final:public QGraphicsItem {
public:
    /** @brief 构造只读块。 @param view 借用视图。 @param ids 借用当前包ID数组。 @param bounds 块覆盖矩形。
     * @param statistics 可空的同包聚合统计，寿命覆盖本图元。 @param details 可空的同包宏/固定ID数组。 */
    BlockItem(Canvas& view,const std::vector<NodeId>& ids,Rect bounds,const BlockStatistics* statistics=nullptr,const std::vector<NodeId>* details=nullptr):view_(view),ids_(ids),bounds_(SceneRect(bounds)),statistics_(statistics),details_(details) {setFlag(ItemUsesExtendedStyleOption);}
    /** @brief 获取含跨块实体的边界。 @return 场景范围。 */
    QRectF boundingRect() const override {return bounds_.adjusted(-0.01,-0.01,0.01,0.01);}
    /** @brief 按LOD批量画可见实体。 @param painter 画笔。 @param option 暴露区域/LOD。 @param widget 可空绘制设备。 无返回值。 */
    void paint(QPainter* painter,const QStyleOptionGraphicsItem* option,QWidget* widget) override {
        Q_UNUSED(widget)
        const double scale=QStyleOptionGraphicsItem::levelOfDetailFromTransform(painter->worldTransform()); ///< 当前像素/DU。
        const auto& packet=*view_.packet_; ///< 当前修订的只读包。
        const bool aggregate=statistics_&&(view_.coarse_||statistics_->maximumMinExtent*scale<1); // 仅标准单元的LOD，不受同块固定大对象拖累。
        if(aggregate&&view_.layers_.standard) {
            const double area=statistics_->areaByKind[0]; // 标准单元面积和，DU²，重叠可重复。
            if(area>0) {
                ++view_.renderStatistics_.aggregatedBlocks;
                QColor color(220,76,76); // 与CImg相同的标准单元色。
                const QRectF bounds=SceneRect(statistics_->standardBounds); // 标准单元聚合显示范围。
                color.setAlphaF(.15+.75*std::clamp(area/std::max(bounds.width()*bounds.height(),1e-20),0.0,1.0));
                painter->setPen(Qt::NoPen);painter->setBrush(color);painter->drawRect(bounds);
            }
        }
        const std::vector<NodeId>& visibleIds=aggregate&&details_?*details_:ids_; // 聚合标准后只访问宏/固定对象，其余使用完整候选。
        for(NodeId id:visibleIds) { // id 为块内稳定实体ID。
            ++view_.renderStatistics_.entityVisits;
            const Node& node=packet.snapshot->database->nodes[id]; ///< 节点类别及固定性。
            if(!Visible(node,view_.layers_)) continue;
            const QRectF rect=SceneRect(packet.boxes[id]); ///< 场景绘制矩形。
            if(!rect.adjusted(-1/scale,-1/scale,1/scale,1/scale).intersects(option->exposedRect)) continue;
            const QColor color=node.fixed?QColor(71,85,105):node.kind==NodeKind::Macro?QColor(35,155,86):QColor(220,76,76); ///< 与CImg共享的类别填色。
            const double pixels=std::min(rect.width(),rect.height())*scale; ///< 最小投影跨度，px。
            if(view_.coarse_||pixels<1) {painter->setPen(QPen(color,0));painter->drawPoint(rect.center());continue;}
            painter->setBrush(color);painter->setPen(pixels>8?QPen(color.darker(155),0):Qt::NoPen);painter->drawRect(rect);
        }
    }
private:
    Canvas& view_; ///< 借用主线程拥有的父视图。
    const std::vector<NodeId>& ids_; ///< 当前packet中的稳定ID数组，随场景销毁前有效。
    QRectF bounds_; ///< 含实体外沿的场景包围盒。
    const BlockStatistics* statistics_; ///< 可空只读块统计，借用当前packet，图元先于packet销毁。
    const std::vector<NodeId>* details_; ///< 可空宏/固定列表，标准聚合后继续保持类别与图层独立。
};
/** @brief 创建主线程空场景与固定坐标约定。 */
Canvas::Canvas(QWidget* parent):QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));setBackgroundBrush(QColor(20,28,38));setRenderHint(QPainter::Antialiasing,false);
    setViewportUpdateMode(MinimalViewportUpdate);setTransformationAnchor(NoAnchor);setResizeAnchor(AnchorViewCenter);setMouseTracking(true);setFocusPolicy(Qt::StrongFocus);setFrameShape(QFrame::NoFrame);
}
/** @brief 整体切换修订包并重建最多4097个批量图元。 */
void Canvas::setPacket(std::shared_ptr<const RenderPacket> packet) {
    if(packet_==packet)return;
    const bool refit=!packet_||!packet||packet_->snapshot->datasetId!=packet->snapshot->datasetId; ///< 新数据集需要fit。
    scene()->clear();packet_=std::move(packet);heat_.reset();network_=NetworkOverlay{};if(refit)selected_.clear();
    if(packet_) {
        for(std::size_t i=0;i<packet_->drawBlocks.size();++i) { // i 为64×64空间块编号。
            if(!packet_->drawBlocks[i].empty()) scene()->addItem(new BlockItem(*this,packet_->drawBlocks[i],packet_->blockBounds[i],&packet_->statistics[i],&packet_->detailBlocks[i]));
        }
        if(!packet_->large.empty()) {Rect bounds=packet_->chip; /* bounds 为大宏绘制层覆盖区域。 */auto* item=new BlockItem(*this,packet_->large,bounds); /* item 为大宏独立批量层。 */item->setZValue(-1);scene()->addItem(item);}
        const QRectF bounds=SceneRect(packet_->chip); ///< 整个芯片场景边界。
        const double margin=std::max({bounds.width(),bounds.height(),1.0})*1000; ///< 允许数学相机平移的场景边距DU。
        scene()->setSceneRect(bounds.adjusted(-margin,-margin,margin,margin));if(refit) fit();
    }
    viewport()->update();
}
/** @brief 读取当前包。 */
std::shared_ptr<const RenderPacket> Canvas::packet() const {return packet_;}
/** @brief 读取数学相机。 */
Camera Canvas::camera() const {const QPointF center=mapToScene(viewport()->rect().center()); /* center 为负y场景中心。 */return {{center.x(),-center.y()},1/std::max(transform().m11(),1e-20)};}
/** @brief 同步数学相机，不再次发信号。 */
void Canvas::setCamera(Camera camera) {if(!(camera.duPerPixel>0&&std::isfinite(camera.duPerPixel))) return;const double scale=std::clamp(1/camera.duPerPixel,fitScale_*1e-3,fitScale_*1e3); /* scale 为受限像素/DU。 */setTransform(QTransform::fromScale(scale,scale));centerOn(camera.center.x,-camera.center.y);viewport()->update();}
/** @brief 适应完整显示域或core。 */
void Canvas::fit(bool core) {if(!packet_) return;const QRectF bounds=SceneRect(core?packet_->core:packet_->chip); /* bounds 为场景目标域。 */fitScale_=0.9*std::min(viewport()->width()/std::max(bounds.width(),1.0),viewport()->height()/std::max(bounds.height(),1.0));setCamera({{bounds.center().x(),-bounds.center().y()},1/fitScale_});notifyCamera();}
/** @brief 定位目标并留10%空白。 */
void Canvas::locate(Rect rect) {const double width=std::max(rect.width()*1.2,1.0); /* width 为带边距横跨度DU。 */const double height=std::max(rect.height()*1.2,1.0); /* height 为带边距纵跨度DU。 */setCamera({{(rect.lx+rect.ux)/2,(rect.ly+rect.uy)/2},std::max(width/std::max(1,viewport()->width()),height/std::max(1,viewport()->height()))});notifyCamera();}
/** @brief 同步只读选择并可发通知。 */
void Canvas::setSelection(QSet<qulonglong> ids,bool notify) {selected_=std::move(ids);viewport()->update();if(notify) emit selectionChanged(selected_);}
/** @brief 读取稳定ID选择。 */
QSet<qulonglong> Canvas::selection() const{return selected_;}
/** @brief 切换图层。 */
void Canvas::setLayers(Layers layers){layers_=layers;scene()->update();}
/** @brief 读取图层。 */
Layers Canvas::layers() const{return layers_;}
/** @brief 仅安装同数据/运行/修订的热图。 */
void Canvas::setHeatmap(std::shared_ptr<const Heatmap> heat){if(heat&&packet_&&(heat->dataset!=packet_->snapshot->datasetId||heat->run!=packet_->snapshot->runId||heat->stage!=packet_->snapshot->stage||heat->revision!=packet_->snapshot->revision))return;heat_=std::move(heat);viewport()->update();}
/** @brief 设置至多5000条网络示意线。 */
void Canvas::setNetwork(NetworkOverlay overlay){network_=std::move(overlay);viewport()->update();}
/** @brief 恢复详细渲染。 */
void Canvas::restoreDetail(){coarse_=false;slowFrames_=0;scene()->update();}
/** @brief 主线程读取真实绘制策略计数，无输入，不改变场景。 */
RenderStatistics Canvas::renderStatistics() const {RenderStatistics result=renderStatistics_; /* result为累计与最近帧值副本。 */result.coarse=coarse_;return result;}
/** @brief 通过独立空间索引命中。 */
std::vector<NodeId> Canvas::hits(Vec2 point) const{return packet_?HitTest(*packet_,point,4*camera().duPerPixel,layers_):std::vector<NodeId>{};}
/** @brief 绘制背景、行与有界密度网格。 */
void Canvas::drawBackground(QPainter* painter,const QRectF& rect){
    painter->fillRect(rect,QColor(20,28,38));if(!packet_) return;
    painter->setPen(QPen(QColor(158,176,193),0));painter->setBrush(QColor(32,43,55));painter->drawRect(SceneRect(packet_->core));
    if(layers_.fixed) {painter->setPen(Qt::NoPen);painter->setBrush(QColor(71,85,105));for(const Obstacle& obstacle:packet_->snapshot->database->obstacles)painter->drawRect(SceneRect(obstacle.rect));} // 硬阻挡与CImg/GDS共用固定几何层。
    if(layers_.rows){painter->setPen(QPen(QColor(64,79,94),0));for(const RowSegment& row:packet_->snapshot->database->rows) painter->drawRect(SceneRect(rowRect(row)));} // row 为只读行段。
    const int nx=heat_?heat_->nx:packet_->snapshot->gridNx; ///< 横向网格数，0表示未知。
    const int ny=heat_?heat_->ny:packet_->snapshot->gridNy; ///< 纵向网格数。
    if(nx<=0||ny<=0) return;
    const double dx=packet_->core.width()/nx; ///< bin宽DU。
    const double dy=packet_->core.height()/ny; ///< bin高DU。
    if(layers_.heat&&heat_){
        const int x0=std::clamp(static_cast<int>((rect.left()-packet_->core.lx)/dx),0,nx-1); ///< 可见首列。
        const int x1=std::clamp(static_cast<int>((rect.right()-packet_->core.lx)/dx),0,nx-1); ///< 可见末列。
        const int y0=std::clamp(static_cast<int>((-rect.bottom()-packet_->core.ly)/dy),0,ny-1); ///< 可见首行。
        const int y1=std::clamp(static_cast<int>((-rect.top()-packet_->core.ly)/dy),0,ny-1); ///< 可见末行。
        painter->setPen(Qt::NoPen);
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x){ // y/x 为可见热图网格。
            const double ratio=std::clamp(heat_->density[y*nx+x]/std::max(heat_->target,1e-20),0.0,2.0); ///< 固定目标归一化密度。
            painter->setBrush(QColor::fromHsvF((2-ratio)/3,0.8,0.85,0.45));painter->drawRect(SceneRect({packet_->core.lx+x*dx,packet_->core.ly+y*dy,packet_->core.lx+(x+1)*dx,packet_->core.ly+(y+1)*dy}));
        }
    }
    if(layers_.bins){painter->setPen(QPen(QColor(106,119,132,100),0));for(int x=0;x<=nx;++x) painter->drawLine(QPointF(packet_->core.lx+x*dx,-packet_->core.ly),QPointF(packet_->core.lx+x*dx,-packet_->core.uy)); /* x 为网格边界列。 */for(int y=0;y<=ny;++y)painter->drawLine(QPointF(packet_->core.lx,-packet_->core.ly-y*dy),QPointF(packet_->core.ux,-packet_->core.ly-y*dy)); /* y 为网格边界行。 */}
}
/** @brief 绘制网络、违规和跨隐藏图层的选择轮廓。 */
void Canvas::drawForeground(QPainter* painter,const QRectF& rect){
    Q_UNUSED(rect)
    if(!packet_)return;
    painter->setPen(QPen(QColor(71,85,105),0));
    for(const NodeId id:packet_->ports) { // 零面积端口用固定像素十字，聚合LOD也保留可见性。
        if(!Visible(packet_->snapshot->database->nodes[id],layers_))continue;
        const Rect box=packet_->boxes[id]; // 端口数学锚点，DU。
        const double radius=3*camera().duPerPixel; // 三逻辑像素的DU半径。
        painter->drawLine(QPointF(box.lx-radius,-box.ly),QPointF(box.lx+radius,-box.ly));
        painter->drawLine(QPointF(box.lx,-box.ly-radius),QPointF(box.lx,-box.ly+radius));
    }
    painter->setBrush(Qt::NoBrush);painter->setPen(QPen(QColor(188,120,244,180),0));
    for(Vec2 pin:network_.pins)painter->drawLine(QPointF(pin.x,-pin.y),QPointF(network_.mean.x,-network_.mean.y)); // pin 为抽样引脚数学坐标。
    if(layers_.violations){painter->setPen(QPen(QColor(241,87,94,180),0));for(const Violation& violation:packet_->violations.records)painter->drawRect(SceneRect(violation.region));} // violation 为有界违规定位框。
    painter->setPen(QPen(QColor(255,233,123),0));for(qulonglong id:selected_)if(id<packet_->boxes.size())painter->drawRect(SceneRect(packet_->boxes[id]).adjusted(-camera().duPerPixel,-camera().duPerPixel,camera().duPerPixel,camera().duPerPixel)); // id 为选中对象。
    if(selecting_&&(last_-press_).manhattanLength()>5){painter->setPen(QPen(QColor(120,200,255),0,Qt::DashLine));painter->drawRect(QRectF(mapToScene(press_),mapToScene(last_)).normalized());}
}
/** @brief 屏幕覆盖层保持文字正向，计时只影响LOD。 */
void Canvas::paintEvent(QPaintEvent* event){
    ++renderStatistics_.paintCount;renderStatistics_.aggregatedBlocks=0;renderStatistics_.entityVisits=0;
    QElapsedTimer timer; ///< 当前帧耗时计时器。
    timer.start();QGraphicsView::paintEvent(event);
    QPainter painter(viewport()); ///< 主线程屏幕覆盖层画笔。
    painter.setPen(QColor(224,233,243));
    if(!packet_) {painter.drawText(viewport()->rect(),Qt::AlignCenter,tr("打开 AUX 或 LEF / DEF 开始"));return;}
    const QString caption=QString::fromStdString(packet_->snapshot->stage)+tr(" · iteration %1 · revision %2 · %3").arg(packet_->snapshot->iteration).arg(packet_->snapshot->revision).arg(QString::fromStdString(packet_->snapshot->database->unit)); ///< 不随场景翻转的身份文本。
    painter.drawText(12,22,caption);
    painter.drawText(12,viewport()->height()-12,tr("亚像素对象按空间块聚合；选择、属性和导出始终使用原始几何"));
    if(layers_.heat)painter.drawText(12,42,heat_?tr("真实+静态电荷（不含 filler）：蓝 0 → 绿 d=%1 → 红 2d").arg(heat_->target):tr("密度热图待后台计算"));
    if(network_.originalCount>5000)painter.drawText(12,62,tr("网络示意：确定性抽样 5000 / %1 引脚，非布线").arg(network_.originalCount));
    int count=0; ///< 限制屏幕名称标签数量。
    for(qulonglong id:selected_){ // id 为选中实体稳定ID。
        if(id>=packet_->boxes.size()||count++>=100)continue;
        const Rect r=packet_->boxes[id]; ///< 选中物体数学矩形。
        painter.drawText(mapFromScene(QPointF(r.lx,-r.uy))+QPoint(3,-3),QString::fromStdString(packet_->snapshot->database->nodes[id].name));
    }
    if(timer.elapsed()>100){if(++slowFrames_>=10&&!coarse_){coarse_=true;++renderStatistics_.coarseTransitions;emit renderWarning(tr("连续慢帧，已启用粗 LOD；视图菜单可恢复精细绘制"));}}else slowFrames_=0;
}
/** @brief 用户相机通知只携数学中心和比例。 */
void Canvas::notifyCamera(){const Camera current=camera(); /* current 为当前数学相机。 */emit cameraChanged(current.center.x,current.center.y,current.duPerPixel);}
/** @brief 鼠标点锚定按档1.2缩放。 */
void Canvas::wheelEvent(QWheelEvent* event){
    if(!packet_){event->accept();return;}
    const QPoint pixel=event->position().toPoint(); ///< 鼠标锚点逻辑像素。
    const QPointF before=mapToScene(pixel); ///< 缩放前锚点场景坐标。
    Camera current=camera(); ///< 可变数学相机。
    current.duPerPixel/=std::pow(1.2,event->angleDelta().y()/120.0);setCamera(current);
    const QPointF after=mapToScene(pixel); ///< 缩放后锚点场景坐标。
    current=camera();current.center.x+=before.x()-after.x();current.center.y-=before.y()-after.y();setCamera(current);notifyCamera();event->accept();
}
/** @brief 按下时只开始相机平移或选择，不移动实体。 */
void Canvas::mousePressEvent(QMouseEvent* event){press_=last_=event->pos();panning_=event->button()==Qt::MiddleButton||(space_&&event->button()==Qt::LeftButton);selecting_=event->button()==Qt::LeftButton&&!panning_;if(panning_)setCursor(Qt::ClosedHandCursor);event->accept();}
/** @brief 相机平移使用DU/px换算。 */
void Canvas::mouseMoveEvent(QMouseEvent* event){
    const QPointF scenePoint=mapToScene(event->pos()); ///< 鼠标场景位置。
    emit pointerChanged(scenePoint.x(),-scenePoint.y());
    if(panning_){Camera current=camera(); /* current 为拖动前相机。 */const QPoint delta=event->pos()-last_; /* delta 为屏幕移动px。 */current.center.x-=delta.x()*current.duPerPixel;current.center.y+=delta.y()*current.duPerPixel;setCamera(current);notifyCamera();}
    last_=event->pos();if(selecting_)viewport()->update();event->accept();
}
/** @brief 提交只读框选/点选，Alt循环与Ctrl增减。 */
void Canvas::mouseReleaseEvent(QMouseEvent* event){
    if(selecting_&&packet_){
        QSet<qulonglong> next=(event->modifiers()&Qt::ControlModifier)?selected_:QSet<qulonglong>{}; ///< 新选择集。
        if((event->pos()-press_).manhattanLength()>5){
            const QRectF area=QRectF(mapToScene(press_),mapToScene(event->pos())).normalized(); ///< 只读框选区域，场景单位。
            // 框选遍历空间块候选，并按稳定ID去重，不创建逐物体图元。
            for(std::size_t index=0;index<packet_->drawBlocks.size();++index){ // index 为绘制块编号。
                if(!SceneRect(packet_->blockBounds[index]).intersects(area))continue;
                for(NodeId id:packet_->drawBlocks[index])if(Visible(packet_->snapshot->database->nodes[id],layers_)&&area.intersects(SceneRect(packet_->boxes[id])))next.insert(id); // id 为块内候选。
            }
            for(NodeId id:packet_->large)if(Visible(packet_->snapshot->database->nodes[id],layers_)&&area.intersects(SceneRect(packet_->boxes[id])))next.insert(id); // id 为大对象候选。
        }else{
            const QPointF point=mapToScene(event->pos()); ///< 点选场景点。
            const auto candidates=hits({point.x(),-point.y()}); ///< 有序精确命中候选。
            if(!candidates.empty()){if(event->modifiers()&Qt::AltModifier)++cycle_;else cycle_=0;const NodeId id=candidates[cycle_%candidates.size()]; /* id 为本轮点选对象。 */if(next.contains(id))next.remove(id);else next.insert(id);}
        }
        setSelection(std::move(next));
    }
    selecting_=false;panning_=false;unsetCursor();viewport()->update();event->accept();
}
/** @brief 快捷键不修改领域坐标。 */
void Canvas::keyPressEvent(QKeyEvent* event){if(event->key()==Qt::Key_Space){space_=true;event->accept();}else if(event->key()==Qt::Key_F){fit(event->modifiers()&Qt::ShiftModifier);event->accept();}else if(event->key()==Qt::Key_Escape){setSelection({});setNetwork({});event->accept();}else QGraphicsView::keyPressEvent(event);}
/** @brief 释放空格平移状态。 */
void Canvas::keyReleaseEvent(QKeyEvent* event){if(event->key()==Qt::Key_Space){space_=false;event->accept();}else QGraphicsView::keyReleaseEvent(event);}
} // namespace placement::gui
