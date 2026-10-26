#pragma once
#include <placement/core/model.hpp>
#include <map>

namespace placement {
/** @brief 单条独立几何违规；值类型允许跨线程发布，不持有界面对象。 */
struct Violation {
    std::string kind; ///< 稳定违规类别，参见设计07。
    std::vector<NodeId> objectIds; ///< 关联对象，按稳定ID排序。
    Rect region; ///< 用于定位的数学坐标范围，DU。
    double magnitude=0; ///< 面积违规为DU²，对齐/越界偏差为DU，固定性为变化标记。
    std::string message; ///< 对象、障碍来源及违规说明。
};
/** @brief 独立检查报告；详情上限不限制完整计数。 */
struct ViolationReport {
    std::string scope="all"; ///< all 或 macro；macro 不检查标准单元间碰撞和行约束。
    std::map<std::string,std::uint64_t> countByType; ///< 全部违规按类型计数。
    std::vector<Violation> records; ///< 排序后最多指定上限的详情。
    std::uint64_t totalCount=0; ///< 未截断的总违规数。
    bool truncated=false; ///< 详情是否被截断。
};
/** @brief 独立计算真实引脚线长和可比位移，保留当前已计算密度指标。
 * @param snapshot 待评价完整快照。 @param baseline 可空位移参考，必须同dataset。
 * @return 新指标，无位置修改；不同数据集或无效引用抛Error。 */
Metrics ComputeMetrics(const Snapshot& snapshot,const Snapshot* baseline=nullptr);
/** @brief 从最终几何重新检查，无算法成功标记依赖。
 * @param snapshot 任意待检查快照。 @param scope all/macro。
 * @param limit 详情上限，完整计数继续。 @param control 可空取消令牌。
 * @return 确定性排序报告；不改变任何位置，NON_FINITE作为违规而非成功。 */
ViolationReport CheckLegality(const Snapshot& snapshot,const std::string& scope="all",std::size_t limit=10000,ControlToken* control=nullptr);
/** @brief 同数据集阶段比较结果；不适用的改善率为null。 */
struct StageComparison {
    Metrics before; ///< 参考阶段指标。
    Metrics after; ///< 当前阶段指标及对参考的位移。
    std::optional<double> hpwlImprovement; ///< (before-after)/before，before=0时null。
    std::vector<double> displacement; ///< 按NodeId的中心欧氏位移，DU。
};
/** @brief 比较两个阶段，不改变源快照。
 * @param before 参考。 @param after 当前。 @return 比较值；跨dataset拒绝。 */
StageComparison CompareStages(const Snapshot& before,const Snapshot& after);
} // namespace placement
