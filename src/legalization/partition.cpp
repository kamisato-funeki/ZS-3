#include <placement/application/legalization.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace placement {
namespace {
/** @brief 一维物理阻挡投影。 */
struct Span {
    double left = 0; ///< 左边界，DU。
    double right = 0; ///< 右边界，DU。
};
} // namespace

/** @brief 从原行扣除宏、固定实体和硬阻挡，再按原网格内向取整；完整契约见公共声明。 */
std::vector<RowInterval> PartitionRows(const Snapshot& macro, ControlToken* control) {
    if (!macro.database || macro.datasetId != macro.database->datasetId || macro.positions.size() != macro.database->nodes.size()) Throw("INPUT_INVALID", "行切分快照身份或位置数量无效");
    if (macro.stage != "macro_legal") Throw("INPUT_INVALID", "行切分需要 macro_legal 阶段", macro.stage, true);
    if (CheckLegality(macro, "macro", 1, control).totalCount != 0) Throw("INPUT_INVALID", "宏阶段未通过独立检查，禁止切分行");
    const DesignDatabase& database = *macro.database; // 共享只读拓扑。
    const double epsilon = collisionTolerance(database); // 每轴碰撞容差，DU。
    std::vector<Rect> obstacles; // 固定节点、已合法宏和硬阻挡的只读范围，DU。
    for (const Obstacle& obstacle : database.obstacles) obstacles.push_back(obstacle.rect); // 当前硬阻挡。
    for (const Node& node : database.nodes) { // 当前真实节点，标准可移动对象不扣除。
        if (node.fixed || node.kind == NodeKind::Macro) {
            const Rect rectangle = nodeRect(node, macro.positions[node.id]); // 当前固定/宏物理框，DU。
            if (rectangle.width() > epsilon && rectangle.height() > epsilon) obstacles.push_back(rectangle);
        }
    }
    std::vector<RowInterval> intervals; // 输出非空整站点区间。
    std::size_t inspected = 0; // 障碍查询批次计数，用于取消响应。
    for (const RowSegment& row : database.rows) { // 当前原始行段。
        if (control) control->checkCancelled();
        const double siteEpsilon = siteTolerance(database, row.sitePitch); // 当前行站点比较容差，DU。
        if (std::abs(row.siteWidth - row.sitePitch) > siteEpsilon) Throw("UNSUPPORTED", "Abacus 要求 siteWidth 等于 sitePitch", std::to_string(row.id), true);
        if (row.numSites > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) Throw("UNSUPPORTED", "行站点数量超出 int64 范围", std::to_string(row.id));
        const Rect rectangle = rowRect(row); // 整条原行范围，DU。
        std::vector<Span> projections; // 与该行有实际碰撞的障碍x投影。
        for (const Rect& obstacle : obstacles) { // 当前候选障碍矩形，DU。
            if (control && inspected++ % 4096 == 0) control->checkCancelled();
            if (std::min(rectangle.uy, obstacle.uy) - std::max(rectangle.ly, obstacle.ly) <= epsilon) continue;
            const double left = std::max(rectangle.lx, obstacle.lx); // 裁剪后的障碍投影左边界，DU。
            const double right = std::min(rectangle.ux, obstacle.ux); // 裁剪后的障碍投影右边界，DU。
            if (right - left > epsilon) projections.push_back({left, right});
        }
        /** @brief 按物理左边界/右边界排序。 @param left 左投影。 @param right 右投影。 @return 稳定顺序。 */
        std::sort(projections.begin(), projections.end(), [](const Span& left, const Span& right) { return left.left < right.left || (left.left == right.left && left.right < right.right); });
        std::vector<Span> merged; // 障碍投影并区间，避免重复扣除。
        for (const Span& projection : projections) { // 当前已排序投影。
            if (merged.empty() || projection.left > merged.back().right) merged.push_back(projection);
            else merged.back().right = std::max(merged.back().right, projection.right);
        }
        /** @brief 将一个物理空闲跨度内向对齐到原始站点。
         * @param left 物理左边界，DU。 @param right 物理右边界，DU。 无返回值；非空区间追加输出。 */
        const auto appendFree = [&intervals, &row, siteEpsilon](double left, double right) {
            const long double first = std::ceil(((static_cast<long double>(left) - row.xOrigin) - siteEpsilon) / row.sitePitch); // εs仅消除边界舍入，单位站点。
            const long double last = std::floor(((static_cast<long double>(right) - row.xOrigin) + siteEpsilon) / row.sitePitch); // 排他右端站点编号。
            const std::int64_t start = static_cast<std::int64_t>(std::clamp(first, 0.0L, static_cast<long double>(row.numSites))); // 向右对齐后的首站点。
            const std::int64_t end = static_cast<std::int64_t>(std::clamp(last, 0.0L, static_cast<long double>(row.numSites))); // 向左对齐后的排他末端。
            if (end <= start) return;
            intervals.push_back({row.id, row.xOrigin, row.y, row.height, row.sitePitch, row.orientation, row.allowedOrientations, start, end});
        }; // 空闲跨度收集器，始终保持原行origin。
        double cursor = rectangle.lx; // 已扣除障碍后下一个空闲跨度的起点，DU。
        for (const Span& obstacle : merged) { // 当前合并投影。
            appendFree(cursor, obstacle.left);
            cursor = std::max(cursor, obstacle.right);
        }
        appendFree(cursor, rectangle.ux);
    }
    if (control) control->checkCancelled();
    return intervals;
}
} // namespace placement
