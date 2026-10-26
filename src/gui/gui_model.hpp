#pragma once
#include <placement/application/service.hpp>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <array>
#include <deque>
#include <mutex>
#include <set>

namespace placement::gui {
/** @brief 后台网络示意结果，身份必须与显示修订一致，绝不解释为实际布线。 */
struct NetworkOverlay {
    std::string dataset; ///< 数据集身份。
    std::string run; ///< 任务身份。
    std::string stage; ///< 源几何阶段，避免同run不同阶段同revision碰撞。
    std::uint64_t revision=0; ///< 源几何修订。
    NetId netId=0; ///< 本次请求网络ID，不能仅用快照身份区分选择。
    std::uint64_t requestId=0; ///< 选择/隐藏递增序号，迟到请求不得覆盖新选择。
    std::vector<Vec2> pins; ///< 至多5000个确定性抽样引脚，DU。
    Vec2 mean; ///< 全部引脚的平均数学中心，DU。
    std::size_t originalCount=0; ///< 原始引脚数。
};
/** @brief 主线程可观察运行状态；导出状态独立于此枚举。 */
enum class RunState { Empty, ///< 尚无数据。
    Loading, ///< 事务式导入中。
    Ready, ///< 有 initial 可启动。
    Running, ///< 算法正在执行。
    Pausing, ///< 等待核心安全点确认。
    Paused, ///< 核心已确认暂停。
    Cancelling, ///< 等待协作取消结束。
    Completed, ///< 已完成。
    Cancelled, ///< 已取消，保留完整阶段。
    Failed ///< 失败，保留原项目及成功阶段。
};
/** @brief 主线程只读显示网格；身份必须匹配当前场景。 */
struct Heatmap {
    std::string dataset; ///< 数据集身份。
    std::string run; ///< 运行身份。
    std::string stage; ///< 源几何阶段。
    std::uint64_t revision=0; ///< 源几何修订。
    int nx=0; ///< 横向 bin 数。
    int ny=0; ///< 纵向 bin 数。
    double target=0; ///< 绑定色标的目标密度。
    std::vector<double> density; ///< 真实移动与静态电荷的无量纲密度，按y*nx+x，不含任务私有filler。
};
/** @brief 同数据集跨修订共享的只读检索索引，后台构建一次。 */
struct TopologyIndex {
    std::vector<NodeId> names; ///< 按UTF-8名称升序排列的稳定ID，用于二分精确/前缀检索。
    std::vector<std::vector<NetId>> nodeNets; ///< 按NodeId索引的去重所属网络。
};
/** @brief 一个空间块的保守可视化统计；只用于低LOD，不影响选择、指标和导出。 */
struct BlockStatistics {
    double maximumMinExtent=0; ///< 块内可移动标准单元的最大最短边，DU；全部亚像素时允许聚合。
    Rect standardBounds; ///< 块内标准单元的包围盒，避免固定大对象扩大聚合显示区。
    std::array<double,3> areaByKind{}; ///< 标准/宏/固定的面积和，DU²；重叠可重复，仅表示显示覆盖率。
};
/** @brief 后台构造、主线程整体切换的只读几何包，无 Qt 场景对象。 */
struct RenderPacket {
    std::shared_ptr<const Snapshot> snapshot; ///< 共享真实快照，不复制拓扑。
    std::shared_ptr<const TopologyIndex> topology; ///< 跨修订共享名称/网络索引。
    Rect core; ///< 优化域，DU。
    Rect chip; ///< 含终端的显示域，DU。
    std::vector<Rect> boxes; ///< 按 NodeId 索引的真实矩形，DU。
    std::array<std::vector<NodeId>,4096> drawBlocks; ///< 中心所属64×64块的批量绘制ID。
    std::array<std::vector<NodeId>,4096> detailBlocks; ///< 同块宏/固定对象，标准单元聚合后仍逐个绘制。
    std::array<std::vector<NodeId>,4096> hitBlocks; ///< 与64×64块相交的精确命中ID。
    std::array<Rect,4096> blockBounds; ///< 中心绘制块实际实体包围盒。
    std::array<BlockStatistics,4096> statistics; ///< 后台预计算的聚合绘制统计，避免全局视图逐单元绘点。
    std::vector<NodeId> large; ///< 跨越超过16块的大对象，独立批量层。
    std::vector<NodeId> ports; ///< core 外零面积端口的独立命中候选。
    ViolationReport violations; ///< 后台独立检查，详情有界。
};
/** @brief 从真实快照建立空间包，后台线程调用。
 * @param snapshot 完整只读快照。 @param control 取消令牌。 @param inspect 是否做独立违规检查。
 * @return 完整不可变包；取消或非法快照抛 Error。 */
std::shared_ptr<const RenderPacket> BuildRenderPacket(std::shared_ptr<const Snapshot> snapshot,ControlToken& control,bool inspect=true);
/** @brief 数学矩形转场景，场景只采用(x,-y)。 @param rect DU矩形。 @return 场景矩形。 */
QRectF SceneRect(Rect rect);
/** @brief 主线程图层配置，名称不承载领域数据。 */
struct Layers {
    bool standard=true; ///< 显示标准单元。
    bool macro=true; ///< 显示宏。
    bool fixed=true; ///< 显示固定对象及端口。
    bool rows=false; ///< 显示行边界。
    bool bins=false; ///< 显示bin网格。
    bool heat=false; ///< 显示密度热图。
    bool violations=true; ///< 显示违规区域。
};
/** @brief 判断节点图层是否开启。 @param node 真实实体。 @param layers 当前开关。 @return 可见性。 */
bool Visible(const Node& node,const Layers& layers);
/** @brief 通过空间索引查询数学点命中，精确物理矩形优先。
 * @param packet 当前几何。 @param point 数学DU点。 @param tolerance 零面积端口命中容差DU。
 * @param layers 图层开关。 @return 顶层固定/标准/宏优先，同层面积及ID升序。 */
std::vector<NodeId> HitTest(const RenderPacket& packet,Vec2 point,double tolerance,const Layers& layers);
/** @brief 小型曲线点；不持有全坐标或数据库。 */
struct MetricPoint { std::uint64_t iteration=0; ///< 迭代号。
    Metrics metrics; ///< 同轮指标。
    std::string stage; ///< 阶段标记。
};
/** @brief 200ms定时消费的有界邮件批次，终态不经过可覆盖预览槽。 */
struct MailBatch {
    std::shared_ptr<const RenderPacket> preview; ///< 至多一份最新预览。
    std::vector<std::shared_ptr<const RenderPacket>> stages; ///< 至多四份完整阶段。
    std::deque<MetricPoint> metrics; ///< 至多5000个小型指标点。
    std::deque<QString> logs; ///< 至多5000条日志。
    std::vector<std::shared_ptr<const NetworkOverlay>> networks; ///< 双视图至多两份后台网络高亮。
    std::vector<std::shared_ptr<const Heatmap>> heatmaps; ///< 双视图至多两份后台热图。
    std::optional<RunResult> result; ///< 唯一终态，可靠消费一次。
    QString error; ///< 加载/导出错误，独立于主算法终态。
    bool success=false; ///< 非流水线任务成功标记。
    std::uint64_t droppedLogs=0; ///< 被环形缓冲覆盖的日志数量。
};
/** @brief 工作线程生产、主线程消费的容量受限传递器；所有方法线程安全。 */
class Mailbox {
public:
    /** @brief 重置任务身份及缓冲。 @param dataset 可空加载期身份。 @param run 新任务身份。 无返回值。 */
    void reset(std::string dataset,std::string run);
    /** @brief 校验事件身份与严格递增序列。 @param event 小型应用事件。 @return 是否接纳；不构建几何。 */
    bool accept(const Event& event);
    /** @brief 放入完整几何；预览覆盖，完整阶段按名字替换并限制四份。
     * @param packet 完整包。 @param stage true为完成阶段。 无返回值。 */
    void packet(std::shared_ptr<const RenderPacket> packet,bool stage);
    /** @brief 写入唯一流水线终态。 @param result 完整结果。 无返回值；首次写入获胜。 */
    void finish(RunResult result);
    /** @brief 写入非算法任务状态。 @param error 可空错误。 @param success 成功。 无返回值。 */
    void finishTask(QString error,bool success);
    /** @brief 写入后台热图。 @param heat 完整网格。 无返回值。 */
    void heat(std::shared_ptr<const Heatmap> heat);
    /** @brief 写入后台网络。 @param network 完整高亮。 无返回值。 */
    void network(std::shared_ptr<const NetworkOverlay> network);
    /** @brief 原子取走积累批次。 @return 有界批次；消费后不重复终态。 */
    MailBatch take();
    /** @brief 当前待消费预览数量，用于测试和诊断。 @return 0或1。 */
    std::size_t previewCount() const;
private:
    mutable std::mutex mutex_; ///< 保护所有字段，线程安全。
    std::string dataset_; ///< 当前允许数据集，可空等待加载。
    std::string run_; ///< 当前唯一任务身份。
    std::uint64_t sequence_=0; ///< 最后接纳序列。
    bool haveSequence_=false; ///< 是否已接纳第一条事件。
    bool terminated_=false; ///< 保证终态只写一次。
    MailBatch batch_; ///< 有界待消费数据。
};
/** @brief 合并曲线旧点时保留overflow极值并始终保留末点。
 * @param points 可变曲线点，最多保留5000。 无返回值。 */
void CompactMetrics(std::deque<MetricPoint>& points);
} // namespace placement::gui
