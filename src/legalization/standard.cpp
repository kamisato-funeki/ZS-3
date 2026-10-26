#include <placement/application/legalization.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace placement {
namespace {
/** @brief 行内紧靠簇；成员范围引用所属行的按参考x排序节点列表。 */
struct Cluster {
    std::size_t begin = 0; ///< 所属行节点列表起始索引。
    std::size_t end = 0; ///< 所属行节点列表排他末端。
    std::int64_t width = 0; ///< 整数站点总宽。
    long double weight = 0; ///< 单位权重总和，即节点数量。
    long double q = 0; ///< Σ(t参考-簇内偏移)，单位站点。
    std::int64_t start = 0; ///< 约束和整数化后的簇起点。
};
/** @brief 一个可用区间的已提交Abacus状态，由当前合法化任务独占。 */
struct RowState {
    std::vector<NodeId> ids; ///< 按(referenceX,NodeId)稳定顺序追加的单元。
    std::vector<Cluster> clusters; ///< 由左到右、彼此不重叠的紧靠簇。
    std::int64_t occupied = 0; ///< 当前使用站点总数。
    long double cost = 0; ///< 当前行完整水平及垂直位移平方和，DU²。
};
/** @brief 判断方向是否满足行允许集合。 @param orientation 输入实例方向。 @param interval 候选区间。 @return 方向是否可用。 */
bool orientationAllowed(Orientation orientation, const RowInterval& interval) {
    return interval.allowedOrientations.empty() ? orientation == interval.orientation :
        std::find(interval.allowedOrientations.begin(), interval.allowedOrientations.end(), orientation) != interval.allowedOrientations.end();
}
/** @brief 求簇最优整数起点；等距选择较小整数。
 * @param cluster 当前聚合簇，原位更新起点。 @param interval 容量边界。 无返回值；容量不足抛内部数值错误。 */
void placeCluster(Cluster& cluster, const RowInterval& interval) {
    const std::int64_t maximum = interval.endSite - cluster.width; // 当前簇最右允许起点，站点。
    if (maximum < interval.startSite) Throw("NUMERIC_FAILURE", "簇宽超过已预检区间容量");
    const long double continuous = std::clamp(cluster.q / cluster.weight, static_cast<long double>(interval.startSite), static_cast<long double>(maximum)); // 连续最优起点，站点。
    const long double lower = std::floor(continuous); // 最近整数比较的下邻居，站点。
    cluster.start = static_cast<std::int64_t>(continuous - lower > 0.5L ? lower + 1 : lower);
}
/** @brief 在候选行状态末尾追加单元并完成簇合并，调用方用值复制实现回滚。
 * @param state 候选行状态，原位修改。 @param interval 对应可用区间。
 * @param id 当前节点编号。 @param widths 按NodeId的整数站点宽。
 * @param reference 不可变macro参考快照。 @param control 取消令牌。 无返回值。 */
void appendCell(RowState& state, const RowInterval& interval, NodeId id, const std::vector<std::int64_t>& widths,
                const Snapshot& reference, ControlToken& control) {
    const std::size_t begin = state.ids.size(); // 新单元在当前行列表的索引。
    state.ids.push_back(id); state.occupied += widths[id];
    const long double target = (static_cast<long double>(reference.positions[id].x) - interval.origin) / interval.pitch; // 原始参考x换算站点坐标。
    state.clusters.push_back({begin, begin + 1, widths[id], 1, target, 0});
    placeCluster(state.clusters.back(), interval);
    while (state.clusters.size() >= 2) {
        control.checkCancelled();
        const Cluster right = state.clusters.back(); // 待与左簇比较/合并的右簇值。
        Cluster& left = state.clusters[state.clusters.size() - 2]; // 可能被合并的左簇。
        if (left.start + left.width <= right.start) break;
        // 设计07：右簇整体右移左簇宽度，Q合并需减 E右×W左，不能只平均当前位置。
        left.q += right.q - right.weight * static_cast<long double>(left.width);
        left.weight += right.weight; left.width += right.width; left.end = right.end;
        state.clusters.pop_back();
        placeCluster(state.clusters.back(), interval);
    }
    state.cost = 0;
    for (const Cluster& cluster : state.clusters) { // 当前已最优整数化的簇。
        std::int64_t cursor = cluster.start; // 当前簇内下一节点起始站点。
        for (std::size_t index = cluster.begin; index < cluster.end; ++index) { // 当前簇成员的行内索引。
            if (index % 4096 == 0) control.checkCancelled();
            const NodeId node = state.ids[index]; // 当前成员稳定编号。
            const long double dx = static_cast<long double>(interval.origin) + static_cast<long double>(cursor) * interval.pitch - reference.positions[node].x; // 参考水平位移，DU。
            const long double dy = static_cast<long double>(interval.y) - reference.positions[node].y; // 参考垂直位移，DU。
            state.cost += dx * dx + dy * dy;
            cursor += widths[node];
        }
    }
}
/** @brief 从已提交行簇恢复真实单元坐标，不修改方向或拓扑。
 * @param state 已提交行状态。 @param interval 原始可用行。 @param widths 站点宽。
 * @param positions 输出当前任务位置缓冲。 无返回值。 */
void recoverRow(const RowState& state, const RowInterval& interval, const std::vector<std::int64_t>& widths, PositionState& positions) {
    for (const Cluster& cluster : state.clusters) { // 当前已提交簇。
        std::int64_t cursor = cluster.start; // 当前簇的下一起点，站点。
        for (std::size_t index = cluster.begin; index < cluster.end; ++index) { // 簇成员索引。
            const NodeId id = state.ids[index]; // 当前成员稳定编号。
            positions[id].x = interval.origin + static_cast<double>(cursor) * interval.pitch;
            positions[id].y = interval.y;
            cursor += widths[id];
        }
    }
}
} // namespace

/** @brief 运行稳定候选顺序、整行增量比较和可撤销试放的Abacus；完整契约见公共声明。 */
Snapshot LegalizeStandard(const Snapshot& macro, const std::vector<RowInterval>& intervals, const Config& config,
                          ControlToken& control, const std::function<void(const Snapshot&)>& sink) {
    validateConfig(config); control.checkCancelled();
    if (!macro.database || macro.datasetId != macro.database->datasetId || macro.stage != "macro_legal") Throw("INPUT_INVALID", "标准合法化需要身份有效的 macro_legal 快照", macro.stage, true);
    const DesignDatabase& database = *macro.database; // 只读拓扑，标准合法化不修改固定性。
    validatePositions(database, macro.positions);
    if (CheckLegality(macro, "macro", 1, &control).totalCount != 0) Throw("INPUT_INVALID", "宏阶段独立检查失败，禁止标准合法化");
    const auto started = std::chrono::steady_clock::now(); // 单调耗时起点。
    Snapshot result = macro; // 当前任务独占候选位置，输入快照保持不变。
    result.stage = "standard_preview"; result.termination.clear(); result.legalitySummary.clear();
    result.metrics.overflow.reset(); result.metrics.densityEnergy.reset();
    if (CheckLegality(macro, "all", 1, &control).totalCount == 0) {
        control.safePoint();
        result.stage = "standard_legal"; result.termination = "completed"; result.legalitySummary = "legal";
        result.revision = macro.revision + 1; result.iteration = 0;
        result.metrics = ComputeMetrics(result, &macro); result.metrics.elapsedMs = 0;
        if (sink) sink(result);
        return result;
    }
    std::vector<RowState> states(intervals.size()); // 每个可用区间的独占已提交簇。
    std::vector<std::int64_t> widths(database.nodes.size(), 0); // 每个节点在支持站点上的整数宽度。
    std::vector<NodeId> cells; // 本次真正移动的标准单元编号。
    long double capacityArea = 0; // 所有可用区间面积，DU²。
    for (std::size_t index = 0; index < intervals.size(); ++index) { // 当前可用区间索引。
        const RowInterval& interval = intervals[index]; // 待验证可用区间。
        if (interval.rowId >= database.rows.size()) Throw("REFERENCE_MISSING", "可用区间行编号不存在");
        const RowSegment& row = database.rows[interval.rowId]; // 对应原始行基准。
        const double epsilon = siteTolerance(database, row.sitePitch); // 该行站点比较容差，DU。
        if (interval.startSite < 0 || interval.endSite <= interval.startSite || static_cast<std::uint64_t>(interval.endSite) > row.numSites ||
            interval.origin != row.xOrigin || interval.y != row.y || interval.height != row.height || interval.pitch != row.sitePitch ||
            interval.orientation != row.orientation || interval.allowedOrientations != row.allowedOrientations || std::abs(row.siteWidth - row.sitePitch) > epsilon) Throw("INPUT_INVALID", "可用行区间与原始行约束不一致", std::to_string(index));
        for (std::size_t previous = 0; previous < index; ++previous) { // 已验证区间索引，禁止重复容量。
            if (interval.rowId == intervals[previous].rowId && std::min(interval.endSite, intervals[previous].endSite) > std::max(interval.startSite, intervals[previous].startSite)) Throw("INPUT_INVALID", "可用行区间相互重叠");
        }
        capacityArea += static_cast<long double>(interval.endSite - interval.startSite) * interval.pitch * interval.height;
    }
    long double requiredArea = 0; // 可移动标准单元物理面积，DU²。
    for (const Node& node : database.nodes) { // 当前拓扑节点。
        if (node.fixed || node.kind != NodeKind::Standard) continue;
        control.checkCancelled();
        const Rect rectangle = nodeRect(node, macro.positions[node.id]); // 输入方向后的实际尺寸，DU。
        bool supported = false; // 是否属于任一原始行的单高度整数宽度支持子集。
        bool hasCapacity = false; // 是否有方向/尺寸兼容且容量足够的实际区间。
        for (const RowSegment& row : database.rows) { // 原始支持行，不受障碍切分影响。
            const double epsilon = siteTolerance(database, row.sitePitch); // 站点比较容差，DU。
            const double sites = rectangle.width() / row.sitePitch; // 当前节点宽度折合站点数。
            if (sites > static_cast<double>(std::numeric_limits<std::int64_t>::max()) || sites < 0.5 ||
                std::abs(row.siteWidth - row.sitePitch) > epsilon || std::abs(rectangle.height() - row.height) > epsilon ||
                std::abs(rectangle.width() - std::round(sites) * row.sitePitch) > epsilon) continue;
            RowInterval direction; // 临时方向视图，仅用于复用允许方向判定。
            direction.orientation = row.orientation; direction.allowedOrientations = row.allowedOrientations;
            if (orientationAllowed(macro.positions[node.id].orientation, direction)) supported = true;
        }
        if (!supported) Throw("UNSUPPORTED", "标准单元不满足单高度、整数站点宽度或行方向支持子集", node.name, true);
        for (const RowInterval& interval : intervals) { // 当前已扣障碍的可用行区间。
            const double epsilon = siteTolerance(database, interval.pitch); // 候选站点比较容差，DU。
            const double sites = rectangle.width() / interval.pitch; // 当前候选上的宽度，站点。
            if (std::abs(rectangle.height() - interval.height) <= epsilon && std::abs(rectangle.width() - std::round(sites) * interval.pitch) <= epsilon &&
                orientationAllowed(macro.positions[node.id].orientation, interval) && sites <= static_cast<double>(interval.endSite - interval.startSite) + epsilon / interval.pitch) hasCapacity = true;
        }
        if (!hasCapacity) Throw("CAPACITY_INFEASIBLE", "标准单元不存在尺寸和方向兼容且足够宽的可用区间", node.name, true);
        cells.push_back(node.id); requiredArea += rectangle.area();
    }
    if (requiredArea > capacityArea + static_cast<long double>(geometryTolerance(database)) * geometryTolerance(database)) Throw("CAPACITY_INFEASIBLE", "可用行站点面积小于标准单元总面积", "Abacus", true);
    /** @brief 以参考x及稳定ID处理标准单元。 @param left 左ID。 @param right 右ID。 @return 稳定顺序。 */
    std::sort(cells.begin(), cells.end(), [&macro](NodeId left, NodeId right) { return macro.positions[left].x < macro.positions[right].x || (macro.positions[left].x == macro.positions[right].x && left < right); });
    auto lastPreview = started - std::chrono::seconds(1); // 预览限频时间，首个完整提交允许发布。
    std::size_t accepted = 0; // 已完整提交的标准单元数量。
    for (const NodeId id : cells) { // 当前按参考x排序的标准单元。
        control.checkCancelled();
        const Rect rectangle = nodeRect(database.nodes[id], macro.positions[id]); // 当前单元方向后的尺寸，DU。
        std::vector<std::size_t> candidates(intervals.size()); // 当前单元的候选区间索引。
        std::iota(candidates.begin(), candidates.end(), std::size_t{0});
        /** @brief 先按垂直距离搜索，再以行号/起点确定稳定次序。 @param left 左候选索引。 @param right 右候选索引。 @return 搜索顺序。 */
        std::sort(candidates.begin(), candidates.end(), [&intervals, &macro, id](std::size_t left, std::size_t right) {
            return std::make_tuple(std::abs(intervals[left].y - macro.positions[id].y), intervals[left].rowId, intervals[left].startSite) <
                   std::make_tuple(std::abs(intervals[right].y - macro.positions[id].y), intervals[right].rowId, intervals[right].startSite);
        });
        std::optional<std::size_t> best; // 最佳可行候选区间，空表示尚未找到。
        RowState bestState; // 最佳候选的完整行状态，其他试放自动回滚。
        std::int64_t bestWidth = 0; // 最佳区间上的单元整数站点宽。
        long double bestCost = std::numeric_limits<long double>::infinity(); // 最佳整行新增位移平方成本，DU²。
        for (const std::size_t index : candidates) { // 当前候选区间索引。
            control.checkCancelled();
            const RowInterval& interval = intervals[index]; // 当前可用区间。
            const double epsilon = siteTolerance(database, interval.pitch); // 候选行站点比较容差，DU。
            const double sites = rectangle.width() / interval.pitch; // 当前单元宽度折合站点数。
            if (sites >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) || std::abs(rectangle.height() - interval.height) > epsilon || std::abs(rectangle.width() - std::round(sites) * interval.pitch) > epsilon || !orientationAllowed(macro.positions[id].orientation, interval)) continue;
            const std::int64_t width = static_cast<std::int64_t>(std::llround(sites)); // 当前候选整数站点宽。
            if (width <= 0 || width > interval.endSite - interval.startSite - states[index].occupied) continue;
            const long double vertical = static_cast<long double>(interval.y) - macro.positions[id].y; // 新单元垂直位移，DU。
            if (best && vertical * vertical > bestCost) continue; // 原行相同顺序解最优，新单元垂直成本是整行增量的下界。
            RowState trial = states[index]; // 候选值复制，拒绝无需撤销共享坐标。
            widths[id] = width;
            appendCell(trial, interval, id, widths, macro, control);
            const long double incremental = trial.cost - states[index].cost; // 整行新旧总代价差，包含已有单元被推动的影响。
            if (!best || incremental < bestCost || (incremental == bestCost && std::tie(interval.rowId, interval.startSite) < std::tie(intervals[*best].rowId, intervals[*best].startSite))) {
                best = index; bestCost = incremental; bestState = std::move(trial); bestWidth = width;
            }
        }
        if (!best) Throw("NO_LEGAL_SOLUTION_FOUND", "按当前稳定分配顺序未找到可行行，未证明全局不可行", database.nodes[id].name, true);
        widths[id] = bestWidth; states[*best] = std::move(bestState);
        recoverRow(states[*best], intervals[*best], widths, result.positions);
        ++accepted; result.iteration = accepted; result.revision = macro.revision + accepted;
        const auto now = std::chrono::steady_clock::now(); // 当前完整提交后的单调时间。
        if (sink && now - lastPreview >= std::chrono::duration<double>(1.0 / config.previewHz)) {
            result.metrics = ComputeMetrics(result, &macro);
            result.metrics.elapsedMs = std::chrono::duration<double, std::milli>(now - started).count();
            sink(result); lastPreview = now;
        }
        control.safePoint();
    }
    validatePositions(database, result.positions);
    for (const Node& node : database.nodes) { // 独立核对本阶段锁定的全部宏。
        if (node.kind == NodeKind::Macro && (result.positions[node.id].x != macro.positions[node.id].x || result.positions[node.id].y != macro.positions[node.id].y || result.positions[node.id].orientation != macro.positions[node.id].orientation)) Throw("NUMERIC_FAILURE", "标准合法化改变了锁定宏", node.name);
    }
    const ViolationReport report = CheckLegality(result, "all", 1, &control); // 独立最终检查不依赖簇成功标记。
    if (report.totalCount != 0) Throw("NO_LEGAL_SOLUTION_FOUND", "Abacus 候选未通过独立全量检查", report.records.empty() ? "unknown" : report.records.front().kind, true);
    control.safePoint();
    result.stage = "standard_legal"; result.termination = "completed"; result.legalitySummary = "legal";
    result.revision = std::max(result.revision, macro.revision + 1);
    result.metrics = ComputeMetrics(result, &macro);
    result.metrics.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (sink) sink(result);
    return result;
}
} // namespace placement
