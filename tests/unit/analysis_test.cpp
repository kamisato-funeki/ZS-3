#include <placement/analysis/analysis.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace placement {
namespace {
/** @brief 创建多行矩形core测试拓扑。 @param width 行宽，DU。 @param height core高，DU。 @param pitch 站点间距，DU。 @return 独占可写数据库。 */
std::shared_ptr<DesignDatabase> makeDatabase(double width = 20.0, double height = 10.0, double pitch = 1.0) {
    auto database = std::make_shared<DesignDatabase>(); // 测试独占拓扑，发布给快照时转只读。
    database->datasetId = "analysis";
    for (double y = 0; y < height; y += 2.0) { // 当前2 DU行的底边。
        RowSegment row; // 当前水平行。
        row.id = database->rows.size(); row.y = y; row.height = 2.0;
        row.siteWidth = pitch; row.sitePitch = pitch;
        row.numSites = static_cast<std::uint64_t>(std::llround(width / pitch));
        database->rows.push_back(row);
    }
    return database;
}
/** @brief 添加真实节点和基线。 @param database 目标数据库。 @param name 唯一名称。 @param kind 物理类别。 @param width 宽，DU。 @param height 高，DU。 @param x 左下横坐标，DU。 @param y 左下纵坐标，DU。 @param fixed 固定标记。 无返回值。 */
void addNode(DesignDatabase& database, const std::string& name, NodeKind kind, double width, double height, double x, double y, bool fixed = false) {
    Node node; // 依次分配稳定编号的测试节点。
    node.id = database.nodes.size(); node.name = name; node.kind = kind;
    node.width = width; node.height = height; node.fixed = fixed;
    database.nodes.push_back(node); database.originalPositions.push_back({x, y, Orientation::N});
}
/** @brief 创建引用给定数据库的快照。 @param database 只读拓扑共享指针。 @return 使用原始位置的global快照。 */
Snapshot makeSnapshot(const std::shared_ptr<DesignDatabase>& database) {
    Snapshot snapshot; // 测试独占几何快照。
    snapshot.database = database; snapshot.datasetId = database->datasetId;
    snapshot.runId = "test"; snapshot.stage = "global"; snapshot.positions = database->originalPositions;
    return snapshot;
}
/** @brief TC-VIS-03/F-A：真实偏移引脚HPWL及权重、零网络独立手算，无输入、无返回值。 */
TEST(AnalysisMetrics, RealPinHpwlAndWeights) {
    auto database = makeDatabase(); // 独立F-A拓扑。
    addNode(*database, "A", NodeKind::Standard, 2, 2, 1, 1);
    addNode(*database, "B", NodeKind::Standard, 2, 2, 7, 5);
    database->pins = {{0, 0, 0, {0.5, -0.5}}, {1, 1, 0, {-0.5, 0.5}}};
    database->nets = {{0, "pair", {0, 1}, 2.0}, {1, "empty", {}, 1.0}};
    Snapshot snapshot = makeSnapshot(database); // F-A当前位置。
    snapshot.metrics.overflow = 0.25;
    const Metrics metrics = ComputeMetrics(snapshot); // 真实线长手算10，加权20 DU。
    EXPECT_DOUBLE_EQ(*metrics.hpwl, 10.0);
    EXPECT_DOUBLE_EQ(*metrics.weightedHpwl, 20.0);
    EXPECT_DOUBLE_EQ(*metrics.overflow, 0.25);
    EXPECT_FALSE(metrics.meanDisplacement.has_value());
}
/** @brief 比较可移动节点中心位移并拒绝跨数据集，无输入、无返回值。 */
TEST(AnalysisMetrics, StageComparisonUsesCentersAndNullableImprovement) {
    auto database = makeDatabase(); // 单个可移动标准单元和固定终端。
    addNode(*database, "A", NodeKind::Standard, 2, 2, 0, 0);
    addNode(*database, "T", NodeKind::Terminal, 0, 0, 20, 20, true);
    const Snapshot before = makeSnapshot(database); // 参考阶段。
    Snapshot after = before; // 可独立修改的当前阶段。
    after.positions[0].x = 3; after.positions[0].y = 4;
    const StageComparison comparison = CompareStages(before, after); // 3-4-5三角形位移真值。
    EXPECT_DOUBLE_EQ(comparison.displacement[0], 5);
    EXPECT_DOUBLE_EQ(*comparison.after.meanDisplacement, 5);
    EXPECT_DOUBLE_EQ(*comparison.after.quadraticDisplacement, 25);
    EXPECT_FALSE(comparison.hpwlImprovement.has_value());
    after.datasetId = "other";
    EXPECT_THROW(CompareStages(before, after), Error);
}
/** @brief F-D/TC-LG-01：逐对面积、硬障碍和完整截断计数独立核对，无输入、无返回值。 */
TEST(AnalysisLegality, MacroCollisionsAreIndependentlyMeasured) {
    auto database = makeDatabase(); // F-D的20×10 core。
    addNode(*database, "M1", NodeKind::Macro, 4, 4, 6, 0);
    addNode(*database, "M2", NodeKind::Macro, 4, 4, 7, 1);
    database->obstacles.push_back({{8, 0, 12, 4}, "fixed-block"});
    const Snapshot snapshot = makeSnapshot(database); // F-D重叠输入。
    const ViolationReport report = CheckLegality(snapshot, "macro"); // 三个独立违规。
    ASSERT_EQ(report.totalCount, 3U);
    EXPECT_EQ(report.countByType.at("OVERLAP"), 1U);
    EXPECT_EQ(report.countByType.at("HARD_BLOCKAGE"), 2U);
    double overlap = 0.0; // 宏对交集面积，DU²。
    double blockage = 0.0; // 两宏与障碍面积之和，DU²。
    for (const Violation& violation : report.records) { // 逐条独立检查明细。
        if (violation.kind == "OVERLAP") overlap += violation.magnitude;
        if (violation.kind == "HARD_BLOCKAGE") blockage += violation.magnitude;
    }
    EXPECT_DOUBLE_EQ(overlap, 9.0); EXPECT_DOUBLE_EQ(blockage, 17.0);
    const ViolationReport truncated = CheckLegality(snapshot, "macro", 1); // 限制明细但不能限制总数。
    EXPECT_EQ(truncated.totalCount, 3U); EXPECT_EQ(truncated.records.size(), 1U); EXPECT_TRUE(truncated.truncated);
    EXPECT_EQ(CheckLegality(snapshot, "macro", 0).totalCount, 3U);
}
/** @brief 薄长交集按两轴εc而非面积/εg判定，非单位pitch的OFF_SITE按DU残差判断，无输入、无返回值。 */
TEST(AnalysisLegality, ThinLongOverlapAndNonUnitSiteTolerance) {
    auto database = makeDatabase(1000000, 2, 0.001); // 大core使εg=.01，εc=1e-9 DU。
    addNode(*database, "A", NodeKind::Macro, 100, 2, 0, 0);
    addNode(*database, "B", NodeKind::Macro, 100, 2, 100 - 1e-7, 0);
    EXPECT_EQ(CheckLegality(makeSnapshot(database), "macro").countByType.at("OVERLAP"), 1U);
    database->nodes.clear(); database->originalPositions.clear();
    addNode(*database, "C", NodeKind::Standard, 0.003, 2, 0.002 + 5e-10, 0);
    EXPECT_EQ(CheckLegality(makeSnapshot(database)).totalCount, 0U);
    database->originalPositions[0].x = 0.002 + 2e-9;
    EXPECT_EQ(CheckLegality(makeSnapshot(database)).countByType.at("OFF_SITE"), 1U);
}
/** @brief TC-LG-01：接边不碰撞，macro范围忽略可移动标准单元，但固定之间冲突仍报告，无输入、无返回值。 */
TEST(AnalysisLegality, ScopeAndFixedConflictsAreExplicit) {
    auto database = makeDatabase(); // 两个接边宏和一个暂时重叠的标准单元。
    addNode(*database, "M1", NodeKind::Macro, 2, 2, 0, 0);
    addNode(*database, "M2", NodeKind::Macro, 2, 2, 2, 0);
    addNode(*database, "S", NodeKind::Standard, 2, 2, 0, 0);
    EXPECT_EQ(CheckLegality(makeSnapshot(database), "macro").totalCount, 0U);
    EXPECT_EQ(CheckLegality(makeSnapshot(database)).countByType.at("OVERLAP"), 1U);
    database->nodes[0].fixed = true; database->nodes[2].fixed = true;
    EXPECT_EQ(CheckLegality(makeSnapshot(database), "macro").countByType.at("OVERLAP"), 1U);
    Snapshot changed = makeSnapshot(database); // 固定宏发生单ULP移动的非法外部快照。
    changed.positions[0].x = std::nextafter(0.0, 1.0);
    EXPECT_EQ(CheckLegality(changed, "macro").countByType.at("FIXED_CHANGED"), 1U);
}
/** @brief 行包围盒内部的暗区仍属于越界，非有限候选不会被视为合法，无输入、无返回值。 */
TEST(AnalysisLegality, AllowedRegionIsUnionOfRows) {
    auto database = makeDatabase(10, 2); // 仅一条底部行。
    RowSegment row = database->rows[0]; // 与底部行有2 DU暗区的上部行。
    row.id = 1; row.y = 4; database->rows.push_back(row);
    addNode(*database, "M", NodeKind::Macro, 2, 4, 0, 0);
    EXPECT_EQ(CheckLegality(makeSnapshot(database), "macro").countByType.at("OUT_OF_BOUNDS"), 1U);
    Snapshot invalid = makeSnapshot(database); // 无穷坐标外部结果。
    invalid.positions[0].x = std::numeric_limits<double>::infinity();
    EXPECT_EQ(CheckLegality(invalid).countByType.at("NON_FINITE"), 1U);
}
/** @brief 活动区间树与独立平方枚举对照，详情截断不影响扫描结果，无输入、无返回值。 */
TEST(AnalysisLegality, SweepLineMatchesIndependentPairEnumeration) {
    auto database = makeDatabase(200, 20); // 允许区域足够大，不产生越界。
    for (int index = 0; index < 80; ++index) { // 确定性分布的实体编号。
        addNode(*database, std::to_string(index), NodeKind::Macro, 7, 3,
                static_cast<double>((index * 17) % 180), static_cast<double>((index * 11) % 16));
    }
    std::uint64_t expected = 0; // 完全独立的轴区间枚举交集数量。
    for (std::size_t first = 0; first < database->nodes.size(); ++first) { // 无序对首索引。
        for (std::size_t second = first + 1; second < database->nodes.size(); ++second) { // 无序对次索引。
            const Position& a = database->originalPositions[first]; // 第一矩形左下角。
            const Position& b = database->originalPositions[second]; // 第二矩形左下角。
            if (std::abs(a.x - b.x) < 7 && std::abs(a.y - b.y) < 3) ++expected;
        }
    }
    const ViolationReport report = CheckLegality(makeSnapshot(database), "macro", 3); // 有界明细检查。
    EXPECT_EQ(report.countByType.at("OVERLAP"), expected);
    EXPECT_EQ(report.totalCount, expected);
    EXPECT_EQ(report.records.size(), 3U);
}
/** @brief 已请求取消的独立检查及时退出，无输入、无返回值。 */
TEST(AnalysisLegality, CancellationIsChecked) {
    auto database = makeDatabase(); // 空拓扑仍必须响应取消。
    ControlToken control; // 取消令牌。
    control.cancel();
    EXPECT_THROW(CheckLegality(makeSnapshot(database), "all", 100, &control), Error);
}
} // namespace
} // namespace placement
