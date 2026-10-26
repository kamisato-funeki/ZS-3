#include <placement/application/service.hpp>
#include <algorithm>
#include <csignal>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace {
/** @brief 信号处理器只写sig_atomic_t，避免在异步信号中调用互斥量或IO。 */
volatile std::sig_atomic_t interrupted=0;
/** @brief 记录SIGINT。 @param signal 信号编号，不消费。 无返回值，无非信号安全调用。 */
void interruptHandler(int) {interrupted=1;}
/** @brief 命令行参数，位置参数只接受一个子命令。 */
struct Arguments {
    std::string command; ///< inspect/run/check/export。
    placement::InputSpec input; ///< 格式路径。
    std::string configPath; ///< 可空JSON配置。
    std::string snapshotPath; ///< 可空前置placement。
    std::string output; ///< 输出目录或文件。
    std::string stage="all"; ///< run阶段。
    std::string format="placement"; ///< export格式。
    std::string scope="all"; ///< check范围。
    bool overwrite=false; ///< 显式覆盖许可，仅单文件导出。
    bool continueUnconverged=false; ///< 显式允许继续合法化。
    std::map<std::string,std::string> overrides; ///< 仅显式给出的共享配置覆盖。
};
/** @brief 输出简明用法，无参数、无返回值，写标准输出。 */
void help() {
    std::cout<<"placement_cli inspect|run|check|export --aux FILE\n"
        "  或 --lef LIB.lef [--lef MORE.lef] --def DESIGN.def\n"
        "  --config FILE --seed N --target-density D --grid-nx N --grid-ny N\n"
        "  run: --stage global|macro|standard|all --snapshot placement.json --output DIR\n"
        "       --continue-unconverged --max-iterations N\n"
        "  check: --snapshot FILE --scope all|macro [--output report.json]\n"
        "  export: --snapshot FILE --format placement|bmp|gds|report --output FILE [--overwrite]\n"
        "  其他共享参数用 --set snake_case_key=value；配置与结果schemaVersion=1。\n";
}
/** @brief 严格解析显式参数，不隐式覆盖配置文件默认。
 * @param argc 参数数。 @param argv 参数数组，只读借用。 @return 参数请求，失败抛Error。 */
Arguments parseArguments(int argc,char** argv) {
    Arguments result; // 已解析请求。
    if(argc<2) placement::Throw("INPUT_INVALID","缺少子命令");
    result.command=argv[1];
    const std::set<std::string> configuration={"seed","initial_mode","target_density","grid_nx","grid_ny","overflow_stop","max_iterations","backtrack_limit","shrink","fft_threads","lambda_update_min","lambda_update_max","macro_sweeps","moves_per_macro","macro_seed","preview_hz","image_width","margin","gds_dbu_um","filler_limit","microns_per_du"}; // 常用长参数对应共享配置。
    for(int index=2;index<argc;++index) { // 当前argv索引。
        std::string key=argv[index]; // 当前选项名。
        if(key=="--overwrite") {result.overwrite=true;continue;}
        if(key=="--continue-unconverged") {result.continueUnconverged=true;continue;}
        if(key.rfind("--",0)!=0||index+1>=argc) placement::Throw("INPUT_INVALID","选项缺少值或格式错误",key);
        const std::string value=argv[++index]; // 当前显式选项值。
        key=key.substr(2);std::replace(key.begin(),key.end(),'-','_');
        if(key=="aux") result.input.auxPath=value;
        else if(key=="lef") result.input.lefPaths.push_back(value);
        else if(key=="def") result.input.defPath=value;
        else if(key=="config") result.configPath=value;
        else if(key=="snapshot") result.snapshotPath=value;
        else if(key=="output") result.output=value;
        else if(key=="stage") result.stage=value;
        else if(key=="format") result.format=value;
        else if(key=="scope") result.scope=value;
        else if(key=="set") {
            const auto separator=value.find('='); // 高级键值分隔点。
            if(separator==std::string::npos) placement::Throw("INPUT_INVALID","--set需key=value");
            result.overrides[value.substr(0,separator)]=value.substr(separator+1);
        }else if(configuration.count(key)) result.overrides[key]=value;
        else placement::Throw("INPUT_INVALID","未知命令选项",key);
    }
    return result;
}
} // namespace
/** @brief CLI入口，耗时工作复用应用服务。 @param argc 参数数。 @param argv UTF-8参数。 @return 文档04退出码。 */
int main(int argc,char** argv) {
    if(argc==1||(argc==2&&(std::string(argv[1])=="--help"||std::string(argv[1])=="help"))) {help();return 0;}
    try {
        const Arguments arguments=parseArguments(argc,argv); // 命令行显式意图。
        placement::Config config=arguments.configPath.empty()?placement::Config{}:placement::ParseConfig(placement::ReadText(arguments.configPath)); // 文件覆盖默认。
        config=placement::OverrideConfig(config,arguments.overrides);
        placement::ControlToken control; // 当前CLI任务令牌。
        std::signal(SIGINT,interruptHandler);
        std::atomic<bool> finished{false}; // 监视线程退出条件，不进入算法数据。
        /** @brief 轮询信号标志并协作取消；无输入，无返回值，只通过令牌通信。 */
        std::thread signalMonitor([&control,&finished] {
            while(!finished.load()) {if(interrupted) control.cancel();std::this_thread::sleep_for(std::chrono::milliseconds(20));}
        }); // 独立信号桥接线程，避免异步signal调用条件变量。
        /** @brief 保证所有异常路径结束信号监视线程。 */
        struct MonitorGuard {
            std::atomic<bool>& finished; ///< 被保护的退出标志。
            std::thread& thread; ///< main拥有的监视线程。
            /** @brief 析构置标志并join；无参数、无返回值，最多等一个20ms轮询周期。 */
            ~MonitorGuard() {finished.store(true);thread.join();}
        } guard{finished,signalMonitor}; // 异常安全的线程生命周期。
        if(arguments.command=="run") {
            placement::RunRequest request; // 共享应用流水线请求。
            request.input=arguments.input;request.config=config;request.stage=arguments.stage;request.outputDirectory=arguments.output;request.continueUnconverged=arguments.continueUnconverged;
            if(!arguments.snapshotPath.empty()) {
                request.database=placement::LoadDesign(arguments.input,config,control);
                request.startingSnapshot=std::make_shared<const placement::Snapshot>(placement::LoadSnapshot(request.database,arguments.snapshotPath,control));
            }
            /** @brief CLI仅输出阶段及终态诊断，完整指标由应用流式保存。 @param event 共享事件。 无返回值。 */
            const auto result=placement::RunPipeline(request,control,[](const placement::Event& event) {if(event.kind=="stage"||event.kind=="terminal") std::cerr<<event.kind<<" "<<event.stage<<" "<<event.message<<'\n';}); // 唯一终态。
            std::cout<<"run_id="<<result.runId<<" output="<<result.outputDirectory<<" status="<<result.termination<<'\n';return result.exitCode;
        }
        const auto database=placement::LoadDesign(arguments.input,config,control); // 只读数据事务。
        if(arguments.command=="inspect") {std::cout<<placement::SerializeAudit(*database)<<'\n';return 0;}
        if(arguments.command!="check"&&arguments.command!="export") placement::Throw("INPUT_INVALID","未知子命令",arguments.command);
        const placement::Snapshot snapshot=arguments.snapshotPath.empty()?placement::PrepareInitial(database,config,placement::NewRunId(),control):placement::LoadSnapshot(database,arguments.snapshotPath,control); // 当前检查/导出来源。
        if(arguments.command=="check") {
            const auto report=placement::CheckLegality(snapshot,arguments.scope,10000,&control); // 独立几何结论。
            const std::string text=placement::SerializeViolations(snapshot,report); // 完整计数和有限明细。
            if(arguments.output.empty()) std::cout<<text<<'\n';else placement::WriteTextAtomic(arguments.output,text,control,arguments.overwrite);
            return report.totalCount?3:0;
        }
        if(arguments.output.empty()) placement::Throw("INPUT_INVALID","export必须指定--output");
        placement::ExportSnapshot(snapshot,arguments.format,arguments.output,config,control,arguments.overwrite);return 0;
    }catch(const placement::Error& error) {
        std::cerr<<error.code<<": "<<error.what()<<" ["<<error.context<<"]\n";
        if(error.code=="CANCELLED") return 130;
        if(error.code=="NO_LEGAL_SOLUTION_FOUND"||error.code=="MAX_ITERATIONS") return 3;
        if(error.code=="IO_FAILURE"||error.code=="NUMERIC_FAILURE") return 4;
        return 2;
    }catch(const std::exception& error) {std::cerr<<"INTERNAL_FAILURE: "<<error.what()<<'\n';return 4;}
}
