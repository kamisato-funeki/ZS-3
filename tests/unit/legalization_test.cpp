#include <placement/application/legalization.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace placement {
namespace {
/** @brief 建立测试行拓扑。 @param width 每行宽度，DU。 @param ys 行y坐标列表，DU。 @param pitch 站点间距，DU。 @return 可独占修改的测试数据库。 */
std::shared_ptr<DesignDatabase> legalDatabase(double width, const std::vector<double>& ys, double pitch = 1) {
    auto database = std::make_shared<DesignDatabase>(); // 测试独占拓扑，快照以const共享。
    database->datasetId = "legal-unit";
    for (const double y : ys) { // 当前行底部纵坐标，DU。
        RowSegment row; // 当前2 DU高原始行。
        row.id = database->rows.size(); row.y = y; row.height = 2;
        row.siteWidth = pitch; row.sitePitch = pitch; row.numSites = static_cast<std::uint64_t>(std::llround(width / pitch));
        database->rows.push_back(row);
    }
    return database;
}
/** @brief 追加测试节点和原始位置。 @param database 目标数据库。 @param name 名称。
 * @param kind 类别。 @param width 未旋转宽度，DU。 @param height 未旋转高度，DU。
 * @param x 输入x，DU。 @param y 输入y，DU。 @param fixed 固定标记。 无返回值。 */
void legalNode(DesignDatabase& database, const std::string& name, NodeKind kind, double width, double height, double x, double y, bool fixed = false) {
    Node node; // 顺序编号真实节点。
    node.id = database.nodes.size(); node.name = name; node.kind = kind; node.width = width; node.height = height; node.fixed = fixed;
    database.nodes.push_back(node); database.originalPositions.push_back({x, y, Orientation::N});
}
/** @brief 构造阶段快照。 @param database 目标拓扑。 @param stage 阶段文本。 @return 引用原始几何的快照值。 */
Snapshot legalSnapshot(const std::shared_ptr<DesignDatabase>& database, const std::string& stage = "macro_legal") {
    Snapshot snapshot; // 测试独占位置快照。
    snapshot.database = database; snapshot.datasetId = database->datasetId; snapshot.runId = "test";
    snapshot.stage = stage; snapshot.positions = database->originalPositions;
    return snapshot;
}
/** @brief F-C/TC-LG-08：重叠障碍先并集，跨多行切分保留原始站点origin，无输入、无返回值。 */
TEST(LegalizationPartition, MergedObstacleCutsEveryCrossedRow) {
    auto database = legalDatabase(10, {0, 2}); // 两条接边水平行。
    database->obstacles = {{{4, 0, 6, 4}, "block"}, {{4.5, 0, 5.5, 4}, "duplicate"}};
    const std::vector<RowInterval> intervals = PartitionRows(legalSnapshot(database)); // 每行应有[0,4]与[6,10]。
    ASSERT_EQ(intervals.size(), 4U);
    EXPECT_EQ(intervals[0].startSite, 0); EXPECT_EQ(intervals[0].endSite, 4);
    EXPECT_EQ(intervals[1].startSite, 6); EXPECT_EQ(intervals[1].endSite, 10);
    EXPECT_EQ(intervals[2].rowId, 1U); EXPECT_EQ(intervals[2].endSite, 4);
    EXPECT_DOUBLE_EQ(intervals[3].origin, 0);
}
/** @brief 非单位pitch和非零origin时按原网格取整，障碍末端不能成为新origin，无输入、无返回值。 */
TEST(LegalizationPartition, NonUnitPitchKeepsOriginalGrid) {
    auto database = legalDatabase(20, {0}, 2); // 十个2 DU站点。
    database->rows[0].xOrigin = 3;
    database->obstacles.push_back({{8, 0, 12, 2}, "fractional"});
    const auto intervals = PartitionRows(legalSnapshot(database)); // 物理空闲[3,8]与[12,23]应对齐[3,7]和[13,23]。
    ASSERT_EQ(intervals.size(), 2U);
    EXPECT_EQ(intervals[0].startSite, 0); EXPECT_EQ(intervals[0].endSite, 2);
    EXPECT_EQ(intervals[1].startSite, 5); EXPECT_EQ(intervals[1].endSite, 10);
    EXPECT_DOUBLE_EQ(intervals[1].origin, 3);
}
/** @brief F-B/TC-LG-10：簇应将A/B从1/2移到0/3，平方成本为2，无输入、无返回值。 */
TEST(LegalizationStandard, AbacusMatchesHandComputedCluster) {
    auto database = legalDatabase(10, {0}); // F-B单行。
    legalNode(*database, "A", NodeKind::Standard, 3, 2, 1, 0);
    legalNode(*database, "B", NodeKind::Standard, 3, 2, 2, 0);
    const Snapshot macro = legalSnapshot(database); // 参考位置不可改写。
    ControlToken control; // 当前任务控制令牌。
    const Snapshot result = LegalizeStandard(macro, PartitionRows(macro), Config{}, control); // 完整Abacus结果。
    EXPECT_EQ(result.stage, "standard_legal");
    EXPECT_DOUBLE_EQ(result.positions[0].x, 0); EXPECT_DOUBLE_EQ(result.positions[1].x, 3);
    EXPECT_DOUBLE_EQ(*result.metrics.quadraticDisplacement, 2);
    EXPECT_EQ(CheckLegality(result).totalCount, 0U);
    EXPECT_DOUBLE_EQ(macro.positions[1].x, 2);
}
/** @brief F-I/TC-LG-12：双行候选成本与独立全部站点/行分配穷举一致，无输入、无返回值。 */
TEST(LegalizationStandard, TwoRowsMatchIndependentSmallEnumeration) {
    auto database = legalDatabase(10, {0, 4}); // F-I相隔2 DU的双行。
    legalNode(*database, "A", NodeKind::Standard, 3, 2, 1, 1);
    legalNode(*database, "B", NodeKind::Standard, 3, 2, 2, 3);
    const Snapshot macro = legalSnapshot(database); // F-I参考位置。
    long double optimum = std::numeric_limits<long double>::infinity(); // 独立站点枚举最小平方位移，DU²。
    for (int rowA = 0; rowA < 2; ++rowA) { // A候选行索引。
        for (int rowB = 0; rowB < 2; ++rowB) { // B候选行索引。
            for (int xA = 0; xA <= 7; ++xA) { // A整数起点，DU=站点。
                for (int xB = 0; xB <= 7; ++xB) { // B整数起点，DU=站点。
                    if (rowA == rowB && std::abs(xA - xB) < 3) continue;
                    const long double cost = (xA - 1) * (xA - 1) + (xB - 2) * (xB - 2) + (4 * rowA - 1) * (4 * rowA - 1) + (4 * rowB - 3) * (4 * rowB - 3); // 独立坐标平方公式。
                    optimum = std::min(optimum, cost);
                }
            }
        }
    }
    ControlToken control; // 同步任务控制。
    const Snapshot result = LegalizeStandard(macro, PartitionRows(macro), Config{}, control); // 被测双行分配。
    EXPECT_DOUBLE_EQ(*result.metrics.quadraticDisplacement, static_cast<double>(optimum));
    EXPECT_DOUBLE_EQ(result.positions[0].y, 0); EXPECT_DOUBLE_EQ(result.positions[1].y, 4);
}
/** @brief 候选比较必须包含旧单元被簇推动的成本，不能只比较新单元自己的位置，无输入、无返回值。 */
TEST(LegalizationStandard, CandidateCostIncludesAlreadyPlacedCells) {
    auto database = legalDatabase(10, {0, 4}); // 双行容量都足够，区分代价比较而非可行性。
    legalNode(*database, "A", NodeKind::Standard, 3, 2, 1, 0);
    legalNode(*database, "B", NodeKind::Standard, 3, 2, 2, 1.8);
    const Snapshot macro = legalSnapshot(database); // A已占底部行，B参考y为1.8 DU。
    ControlToken control; // 当前任务控制。
    const Snapshot result = LegalizeStandard(macro, PartitionRows(macro), Config{}, control); // 被测整行增量选择。
    // B去底行会推动A：总代价1+(1+1.8²)=5.24；去顶行总代价(4-1.8)²=4.84。
    EXPECT_DOUBLE_EQ(result.positions[0].x, 1);
    EXPECT_DOUBLE_EQ(result.positions[1].x, 2);
    EXPECT_DOUBLE_EQ(result.positions[1].y, 4);
    EXPECT_NEAR(*result.metrics.quadraticDisplacement, 4.84, 1e-12);
}
/** @brief 小单行固定顺序的整数簇结果，对全部可行站点排列作独立穷举，无输入、无返回值。 */
TEST(LegalizationStandard, IntegerClustersMatchExhaustiveOrderConstrainedOptima) {
    for (int sample = 0; sample < 35; ++sample) { // 确定性参考坐标/宽度组合编号。
        auto database = legalDatabase(9, {0}); // 九站点短行，适合完全穷举。
        const int widthA = 1 + sample % 2; // A物理宽度，整数DU。
        const int widthB = 1 + (sample / 2) % 2; // B物理宽度，整数DU。
        const int widthC = 1 + (sample / 4) % 2; // C物理宽度，整数DU。
        const double targetA = 0.25 * (sample % 5); // 按参考x严格递增的A目标，DU。
        const double targetB = targetA + 0.25 + 0.25 * ((sample / 3) % 5); // B参考目标，DU。
        const double targetC = targetB + 0.25 + 0.25 * ((sample / 5) % 5); // C参考目标，DU。
        legalNode(*database, "A", NodeKind::Standard, widthA, 2, targetA, 0);
        legalNode(*database, "B", NodeKind::Standard, widthB, 2, targetB, 0);
        legalNode(*database, "C", NodeKind::Standard, widthC, 2, targetC, 0);
        long double optimum = std::numeric_limits<long double>::infinity(); // 完全枚举的独立最优平方成本，DU²。
        for (int xA = 0; xA + widthA <= 9; ++xA) { // A可行起点。
            for (int xB = xA + widthA; xB + widthB <= 9; ++xB) { // B保持参考顺序的可行起点。
                for (int xC = xB + widthB; xC + widthC <= 9; ++xC) { // C保持参考顺序的可行起点。
                    const long double dxA = xA - targetA; // A位移，DU。
                    const long double dxB = xB - targetB; // B位移，DU。
                    const long double dxC = xC - targetC; // C位移，DU。
                    optimum = std::min(optimum, dxA * dxA + dxB * dxB + dxC * dxC);
                }
            }
        }
        const Snapshot macro = legalSnapshot(database); // 当前组合的不可变参考。
        ControlToken control; // 当前组合独立令牌。
        const Snapshot result = LegalizeStandard(macro, PartitionRows(macro), Config{}, control); // 被测整数簇解。
        EXPECT_NEAR(*result.metrics.quadraticDisplacement, static_cast<double>(optimum), 1e-12) << sample;
    }
}
/** @brief F-C/TC-LG-09：总空白足够但任何单区间都容纳不了宽5单元，应明确容量失败，无输入、无返回值。 */
TEST(LegalizationStandard, FragmentedCapacityDoesNotImplyFit) {
    auto database = legalDatabase(10, {0}); // F-C单行。
    database->obstacles.push_back({{4, 0, 6, 2}, "middle"});
    legalNode(*database, "wide", NodeKind::Standard, 5, 2, 1, 0);
    const Snapshot macro = legalSnapshot(database); // 该标准单元不进入macro硬约束。
    ControlToken control; // 当前任务令牌。
    try { LegalizeStandard(macro, PartitionRows(macro), Config{}, control); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code, "CAPACITY_INFEASIBLE"); } // 依据单区间宽度给出的必要条件证据。
}
/** @brief TC-LG-14/15：拒绝多高度，已合法输入逐坐标精确保留且宏和固定不动，无输入、无返回值。 */
TEST(LegalizationStandard, SupportedSubsetAndIdempotence) {
    auto database = legalDatabase(12, {0, 2}); // 支持两行的core。
    legalNode(*database, "M", NodeKind::Macro, 2, 4, 8, 0);
    legalNode(*database, "F", NodeKind::Terminal, 0, 0, -3, 1, true);
    legalNode(*database, "S", NodeKind::Standard, 3, 2, 1, 0);
    const Snapshot macro = legalSnapshot(database); // 本身已完整合法。
    ControlToken control; // 第一遍令牌。
    const Snapshot first = LegalizeStandard(macro, PartitionRows(macro), Config{}, control); // 幂等快速路径。
    for (std::size_t index = 0; index < macro.positions.size(); ++index) { // 所有节点逐项精确核对。
        EXPECT_DOUBLE_EQ(first.positions[index].x, macro.positions[index].x);
        EXPECT_DOUBLE_EQ(first.positions[index].y, macro.positions[index].y);
    }
    Snapshot repeated = first; // 将已合法几何作为新macro阶段基线。
    repeated.stage = "macro_legal";
    const Snapshot second = LegalizeStandard(repeated, PartitionRows(repeated), Config{}, control); // 第二遍合法化。
    EXPECT_DOUBLE_EQ(second.positions[2].x, first.positions[2].x);
    database->nodes[2].height = 3;
    const Snapshot unsupported = legalSnapshot(database); // 高度3不匹配任何2 DU单行。
    EXPECT_THROW(LegalizeStandard(unsupported, PartitionRows(unsupported), Config{}, control), Error);
}
/** @brief F-D/TC-LG-05：SA实际消除宏对及固定障碍冲突，种子可复现，输入和标准位置不变，无输入、无返回值。 */
TEST(LegalizationMacros, AnnealingFindsIndependentlyLegalReproducibleGeometry) {
    auto database = legalDatabase(20, {0, 2, 4, 6, 8}); // F-D允许区域。
    database->obstacles.push_back({{8, 0, 12, 4}, "fixed"});
    legalNode(*database, "M1", NodeKind::Macro, 4, 4, 6, 0);
    legalNode(*database, "M2", NodeKind::Macro, 4, 4, 7, 1);
    legalNode(*database, "S", NodeKind::Standard, 2, 2, 2, 1);
    database->pins = {{0, 0, 0, {0.5, -0.5}}, {1, 0, 0, {-0.5, 0.5}},
        {2, 1, 0, {1, 0}}, {3, 2, 0, {0, 0}}, {4, 0, 1, {0, 0}}, {5, 1, 1, {0, 0}}};
    database->nets = {{0, "shared-multiple-pins", {0, 1, 2, 3}, 2.0}, {1, "macro-pair", {4, 5}, 0.5}};
    const Snapshot global = legalSnapshot(database, "global"); // 碰撞global输入。
    Config config; // 缩短测试预算，仍保留完整温度/η更新逻辑。
    config.macroSweeps = 40; config.movesPerMacro = 15;
    ControlToken control; // 第一遍控制令牌。
    const Snapshot first = LegalizeMacros(global, config, control); // 第一遍真实SA结果。
    EXPECT_EQ(first.stage, "macro_legal"); EXPECT_EQ(CheckLegality(first, "macro").totalCount, 0U);
    EXPECT_DOUBLE_EQ(first.positions[2].x, global.positions[2].x); EXPECT_DOUBLE_EQ(first.positions[2].y, global.positions[2].y);
    EXPECT_DOUBLE_EQ(global.positions[0].x, 6);
    ControlToken repeatedControl; // 第二遍独立令牌。
    const Snapshot second = LegalizeMacros(global, config, repeatedControl); // 同种子重复运行。
    for (std::size_t index = 0; index < first.positions.size(); ++index) { // 忽略耗时，几何须逐项一致。
        EXPECT_DOUBLE_EQ(first.positions[index].x, second.positions[index].x);
        EXPECT_DOUBLE_EQ(first.positions[index].y, second.positions[index].y);
    }
}
/** @brief TC-LG-06：固定碰撞与超大宏给明确必要条件失败，预算无解使用搜索失败代码，无输入、无返回值。 */
TEST(LegalizationMacros, ProvenInfeasibilityAndSearchFailureStayDistinct) {
    auto database = legalDatabase(10, {0, 2}); // 小core。
    legalNode(*database, "fixedA", NodeKind::Macro, 3, 2, 0, 0, true);
    legalNode(*database, "fixedB", NodeKind::Macro, 3, 2, 1, 0, true);
    ControlToken control; // 当前任务令牌。
    try { LegalizeMacros(legalSnapshot(database, "global"), Config{}, control); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code, "CAPACITY_INFEASIBLE"); } // 固定冲突无法通过任何宏移动修复。
    database->nodes.clear(); database->originalPositions.clear();
    legalNode(*database, "oversized", NodeKind::Macro, 11, 2, 0, 0);
    try { LegalizeMacros(legalSnapshot(database, "global"), Config{}, control); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code, "CAPACITY_INFEASIBLE"); } // 物理宽度超core的确定证据。
    database = legalDatabase(10, {0, 4}); // 暗区把允许区域分成两条2 DU高行，3 DU高宏无处可容纳。
    legalNode(*database, "gap", NodeKind::Macro, 1, 3, 1, 0);
    Config tiny; // 面积必要条件充足，但仅预算搜索不能证明不可行。
    tiny.macroSweeps = 1; tiny.movesPerMacro = 1;
    try { LegalizeMacros(legalSnapshot(database, "global"), tiny, control); FAIL(); }
    catch (const Error& error) { EXPECT_EQ(error.code, "NO_LEGAL_SOLUTION_FOUND"); }
}
/** @brief TC-LG-07：已合法宏精确保留坐标并发布阶段，取消不发布结果，无输入、无返回值。 */
TEST(LegalizationMacros, AlreadyLegalGeometryIsPreservedAndCancellationHonored) {
    auto database = legalDatabase(10, {0, 2}); // 合法宏输入。
    legalNode(*database, "M", NodeKind::Macro, 2, 4, 1.25, 0);
    const Snapshot global = legalSnapshot(database, "global"); // 参考完整几何。
    ControlToken control; // 正常任务令牌。
    const Snapshot result = LegalizeMacros(global, Config{}, control); // 应直接发布相同几何。
    EXPECT_DOUBLE_EQ(result.positions[0].x, 1.25); EXPECT_EQ(result.iteration, 0U);
    control.cancel(); EXPECT_THROW(LegalizeMacros(global, Config{}, control), Error);
}
/** @brief 标准单元完整提交预览后取消，不发布成功阶段且输入完全不动，无输入、无返回值。 */
TEST(LegalizationStandard, CancellationAfterPreviewDoesNotPublishSuccess) {
    auto database = legalDatabase(10, {0}); // 必须执行真实簇合并的重叠输入。
    legalNode(*database, "A", NodeKind::Standard, 3, 2, 1, 0);
    legalNode(*database, "B", NodeKind::Standard, 3, 2, 2, 0);
    const Snapshot macro = legalSnapshot(database); // 不可变输入。
    ControlToken control; // 由预览回调协作取消。
    std::size_t previews = 0; // 已接收到的完整提交预览数。
    /** @brief 观察完整提交后请求取消。 @param snapshot 完整预览。 无返回值。 */
    const auto sink = [&control, &previews](const Snapshot& snapshot) {
        ++previews;
        EXPECT_EQ(snapshot.stage, "standard_preview");
        control.cancel();
    }; // 同步回调，禁止出现standard_legal成功发布。
    EXPECT_THROW(LegalizeStandard(macro, PartitionRows(macro), Config{}, control, sink), Error);
    EXPECT_EQ(previews, 1U);
    EXPECT_DOUBLE_EQ(macro.positions[0].x, 1);
    EXPECT_DOUBLE_EQ(macro.positions[1].x, 2);
}
} // namespace
} // namespace placement
