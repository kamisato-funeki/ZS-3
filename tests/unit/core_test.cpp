#include "placement/core/model.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <string>

namespace placement {
namespace {

/** @brief 构造一个可手算的单行、固定终端数据库；无参数。
 * @return 行 [0,10]×[0,2]，固定零面积终端 (-3,7) 的独立测试值。 */
DesignDatabase smallDatabase() {
    DesignDatabase database; // 每个测试独占的最小有效数据库。
    database.datasetId = "core-unit";
    Node terminal; // core 外零面积固定终端。
    terminal.id = 0;
    terminal.name = "port";
    terminal.kind = NodeKind::Terminal;
    terminal.fixed = true;
    database.nodes.push_back(terminal);
    database.originalPositions.push_back({-3.0, 7.0, Orientation::N});
    RowSegment row; // 十个 1 DU 站点构成的 2 DU 高行段。
    row.height = 2.0;
    row.siteWidth = 1.0;
    row.sitePitch = 1.0;
    row.numSites = 10;
    database.rows.push_back(row);
    return database;
}

/** @brief TC-IN-09：逐方向对照手工变换矩阵和中心坐标，无输入、无返回值。 */
TEST(CoreGeometry, EightOrientationsMatchIndependentCoordinates) {
    const std::array<Orientation, 8> orientations{{Orientation::N, Orientation::S, Orientation::W, Orientation::E,
        Orientation::FN, Orientation::FS, Orientation::FW, Orientation::FE}}; // 设计 04 八方向顺序。
    const std::array<Vec2, 8> expectedOffsets{{{1.0, 2.0}, {-1.0, -2.0}, {-2.0, 1.0}, {2.0, -1.0},
        {-1.0, 2.0}, {1.0, -2.0}, {2.0, 1.0}, {-2.0, -1.0}}}; // 纸面矩阵作用于 (1,2) 的真值，DU。
    const std::array<Vec2, 8> expectedPins{{{13.0, 25.0}, {11.0, 21.0}, {11.0, 23.0}, {15.0, 21.0},
        {11.0, 25.0}, {13.0, 21.0}, {15.0, 23.0}, {11.0, 21.0}}}; // master 4×6、左下角(10,20) 的全局引脚真值，DU。
    Node node; // 原始 master 矩形宽 4、高 6 DU。
    node.width = 4.0;
    node.height = 6.0;
    for (std::size_t index = 0; index < orientations.size(); ++index) { // 对照表索引。
        const Vec2 offset = transformOffset({1.0, 2.0}, orientations[index]); // 待验证变换偏移，DU。
        EXPECT_DOUBLE_EQ(offset.x, expectedOffsets[index].x);
        EXPECT_DOUBLE_EQ(offset.y, expectedOffsets[index].y);
        const Vec2 pin = pinPosition(node, {10.0, 20.0, orientations[index]}, {1.0, 2.0}); // 待验证全局引脚点，DU。
        EXPECT_DOUBLE_EQ(pin.x, expectedPins[index].x);
        EXPECT_DOUBLE_EQ(pin.y, expectedPins[index].y);
        const Rect rectangle = nodeRect(node, {10.0, 20.0, orientations[index]}); // 待验证物理包围盒，DU。
        EXPECT_DOUBLE_EQ(rectangle.area(), 24.0);
        EXPECT_EQ(parseOrientation(orientationName(orientations[index])), orientations[index]);
    }
    EXPECT_THROW(parseOrientation("R90"), Error);
    EXPECT_THROW(orientationName(static_cast<Orientation>(99)), Error);
}

/** @brief TC-GP-05/TC-LG-01：并集覆盖重叠、包含、接边和退化矩形，无输入、无返回值。 */
TEST(CoreGeometry, UnionAreaNeverDoubleCounts) {
    const std::vector<Rect> rectangles{{0, 0, 2, 2}, {1, 1, 3, 3}, {0.5, 0.5, 1.5, 1.5},
        {3, 0, 4, 3}, {4, 1, 4, 8}}; // 两个面积4矩形交1，加接边面积3矩形，总面积10 DU²。
    EXPECT_DOUBLE_EQ(unionArea(rectangles), 10.0);
    EXPECT_DOUBLE_EQ(unionArea({}), 0.0);
    EXPECT_DOUBLE_EQ(unionArea({{0, 0, 1, 1}, {1, 0, 2, 1}}), 2.0);
    EXPECT_THROW(unionArea({{1, 0, 0, 1}}), Error);
    EXPECT_THROW(unionArea({{0, 0, std::numeric_limits<double>::infinity(), 1}}), Error);
}

/** @brief TC-VIS-02：显示区包含 core 外零面积终端，不把原点强加到无行输入，无输入、无返回值。 */
TEST(CoreGeometry, ChipBoundsIncludeZeroAreaTerminal) {
    DesignDatabase database = smallDatabase(); // 有 core 和 core 外端口的独立输入。
    const Rect core = coreBounds(database); // 预期 [0,10]×[0,2]，DU。
    EXPECT_DOUBLE_EQ(core.width(), 10.0);
    EXPECT_DOUBLE_EQ(core.height(), 2.0);
    const Rect chip = chipBounds(database); // 包含终端的显示范围，DU。
    EXPECT_DOUBLE_EQ(chip.lx, -3.0);
    EXPECT_DOUBLE_EQ(chip.uy, 7.0);
    database.rows.clear();
    const Rect terminalOnly = chipBounds(database); // 无行时的单点显示范围，DU。
    EXPECT_DOUBLE_EQ(terminalOnly.lx, -3.0);
    EXPECT_DOUBLE_EQ(terminalOnly.ux, -3.0);
    EXPECT_DOUBLE_EQ(terminalOnly.ly, 7.0);
}

/** @brief TC-LG-01/03 的容差基础：大 core 配细 pitch 时碰撞与站点容差一致，无输入、无返回值。 */
TEST(CoreGeometry, CollisionToleranceRespectsMinimumPitch) {
    DesignDatabase database = smallDatabase(); // 通过站点数量制造大 core 尺度的测试输入。
    database.rows[0].numSites = 100000000;
    EXPECT_DOUBLE_EQ(geometryTolerance(database), 1.0);
    EXPECT_DOUBLE_EQ(siteTolerance(database, 1e-3), 1e-9);
    EXPECT_DOUBLE_EQ(collisionTolerance(database), 1e-6);
    database.rows[0].siteWidth = 1e-3;
    database.rows[0].sitePitch = 1e-3;
    EXPECT_DOUBLE_EQ(collisionTolerance(database), 1e-9);
    database.rows.clear();
    EXPECT_DOUBLE_EQ(collisionTolerance(database), 1e-8);
    EXPECT_THROW(siteTolerance(database, 0.0), Error);
}

/** @brief TC-LG-03：固定性拒绝小于任何几何容差的单 ULP 移动，无输入、无返回值。 */
TEST(CoreValidation, FixedCoordinatesAreComparedExactly) {
    const DesignDatabase database = smallDatabase(); // 原始固定端口坐标为 (-3,7) DU。
    PositionState positions = database.originalPositions; // 独立候选位置缓冲。
    EXPECT_NO_THROW(validateDatabase(database));
    EXPECT_NO_THROW(validatePositions(database, positions));
    positions[0].x = std::nextafter(positions[0].x, 0.0);
    try {
        validatePositions(database, positions);
        FAIL() << "固定坐标的单 ULP 改动应拒绝";
    } catch (const Error& error) { // 保留可机器判断的错误信息。
        EXPECT_EQ(error.code, "FIXED_CHANGED");
        EXPECT_EQ(error.context, "port");
    }
}

/** @brief TC-IN-03：验证双向网络引用及稳定 ID，拒绝重复归属，无输入、无返回值。 */
TEST(CoreValidation, PinTopologyMustBeBidirectionalAndUnique) {
    DesignDatabase database = smallDatabase(); // 加入单引脚网络的最小拓扑。
    Pin pin; // 归属节点0、网络0的中心引脚。
    database.pins.push_back(pin);
    Net net; // 单引脚网络，线长虽为0但引用必须完整。
    net.name = "net";
    net.pinIds.push_back(0);
    database.nets.push_back(net);
    EXPECT_NO_THROW(validateDatabase(database));
    database.nets[0].pinIds.push_back(0);
    EXPECT_THROW(validateDatabase(database), Error);
    database.nets[0].pinIds.clear();
    EXPECT_THROW(validateDatabase(database), Error);
}

/** @brief TC-IN-10：准备前允许可移动缺失输入，固定对象永不允许缺失，无输入、无返回值。 */
TEST(CoreValidation, MissingMovableInputIsExplicitlyAllowedBeforePreparation) {
    DesignDatabase database = smallDatabase(); // 将终端改为可移动仅验证缺失位置模式。
    database.nodes[0].fixed = false;
    database.originalPositions[0].x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_NO_THROW(validateDatabase(database, false));
    EXPECT_THROW(validateDatabase(database), Error);
    database.nodes[0].fixed = true;
    EXPECT_THROW(validateDatabase(database, false), Error);
}

/** @brief TC-UI-03：默认值有效，网格、单位和关系约束逐项拒绝，无输入、无返回值。 */
TEST(CoreValidation, ConfigurationHasValidatedDefaults) {
    Config config; // 设计契约的集中默认配置。
    EXPECT_NO_THROW(validateConfig(config));
    config.gridNx = 32;
    EXPECT_THROW(validateConfig(config), Error);
    config.gridNy = 64;
    EXPECT_NO_THROW(validateConfig(config));
    config.gridNx = 30;
    EXPECT_THROW(validateConfig(config), Error);
    config.gridNx = 32;
    config.targetDensity = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(validateConfig(config), Error);
    config.targetDensity = 0.8;
    config.micronsPerDu = 0.0;
    EXPECT_THROW(validateConfig(config), Error);
}

/** @brief TC-UI-09：确认暂停屏障后取消必须唤醒；不使用任意 sleep，无输入、无返回值。 */
TEST(CoreControl, CancellationWakesConfirmedPause) {
    ControlToken token; // 主测试线程控制、单个异步工作线程等待的令牌。
    token.requestPause();
    std::promise<void> paused; // 工作线程到达完整安全点的确认屏障。
    std::future<void> pausedFuture = paused.get_future(); // 主线程等待暂停确认的句柄。
    /** @brief 模拟完整提交后的工作线程。 @return 捕获的业务终态代码。 */
    std::future<std::string> worker = std::async(std::launch::async, [&token, &paused] {
        try {
            /** @brief 发布已到达安全点的确认；无输入、无返回值。 */
            token.safePoint([&paused] { paused.set_value(); });
            return std::string("returned");
        } catch (const Error& error) { // 工作线程按统一错误代码报告终态。
            return error.code;
        }
    });
    const std::future_status confirmation = pausedFuture.wait_for(std::chrono::seconds(3)); // 只作死锁诊断期限，不用于同步猜测。
    token.cancel(); // 无论确认是否超时都清理等待，保证测试失败也不会遗留暂停线程。
    EXPECT_EQ(confirmation, std::future_status::ready);
    EXPECT_EQ(worker.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(worker.get(), "CANCELLED");
    EXPECT_TRUE(token.isCancelled());
    EXPECT_FALSE(token.isPauseRequested());
    token.requestPause();
    EXPECT_FALSE(token.isPauseRequested());
    EXPECT_THROW(token.safePoint(), Error);
}

/** @brief TC-UI-08/09：确认回调可重入取消，不持令牌锁且不丢失等待前通知，无输入、无返回值。 */
TEST(CoreControl, PauseCallbackCanCancelBeforeWaitBegins) {
    ControlToken token; // 在同一线程验证回调锁释放及取消优先级。
    token.requestPause();
    /** @brief 确认时请求取消；无输入、无返回值。 */
    EXPECT_THROW(token.safePoint([&token] { token.cancel(); }), Error);
}

/** @brief TC-UI-08：恢复可唤醒已确认暂停，完成后无取消错误，无输入、无返回值。 */
TEST(CoreControl, ResumeWakesConfirmedPause) {
    ControlToken token; // 可恢复暂停令牌。
    std::promise<void> paused; // 确认安全点的同步屏障。
    std::future<void> confirmation = paused.get_future(); // 主线程确认等待句柄。
    token.requestPause();
    /** @brief 在安全点等待，恢复后正常返回；无输入、无返回值。 */
    std::future<void> worker = std::async(std::launch::async, [&token, &paused] {
        /** @brief 确认已进入完整安全点；无输入、无返回值。 */
        token.safePoint([&paused] { paused.set_value(); });
    });
    const std::future_status status = confirmation.wait_for(std::chrono::seconds(3)); // 死锁诊断期限。
    token.resume();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(worker.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_NO_THROW(worker.get());
    EXPECT_FALSE(token.isCancelled());
}

} // namespace
} // namespace placement
