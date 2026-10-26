#pragma once
#include "placement/core/model.hpp"

namespace placement {
/** @brief WA 平滑线长及精确导数；梯度按 NodeId 索引，固定节点为零。 */
struct WirelengthEvaluation {
    double value = 0; ///< 加权 WA，DU。
    std::vector<Vec2> gradient; ///< 对真实节点中心的导数，无量纲。
};
/** @brief 计算设计05第5节的加权 WA；不改变拓扑和位置。
 * @param database 已验证拓扑。 @param positions 完整位置。 @param gamma 正平滑长度，DU。
 * @param control 可空取消令牌，不处理暂停。 @return WA 与按节点累积的梯度；非法输入抛 Error。 */
WirelengthEvaluation EvaluateWirelength(const DesignDatabase& database, const PositionState& positions,
                                       double gamma, ControlToken* control = nullptr);
/** @brief 正交二维 DCT-II/III，存储索引为 y*nx+x；FFTW 类型不泄漏。
 * @param input 有限网格。 @param nx 横向长度。 @param ny 纵向长度。
 * @param inverse true 执行正交逆变换。 @param control 可空取消令牌。
 * @return 同尺寸结果；非法尺寸、数值或 FFTW 计划失败抛 Error；线程缓冲私有。 */
std::vector<double> OrthonormalDct(const std::vector<double>& input, int nx, int ny,
                                  bool inverse = false, ControlToken* control = nullptr);
/** @brief 求离散 Neumann 泊松方程，先去均值且零频电势为零。
 * @param density 无量纲网格。 @param nx 横向长度。 @param ny 纵向长度。
 * @param dx 横向 bin 宽，DU。 @param dy 纵向 bin 高，DU。 @param control 可空取消令牌。
 * @return 零均值电势，DU²；输入无效抛 Error。 */
std::vector<double> SolvePoisson(const std::vector<double>& density, int nx, int ny,
                                double dx, double dy, ControlToken* control = nullptr);
/** @brief 单次密度计算的完整值；filler 不参与 overflow。 */
struct DensityEvaluation {
    double energy = 0; ///< 离散静电能量，DU⁴（坐标缩放下的数学量）。
    double overflow = 0; ///< 真实电荷正溢出除以真实总电荷，无量纲。
    std::vector<double> totalCharge; ///< 静态、真实及 filler 总电荷，每 bin DU²。
    std::vector<double> realCharge; ///< 真实移动对象电荷，每 bin DU²。
    std::vector<double> staticCharge; ///< 缓存静态电荷，每 bin DU²。
    std::vector<double> fillerCharge; ///< 临时 filler 电荷，每 bin DU²。
    std::vector<double> potential; ///< 零均值电势，DU²。
    std::vector<Vec2> gradient; ///< 真实节点中心梯度，固定节点为零，DU³。
    std::vector<Vec2> fillerGradient; ///< 按 filler 顺序的中心梯度，DU³。
};
/** @brief 单线程任务私有密度模型，拥有静态区域、filler 及网格缓存，不拥有数据库。 */
class DensityModel {
public:
    /** @brief 建立精确静态并集并执行容量预检，生成守恒 filler。
     * @param database 生命周期须覆盖本对象的只读拓扑。 @param referencePositions 固定物体位置。
     * @param config 有效配置副本。 @param control 可空、生命周期覆盖本对象的取消令牌。
     * 无返回值；容量或 filler 尺寸不可行抛 Error。 */
    DensityModel(const DesignDatabase& database, const PositionState& referencePositions,
                 const Config& config, ControlToken* control = nullptr);
    /** @brief 释放私有缓存；无参数、无返回值。 */
    ~DensityModel();
    /** @brief 禁止复制，避免隐式共享私有任务缓存。 @param other 禁止复制源。 */
    DensityModel(const DensityModel& other) = delete;
    /** @brief 禁止复制赋值。 @param other 禁止复制源。 @return 不可调用。 */
    DensityModel& operator=(const DensityModel& other) = delete;
    /** @brief 计算完整密度及精确梯度，不改变参数。
     * @param positions 真实节点位置。 @param fillerCenters 与初始 filler 数一致的中心，DU。
     * @return 完整密度结果；非法输入、取消、非有限值抛 Error。 */
    DensityEvaluation evaluate(const PositionState& positions, const std::vector<Vec2>& fillerCenters) const;
    /** @brief 获取初始 filler 中心，无副作用。 @return 模型生命周期内有效只读引用，DU。 */
    const std::vector<Vec2>& fillerPositions() const;
    /** @brief 获取 filler 尺寸，无副作用。 @return 与中心对应的宽高，DU。 */
    const std::vector<Vec2>& fillerSizes() const;
    /** @brief 获取横向网格数。 @return 正整数。 */
    int gridNx() const;
    /** @brief 获取纵向网格数。 @return 正整数。 */
    int gridNy() const;
    /** @brief 获取横向网格宽。 @return DU。 */
    double binWidth() const;
    /** @brief 获取纵向网格高。 @return DU。 */
    double binHeight() const;
    /** @brief 查询是否因数量上限扩大 filler。 @return 数量受限标志。 */
    bool fillerCapped() const;
    /** @brief 获取有效可放置面积。 @return DU²。 */
    double availableArea() const;
private:
    struct Impl; ///< 不向公共接口泄漏 FFTW 或密度核内部实现。
    std::unique_ptr<Impl> impl_; ///< 独占模型缓存。
};
/** @brief 按设计05运行投影 Nesterov、冻结目标回溯与参数调度。
 * @param initial 已准备的完整初始快照，身份保留。 @param config 有效配置。
 * @param control 取消/暂停令牌，暂停仅在完整提交点生效。
 * @param sink 可空完整快照接收器，在工作线程调用，异常原样传播。
 * @return stage=global 的完整终态快照；converged/max_iterations 明确区分；数值失败抛 Error。
 * 不执行 I/O、不保存 filler，不改变固定节点；取消抛 CANCELLED。 */
Snapshot RunGlobal(const Snapshot& initial, const Config& config, ControlToken& control,
                   const std::function<void(const Snapshot&)>& sink = {});
} // namespace placement
