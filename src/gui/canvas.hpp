#pragma once
#include "gui_model.hpp"
#include <QGraphicsView>
#include <QElapsedTimer>
#include <QSet>

namespace placement::gui {
/** @brief 两个视图同步的数学相机，完全独立于场景负y映射。 */
struct Camera { Vec2 center; ///< 数学中心，DU。
    double duPerPixel=1; ///< 一个逻辑像素对应DU，正数。
};
/** @brief 主线程最近一次真实绘制的诊断值；仅测量显示策略，不影响领域数据。 */
struct RenderStatistics {
    bool coarse=false; ///< 当前是否启用连续慢帧后的强制粗化。
    std::uint64_t coarseTransitions=0; ///< 本画布累计自动粗化次数，不含正常亚像素聚合。
    std::uint64_t paintCount=0; ///< 已执行的真实paintEvent数。
    std::size_t aggregatedBlocks=0; ///< 最近一次绘制中按亚像素/粗化聚合的块数。
    std::size_t entityVisits=0; ///< 最近一次绘制中逐实体访问的候选数。
};
/** @brief 唯读布局视图；全部Qt场景对象仅主线程访问。 */
class Canvas : public QGraphicsView {
    Q_OBJECT
public:
    /** @brief 构造空场景。 @param parent 主线程父控件，可空。 */
    explicit Canvas(QWidget* parent=nullptr);
    /** @brief 原子切换完整几何包。 @param packet 后台已构建包，可空。 无返回值。 */
    void setPacket(std::shared_ptr<const RenderPacket> packet);
    /** @brief 读取当前包。 @return 共享不可变包，可空。 */
    std::shared_ptr<const RenderPacket> packet() const;
    /** @brief 获取数学相机。 @return 中心与DU/px。 */
    Camera camera() const;
    /** @brief 安装数学相机，不再发同步信号。 @param camera 另一视图相机。 无返回值。 */
    void setCamera(Camera camera);
    /** @brief 适应芯片或core。 @param core true选择core。 无返回值，会发相机变更。 */
    void fit(bool core=false);
    /** @brief 放大定位数学矩形。 @param rect 目标DU区域。 无返回值。 */
    void locate(Rect rect);
    /** @brief 设置只读选择。 @param ids 稳定对象ID。 @param notify 是否发选择信号。 无返回值。 */
    void setSelection(QSet<qulonglong> ids,bool notify=true);
    /** @brief 获取当前选择。 @return 按稳定ID的集合。 */
    QSet<qulonglong> selection() const;
    /** @brief 设置实体/辅助图层。 @param layers 新图层。 无返回值。 */
    void setLayers(Layers layers);
    /** @brief 读取图层。 @return 当前开关。 */
    Layers layers() const;
    /** @brief 安装同几何热图。 @param heat 可空后台结果。 无返回值；身份不符忽略。 */
    void setHeatmap(std::shared_ptr<const Heatmap> heat);
    /** @brief 设置后台已算好的网络。 @param overlay 星形示意。 无返回值。 */
    void setNetwork(NetworkOverlay overlay);
    /** @brief 恢复精细LOD并清除超预算计数；无参数、无返回值。 */
    void restoreDetail();
    /** @brief 获取最近绘制策略和工作量。 @return 主线程值副本，无副作用。 */
    RenderStatistics renderStatistics() const;
    /** @brief 单击命中，用于测试或属性查找。 @param point 数学DU点。 @return 有序ID。 */
    std::vector<NodeId> hits(Vec2 point) const;
signals:
    /** @brief 数学相机发生用户变更。 @param x 中心DU。 @param y 中心DU。 @param duPerPixel DU/px。 无返回值。 */
    void cameraChanged(double x,double y,double duPerPixel);
    /** @brief 稳定ID选择变化，无坐标编辑。 @param ids 当前集合。 无返回值。 */
    void selectionChanged(QSet<qulonglong> ids);
    /** @brief 鼠标数学坐标更新。 @param x DU。 @param y DU。 无返回值。 */
    void pointerChanged(double x,double y);
    /** @brief 连续慢帧触发粗LOD。 @param message 可读说明。 无返回值。 */
    void renderWarning(QString message);
protected:
    /** @brief 批量辅助图层。 @param painter 场景画笔。 @param rect 暴露场景矩形。 无返回值。 */
    void drawBackground(QPainter* painter,const QRectF& rect) override;
    /** @brief 绘制只读选择/网络/违规。 @param painter 场景画笔。 @param rect 暴露区域。 无返回值。 */
    void drawForeground(QPainter* painter,const QRectF& rect) override;
    /** @brief 绘制屏幕文字覆盖层并计时。 @param event 重绘区域。 无返回值。 */
    void paintEvent(QPaintEvent* event) override;
    /** @brief 鼠标锚定缩放。 @param event 滚轮事件。 无返回值。 */
    void wheelEvent(QWheelEvent* event) override;
    /** @brief 记录平移或只读选择起点。 @param event 按下事件。 无返回值。 */
    void mousePressEvent(QMouseEvent* event) override;
    /** @brief 执行相机平移或框选预览。 @param event 移动事件。 无返回值。 */
    void mouseMoveEvent(QMouseEvent* event) override;
    /** @brief 提交点选或可见图层框选。 @param event 松开事件。 无返回值。 */
    void mouseReleaseEvent(QMouseEvent* event) override;
    /** @brief 处理适应/清除/空格平移快捷键。 @param event 键盘事件。 无返回值。 */
    void keyPressEvent(QKeyEvent* event) override;
    /** @brief 结束空格平移。 @param event 键盘事件。 无返回值。 */
    void keyReleaseEvent(QKeyEvent* event) override;
private:
    /** @brief 发送当前相机，不携领域位置写入；无参数、无返回值。 */
    void notifyCamera();
    std::shared_ptr<const RenderPacket> packet_; ///< 主线程当前完整包。
    std::shared_ptr<const Heatmap> heat_; ///< 与当前包身份相同的热图。
    Layers layers_; ///< 主线程图层。
    QSet<qulonglong> selected_; ///< 稳定ID选择。
    NetworkOverlay network_; ///< 少量示意线，不创建引脚图元。
    QPoint press_; ///< 鼠标按下逻辑像素。
    QPoint last_; ///< 上次平移逻辑像素。
    bool panning_=false; ///< 当前正在移动相机。
    bool space_=false; ///< 空格平移修饰键。
    bool selecting_=false; ///< 当前正在只读框选。
    bool coarse_=false; ///< 连续慢帧后的粗LOD。
    int slowFrames_=0; ///< 连续超过100ms帧数。
    RenderStatistics renderStatistics_; ///< 最近真实绘制及累计粗化计数。
    std::size_t cycle_=0; ///< Alt点选候选循环位置。
    double fitScale_=1; ///< 初始fit像素/DU，限制缩放范围。
    friend class BlockItem; ///< 批量图元读取图层及粗LOD。
};
} // namespace placement::gui
