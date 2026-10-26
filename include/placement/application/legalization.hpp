#pragma once
#include <placement/analysis/analysis.hpp>

namespace placement {
/** @brief 扣除障碍后的整站点可放置区间；endSite为右侧排他站点编号。 */
struct RowInterval {
    std::uint64_t rowId=0; ///< 原始行段ID。
    double origin=0; ///< 原行零站点基准，DU，障碍不能重置此原点。
    double y=0; ///< 行底坐标，DU。
    double height=0; ///< 单行高度，DU。
    double pitch=0; ///< 站点间距，DU。
    Orientation orientation=Orientation::N; ///< 原始行方向。
    std::vector<Orientation> allowedOrientations; ///< 允许方向，空时仅orientation。
    std::int64_t startSite=0; ///< 可用首站点序号。
    std::int64_t endSite=0; ///< 右侧排他站点序号。
};
/** @brief 在宏/固定障碍约束下模拟退火，合法最优解才发布阶段。
 * @param global 必须为global阶段。 @param config 宏配置。 @param control 控制令牌。
 * @param sink 完整候选提交/回滚后的可空预览回调，不得访问Qt。
 * @return macro_legal快照；未找到解抛NO_LEGAL_SOLUTION_FOUND，不改变输入。 */
Snapshot LegalizeMacros(const Snapshot& global,const Config& config,ControlToken& control,const std::function<void(const Snapshot&)>& sink={});
/** @brief 扣除宏/固定实体和硬阻挡，沿原站点网格切分行。
 * @param macro 宏已独立合法的快照。 @param control 可空取消令牌。
 * @return 非空容量整站点区间；不支持站点形状抛UNSUPPORTED。 */
std::vector<RowInterval> PartitionRows(const Snapshot& macro,ControlToken* control=nullptr);
/** @brief 保持宏和输入固定对象不动，执行整站点Abacus并独立全检。
 * @param macro 前置macro_legal快照。 @param intervals 可用行区间。
 * @param config 集中配置。 @param control 暂停取消令牌。 @param sink 完整单元提交预览回调。
 * @return standard_legal快照；失败不发布部分结果，不修改输入。 */
Snapshot LegalizeStandard(const Snapshot& macro,const std::vector<RowInterval>& intervals,const Config& config,ControlToken& control,const std::function<void(const Snapshot&)>& sink={});
} // namespace placement
