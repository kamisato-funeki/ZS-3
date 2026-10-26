#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/** @brief 与界面、文件格式和第三方库无关的布局领域模型；坐标均为 DU。 */
namespace placement {

/** @brief 稳定节点编号，数值必须等于数据库 nodes 中的索引。 */
using NodeId = std::uint64_t;
/** @brief 稳定引脚编号，数值必须等于数据库 pins 中的索引。 */
using PinId = std::uint64_t;
/** @brief 稳定网络编号，数值必须等于数据库 nets 中的索引。 */
using NetId = std::uint64_t;

/** @brief 数学坐标向量；x 向右、y 向上，值类型不共享可变状态。 */
struct Vec2 {
    double x = 0.0; ///< 横向坐标或偏移，DU。
    double y = 0.0; ///< 纵向坐标或偏移，DU。
};

/** @brief 半开轴对齐矩形 [lx,ux)×[ly,uy)，有效矩形满足有限且下界不大于上界。 */
struct Rect {
    double lx = 0.0; ///< 左边界，DU。
    double ly = 0.0; ///< 下边界，DU。
    double ux = 0.0; ///< 右边界，DU。
    double uy = 0.0; ///< 上边界，DU。
    /** @brief 求横向跨度；无参数，无副作用。 @return ux-lx，DU。 */
    double width() const noexcept;
    /** @brief 求纵向跨度；无参数，无副作用。 @return uy-ly，DU。 */
    double height() const noexcept;
    /** @brief 求非负面积；退化或倒置矩形面积为零。 @return 面积，DU²。 */
    double area() const noexcept;
};

/** @brief 以未旋转 master 中心为原点的八种正交方向。 */
enum class Orientation {
    N, ///< 不变换。
    S, ///< 旋转 180°。
    W, ///< 逆时针旋转 90°。
    E, ///< 顺时针旋转 90°。
    FN, ///< 关于纵轴镜像。
    FS, ///< 关于横轴镜像。
    FW, ///< 交换 x/y，即关于 y=x 镜像。
    FE ///< 交换 x/y 并取负，即关于 y=-x 镜像。
};

/** @brief 节点物理类别，分类不由名称推断。 */
enum class NodeKind {
    Standard, ///< 单高度标准单元。
    Macro, ///< 矩形宏单元。
    Terminal ///< 固定终端或顶层端口，允许零面积。
};

/** @brief 只读拓扑中的节点；尺寸是 N 朝向 master 尺寸，位置另存。 */
struct Node {
    NodeId id = 0; ///< 稳定编号，等于 nodes 索引。
    std::string name; ///< 数据集内唯一、区分大小写的名称。
    NodeKind kind = NodeKind::Standard; ///< 几何/合法化类别。
    double width = 0.0; ///< 未旋转宽度，DU；非 terminal 必须严格为正。
    double height = 0.0; ///< 未旋转高度，DU；非 terminal 必须严格为正。
    Orientation orientation = Orientation::N; ///< 输入 master 实例方向；算法不旋转对象。
    bool fixed = false; ///< 输入固定性，运行时锁定宏不得改写此字段。
    std::string masterName; ///< master 名称；BookShelf 可为空。
    std::vector<Orientation> allowedOrientations; ///< master 允许方向；空集合表示未施加额外方向约束。
};

/** @brief 单个节点当前左下角及方向；不是 DEF 原始放置锚点。 */
struct Position {
    double x = 0.0; ///< 方向变换后包围盒左下角横坐标，DU。
    double y = 0.0; ///< 方向变换后包围盒左下角纵坐标，DU。
    Orientation orientation = Orientation::N; ///< 当前方向，提交快照时必须等于输入方向。
};
/** @brief 按 NodeId 直接索引的位置数组；每个数组由所属任务独占修改。 */
using PositionState = std::vector<Position>;

/** @brief 网络引脚；顶层端口也通过一个 Terminal 节点归属。 */
struct Pin {
    PinId id = 0; ///< 稳定引脚编号，等于 pins 索引。
    NodeId nodeId = 0; ///< 所属节点，必须有效。
    NetId netId = 0; ///< 所属网络，必须与网络 pinIds 双向一致。
    Vec2 offset; ///< 相对未旋转 master 中心的偏移，DU。
};

/** @brief 超边网络；零/单引脚网络的线长为零。 */
struct Net {
    NetId id = 0; ///< 稳定网络编号，等于 nets 索引。
    std::string name; ///< 数据集内唯一网络名。
    std::vector<PinId> pinIds; ///< 唯一引脚编号，每个引脚只能出现一次。
    double weight = 1.0; ///< 非负有限线长权重，无量纲。
};

/** @brief 水平可放置行段；numSites 个站点的末端为 origin+(n-1)pitch+width。 */
struct RowSegment {
    std::uint64_t id = 0; ///< 行段稳定编号，等于 rows 索引。
    double y = 0.0; ///< 行底部纵坐标，DU。
    double height = 0.0; ///< 正行高，DU。
    double xOrigin = 0.0; ///< 第零站点左边界，DU。
    double siteWidth = 0.0; ///< 正站点物理宽度，DU，不能超过 sitePitch。
    double sitePitch = 0.0; ///< 正站点间距，DU。
    std::uint64_t numSites = 0; ///< 正站点数量。
    Orientation orientation = Orientation::N; ///< 输入行方向。
    std::vector<Orientation> allowedOrientations; ///< 行允许方向；空集合只允许 orientation。
};

/** @brief 固定硬阻挡；允许与固定节点重合，面积统计必须求并集。 */
struct Obstacle {
    Rect rect; ///< 阻挡物理范围，DU。
    std::string source; ///< 来源文件/对象标识，供违规定位。
};

/** @brief 输入文件审计记录；仅保存指纹，数据库不持有打开的文件。 */
struct InputFile {
    std::string path; ///< 规范化或可追溯输入路径。
    std::string sha256; ///< 文件 SHA-256 十六进制摘要。
    std::uint64_t size = 0; ///< 文件字节数。
};

/** @brief 拓扑、单位与原始位置数据库；验证后以 shared_ptr<const> 跨线程共享。 */
struct DesignDatabase {
    std::string datasetId; ///< 由输入指纹生成的数据集身份。
    std::string unit = "DU"; ///< 内部长度单位显示名，DU 或 um。
    double micronsPerDu = 1.0; ///< 每 DU 对应 μm；未知物理比例时为显示映射 1。
    bool physicalScaleKnown = false; ///< 是否有真实物理比例，false 时不得宣称 DU 等于真实 μm。
    std::vector<Node> nodes; ///< 按稳定 NodeId 排序的拓扑节点。
    std::vector<Pin> pins; ///< 按稳定 PinId 排序的引脚。
    std::vector<Net> nets; ///< 按稳定 NetId 排序的网络。
    std::vector<RowSegment> rows; ///< 按稳定行段编号排序的水平行。
    std::vector<Obstacle> obstacles; ///< 硬阻挡原始矩形，允许相互重叠。
    PositionState originalPositions; ///< 输入位置基线；准备前可移动缺失位置用 NaN 表示，固定位置必须有限。
    std::vector<InputFile> inputFiles; ///< 用于回读身份核对的输入清单。
    std::vector<std::string> auditWarnings; ///< 兼容映射、分类推断和非致命输入审计说明。
};

/** @brief 同一快照几何的指标；未计算或不适用的指标使用 nullopt。 */
struct Metrics {
    std::optional<double> hpwl; ///< 无权真实半周长线长，DU。
    std::optional<double> weightedHpwl; ///< 加权真实半周长线长，DU。
    std::optional<double> overflow; ///< 不包含 filler 的真实电荷溢出率，无量纲。
    std::optional<double> densityEnergy; ///< 当前网格离散静电能量，DU²。
    std::optional<double> elapsedMs; ///< 从任务开始累计耗时，毫秒。
    std::optional<double> meanDisplacement; ///< 对可比可移动对象的平均欧氏位移，DU。
    std::optional<double> maxDisplacement; ///< 对可比可移动对象的最大欧氏位移，DU。
    std::optional<double> quadraticDisplacement; ///< 可比可移动对象位移平方和，DU²。
};

/** @brief 完整提交的布局快照；发布后由 shared_ptr<const Snapshot> 持有，不保存 filler。 */
struct Snapshot {
    std::shared_ptr<const DesignDatabase> database; ///< 同一数据集只读拓扑；有效快照不可为空。
    std::string datasetId; ///< 必须与 database->datasetId 相同。
    std::string runId; ///< 任务身份，拒绝旧任务的迟到事件。
    std::string stage = "initial"; ///< initial/global/macro_legal/standard_legal 或明确的诊断阶段。
    std::uint64_t iteration = 0; ///< 已完整接受的迭代数。
    std::uint64_t revision = 0; ///< 单次运行中单调递增的几何修订号。
    PositionState positions; ///< 与本次 metrics 同轮的真实节点位置。
    Metrics metrics; ///< 当前几何的已计算指标。
    std::string termination; ///< 终止原因；空字符串表示阶段仍在运行。
    std::string legalitySummary; ///< 独立检查摘要；空字符串表示尚未检查。
};

/** @brief 集中有效配置；值类型按任务复制，默认值与设计 04/05/07/08 一致。 */
struct Config {
    std::uint64_t seed = 42; ///< 初始位置及派生随机序列的根种子。
    std::string initialMode = "auto"; ///< auto/preserve/random 初始位置策略。
    double targetDensity = 0.8; ///< 目标有效面积密度，(0,1]。
    int gridNx = 0; ///< 横向 bin 数；0 自动，否则 32～512 的 2 次幂。
    int gridNy = 0; ///< 纵向 bin 数；0 自动，必须与 gridNx 同时自动或显式。
    double overflowStop = 0.1; ///< 收敛溢出率阈值，[0,1)。
    int maxIterations = 2000; ///< 全局布局最大接受迭代数，正数。
    int backtrackLimit = 20; ///< 单次梯度步回溯次数上限，正数。
    double shrink = 0.5; ///< 回溯步长乘数，(0,1)。
    int fftThreads = 1; ///< FFTW 执行线程数；确定性基线仅支持 1。
    double lambdaUpdateMin = 0.75; ///< 每轮 λ 更新倍率下界，正数。
    double lambdaUpdateMax = 1.1; ///< 每轮 λ 更新倍率上界，不能小于下界。
    int macroSweeps = 100; ///< 宏模拟退火 sweep 数，正数。
    int movesPerMacro = 20; ///< 每个 sweep 每个宏的候选次数，正数。
    std::optional<std::uint64_t> macroSeed; ///< 独立宏随机种子；空时由 seed 派生。
    int previewHz = 5; ///< 完整预览生成频率，1～10 Hz。
    int imageWidth = 1600; ///< 标准导出图宽，256～8192 像素。
    int margin = 24; ///< 图像四侧留白，非负像素，不能占满宽度。
    double gdsDbuUm = 0.001; ///< GDS 数据库单位，正 μm。
    std::size_t fillerLimit = 20000; ///< 临时 filler 数量上限，正数。
    std::optional<double> micronsPerDu; ///< 用户指定 μm/DU；空时遵循输入格式映射。
    double lambdaMin = 1e-8; ///< 密度惩罚系数的正绝对下界。
    double lambdaMax = 1e8; ///< 密度惩罚系数的有限绝对上界。
    int convergenceWindow = 5; ///< overflow 连续达标次数，正数。
    int stagnationWindow = 20; ///< λ 触顶且无改善的允许连续次数，正数。
    double macroHpwlWeight = 1.0; ///< 宏代价归一化线长权重 α，非负。
    double macroDisplacementWeight = 0.1; ///< 宏代价归一化位移权重 β，非负。
    double macroOverlapWeight = 10.0; ///< 宏代价初始重叠权重 η，正数。
    double macroOverlapWeightMax = 1e6; ///< 宏重叠权重最大值，不小于初始 η。
};

/** @brief 带可机器判断代码和上下文的统一业务异常；what() 保存面向人的说明。 */
class Error : public std::runtime_error {
public:
    std::string code; ///< 稳定大写错误代码，如 INPUT_INVALID/CANCELLED。
    std::string context; ///< 文件、对象、阶段等定位上下文，可为空。
    bool recoverable = false; ///< 是否允许用户修改配置或输入后重新发起。
    /** @brief 构造业务错误，不做日志或线程操作。
     * @param errorCode 稳定错误代码。 @param message 错误说明。
     * @param errorContext 定位上下文，可为空。 @param canRecover 是否可恢复。 */
    Error(std::string errorCode, std::string message, std::string errorContext = {}, bool canRecover = false);
};

/** @brief 抛出统一错误；无返回值，不改变数据库。
 * @param code 稳定错误代码。 @param message 面向人的说明。
 * @param context 对象/文件上下文，可空。 @param recoverable 是否允许修正后重试。 */
[[noreturn]] void Throw(const std::string& code, const std::string& message,
                        const std::string& context = {}, bool recoverable = false);

/** @brief 跨线程协作控制；一个工作线程使用 safePoint，任意控制线程可请求状态变更。 */
class ControlToken {
public:
    /** @brief 默认创建未暂停、未取消的令牌；无参数，无额外副作用。 */
    ControlToken() = default;
    /** @brief 请求工作线程在下个安全点暂停；无参数、无返回值，幂等。 */
    void requestPause();
    /** @brief 清除暂停请求并唤醒工作线程；无参数、无返回值，幂等。 */
    void resume();
    /** @brief 永久设置取消状态并唤醒暂停线程；无参数、无返回值，取消优先。 */
    void cancel();
    /** @brief 查询取消请求，无锁、无线程副作用。 @return 是否已取消。 */
    bool isCancelled() const noexcept;
    /** @brief 查询暂停请求，不代表工作线程已经停住。 @return 是否请求暂停。 */
    bool isPauseRequested() const noexcept;
    /** @brief 检查取消，无参数、无返回值；取消时抛出 Error(CANCELLED)。 */
    void checkCancelled() const;
    /** @brief 在完整提交/回滚之后处理暂停与取消；等待期间不持有业务锁。
     * @param onPaused 可空回调；工作线程确认暂停后调用一次，调用时不持内部锁；可发事件或取消。
     * 无返回值；已取消/等待中取消均抛出 Error(CANCELLED)。回调异常原样传播。
     * 回调执行期间 resume 可能到达，因此确认事件只表示已到达安全点，不是控制线程即时状态。 */
    void safePoint(const std::function<void()>& onPaused = {});
private:
    std::atomic<bool> cancelled_{false}; ///< 发布不可逆取消标志，跨线程 acquire/release。
    std::atomic<bool> pauseRequested_{false}; ///< 发布暂停请求，不能替代暂停确认事件。
    std::mutex mutex_; ///< 保护条件变量的检查与等待，防止丢失唤醒。
    std::condition_variable condition_; ///< resume/cancel 唤醒安全点等待。
};

/** @brief 获取方向标准文本，无副作用。 @param orientation 有效方向枚举。 @return N/S/W/E/FN/FS/FW/FE；无效值抛 INPUT_INVALID。 */
std::string orientationName(Orientation orientation);
/** @brief 解析严格大写方向，无副作用。 @param name 标准文本。 @return 方向；未知文本抛 INPUT_INVALID。 */
Orientation parseOrientation(const std::string& name);
/** @brief 按设计 04 表变换中心偏移，无平移。 @param offset 未旋转中心偏移，DU。 @param orientation 方向。 @return 变换偏移，DU；无效方向抛 INPUT_INVALID。 */
Vec2 transformOffset(Vec2 offset, Orientation orientation);
/** @brief 计算节点方向后的半开包围盒。 @param node master 尺寸。 @param position 当前左下角及方向。 @return 物理矩形，DU；无效方向抛 INPUT_INVALID。 */
Rect nodeRect(const Node& node, const Position& position);
/** @brief 计算实际引脚点。 @param node 所属节点。 @param position 实例位置。 @param offset 中心相对偏移，DU。 @return 全局引脚坐标，DU。 */
Vec2 pinPosition(const Node& node, const Position& position, Vec2 offset);
/** @brief 经稳定 ID 计算数据库引脚点。 @param database 只读拓扑。 @param positions 当前完整位置。 @param pin 引脚。 @return 全局引脚坐标，DU；引用无效抛 REFERENCE_MISSING。 */
Vec2 pinPosition(const DesignDatabase& database, const PositionState& positions, const Pin& pin);
/** @brief 计算行段物理跨度。 @param row 有效行。 @return 行矩形，DU；零站点返回零宽矩形。 */
Rect rowRect(const RowSegment& row);
/** @brief 计算所有可放置行的包围盒。 @param database 只读拓扑。 @return 包围盒，DU；无行返回零矩形。 */
Rect coreBounds(const DesignDatabase& database);
/** @brief 计算 core 与所有 Terminal 的显示包围盒。 @param database 只读拓扑。 @param positions 当前位置。 @return 显示范围，DU；引用无效抛 INPUT_INVALID。 */
Rect chipBounds(const DesignDatabase& database, const PositionState& positions);
/** @brief 计算原始位置显示范围。 @param database 包含原始位置的只读拓扑。 @return core 和 Terminal 的包围盒，DU。 */
Rect chipBounds(const DesignDatabase& database);
/** @brief 精确求矩形并集面积，不重复计数、不应用容差。 @param rectangles 有限非倒置矩形，可空，退化项被忽略。 @return 并集面积，DU²；非法矩形抛 INPUT_INVALID。 */
double unionArea(const std::vector<Rect>& rectangles);
/** @brief 计算统一几何尺度容差。 @param database 提供 core 尺度的拓扑。 @return 1e-8×max(1,core宽,core高)，DU。 */
double geometryTolerance(const DesignDatabase& database);
/** @brief 计算某行站点比较容差。 @param database core 尺度。 @param pitch 正有限站点间距，DU。 @return min(εg,1e-6×pitch)，DU；非法 pitch 抛 INPUT_INVALID。 */
double siteTolerance(const DesignDatabase& database, double pitch);
/** @brief 计算碰撞比较容差，避免站点尺度小于 core εg。 @param database core 和行间距。 @return min(εg,1e-6×最小pitch)，无行时 εg，DU。 */
double collisionTolerance(const DesignDatabase& database);
/** @brief 验证完整拓扑、单位、ID、双向引脚、行段和输入基线，不修改输入。
 * @param database 待验证数据库。 @param requirePositions true 要求所有输入位置有限；false 允许可移动 x/y 缺失，但固定仍须有限。
 * 无返回值；违反契约抛带对象上下文的 Error；不把重叠布局作为拓扑错误。 */
void validateDatabase(const DesignDatabase& database, bool requirePositions = true);
/** @brief 验证提交位置，固定对象必须与输入逐项完全相等，不应用 ε。
 * @param database 已验证只读拓扑。 @param positions 当前完整位置。 @param enforceFixed 是否检查固定坐标和方向精确不变。
 * 无返回值；数量/有限性/方向或固定性失败抛 Error。所有节点方向均必须保持输入方向。 */
void validatePositions(const DesignDatabase& database, const PositionState& positions, bool enforceFixed = true);
/** @brief 验证集中配置范围及跨字段关系，不做容量可行性预检。 @param config 待验证有效配置。 无返回值；非法值抛 INPUT_INVALID。 */
void validateConfig(const Config& config);

} // namespace placement
