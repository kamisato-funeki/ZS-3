#include <placement/application/service.hpp>
#include <placement/application/global.hpp>
#include <placement/application/legalization.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
/** @brief 获取当前Linux进程常驻内存，不把峰值误当每轮值；无输入。 @return KiB，无法读取为0。 */
std::uint64_t residentKiB(){
    std::ifstream input("/proc/self/status"); // 只读当前进程资源，无系统修改。
    std::string key; // 当前资源键。
    while(input>>key){if(key=="VmRSS:"){std::uint64_t value=0; /* value为实际KiB。 */input>>value;return value;}std::string ignored; /* ignored为当前行其他字段。 */std::getline(input,ignored);}
    return 0;
}
/** @brief 测量同步阶段，不吞异常。 @tparam Function 可调用对象。 @param function 被测无参函数。 @return 毫秒。 */
template<class Function> double milliseconds(Function function){const auto start=std::chrono::steady_clock::now(); /* start为阶段单调起点。 */function();return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
/** @brief 构造小型真实合法化成本夹具，宏与标准分开测量。 @param macros true为F-D尺度宏场景，false为128标准单元。 @return 可独占拓扑。 */
std::shared_ptr<placement::DesignDatabase> fixture(bool macros){
    auto db=std::make_shared<placement::DesignDatabase>(); // 此测试任务独占拓扑。
    db->datasetId=macros?"profile-macros":"profile-abacus";
    for(int rowIndex=0;rowIndex<(macros?5:16);++rowIndex){ // 原始规则行索引。
        placement::RowSegment row; // 单高度2 DU的整数站点行。
        row.id=rowIndex;row.y=2*rowIndex;row.height=2;row.siteWidth=1;row.sitePitch=1;row.numSites=macros?20:256;db->rows.push_back(row);
    }
    for(int index=0;index<(macros?2:128);++index){ // 本成本样本稳定节点编号。
        placement::Node node; // 保持固定N方向的真实对象。
        node.id=index;node.name="n"+std::to_string(index);node.kind=macros?placement::NodeKind::Macro:placement::NodeKind::Standard;node.width=macros?4:3;node.height=macros?4:2;db->nodes.push_back(node);
        db->originalPositions.push_back({macros?2.+2*index:double(index%16),macros?2.:2.*(index/16),placement::Orientation::N});
    }
    if(macros)db->obstacles.push_back({{10,0,12,6},"fixed-block"});
    return db;
}
}
/** @brief 真实微阶段计时、全局每轮RSS及小规模SA/Abacus成本；不作为收敛质量证明。
 * @param argc 参数数量。 @param argv AUX OUTPUT_JSON [ITERATIONS]。 @return 已记录为0，输入失败为2。 */
int main(int argc,char** argv){
    if(argc<3){std::cerr<<"usage: placement_profile AUX OUTPUT_JSON [ITERATIONS]\n";return 2;}
    placement::ControlToken token; // 整个测量的协作控制。
    placement::Config config; // 实际32×32测量配置，filler等其余默认值保留。
    config.gridNx=32;config.gridNy=32;config.overflowStop=0;
    nlohmann::json report={{"schemaVersion",1},{"compiler",__VERSION__},{"scope","external full topology global; separate synthetic SA/Abacus cost, not large-dataset legalization"}}; // 明确性能适用范围。
    try{
        config.maxIterations=argc>3?std::stoi(argv[3]):20;
        placement::InputSpec input; // 外部参考资料只读输入。
        input.auxPath=argv[1];
        std::shared_ptr<const placement::DesignDatabase> database; // 已完成输入拓扑。
        report["load_ms"]=milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{database=placement::LoadDesign(input,config,token);}); // 无参数无返回值，赋值database并读取文件。
        placement::Snapshot initial; // 真实准备快照，不保留每轮位置。
        report["prepare_ms"]=milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{initial=placement::PrepareInitial(database,config,"profile",token);}); // 无参数无返回值，独立初始准备。
        report["datasetId"]=database->datasetId;report["effective_config"]=nlohmann::json::parse(placement::SerializeConfig(config));report["rss_before_micro_kib"]=residentKiB();
        {
            placement::DensityModel model(*database,initial.positions,config,&token); // 只用于微测，离开作用域后释放再运行优化器。
            report["wirelength_ms"]=nlohmann::json::array();report["density_with_fft_ms"]=nlohmann::json::array();report["dct_roundtrip_ms"]=nlohmann::json::array();
            std::vector<double> grid(1024,.8); // 独立常量32×32场，用于分离DCT耗时。
            for(int sample=0;sample<3;++sample){ // 包括首次FFT计划和两次热缓存，保留原始样本。
                report["wirelength_ms"].push_back(milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{const auto value=placement::EvaluateWirelength(*database,initial.positions,1,&token); /* value为实际完整网络遍历与梯度。 */report["wirelength_value"]=value.value;})); // 无输入无返回值，保存目标防止优化消除。
                report["density_with_fft_ms"].push_back(milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{const auto value=model.evaluate(initial.positions,model.fillerPositions()); /* value为核+泊松+真实及filler梯度。 */report["density_energy"]=value.energy;})); // 无输入无返回值，保存完整能量。
                report["dct_roundtrip_ms"].push_back(milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{const auto coefficients=placement::OrthonormalDct(grid,32,32); /* coefficients为正交频谱。 */grid=placement::OrthonormalDct(coefficients,32,32,true);})); // 无输入无返回值，原地保留往返网格。
            }
        }
        report["iterations"]=nlohmann::json::array();
        report["rss_before_global_kib"]=residentKiB();
        try{
            /** @brief 同步完整提交点仅记录小指标与RSS，不保存Snapshot。 @param snapshot 当前完整轮。 无返回值。 */
            const auto sink=[&](const placement::Snapshot& snapshot){report["iterations"].push_back({{"iteration",snapshot.iteration},{"rss_kib",residentKiB()},{"elapsed_ms",snapshot.metrics.elapsedMs.value_or(0)},{"hpwl",snapshot.metrics.hpwl.value_or(0)},{"overflow",snapshot.metrics.overflow.value_or(0)}});};
            report["global_ms"]=milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{const auto result=placement::RunGlobal(initial,config,token,sink); /* result只在任务结束时短暂持有。 */report["global_termination"]=result.termination;report["global_iterations"]=result.iteration;}); // 无输入无返回值，保存真实终止原因。
        }catch(const placement::Error& error){report["global_error"]={{"code",error.code},{"message",error.what()}};} // 记录失败，不伪造收敛。
        report["rss_after_global_kib"]=residentKiB();
        for(bool macros:{true,false}){ // 两类小规模成本分别计时，明确不是adaptec完整合法化。
            const auto db=fixture(macros); // 对应人工成本夹具。
            placement::Snapshot source; // 合法化前置完整源。
            source.database=db;source.datasetId=db->datasetId;source.runId="profile";source.stage=macros?"global":"macro_legal";source.positions=db->originalPositions;
            placement::Snapshot result; // 经过独立合法性复核的结果。
            const double elapsed=milliseconds(/** @brief 无参阶段测量；执行本行所述计算并更新对应结果，无返回值。 */[&]{if(macros)result=placement::LegalizeMacros(source,config,token);else result=placement::LegalizeStandard(source,placement::PartitionRows(source),config,token);}); // 无输入无返回值，完成实际搜索/簇候选比较。
            report[macros?"macro_sa":"abacus"]={{"nodes",db->nodes.size()},{"rows",db->rows.size()},{"ms",elapsed},{"iterations",result.iteration},{"violations",placement::CheckLegality(result).totalCount},{"rss_kib",residentKiB()}};
        }
        placement::WriteTextAtomic(argv[2],report.dump(2),token,true);return 0;
    }catch(const std::exception& error){report["failure"]=error.what();try{placement::WriteTextAtomic(argv[2],report.dump(2),token,true);}catch(...){std::cerr<<report.dump(2)<<'\n';}return 2;}
}
