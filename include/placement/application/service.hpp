#pragma once
#include <placement/io/design.hpp>
#include <placement/io/result.hpp>

namespace placement {
/** @brief 后台应用事件；小型指标不持坐标，完整预览仅按previewHz构建。 */
struct Event {
    std::string kind; ///< metrics/preview/stage/log/terminal。
    std::string datasetId; ///< 源数据集身份。
    std::string runId; ///< 任务身份；旧任务事件必须被忽略。
    std::string stage; ///< 当前阶段或preview。
    std::uint64_t iteration=0; ///< 完整迭代号。
    std::uint64_t revision=0; ///< 完整几何版本。
    std::uint64_t sequence=0; ///< 单次运行单调事件号。
    Metrics metrics; ///< 同轮指标副本，不含拓扑或全坐标。
    std::shared_ptr<const Snapshot> snapshot; ///< preview/stage/terminal可携完整快照，其余为空。
    std::string message; ///< 人类诊断或终止原因。
};
/** @brief 一次运行请求；按值持配置，禁止运行中修改。 */
struct RunRequest {
    InputSpec input; ///< 未提供database时使用的输入。
    std::shared_ptr<const DesignDatabase> database; ///< 可空已加载拓扑，优先于input。
    std::shared_ptr<const Snapshot> startingSnapshot; ///< macro需global、standard需macro_legal，global/all可使用initial。
    Config config; ///< 已验证有效配置。
    std::string stage="all"; ///< global/macro/standard/all。
    std::string runId; ///< 非空时沿用调用方身份；空时生成新身份。
    std::string outputDirectory; ///< 独占结果目录，空时runs/<runId>。
    bool continueUnconverged=false; ///< 显式允许全局未收敛后继续合法化，保留全局未收敛结论。
};
/** @brief 一次运行的唯一应用终态；成功阶段快照即使后续失败仍保留。 */
struct RunResult {
    std::string runId; ///< 本次任务身份。
    std::string termination; ///< completed/cancelled或稳定错误码。
    std::string message; ///< 失败上下文或完成摘要。
    int exitCode=0; ///< CLI契约：0/2/3/4/130。
    std::string outputDirectory; ///< 已创建的独占结果目录。
    std::vector<std::shared_ptr<const Snapshot>> stages; ///< 已完整提交阶段，最多四份。
};
/** @brief 生成新任务身份，无文件或线程副作用。 @return UTC时间与进程内单调序号组合。 */
std::string NewRunId();
/** @brief 保留或可重现准备移动位置，固定对象逐项不变。
 * @param database 已验证拓扑。 @param config 初始策略/种子。 @param runId 新任务身份。 @param control 取消令牌。
 * @return initial快照；不修改原始位置，无法容纳或preserve无效时抛Error。 */
Snapshot PrepareInitial(std::shared_ptr<const DesignDatabase> database,const Config& config,const std::string& runId,ControlToken& control);
/** @brief 共用CLI/GUI流水线；同步执行，调用方负责选择工作线程。
 * @param request 完整任务请求。 @param control 协作令牌。 @param sink 可空同步事件接收器，禁止直接访问Qt控件。
 * @return 唯一终态，捕获业务与标准异常并保存已完成阶段；不会把异常传过工作线程入口。
 * 输出包含配置、审计、指标、阶段、违规及BMP/GDS；取消时保留完整成功阶段。 */
RunResult RunPipeline(const RunRequest& request,ControlToken& control,const std::function<void(const Event&)>& sink={});
/** @brief 统一单快照导出，格式为placement/bmp/gds/report。
 * @param snapshot 只读源。 @param format 格式。 @param path 目标。
 * @param config 显示/单位配置。 @param control 独立导出令牌。 @param overwrite 显式覆盖许可。
 * 无返回值；错误只属于本次导出，不更改算法状态。 */
void ExportSnapshot(const Snapshot& snapshot,const std::string& format,const std::string& path,const Config& config,ControlToken& control,bool overwrite=false);
} // namespace placement
