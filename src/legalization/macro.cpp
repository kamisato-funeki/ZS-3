#include <placement/application/legalization.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

namespace placement {
namespace {
/** @brief 求非负矩形交集。 @param left 第一个矩形。 @param right 第二个矩形。 @return 半开交集，DU。 */
Rect clipped(const Rect& left, const Rect& right) {
    const double lx = std::max(left.lx, right.lx); // 交集左边界，DU。
    const double ly = std::max(left.ly, right.ly); // 交集下边界，DU。
    return {lx, ly, std::max(lx, std::min(left.ux, right.ux)), std::max(ly, std::min(left.uy, right.uy))};
}
/** @brief 静态矩形按y下界排序的空间查询索引；只读数据由当前任务拥有。 */
class StaticGeometry {
    std::vector<Rect> rectangles_; ///< 按ly排序的矩形范围，DU。
    double maximumHeight_ = 0; ///< 索引矩形最大高度，DU，用于定位最早可能相交下界。
public:
    /** @brief 建立静态空间索引。 @param rectangles 原始矩形值数组，移入本索引。 */
    explicit StaticGeometry(std::vector<Rect> rectangles) : rectangles_(std::move(rectangles)) {
        /** @brief 按y下界及x下界排序。 @param left 左范围。 @param right 右范围。 @return 确定性顺序。 */
        std::sort(rectangles_.begin(), rectangles_.end(), [](const Rect& left, const Rect& right) { return left.ly < right.ly || (left.ly == right.ly && left.lx < right.lx); });
        for (const Rect& rectangle : rectangles_) maximumHeight_ = std::max(maximumHeight_, rectangle.height()); // 当前矩形更新最大高度。
    }
    /** @brief 查询与范围正面积相交的裁剪矩形。 @param query 查询范围，DU。 @return 原矩形交集列表，DU。 */
    std::vector<Rect> intersections(const Rect& query) const {
        std::vector<Rect> result; // 当前查询裁剪输出。
        /** @brief 定位最早可能相交的下界。 @param rectangle 当前矩形。 @param y 待搜索纵坐标。 @return 下界是否更小。 */
        auto current = std::lower_bound(rectangles_.begin(), rectangles_.end(), query.ly - maximumHeight_, [](const Rect& rectangle, double y) { return rectangle.ly < y; }); // 首个可能相交位置。
        for (; current != rectangles_.end() && current->ly < query.uy; ++current) {
            const Rect intersection = clipped(query, *current); // 当前真实交集，DU。
            if (intersection.width() > 0 && intersection.height() > 0) result.push_back(intersection);
        }
        return result;
    }
};
/** @brief SA缓存的真实代价分量，权重变化时重新组合，不复用旧C值。 */
struct Components {
    double hpwl = 0; ///< 全图加权真实HPWL，DU。
    double displacement = 0; ///< 可移动宏相对global的中心位移平方和，DU²。
    double overlap = 0; ///< 宏对碰撞面积加宏与静态非法区面积，DU²。
};
/** @brief 当前候选对一个受影响网络的完整新线长。 */
struct NetUpdate {
    NetId id = 0; ///< 受影响网络稳定编号。
    double hpwl = 0; ///< 新加权HPWL，DU。
};
/** @brief 可撤销候选的缓存增量，尚未提交时不覆盖全图缓存。 */
struct Trial {
    NodeId id = 0; ///< 被移动宏编号。
    Position position; ///< 候选左下角与保持不变的方向。
    Components values; ///< 候选全图代价分量。
    std::vector<NetUpdate> nets; ///< 受影响网络的新值，用于接受后提交。
};
/** @brief 一次宏SA任务的私有空间索引、网络缓存及几何状态，不跨线程共享。 */
class MacroState {
    const Snapshot& reference_; ///< 不变global基线，生命周期覆盖任务。
    const DesignDatabase& database_; ///< 不变拓扑引用。
    ControlToken& control_; ///< 协作取消令牌，由调用方持有。
    StaticGeometry rows_; ///< 允许行并集的静态空间查询。
    StaticGeometry fixed_; ///< 固定实体和硬阻挡的静态空间查询。
    std::vector<std::vector<NetId>> adjacency_; ///< 按NodeId索引的去重相邻网络。
    std::vector<double> netHpwl_; ///< 按NetId索引的已提交加权线长，DU。
    std::multimap<double, NodeId> spatial_; ///< 可移动宏的当前lx空间索引。
    std::vector<std::multimap<double, NodeId>::iterator> handles_; ///< 各宏在动态空间索引中的迭代器。
    double maximumWidth_ = 0; ///< 所有可移动宏最大宽度，DU，用于x查询下界。
    /** @brief 将宏位置和动态索引一起改为候选，不改线长缓存。
     * @param id 被移动宏。 @param position 新位置。 无返回值；使用node_handle重挂避免回滚分配。 */
    void move(NodeId id, const Position& position) {
        auto handle = spatial_.extract(handles_[id]); // 拥有原空间树节点，无需重新分配。
        handle.key() = position.x;
        handles_[id] = spatial_.insert(std::move(handle));
        snapshot.positions[id] = position;
    }
    /** @brief 当前宏与所有其他可移动宏的交集面积和。
     * @param id 当前宏编号。 @return 当前宏相关宏对交集面积，DU²，固定宏由静态部分处理。 */
    double pairOverlap(NodeId id) const {
        const Rect rectangle = nodeRect(database_.nodes[id], snapshot.positions[id]); // 当前宏物理框，DU。
        double area = 0; // 当前宏相关碰撞面积，DU²。
        for (auto current = spatial_.lower_bound(rectangle.lx - maximumWidth_); current != spatial_.end() && current->first < rectangle.ux; ++current) { // 可能x相交的宏索引迭代器。
            if (current->second != id) area += clipped(rectangle, nodeRect(database_.nodes[current->second], snapshot.positions[current->second])).area();
        }
        return area;
    }
    /** @brief 当前宏覆盖静态非法区域的并集面积，暗区/固定/硬阻挡不双计。
     * @param id 当前宏编号。 @return area(M\P)+area(M∩P∩F)，DU²。 */
    double staticOverlap(NodeId id) const {
        const Rect rectangle = nodeRect(database_.nodes[id], snapshot.positions[id]); // 当前宏物理框，DU。
        std::vector<Rect> allowed = rows_.intersections(rectangle); // M∩P的矩形表示，DU。
        const std::vector<Rect> blocked = fixed_.intersections(rectangle); // M∩F的矩形表示，DU。
        const double blockedArea = unionArea(blocked); // M内静态阻挡并集面积，DU²。
        allowed.insert(allowed.end(), blocked.begin(), blocked.end());
        // area(M)+area(M∩F)-area(M∩(P∪F))，避免暗区和固定障碍重复处罚。
        return std::max(0.0, rectangle.area() + blockedArea - unionArea(allowed));
    }
    /** @brief 当前宏相对global的中心位移平方。
     * @param id 当前宏。 @return 位移平方，DU²；方向固定所以等于左下角差平方。 */
    double displacement(NodeId id) const {
        const double dx = snapshot.positions[id].x - reference_.positions[id].x; // 宏横向位移，DU。
        const double dy = snapshot.positions[id].y - reference_.positions[id].y; // 宏纵向位移，DU。
        return dx * dx + dy * dy;
    }
    /** @brief 重新计算一个网络真实加权HPWL。 @param id 网络编号。 @return 加权HPWL，DU。 */
    double netLength(NetId id) const {
        const Net& net = database_.nets[id]; // 当前受影响网络。
        if (net.pinIds.size() < 2) return 0;
        double minX = std::numeric_limits<double>::infinity(); // 引脚最小x，DU。
        double minY = std::numeric_limits<double>::infinity(); // 引脚最小y，DU。
        double maxX = -std::numeric_limits<double>::infinity(); // 引脚最大x，DU。
        double maxY = -std::numeric_limits<double>::infinity(); // 引脚最大y，DU。
        std::size_t processed = 0; // 当前网络取消检查计数。
        for (const PinId pin : net.pinIds) { // 当前网络引脚编号。
            if (processed++ % 4096 == 0) control_.checkCancelled();
            const Vec2 point = pinPosition(database_, snapshot.positions, database_.pins.at(pin)); // 实际方向引脚坐标，DU。
            minX = std::min(minX, point.x); minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x); maxY = std::max(maxY, point.y);
        }
        const double result = net.weight * ((maxX - minX) + (maxY - minY)); // 网络加权线长，DU。
        if (!std::isfinite(result)) Throw("NUMERIC_FAILURE", "宏代价网络线长非有限", net.name);
        return result;
    }
public:
    Snapshot snapshot; ///< 当前完整接受几何；试算期间只在工作线程内部短暂变化。
    Components values; ///< 当前完整接受几何的全图缓存。
    std::vector<NodeId> macros; ///< 可移动宏稳定编号，不包含固定宏。
    /** @brief 初始化任务独占缓存。 @param reference global快照。 @param control 任务控制。 @param rows 原始行矩形。 @param fixed 固定实体/硬阻挡矩形。 */
    MacroState(const Snapshot& reference, ControlToken& control, std::vector<Rect> rows, std::vector<Rect> fixed)
        : reference_(reference), database_(*reference.database), control_(control), rows_(std::move(rows)), fixed_(std::move(fixed)),
          adjacency_(database_.nodes.size()), netHpwl_(database_.nets.size()), handles_(database_.nodes.size()), snapshot(reference) {
        snapshot.stage = "macro_preview"; snapshot.termination.clear(); snapshot.legalitySummary.clear();
        snapshot.metrics.overflow.reset(); snapshot.metrics.densityEnergy.reset();snapshot.metrics.lambda.reset();snapshot.metrics.gamma.reset();snapshot.metrics.step.reset();
        for (const Node& node : database_.nodes) { // 当前拓扑节点，仅可移动宏加入优化/动态索引。
            if (node.kind == NodeKind::Macro && !node.fixed) {
                macros.push_back(node.id); handles_[node.id] = spatial_.emplace(snapshot.positions[node.id].x, node.id);
                maximumWidth_ = std::max(maximumWidth_, nodeRect(node, snapshot.positions[node.id]).width());
            }
        }
        for (const Pin& pin : database_.pins) adjacency_[pin.nodeId].push_back(pin.netId); // 建立节点到网络的邻接。
        for (std::vector<NetId>& adjacent : adjacency_) { // 当前节点相邻网络去重，防止多引脚网络重复更新。
            std::sort(adjacent.begin(), adjacent.end()); adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
        }
        for (const Net& net : database_.nets) netHpwl_[net.id] = netLength(net.id); // 初始化完整网络缓存。
        values = fullValues();
    }
    /** @brief 完全重新计算当前三项代价，用于每sweep独立核对增量漂移。 @return 当前完整分量。 */
    Components fullValues() const {
        Components result; // 独立全量代价累计。
        for (const Net& net : database_.nets) result.hpwl += netLength(net.id); // 全图真实线长，不使用缓存。
        for (const NodeId id : macros) { // 当前可移动宏。
            control_.checkCancelled();
            result.displacement += displacement(id);
            result.overlap += staticOverlap(id) + pairOverlap(id) * 0.5; // 每对被访问两次，故乘1/2。
        }
        return result;
    }
    /** @brief 试算新位置后总是回滚位置与空间索引，仅返回新缓存值。
     * @param id 被移动宏。 @param position 候选位置。 @return 可接受提交的候选事务值。 */
    Trial evaluate(NodeId id, const Position& position) {
        control_.checkCancelled();
        Trial trial; // 未提交候选，网络新值不覆盖已提交缓存。
        trial.id = id; trial.position = position; trial.values = values;
        const Position original = snapshot.positions[id]; // 回滚用原始完整位置。
        const double oldOverlap = staticOverlap(id) + pairOverlap(id); // 受影响宏对/静态碰撞旧值，DU²。
        const double oldDisplacement = displacement(id); // 当前宏旧位移平方，DU²。
        move(id, position);
        try {
            trial.values.overlap += staticOverlap(id) + pairOverlap(id) - oldOverlap;
            trial.values.displacement += displacement(id) - oldDisplacement;
            for (const NetId net : adjacency_[id]) { // 当前唯一受影响网络。
                const double length = netLength(net); // 候选真实加权HPWL，DU。
                trial.values.hpwl += length - netHpwl_[net]; trial.nets.push_back({net, length});
            }
        } catch (...) { move(id, original); throw; }
        move(id, original);
        // 仅抑制增量加减造成的负零；真正漂移在sweep边界与完整重算核对。
        trial.values.overlap = std::max(0.0, trial.values.overlap);
        trial.values.displacement = std::max(0.0, trial.values.displacement);
        if (!std::isfinite(trial.values.hpwl) || !std::isfinite(trial.values.overlap) || !std::isfinite(trial.values.displacement)) Throw("NUMERIC_FAILURE", "宏候选代价非有限");
        return trial;
    }
    /** @brief 一次性提交已接受事务，所有拒绝事务不触及缓存。
     * @param trial 完整试算结果。 无返回值；位置、空间索引和网络缓存同步更新。 */
    void accept(const Trial& trial) {
        move(trial.id, trial.position);
        for (const NetUpdate& net : trial.nets) netHpwl_[net.id] = net.hpwl; // 只提交受影响网络新值。
        values = trial.values;
    }
    /** @brief 在sweep边界核对全部缓存，重新基准化舍入漂移；无参数、无返回值。 */
    void audit() {
        const Components full = fullValues(); // 独立从当前实际几何重算的真值。
        /** @brief 判断缓存与全量结果是否在舍入阈值内。 @param cached 缓存值。 @param actual 重算值。 @return 是否一致。 */
        const auto matches = [](double cached, double actual) { return std::abs(cached - actual) <= 1e-8 * std::max({1.0, std::abs(cached), std::abs(actual)}); }; // 全量核对判据，非几何合法容差。
        if (!matches(values.hpwl, full.hpwl) || !matches(values.displacement, full.displacement) || !matches(values.overlap, full.overlap)) Throw("NUMERIC_FAILURE", "宏SA增量代价与完整重算不一致");
        values = full;
        for (const Net& net : database_.nets) netHpwl_[net.id] = netLength(net.id); // 重新基准化各网络缓存。
    }
};
/** @brief 归一化宏目标系数和参考量，初始化后不变。 */
struct Scales {
    double hpwl = 1; ///< max(global加权HPWL,1 DU)，数值单位DU。
    double displacement = 1; ///< max(N宏×h行²,1 DU²)。
    double overlap = 1; ///< max(宏总面积,1 DU²)。
};
/** @brief 按当前η重组同一组代价分量。 @param values 几何分量。 @param scales 参考尺度。 @param config 目标权重。 @param eta 当前重叠权重。 @return 无量纲宏代价。 */
double objective(const Components& values, const Scales& scales, const Config& config, double eta) {
    return config.macroHpwlWeight * values.hpwl / scales.hpwl + config.macroDisplacementWeight * values.displacement / scales.displacement + eta * values.overlap / scales.overlap;
}
/** @brief 在core包围盒投影候选，不改变实例方向。
 * @param position 待投影位置。 @param node 当前宏尺寸。 @param bounds core矩形。 @return 容纳完整物理框的左下角。 */
Position project(Position position, const Node& node, const Rect& bounds) {
    const Rect rectangle = nodeRect(node, position); // 候选方向后的尺寸，DU。
    position.x = std::clamp(position.x, bounds.lx, bounds.ux - rectangle.width());
    position.y = std::clamp(position.y, bounds.ly, bounds.uy - rectangle.height());
    return position;
}
} // namespace

/** @brief 运行可复现的宏SA，保存独立检查通过的bestLegal；完整参数契约见公共声明。 */
Snapshot LegalizeMacros(const Snapshot& global, const Config& config, ControlToken& control, const std::function<void(const Snapshot&)>& sink) {
    validateConfig(config); control.checkCancelled();
    if (!global.database || global.datasetId != global.database->datasetId || global.stage != "global") Throw("INPUT_INVALID", "宏合法化需要身份有效的 global 快照", global.stage, true);
    const DesignDatabase& database = *global.database; // 共享只读拓扑。
    validatePositions(database, global.positions);
    const auto started = std::chrono::steady_clock::now(); // 单调耗时起点。
    const ViolationReport initial = CheckLegality(global, "macro", 10000, &control); // 独立初始宏检查。
    if (initial.totalCount == 0) {
        control.safePoint();
        Snapshot result = global; // 已合法输入完整保留坐标，保证幂等。
        result.stage = "macro_legal"; result.termination = "completed"; result.legalitySummary = "legal";
        result.iteration = 0; result.revision = global.revision + 1;
        result.metrics.overflow.reset(); result.metrics.densityEnergy.reset();result.metrics.lambda.reset();result.metrics.gamma.reset();result.metrics.step.reset();
        result.metrics = ComputeMetrics(result, &global); result.metrics.elapsedMs = 0;
        if (sink) sink(result);
        return result;
    }
    for (const Violation& violation : initial.records) { // 当前已发现且可定位的独立违规。
        bool allFixed = !violation.objectIds.empty(); // 本条违规是否只涉及不可移动节点。
        for (const NodeId id : violation.objectIds) allFixed = allFixed && database.nodes[id].fixed; // 独立判定固定冲突证据。
        if (allFixed) Throw("CAPACITY_INFEASIBLE", "固定对象已存在无法通过移动宏修复的几何冲突", violation.kind, true);
        if (violation.kind == "NON_FINITE" || violation.kind == "ORIENTATION") Throw("UNSUPPORTED", "宏输入几何或方向不满足支持约束", violation.message, true);
    }
    const Rect bounds = coreBounds(database); // 投影与半径调度使用的core包围盒，DU。
    std::vector<Rect> rows; // 原始允许行矩形，DU。
    std::vector<double> rowAreas; // 随机重定位按允许行面积抽样的权重，DU²。
    double rowHeight = std::numeric_limits<double>::infinity(); // 最小正行高，用作移动半径下界和位移尺度，DU。
    for (const RowSegment& row : database.rows) { // 当前原始行。
        rows.push_back(rowRect(row)); rowAreas.push_back(rows.back().area()); rowHeight = std::min(rowHeight, row.height);
    }
    if (rows.empty() || bounds.width() <= 0 || bounds.height() <= 0) Throw("CAPACITY_INFEASIBLE", "存在待移动宏但没有正面积可放置区域", "macro", true);
    std::vector<Rect> fixed; // 固定实体/硬阻挡并集的原始表示，DU。
    for (const Obstacle& obstacle : database.obstacles) fixed.push_back(obstacle.rect); // 当前硬阻挡。
    long double macroArea = 0; // 全部可移动宏物理面积，DU²。
    std::size_t macroCount = 0; // 真正优化的宏数量。
    for (const Node& node : database.nodes) { // 当前真实节点。
        const Rect rectangle = nodeRect(node, global.positions[node.id]); // 输入方向后的物理框，DU。
        if (node.fixed && rectangle.area() > 0) fixed.push_back(rectangle);
        if (node.kind == NodeKind::Macro && !node.fixed) {
            if (rectangle.width() > bounds.width() || rectangle.height() > bounds.height()) Throw("CAPACITY_INFEASIBLE", "宏宽高超过core包围盒", node.name, true);
            macroArea += rectangle.area(); ++macroCount;
        }
    }
    if (macroCount == 0) Throw("NO_LEGAL_SOLUTION_FOUND", "不存在可移动宏来修复独立宏检查发现的违规", "macro", true);
    std::vector<Rect> combined = rows; // P∪F的原始矩形表示，用于面积必要条件。
    combined.insert(combined.end(), fixed.begin(), fixed.end());
    const double available = unionArea(combined) - unionArea(fixed); // area(P\F)，固定区/暗区不双计，DU²。
    if (macroArea > static_cast<long double>(available) + static_cast<long double>(geometryTolerance(database)) * geometryTolerance(database)) Throw("CAPACITY_INFEASIBLE", "可移动宏总面积超过允许区域扣除固定障碍后的面积", "macro", true);
    MacroState state(global, control, rows, fixed); // 任务独占的几何/网络/空间缓存。
    Scales scales; // 固定归一化参考量。
    scales.hpwl = std::max(state.values.hpwl, 1.0);
    scales.displacement = std::max(static_cast<double>(macroCount) * rowHeight * rowHeight, 1.0);
    scales.overlap = std::max(static_cast<double>(macroArea), 1.0);
    const std::uint64_t seed = config.macroSeed.value_or(config.seed ^ 0x9e3779b97f4a7c15ULL); // 固定、公开可复现的派生宏种子。
    std::mt19937_64 random(seed); // 当前任务私有随机引擎，不受其他模块消耗影响。
    std::uniform_real_distribution<double> unit(0.0, 1.0); // [0,1)概率及局部位移抽样。
    std::discrete_distribution<std::size_t> rowChoice(rowAreas.begin(), rowAreas.end()); // 按行面积选随机重定位行。
    double eta = config.macroOverlapWeight; // 当前重叠惩罚权重，每10个sweep更新。
    const double initialRadius = 0.1 * std::max(bounds.width(), bounds.height()); // 初始局部移动半径，DU。
    /** @brief 以80%局部平移、20%允许行区域重定位生成候选并投影。
     * @param id 当前宏。 @param radius 当前局部半径，DU。 @return 最多10次重采样后的完整候选位置。 */
    const auto propose = [&state, &database, &bounds, &rows, &random, &unit, &rowChoice](NodeId id, double radius) {
        const Position original = state.snapshot.positions[id]; // 当前完整接受位置。
        Position position = original; // 候选位置，方向保持原值。
        const Rect shape = nodeRect(database.nodes[id], original); // 方向后的宏尺寸，DU。
        for (int attempt = 0; attempt < 10; ++attempt) { // 投影无变化时最多重采样10次。
            position = original;
            if (unit(random) < 0.8) { position.x += (2 * unit(random) - 1) * radius; position.y += (2 * unit(random) - 1) * radius; }
            else {
                const Rect& row = rows[rowChoice(random)]; // 本次随机重定位选中的允许行。
                position.x = row.lx + unit(random) * row.width() - shape.width() * 0.5;
                position.y = row.ly + unit(random) * row.height() - shape.height() * 0.5;
            }
            position = project(position, database.nodes[id], bounds);
            if (position.x != original.x || position.y != original.y) break;
        }
        return position;
    }; // 同一随机源、同一几何决定的可复现提议器。
    control.safePoint();
    std::vector<double> positiveDeltas; // 最多100个独立可撤销样本的正代价增量。
    for (std::size_t sample = 0; sample < 100; ++sample) { // 温度校准样本编号，不提交几何。
        const NodeId id = state.macros[sample % state.macros.size()]; // 按稳定ID轮转采样，避免宏完全漏采。
        const Trial trial = state.evaluate(id, propose(id, std::max(rowHeight, initialRadius))); // 已完整回滚的候选事务。
        const double delta = objective(trial.values, scales, config, eta) - objective(state.values, scales, config, eta); // 同η同尺度目标差。
        if (delta > 0 && std::isfinite(delta)) positiveDeltas.push_back(delta);
        control.safePoint();
    }
    double temperature = std::max(0.01, 0.01 * std::abs(objective(state.values, scales, config, eta))); // 无正增量时的明确定义温度。
    if (!positiveDeltas.empty()) {
        std::sort(positiveDeltas.begin(), positiveDeltas.end());
        const std::size_t middle = positiveDeltas.size() / 2; // 正增量中位数右侧索引。
        const double median = positiveDeltas.size() % 2 == 0 ? positiveDeltas[middle - 1] * 0.5 + positiveDeltas[middle] * 0.5 : positiveDeltas[middle]; // 偶数样本取中间两者均值。
        temperature = -median / std::log(0.8);
    }
    const double initialTemperature = temperature; // T0，几何降温至1e-4 T0。
    std::optional<Snapshot> bestLegal; // 仅保存独立检查零违规的完整候选。
    double bestLegalCost = std::numeric_limits<double>::infinity(); // bestLegal的线长+位移目标，不含重叠惩罚。
    Snapshot bestCandidate = state.snapshot; // 诊断用当前权重下最低代价候选，绝不冒充成功阶段。
    Components bestComponents = state.values; // bestCandidate对应完整代价分量。
    auto lastPreview = started - std::chrono::seconds(1); // 首次完整移动可生成预览。
    std::uint64_t attempts = 0; // 完整候选处理次数，作为预览iteration。
    /** @brief 检查当前候选是否改善bestLegal，并以独立检查作为唯一合法判据；无输入、无返回值。 */
    const auto retainLegal = [&state, &bestLegal, &bestLegalCost, &scales, &config, &control] {
        const double cost = objective(state.values, scales, config, 0.0); // 合法解仅按线长与位移排序。
        if (cost >= bestLegalCost) return;
        if (CheckLegality(state.snapshot, "macro", 1, &control).totalCount == 0) { bestLegal = state.snapshot; bestLegalCost = cost; }
    }; // 独立bestLegal保留器，不读取SA内部成功标记。
    for (int sweep = 0; sweep < config.macroSweeps; ++sweep) { // 当前退火sweep编号。
        control.checkCancelled();
        temperature = initialTemperature * std::pow(1e-4, config.macroSweeps == 1 ? 0.0 : static_cast<double>(sweep) / (config.macroSweeps - 1));
        const double radius = std::max(rowHeight, initialRadius * (1.0 - static_cast<double>(sweep) / config.macroSweeps)); // 设计07半径调度，DU。
        std::vector<NodeId> order = state.macros; // 每个sweep随机打乱的宏稳定编号。
        std::shuffle(order.begin(), order.end(), random);
        for (const NodeId id : order) { // 当前被处理的宏。
            for (int move = 0; move < config.movesPerMacro; ++move) { // 当前宏本sweep的候选编号。
                control.checkCancelled();
                const Trial trial = state.evaluate(id, propose(id, radius)); // 独立可撤销候选，evaluate返回时已回滚。
                const double delta = objective(trial.values, scales, config, eta) - objective(state.values, scales, config, eta); // 冻结η后的目标差。
                const double probability = std::max(unit(random), std::numeric_limits<double>::min()); // 避免log(0)，不改变概率的可表示范围。
                if (delta <= 0 || std::log(probability) < -delta / temperature) state.accept(trial);
                ++attempts; state.snapshot.iteration = attempts; state.snapshot.revision = global.revision + attempts;
                if (objective(state.values, scales, config, eta) < objective(bestComponents, scales, config, eta)) { bestCandidate = state.snapshot; bestComponents = state.values; }
                if (state.values.overlap == 0.0) retainLegal();
                const auto now = std::chrono::steady_clock::now(); // 完整提交/回滚后的单调时间。
                if (sink && now - lastPreview >= std::chrono::duration<double>(1.0 / config.previewHz)) {
                    state.snapshot.metrics = ComputeMetrics(state.snapshot, &global);
                    state.snapshot.metrics.elapsedMs = std::chrono::duration<double, std::milli>(now - started).count();
                    sink(state.snapshot); lastPreview = now;
                }
                control.safePoint();
            }
        }
        state.audit(); // 每sweep从网络与实际几何重算，确认所有增量与回滚一致。
        retainLegal();
        if ((sweep + 1) % 10 == 0 && state.values.overlap > 0) eta = std::min(config.macroOverlapWeightMax, eta * 2.0); // 权重更新后objective直接使用新η，不复用旧C。
        control.safePoint();
    }
    if (!bestLegal) {
        const ViolationReport report = CheckLegality(bestCandidate, "macro", 1, &control); // 未成功候选的独立诊断摘要。
        std::ostringstream context; // 错误上下文保留搜索预算、种子与候选违规，不能当作不可行证明。
        context << "seed=" << seed << ", attempts=" << attempts << ", best_overlap=" << bestComponents.overlap << ", violations=" << report.totalCount;
        bestCandidate.stage="diagnostic";bestCandidate.termination="NO_LEGAL_SOLUTION_FOUND";
        bestCandidate.metrics=ComputeMetrics(bestCandidate,&global);bestCandidate.legalitySummary="macro_violations="+std::to_string(report.totalCount);
        Error error("NO_LEGAL_SOLUTION_FOUND", "宏SA预算内未找到独立合法候选，未证明数学不可行", context.str(), true); // 明确携带失败候选，不能进入成功阶段树。
        error.diagnosticSnapshot=std::make_shared<const Snapshot>(bestCandidate);throw error;
    }
    Snapshot result = *bestLegal; // 只从独立检查通过的候选生成成功快照。
    validatePositions(database, result.positions);
    if (CheckLegality(result, "macro", 1, &control).totalCount != 0) Throw("NUMERIC_FAILURE", "bestLegal 最终独立复核失败");
    control.safePoint();
    result.stage = "macro_legal"; result.termination = "completed"; result.legalitySummary = "legal";
    result.iteration = attempts; result.revision = global.revision + attempts + 1;
    result.metrics = ComputeMetrics(result, &global);
    result.metrics.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (sink) sink(result);
    return result;
}
} // namespace placement
