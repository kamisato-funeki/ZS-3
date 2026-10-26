#include "placement/core/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace placement {
namespace {

/** @brief 判断方向枚举是否属于八个已定义值。
 * @param orientation 待检查方向。 @return 有效时 true，无副作用。 */
bool validOrientation(Orientation orientation) {
    switch (orientation) {
    case Orientation::N: case Orientation::S: case Orientation::W: case Orientation::E:
    case Orientation::FN: case Orientation::FS: case Orientation::FW: case Orientation::FE:
        return true;
    }
    return false;
}

/** @brief 判断矩形边界是否有限且有序。 @param rectangle 待检查矩形，DU。 @return 有效时 true。 */
bool validRect(const Rect& rectangle) {
    return std::isfinite(rectangle.lx) && std::isfinite(rectangle.ly) &&
           std::isfinite(rectangle.ux) && std::isfinite(rectangle.uy) &&
           rectangle.lx <= rectangle.ux && rectangle.ly <= rectangle.uy;
}

/** @brief 把一个有限矩形扩展进已初始化的包围盒。
 * @param bounds 原包围盒，原位修改。 @param rectangle 新矩形。 无返回值。 */
void extendBounds(Rect& bounds, const Rect& rectangle) {
    bounds.lx = std::min(bounds.lx, rectangle.lx);
    bounds.ly = std::min(bounds.ly, rectangle.ly);
    bounds.ux = std::max(bounds.ux, rectangle.ux);
    bounds.uy = std::max(bounds.uy, rectangle.uy);
}

/** @brief 验证方向白名单内容及重复值。
 * @param allowed 白名单。 @param context 失败定位。 无返回值；非法集合抛 INPUT_INVALID。 */
void validateAllowed(const std::vector<Orientation>& allowed, const std::string& context) {
    std::set<Orientation> seen; // 已出现方向，用于禁止重复项。
    for (const Orientation orientation : allowed) { // 当前白名单方向。
        if (!validOrientation(orientation) || !seen.insert(orientation).second) {
            Throw("INPUT_INVALID", "允许方向集合包含无效或重复值", context, true);
        }
    }
}

/** @brief 判断有限值是否严格为正。 @param value 无量纲或长度值。 @return 正且有限时 true。 */
bool positive(double value) { return std::isfinite(value) && value > 0.0; }

/** @brief 判断有限值是否非负。 @param value 无量纲或长度值。 @return 非负且有限时 true。 */
bool nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }

} // namespace

/** @brief 依照公共声明求横向跨度。 @return 宽度，DU。 */
double Rect::width() const noexcept { return ux - lx; }
/** @brief 依照公共声明求纵向跨度。 @return 高度，DU。 */
double Rect::height() const noexcept { return uy - ly; }
/** @brief 依照公共声明求非负矩形面积。 @return 面积，DU²。 */
double Rect::area() const noexcept { return std::max(0.0, width()) * std::max(0.0, height()); }

/** @brief 保存统一业务异常上下文，参数语义见头文件。 */
Error::Error(std::string errorCode, std::string message, std::string errorContext, bool canRecover)
    : std::runtime_error(std::move(message)), code(std::move(errorCode)),
      context(std::move(errorContext)), recoverable(canRecover) {}

/** @brief 依照公共声明抛出统一异常；无返回值。 */
[[noreturn]] void Throw(const std::string& code, const std::string& message,
                        const std::string& context, bool recoverable) {
    throw Error(code, message, context, recoverable);
}

/** @brief 锁内发布暂停请求，使等待谓词与通知有序；无参数、无返回值。 */
void ControlToken::requestPause() {
    std::lock_guard<std::mutex> lock(mutex_); // 与安全点等待共享的状态锁。
    if (!cancelled_.load(std::memory_order_acquire)) {
        pauseRequested_.store(true, std::memory_order_release);
    }
}

/** @brief 锁内清除暂停请求并通知，避免检查后等待的丢失唤醒；无参数、无返回值。 */
void ControlToken::resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_); // 保护暂停谓词变更。
        pauseRequested_.store(false, std::memory_order_release);
    }
    condition_.notify_all();
}

/** @brief 取消不可逆并优先于暂停，清除暂停后唤醒；无参数、无返回值。 */
void ControlToken::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_); // 保证取消与等待谓词原子衔接。
        cancelled_.store(true, std::memory_order_release);
        pauseRequested_.store(false, std::memory_order_release);
    }
    condition_.notify_all();
}

/** @brief 无锁读取取消标志；无参数。 @return 是否取消。 */
bool ControlToken::isCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
/** @brief 无锁读取暂停请求；无参数。 @return 是否请求暂停。 */
bool ControlToken::isPauseRequested() const noexcept { return pauseRequested_.load(std::memory_order_acquire); }
/** @brief 读取工作线程暂停确认；无输入，无副作用。 @return 是否已确认暂停。 */
bool ControlToken::isPaused() const noexcept { return paused_.load(std::memory_order_acquire); }

/** @brief 取消时抛统一 CANCELLED；无参数、无返回值。 */
void ControlToken::checkCancelled() const {
    if (isCancelled()) Throw("CANCELLED", "任务已取消", {}, true);
}

/** @brief 先确认暂停，再以带谓词等待处理恢复或取消；参数契约见头文件。 */
void ControlToken::safePoint(const std::function<void()>& onPaused) {
    checkCancelled();
    std::unique_lock<std::mutex> lock(mutex_); // 保护确认与条件变量等待。
    if (pauseRequested_.load(std::memory_order_acquire)) {
        paused_.store(true, std::memory_order_release);
        lock.unlock();
        try { if (onPaused) onPaused(); }
        catch (...) { paused_.store(false,std::memory_order_release); throw; }
        lock.lock();
        /** @brief 取消或恢复后结束等待，无参数。 @return 是否可以离开暂停等待。 */
        condition_.wait(lock, [this] {
            return cancelled_.load(std::memory_order_acquire) ||
                   !pauseRequested_.load(std::memory_order_acquire);
        });
        paused_.store(false, std::memory_order_release);
    }
    lock.unlock();
    checkCancelled();
}

/** @brief 将八个有效方向转为标准文本，参数及错误契约见头文件。 */
std::string orientationName(Orientation orientation) {
    switch (orientation) {
    case Orientation::N: return "N";
    case Orientation::S: return "S";
    case Orientation::W: return "W";
    case Orientation::E: return "E";
    case Orientation::FN: return "FN";
    case Orientation::FS: return "FS";
    case Orientation::FW: return "FW";
    case Orientation::FE: return "FE";
    }
    Throw("INPUT_INVALID", "未知方向枚举");
}

/** @brief 严格解析标准大写方向，参数及错误契约见头文件。 */
Orientation parseOrientation(const std::string& name) {
    if (name == "N") return Orientation::N;
    if (name == "S") return Orientation::S;
    if (name == "W") return Orientation::W;
    if (name == "E") return Orientation::E;
    if (name == "FN") return Orientation::FN;
    if (name == "FS") return Orientation::FS;
    if (name == "FW") return Orientation::FW;
    if (name == "FE") return Orientation::FE;
    Throw("INPUT_INVALID", "未知方向文本", name, true);
}

/** @brief 实现设计 04 的八方向线性映射，参数与单位见头文件。 */
Vec2 transformOffset(Vec2 offset, Orientation orientation) {
    switch (orientation) {
    case Orientation::N: return {offset.x, offset.y};
    case Orientation::S: return {-offset.x, -offset.y};
    case Orientation::W: return {-offset.y, offset.x};
    case Orientation::E: return {offset.y, -offset.x};
    case Orientation::FN: return {-offset.x, offset.y};
    case Orientation::FS: return {offset.x, -offset.y};
    case Orientation::FW: return {offset.y, offset.x};
    case Orientation::FE: return {-offset.y, -offset.x};
    }
    Throw("INPUT_INVALID", "偏移变换遇到未知方向");
}

/** @brief 交换奇数 90° 旋转/镜像的尺寸，参数及返回值见头文件。 */
Rect nodeRect(const Node& node, const Position& position) {
    if (!validOrientation(position.orientation)) Throw("INPUT_INVALID", "节点方向无效", node.name, true);
    const bool swapped = position.orientation == Orientation::W || position.orientation == Orientation::E ||
                         position.orientation == Orientation::FW || position.orientation == Orientation::FE; // 是否交换 master 宽高。
    const double width = swapped ? node.height : node.width; // 当前方向横向跨度，DU。
    const double height = swapped ? node.width : node.height; // 当前方向纵向跨度，DU。
    return {position.x, position.y, position.x + width, position.y + height};
}

/** @brief 在方向后的包围盒中心累加变换偏移，参数与单位见头文件。 */
Vec2 pinPosition(const Node& node, const Position& position, Vec2 offset) {
    const Rect rectangle = nodeRect(node, position); // 当前实例物理矩形，DU。
    const Vec2 transformed = transformOffset(offset, position.orientation); // 已旋转的中心偏移，DU。
    return {rectangle.lx + rectangle.width() * 0.5 + transformed.x,
            rectangle.ly + rectangle.height() * 0.5 + transformed.y};
}

/** @brief 核对 ID 后计算引脚全局坐标，参数及错误契约见头文件。 */
Vec2 pinPosition(const DesignDatabase& database, const PositionState& positions, const Pin& pin) {
    if (pin.nodeId >= database.nodes.size() || pin.nodeId >= positions.size()) {
        Throw("REFERENCE_MISSING", "引脚引用不存在的节点或位置", std::to_string(pin.id), true);
    }
    return pinPosition(database.nodes[pin.nodeId], positions[pin.nodeId], pin.offset);
}

/** @brief 以最后站点右边界确定行跨度，参数与单位见头文件。 */
Rect rowRect(const RowSegment& row) {
    const double width = row.numSites == 0 ? 0.0 :
        static_cast<double>(row.numSites - 1) * row.sitePitch + row.siteWidth; // 行段实际物理宽度，DU。
    return {row.xOrigin, row.y, row.xOrigin + width, row.y + row.height};
}

/** @brief 归并全部行矩形的包围盒，参数及空输入语义见头文件。 */
Rect coreBounds(const DesignDatabase& database) {
    if (database.rows.empty()) return {};
    Rect bounds = rowRect(database.rows.front()); // 第一行初始化包围盒，DU。
    for (std::size_t index = 1; index < database.rows.size(); ++index) { // 后续行段索引。
        extendBounds(bounds, rowRect(database.rows[index]));
    }
    return bounds;
}

/** @brief 将终端含零面积端口并入 core 显示范围，参数及单位见头文件。 */
Rect chipBounds(const DesignDatabase& database, const PositionState& positions) {
    if (positions.size() != database.nodes.size()) Throw("INPUT_INVALID", "显示位置数量不匹配");
    Rect bounds = coreBounds(database); // core 包围盒，DU；无行时尚未初始化。
    bool initialized = !database.rows.empty(); // 标识是否已有实际几何，防止无行时强制含原点。
    for (const Node& node : database.nodes) { // 当前拓扑节点，仅终端扩展显示范围。
        if (node.kind != NodeKind::Terminal) continue;
        const Rect rectangle = nodeRect(node, positions[node.id]); // 终端当前包围盒，DU。
        if (!validRect(rectangle)) Throw("INPUT_INVALID", "终端显示位置非有限或无效", node.name, true);
        if (initialized) extendBounds(bounds, rectangle);
        else { bounds = rectangle; initialized = true; }
    }
    return bounds;
}

/** @brief 使用输入位置计算显示范围，参数及单位见头文件。 */
Rect chipBounds(const DesignDatabase& database) { return chipBounds(database, database.originalPositions); }

/** @brief 扫描精确 x 边界，并在每条带内合并 y 区间；不重复扣除重叠面积。 */
double unionArea(const std::vector<Rect>& rectangles) {
    std::vector<double> edges; // 所有非退化矩形的精确 x 事件坐标，DU。
    for (const Rect& rectangle : rectangles) { // 待验证并提取边界的矩形。
        if (!validRect(rectangle)) Throw("INPUT_INVALID", "矩形并集包含非有限或倒置边界");
        if (rectangle.width() == 0.0 || rectangle.height() == 0.0) continue;
        edges.push_back(rectangle.lx);
        edges.push_back(rectangle.ux);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    long double area = 0.0L; // 使用扩展精度累计 DU²，减小大量条带相加舍入误差。
    for (std::size_t strip = 1; strip < edges.size(); ++strip) { // 相邻精确 x 事件之间的条带右端索引。
        std::vector<Rect> intervals; // 当前条带活动矩形，使用 ly/uy 表示 y 区间，DU。
        for (const Rect& rectangle : rectangles) { // 当前候选矩形。
            if (rectangle.lx < edges[strip] && rectangle.ux > edges[strip - 1] && rectangle.height() > 0.0) {
                intervals.push_back(rectangle);
            }
        }
        /** @brief 按 y 下界及上界稳定排序。 @param left 左比较矩形。 @param right 右比较矩形。 @return 是否先于右矩形。 */
        std::sort(intervals.begin(), intervals.end(), [](const Rect& left, const Rect& right) {
            return left.ly < right.ly || (left.ly == right.ly && left.uy < right.uy);
        });
        if (intervals.empty()) continue;
        double lower = intervals.front().ly; // 当前合并 y 区间下界，DU。
        double upper = intervals.front().uy; // 当前合并 y 区间上界，DU。
        long double length = 0.0L; // 当前条带的 y 并集长度，DU。
        for (std::size_t index = 1; index < intervals.size(); ++index) { // 后续 y 区间索引。
            if (intervals[index].ly > upper) {
                length += static_cast<long double>(upper) - lower;
                lower = intervals[index].ly;
                upper = intervals[index].uy;
            } else upper = std::max(upper, intervals[index].uy);
        }
        length += static_cast<long double>(upper) - lower;
        area += (static_cast<long double>(edges[strip]) - edges[strip - 1]) * length;
    }
    if (area > std::numeric_limits<double>::max()) Throw("NUMERIC_FAILURE", "矩形并集面积超出 double 范围");
    return static_cast<double>(area);
}

/** @brief 按设计 04 以 core 最大跨度导出 εg，参数及单位见头文件。 */
double geometryTolerance(const DesignDatabase& database) {
    const Rect core = coreBounds(database); // 几何尺度基准，DU。
    return 1e-8 * std::max({1.0, core.width(), core.height()});
}

/** @brief 站点尺度限制 εg，参数及单位见头文件。 */
double siteTolerance(const DesignDatabase& database, double pitch) {
    if (!positive(pitch)) Throw("INPUT_INVALID", "站点间距必须有限且为正");
    return std::min(geometryTolerance(database), 1e-6 * pitch);
}

/** @brief 使用最小行 pitch 统一碰撞容差，参数及无行语义见头文件。 */
double collisionTolerance(const DesignDatabase& database) {
    double tolerance = geometryTolerance(database); // 无行时直接保留的几何容差，DU。
    for (const RowSegment& row : database.rows) { // 逐行求最小站点比较容差。
        tolerance = std::min(tolerance, siteTolerance(database, row.sitePitch));
    }
    return tolerance;
}

/** @brief 验证拓扑及原始基线，不修正任何输入；完整契约见头文件。 */
void validateDatabase(const DesignDatabase& database, bool requirePositions) {
    if (database.datasetId.empty()) Throw("INPUT_INVALID", "数据集身份不能为空", {}, true);
    if (database.unit.empty() || !positive(database.micronsPerDu)) Throw("INPUT_INVALID", "单位或比例无效", {}, true);
    if (database.originalPositions.size() != database.nodes.size()) Throw("INPUT_INVALID", "原始位置数量与节点数不匹配", {}, true);
    std::set<std::string> nodeNames; // 节点唯一名称集合。
    for (std::size_t index = 0; index < database.nodes.size(); ++index) { // 与 NodeId 一一对应的索引。
        const Node& node = database.nodes[index]; // 当前只读节点。
        if (node.id != index || node.name.empty() || !nodeNames.insert(node.name).second) Throw("INPUT_INVALID", "节点编号或唯一名称无效", node.name, true);
        if (node.kind != NodeKind::Standard && node.kind != NodeKind::Macro && node.kind != NodeKind::Terminal) Throw("INPUT_INVALID", "节点类别无效", node.name, true);
        if (!nonnegative(node.width) || !nonnegative(node.height) ||
            (node.kind != NodeKind::Terminal && (!positive(node.width) || !positive(node.height)))) Throw("INPUT_INVALID", "节点尺寸无效", node.name, true);
        if (!validOrientation(node.orientation)) Throw("INPUT_INVALID", "节点输入方向无效", node.name, true);
        validateAllowed(node.allowedOrientations, node.name);
        if (!node.allowedOrientations.empty() && std::find(node.allowedOrientations.begin(), node.allowedOrientations.end(), node.orientation) == node.allowedOrientations.end()) Throw("UNSUPPORTED", "节点输入方向不在 master 允许集合", node.name, true);
        const Position& position = database.originalPositions[index]; // 当前节点原始左下角基线，DU。
        if (position.orientation != node.orientation) Throw("INPUT_INVALID", "原始位置与节点输入方向不一致", node.name, true);
        if ((requirePositions || node.fixed) && (!std::isfinite(position.x) || !std::isfinite(position.y))) Throw("INPUT_INVALID", "节点原始位置必须有限", node.name, true);
        if ((requirePositions || node.fixed) && !validRect(nodeRect(node, position))) Throw("INPUT_INVALID", "节点原始物理包围盒超出有限范围", node.name, true);
        if (!requirePositions && !node.fixed && (std::isinf(position.x) || std::isinf(position.y))) Throw("INPUT_INVALID", "缺失位置仅允许 NaN，不允许无穷大", node.name, true);
    }
    std::vector<unsigned int> pinReferences(database.pins.size(), 0); // 每个引脚被网络引用的次数。
    std::set<std::string> netNames; // 网络唯一名称集合。
    for (std::size_t index = 0; index < database.nets.size(); ++index) { // 与 NetId 一一对应的网络索引。
        const Net& net = database.nets[index]; // 当前只读网络。
        if (net.id != index || net.name.empty() || !netNames.insert(net.name).second || !nonnegative(net.weight)) Throw("INPUT_INVALID", "网络编号、名称或权重无效", net.name, true);
        for (const PinId pinId : net.pinIds) { // 当前网络的引用引脚编号。
            if (pinId >= database.pins.size()) Throw("REFERENCE_MISSING", "网络引用不存在的引脚", net.name, true);
            if (++pinReferences[pinId] != 1 || database.pins[pinId].netId != net.id) Throw("INPUT_INVALID", "引脚重复归属或网络双向引用不一致", net.name, true);
        }
    }
    for (std::size_t index = 0; index < database.pins.size(); ++index) { // 与 PinId 一一对应的引脚索引。
        const Pin& pin = database.pins[index]; // 当前只读引脚。
        if (pin.id != index || pin.nodeId >= database.nodes.size() || pin.netId >= database.nets.size()) Throw("REFERENCE_MISSING", "引脚编号或拓扑引用无效", std::to_string(index), true);
        if (!std::isfinite(pin.offset.x) || !std::isfinite(pin.offset.y) || pinReferences[index] != 1) Throw("INPUT_INVALID", "引脚偏移非有限或未被唯一网络引用", std::to_string(index), true);
    }
    for (std::size_t index = 0; index < database.rows.size(); ++index) { // 与行段 ID 一一对应的索引。
        const RowSegment& row = database.rows[index]; // 当前只读行段。
        if (row.id != index || !std::isfinite(row.y) || !std::isfinite(row.xOrigin) ||
            !positive(row.height) || !positive(row.siteWidth) || !positive(row.sitePitch) ||
            row.siteWidth > row.sitePitch || row.numSites == 0 || !validRect(rowRect(row)) || !validOrientation(row.orientation)) Throw("INPUT_INVALID", "水平行段参数无效", std::to_string(index), true);
        validateAllowed(row.allowedOrientations, std::to_string(index));
        const Rect rectangle = rowRect(row); // 当前行的物理跨度，DU。
        for (std::size_t previous = 0; previous < index; ++previous) { // 已验证行段索引。
            const Rect other = rowRect(database.rows[previous]); // 对照行段物理跨度，DU。
            if (std::min(rectangle.ux, other.ux) > std::max(rectangle.lx, other.lx) &&
                std::min(rectangle.uy, other.uy) > std::max(rectangle.ly, other.ly)) Throw("INPUT_INVALID", "可放置行段发生正面积重叠", std::to_string(index), true);
        }
    }
    for (const Obstacle& obstacle : database.obstacles) { // 当前输入硬阻挡。
        if (!validRect(obstacle.rect)) Throw("INPUT_INVALID", "硬阻挡矩形无效", obstacle.source, true);
    }
}

/** @brief 提交前独立核对有限性、方向和固定精确性，参数及错误契约见头文件。 */
void validatePositions(const DesignDatabase& database, const PositionState& positions, bool enforceFixed) {
    if (positions.size() != database.nodes.size() || database.originalPositions.size() != database.nodes.size()) Throw("INPUT_INVALID", "快照位置或基线数量不匹配");
    for (std::size_t index = 0; index < positions.size(); ++index) { // 稳定节点索引。
        const Position& position = positions[index]; // 当前提交位置，DU。
        const Position& original = database.originalPositions[index]; // 不可改写的输入位置基线，DU。
        const Node& node = database.nodes[index]; // 对应拓扑节点。
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !validRect(nodeRect(node, position))) Throw("NUMERIC_FAILURE", "快照位置或物理包围盒非有限", node.name);
        if (enforceFixed && node.fixed && (position.x != original.x || position.y != original.y || position.orientation != original.orientation)) Throw("FIXED_CHANGED", "固定对象偏离输入基线", node.name);
        if (position.orientation != node.orientation) Throw("UNSUPPORTED", "本版算法不得改变输入方向", node.name, true);
    }
}

/** @brief 检查公开配置默认契约和跨字段关系，无容量计算；参数见头文件。 */
void validateConfig(const Config& config) {
    if (config.initialMode != "auto" && config.initialMode != "preserve" && config.initialMode != "random") Throw("INPUT_INVALID", "initialMode 必须为 auto/preserve/random", "initialMode", true);
    if (!positive(config.targetDensity) || config.targetDensity > 1.0) Throw("INPUT_INVALID", "targetDensity 必须位于 (0,1]", "targetDensity", true);
    /** @brief 判断显式网格是否为支持的 2 次幂。 @param size bin 数。 @return 是否有效。 */
    const auto validGrid = [](int size) { return size >= 32 && size <= 512 && (size & (size - 1)) == 0; }; // 显式网格的独立范围检查器。
    if (!((config.gridNx == 0 && config.gridNy == 0) || (validGrid(config.gridNx) && validGrid(config.gridNy)))) Throw("INPUT_INVALID", "网格须同时自动或为 32～512 的 2 次幂", "gridNx/gridNy", true);
    if (!nonnegative(config.overflowStop) || config.overflowStop >= 1.0) Throw("INPUT_INVALID", "overflowStop 必须位于 [0,1)", "overflowStop", true);
    if (config.maxIterations <= 0 || config.backtrackLimit <= 0 || config.macroSweeps <= 0 || config.movesPerMacro <= 0 || config.fillerLimit == 0 || config.convergenceWindow <= 0 || config.stagnationWindow <= 0) Throw("INPUT_INVALID", "迭代、搜索、收敛窗口与 filler 上限必须为正", "limits", true);
    if (config.convergenceWindow != 5) Throw("INPUT_INVALID", "收敛窗口按实施契约固定为连续五轮", "convergence_window", true);
    if (!positive(config.shrink) || config.shrink >= 1.0) Throw("INPUT_INVALID", "shrink 必须位于 (0,1)", "shrink", true);
    if (config.fftThreads != 1) Throw("UNSUPPORTED", "确定性基线只支持单线程 FFT", "fftThreads", true);
    if (!positive(config.lambdaUpdateMin) || !positive(config.lambdaUpdateMax) || config.lambdaUpdateMin > config.lambdaUpdateMax ||
        !positive(config.lambdaMin) || !positive(config.lambdaMax) || config.lambdaMin > config.lambdaMax) Throw("INPUT_INVALID", "lambda 范围或更新倍率无效", "lambda", true);
    if (config.previewHz < 1 || config.previewHz > 10 || config.imageWidth < 256 || config.imageWidth > 8192 || config.margin < 0 || config.margin >= config.imageWidth / 2) Throw("INPUT_INVALID", "预览频率、图宽或留白范围无效", "display", true);
    if (!positive(config.gdsDbuUm) || (config.micronsPerDu && !positive(*config.micronsPerDu))) Throw("INPUT_INVALID", "导出单位或物理比例必须有限且为正", "units", true);
    if (!nonnegative(config.macroHpwlWeight) || !nonnegative(config.macroDisplacementWeight) || !positive(config.macroOverlapWeight) || !positive(config.macroOverlapWeightMax) || config.macroOverlapWeightMax < config.macroOverlapWeight) Throw("INPUT_INVALID", "宏代价权重无效", "macro weights", true);
}

} // namespace placement
