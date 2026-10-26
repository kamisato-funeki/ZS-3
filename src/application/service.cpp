#include <placement/application/service.hpp>
#include <placement/application/global.hpp>
#include <placement/application/legalization.hpp>
#include <placement/io/visualization.hpp>
#include "build_info.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unistd.h>

namespace placement {
namespace {
/** @brief 使用UTC格式输出墙钟时间。 @param format strftime格式。 @return 格式化时间。 */
std::string utc(const char* format) {
    const std::time_t now=std::time(nullptr); // 当前墙钟秒，仅用于身份和审计，不参与算法。
    std::tm calendar{}; // UTC日历副本，线程私有。
    gmtime_r(&now,&calendar);std::ostringstream text; // 本次UTC格式缓冲。
    text<<std::put_time(&calendar,format);return text.str();
}
/** @brief 输出可空CSV数字，固定点小数分隔符。 @param value 可空值。 @return null为空字段。 */
std::string csvNumber(const std::optional<double>& value) {
    if(!value) return {};
    std::ostringstream stream; // 当前数值的往返精度文本。
    stream.imbue(std::locale::classic());stream<<std::setprecision(17)<<*value;return stream.str();
}
/** @brief 稳定错误到退出码映射。 @param code 业务错误。 @return 文档04退出码。 */
int exitCode(const std::string& code) {
    if(code=="CANCELLED") return 130;
    if(code=="MAX_ITERATIONS"||code=="NO_LEGAL_SOLUTION_FOUND") return 3;
    if(code=="INPUT_INVALID"||code=="REFERENCE_MISSING"||code=="UNSUPPORTED"||code=="CAPACITY_INFEASIBLE"||code=="OUTPUT_EXISTS") return 2;
    return 4;
}
} // namespace
/** @brief 创建新任务身份。 @return UTC+PID+序号，无输入。 */
std::string NewRunId() {
    static std::atomic<std::uint64_t> sequence{0}; // 保证同一进程同一秒多次运行唯一。
    const auto ticks=std::chrono::system_clock::now().time_since_epoch().count(); // 高分辨率时间避免不同PID命名空间同秒冲突。
    return utc("%Y%m%dT%H%M%SZ")+"-"+std::to_string(ticks)+"-"+std::to_string(::getpid())+"-"+std::to_string(sequence.fetch_add(1));
}
/** @brief 单快照导出适配，公共契约见service.hpp。 @param snapshot 源。 @param format 格式。 @param path 目标。 @param config 配置。 @param control 取消。 @param overwrite 覆盖。 无返回值。 */
void ExportSnapshot(const Snapshot& snapshot,const std::string& format,const std::string& path,const Config& config,ControlToken& control,bool overwrite) {
    if(format=="placement"||format=="json") WriteTextAtomic(path,SerializeSnapshot(snapshot),control,overwrite);
    else if(format=="bmp") ExportBmp(snapshot,path,config,control,overwrite);
    else if(format=="gds") ExportGds(snapshot,path,config,control,overwrite);
    else if(format=="report") {
        const ViolationReport report=CheckLegality(snapshot,"all",10000,&control); // 独立报告，包含完整计数。
        WriteTextAtomic(path,SerializeViolations(snapshot,report),control,overwrite);
    }else Throw("INPUT_INVALID","未知导出格式",format,true);
}
/** @brief 执行带事务输出的完整流水线，详见service.hpp。 @param request 任务。 @param control 控制。 @param sink 事件。 @return 唯一终态。 */
RunResult RunPipeline(const RunRequest& request,ControlToken& control,const std::function<void(const Event&)>& sink) {
    RunResult result; // 唯一终态和已完成阶段集合。
    result.runId=request.runId.empty()?NewRunId():request.runId;
    result.outputDirectory=request.outputDirectory.empty()?(std::filesystem::path("runs")/result.runId).string():request.outputDirectory;
    const std::filesystem::path directory=std::filesystem::absolute(result.outputDirectory); // 本次独占输出目录。
    std::shared_ptr<const DesignDatabase> database=request.database; // 当前事务只读拓扑。
    std::ofstream metrics; // 完整迭代指标流，始终在本工作线程写入。
    std::ofstream events; // 事件流，不依赖GUI是否消费。
    std::uint64_t sequence=0; // 同run单调事件号。
    bool ownsDirectory=false; // 只有成功创建后才允许写终态文件。
    bool globalUnconverged=false; // 后续合法化成功不能清除全局未收敛。
    ControlToken persistence; // 仅保存已经完整的结果与终态，用户取消不破坏已完成输出。
    auto lastPreview=std::chrono::steady_clock::time_point::min(); // 最新坐标复制时刻，按配置节流。
    /** @brief 写事件并同步交给适配层。 @param kind 类别。 @param snapshot 可空完整源。 @param message 诊断。 @param carry 是否持全量快照。 无返回值。 */
    const auto emit=[&](const std::string& kind,const Snapshot* snapshot,const std::string& message,bool carry,bool deliver=true) {
        Event event; // 本次不可变事件负载。
        event.kind=kind;event.runId=result.runId;event.sequence=++sequence;event.message=message;
        if(database) event.datasetId=database->datasetId;
        if(snapshot) {
            event.stage=snapshot->stage;event.iteration=snapshot->iteration;event.revision=snapshot->revision;event.metrics=snapshot->metrics;
            if(carry) {
                Snapshot copy=*snapshot; // 节流后才复制坐标，预览不可冒充正式成功阶段。
                if(kind=="preview") {copy.stage="preview";copy.termination="in_progress";}
                event.snapshot=std::make_shared<const Snapshot>(std::move(copy));
            }
        }
        if(events.is_open()) {
            events<<nlohmann::json{{"schemaVersion",1},{"utc",utc("%Y-%m-%dT%H:%M:%SZ")},{"sequence",event.sequence},{"kind",kind},{"datasetId",event.datasetId},{"runId",event.runId},{"stage",event.stage},{"iteration",event.iteration},{"revision",event.revision},{"message",message}}.dump()<<'\n';
        }
        if(deliver&&sink) sink(event);
    };
    std::string lastMetricStage; // 最近CSV行的阶段；空表示尚未写入，覆盖initial或导入前置快照。
    std::uint64_t lastMetricRevision=0; // 同一run内最近CSV行的完整几何修订。
    /** @brief 完整指标落盘并按阶段/修订去重，阶段提交不遗漏initial。 @param snapshot 完整轮次或已提交阶段。 无返回值。 */
    const auto recordMetrics=[&](const Snapshot& snapshot) {
        if(lastMetricStage==snapshot.stage&&lastMetricRevision==snapshot.revision)return;
        const Metrics& value=snapshot.metrics; // 当前完整指标，不由GUI像素反推。
        metrics<<snapshot.runId<<','<<snapshot.stage<<','<<snapshot.iteration<<','<<snapshot.revision<<','<<csvNumber(value.elapsedMs)<<','<<csvNumber(value.hpwl)<<','<<csvNumber(value.weightedHpwl)<<','<<csvNumber(value.overflow)<<','<<csvNumber(value.densityEnergy)<<','<<csvNumber(value.lambda)<<','<<csvNumber(value.gamma)<<','<<csvNumber(value.step)<<','<<snapshot.termination<<'\n';
        lastMetricStage=snapshot.stage;lastMetricRevision=snapshot.revision;
    };
    /** @brief 将一轮完整指标流式落盘并按需复制预览。 @param snapshot 算法完整轮次源。 无返回值。 */
    const auto progress=[&](const Snapshot& snapshot) {
        recordMetrics(snapshot);
        emit("metrics",&snapshot,{},false);
        const auto now=std::chrono::steady_clock::now(); // 单调预览节流时钟。
        if(lastPreview==std::chrono::steady_clock::time_point::min()||std::chrono::duration<double>(now-lastPreview).count()>=1.0/request.config.previewHz) {
            emit("preview",&snapshot,{},true);lastPreview=now;
        }
    };
    /** @brief 检查并保存一个成功阶段，几何记录先于可视化导出。
     * @param snapshot 完整候选。 @param baseline 可空位移参考。 无返回值；失败不发布伪合法阶段。 */
    const auto commit=[&](Snapshot snapshot,const Snapshot* baseline) {
        control.checkCancelled();validatePositions(*database,snapshot.positions);
        const std::string scope=snapshot.stage=="macro_legal"?"macro":"all"; // 宏阶段不要求标准单元已经无重叠。
        const ViolationReport report=CheckLegality(snapshot,scope,10000,&control); // 独立阶段发布门槛。
        if((snapshot.stage=="macro_legal"||snapshot.stage=="standard_legal")&&report.totalCount) Throw("NO_LEGAL_SOLUTION_FOUND","独立检查否决阶段成功",snapshot.stage);
        snapshot.legalitySummary=report.totalCount?"violations="+std::to_string(report.totalCount):"legal";
        snapshot.metrics=ComputeMetrics(snapshot,baseline);
        const auto stageDirectory=directory/"stages"/snapshot.stage; // 当前阶段正式几何目录。
        WriteTextAtomic((stageDirectory/"placement.json").string(),SerializeSnapshot(snapshot),persistence);
        WriteTextAtomic((stageDirectory/"violations.json").string(),SerializeViolations(snapshot,report),persistence);
        result.stages.push_back(std::make_shared<const Snapshot>(std::move(snapshot)));
        const Snapshot& committed=*result.stages.back(); // 已经持有不可变几何的阶段。
        recordMetrics(committed);
        emit("stage",&committed,committed.termination,true);
        ExportBmp(committed,(directory/"images"/(committed.stage+".bmp")).string(),request.config,control);
        ExportGds(committed,(directory/"gds"/(committed.stage+".gds")).string(),request.config,control);
        control.safePoint();
    };
    try {
        validateConfig(request.config);
        if(request.stage!="global"&&request.stage!="macro"&&request.stage!="standard"&&request.stage!="all") Throw("INPUT_INVALID","未知运行阶段",request.stage);
        std::error_code error; // 输出目录创建错误。
        std::filesystem::create_directories(directory.parent_path(),error);if(error) Throw("IO_FAILURE",error.message(),directory.string());
        if(!std::filesystem::create_directory(directory,error)) Throw(error?"IO_FAILURE":"OUTPUT_EXISTS","结果目录不可创建或已存在",directory.string(),true);
        ownsDirectory=true;
        metrics.open(directory/"metrics.csv.tmp");events.open(directory/"events.jsonl.tmp");
        metrics.exceptions(std::ios::badbit|std::ios::failbit);events.exceptions(std::ios::badbit|std::ios::failbit);metrics.imbue(std::locale::classic());
        metrics<<"run_id,stage,iteration,revision,elapsed_ms,hpwl,weighted_hpwl,overflow,density_energy,lambda,gamma,step,status\n";
        if(!database) database=LoadDesign(request.input,request.config,control);
        WriteTextAtomic((directory/"effective-config.json").string(),SerializeConfig(request.config),persistence);
        WriteTextAtomic((directory/"input-audit.json").string(),SerializeAudit(*database),persistence);
        Snapshot initial; // 本次入口阶段，宏/标准入口由显式快照提供。
        if(request.startingSnapshot) {
            initial=*request.startingSnapshot;
            if(initial.datasetId!=database->datasetId) Throw("INPUT_INVALID","前置快照与数据集不匹配");
            initial.runId=result.runId;validatePositions(*database,initial.positions);
            globalUnconverged=initial.globalTermination=="max_iterations"||(initial.stage=="global"&&initial.termination=="max_iterations");
        }else initial=PrepareInitial(database,request.config,result.runId,control);
        if(request.stage=="macro"&&initial.stage!="global") Throw("INPUT_INVALID","宏阶段要求global前置快照");
        if(request.stage=="standard"&&initial.stage!="macro_legal") Throw("INPUT_INVALID","标准阶段要求macro_legal前置快照");
        if((request.stage=="global"||request.stage=="all")&&initial.stage!="initial") Throw("INPUT_INVALID","全局入口要求initial前置快照");
        commit(initial,nullptr);
        Snapshot current=*result.stages.back(); // 下一阶段输入为上一正式成功阶段。
        if(request.stage=="global"||request.stage=="all") {
            const Snapshot baseline=current; // 全局位移基准initial。
            current=RunGlobal(current,request.config,control,progress);globalUnconverged=current.termination!="converged";
            commit(current,&baseline);
            if(globalUnconverged&&!request.continueUnconverged) Throw("MAX_ITERATIONS","全局未收敛；显式允许后才继续合法化");
        }
        if(request.stage=="macro"||request.stage=="all") {
            const Snapshot baseline=current; // 宏位移参考global。
            current=LegalizeMacros(current,request.config,control,progress);commit(current,&baseline);
        }
        if(request.stage=="standard"||request.stage=="all") {
            const Snapshot baseline=current; // 标准位移参考macro_legal。
            const auto intervals=PartitionRows(current,&control); // 扣除宏和静态障碍的可用站点区间。
            current=LegalizeStandard(current,intervals,request.config,control,progress);commit(current,&baseline);
        }
        control.checkCancelled();result.exitCode=globalUnconverged?3:0;result.termination=globalUnconverged?"MAX_ITERATIONS":"completed";
        result.message=globalUnconverged?"请求阶段已完成，全局未收敛结论保留":"请求阶段完成";
    }catch(const Error& error) {
        result.termination=error.code;result.message=std::string(error.what())+(error.context.empty()?"":" ["+error.context+"]");result.exitCode=exitCode(error.code);
        if(ownsDirectory&&error.diagnosticSnapshot) {
            try {
                const Snapshot& diagnostic=*error.diagnosticSnapshot; // 明确非成功的最后完整/最佳失败候选。
                WriteTextAtomic((directory/"diagnostics/candidate.json").string(),SerializeSnapshot(diagnostic),persistence);
                WriteTextAtomic((directory/"diagnostics/violations.json").string(),SerializeViolations(diagnostic,CheckLegality(diagnostic,"all",10000)),persistence);
            }catch(const std::exception& failure) {result.message+="；诊断快照保存失败："+std::string(failure.what());result.exitCode=4;}
        }
    }
    catch(const std::exception& error) {result.termination="INTERNAL_FAILURE";result.message=error.what();result.exitCode=4;}
    try {
        emit("terminal",result.stages.empty()?nullptr:result.stages.back().get(),result.termination+": "+result.message,false,false);
        if(metrics.is_open()) {metrics.close();std::filesystem::rename(directory/"metrics.csv.tmp",directory/"metrics.csv");}
        if(events.is_open()) {events.close();std::filesystem::rename(directory/"events.jsonl.tmp",directory/"events.jsonl");}
        if(ownsDirectory) {
            nlohmann::json stages=nlohmann::json::array(); // 已完成阶段状态摘要。
            for(const auto& stage:result.stages) stages.push_back({{"stage",stage->stage},{"iteration",stage->iteration},{"revision",stage->revision},{"termination",stage->termination},{"legality",stage->legalitySummary}}); // 当前正式阶段。
            nlohmann::json outputs=nlohmann::json::array(); // 实际存在的输出清单，不枚举虚构成功文件。
            for(const auto& entry:std::filesystem::recursive_directory_iterator(directory)) if(entry.is_regular_file()) outputs.push_back(std::filesystem::relative(entry.path(),directory).string()); // 当前真实输出。
            WriteTextAtomic((directory/"manifest.json").string(),nlohmann::json{{"schemaVersion",1},{"softwareVersion","1.0.0"},{"compiler",__VERSION__},
                {"sourceCommit",build::sourceCommit},{"sourceDirty",build::sourceDirty},{"dependencies",nlohmann::json::parse(build::dependencyLock)},
                {"input_audit","input-audit.json"},{"inputs",database?nlohmann::json::parse(SerializeAudit(*database)).at("files"):nlohmann::json::array()},
                {"datasetId",database?database->datasetId:""},{"runId",result.runId},{"seed",request.config.seed},{"macro_seed",request.config.macroSeed.value_or(request.config.seed^0x9e3779b97f4a7c15ULL)},
                {"unit",database?database->unit:""},{"physical_scale_known",database?database->physicalScaleKnown:false},{"effective_config","effective-config.json"},
                {"termination",result.termination},{"message",result.message},{"exitCode",result.exitCode},{"stages",stages},{"outputs",outputs},
                {"limbo_commit","6c115bfb1d4dca3a92044d505ee00456aac2bd45"}}.dump(2),persistence);
        }
    }catch(const std::exception& error) {result.exitCode=4;result.termination="IO_FAILURE";result.message=std::string("保存终态失败：")+error.what();}
    // 所有可能失败的结果写出结束后，只向适配层交付一次最终结论。
    if(sink) {
        Event terminal; // 已原子确定的唯一终态事件，不再进行文件写入。
        terminal.kind="terminal";terminal.datasetId=database?database->datasetId:"";terminal.runId=result.runId;terminal.sequence=++sequence;
        terminal.message=result.termination+": "+result.message;
        if(!result.stages.empty()) {terminal.snapshot=result.stages.back();terminal.stage=terminal.snapshot->stage;terminal.iteration=terminal.snapshot->iteration;terminal.revision=terminal.snapshot->revision;terminal.metrics=terminal.snapshot->metrics;}
        try {sink(terminal);}catch(const std::exception& error) {result.message+="；终态接收器失败："+std::string(error.what());result.exitCode=4;}
        catch(...) {result.message+="；终态接收器发生未知异常";result.exitCode=4;}
    }
    return result;
}
} // namespace placement
