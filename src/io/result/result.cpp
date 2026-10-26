#include <placement/io/result.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <limits>
#include <set>
#include <sstream>
#include <sys/syscall.h>
#include <unistd.h>
#include <type_traits>

namespace placement {
namespace {
/** @brief JSON实现别名，仅在序列化模块内部使用。 */
using Json=nlohmann::json;
/** @brief 严格转换配置字段，整数拒绝小数截断和溢出。
 * @tparam Value Config成员类型。 @param value JSON值。 @param key 错误键。 @return 转换值。 */
template<class Value> Value configValue(const Json& value,const std::string& key) {
    if constexpr(std::is_integral_v<Value>) {
        if(!value.is_number_integer() || value.get<long double>()<0 || value.get<long double>()>static_cast<long double>(std::numeric_limits<Value>::max()))
            Throw("INPUT_INVALID","整数配置类型或范围错误",key);
    }
    return value.get<Value>();
}
/** @brief 集中snake_case配置键与C++字段映射；X必须接收键和成员两个标识符。 */
#define PLACEMENT_CONFIG_FIELDS(X) \
 X(seed,seed) X(initial_mode,initialMode) X(target_density,targetDensity) \
 X(grid_nx,gridNx) X(grid_ny,gridNy) X(overflow_stop,overflowStop) X(max_iterations,maxIterations) \
 X(backtrack_limit,backtrackLimit) X(shrink,shrink) X(fft_threads,fftThreads) \
 X(lambda_update_min,lambdaUpdateMin) X(lambda_update_max,lambdaUpdateMax) \
 X(macro_sweeps,macroSweeps) X(moves_per_macro,movesPerMacro) X(preview_hz,previewHz) \
 X(image_width,imageWidth) X(margin,margin) X(gds_dbu_um,gdsDbuUm) X(filler_limit,fillerLimit) \
 X(lambda_min,lambdaMin) X(lambda_max,lambdaMax) X(convergence_window,convergenceWindow) \
 X(stagnation_window,stagnationWindow) X(macro_hpwl_weight,macroHpwlWeight) \
 X(macro_displacement_weight,macroDisplacementWeight) X(macro_overlap_weight,macroOverlapWeight) \
 X(macro_overlap_weight_max,macroOverlapWeightMax)
/** @brief 配置转JSON值；输入需已验证。 @param config 配置。 @return JSON对象。 */
Json configJson(const Config& config) {
    Json result={{"schemaVersion",1},{"abacus_candidate_rows","all"}}; // 完整配置对象。
    /** @brief 写入一项配置；key是输出键，member是Config字段。 */
    #define WRITE_CONFIG(key,member) result[#key]=config.member;
    PLACEMENT_CONFIG_FIELDS(WRITE_CONFIG)
    #undef WRITE_CONFIG
    result["macro_seed"]=config.macroSeed?Json(*config.macroSeed):Json(nullptr);
    result["microns_per_du"]=config.micronsPerDu?Json(*config.micronsPerDu):Json(nullptr);
    return result;
}
/** @brief 编码可空数值，不适用项保留JSON null。 @param value 数值。 @return JSON值；非有限值抛错误。 */
Json optionalNumber(const std::optional<double>& value) {
    if(!value) return nullptr;
    if(!std::isfinite(*value)) Throw("NUMERIC_FAILURE","指标含非有限数");
    return *value;
}
/** @brief 编码独立类别位移。 @param value 可空类别结果。 @return 字段齐全的统计对象或null。 */
Json displacementJson(const std::optional<DisplacementMetrics>& value) {
    if(!value) return nullptr;
    return {{"count",value->count},{"mean",value->mean},{"maximum",value->maximum},{"l1",value->l1},{"quadratic",value->quadratic}};
}
/** @brief 指标JSON值。 @param metrics 当前快照指标。 @return 对象。 */
Json metricsJson(const Metrics& metrics) {
    return {{"hpwl",optionalNumber(metrics.hpwl)},{"weightedHpwl",optionalNumber(metrics.weightedHpwl)},
        {"overflow",optionalNumber(metrics.overflow)},{"densityEnergy",optionalNumber(metrics.densityEnergy)},
        {"elapsedMs",optionalNumber(metrics.elapsedMs)},{"meanDisplacement",optionalNumber(metrics.meanDisplacement)},
        {"maxDisplacement",optionalNumber(metrics.maxDisplacement)},{"quadraticDisplacement",optionalNumber(metrics.quadraticDisplacement)},
        {"lambda",optionalNumber(metrics.lambda)},{"gamma",optionalNumber(metrics.gamma)},{"step",optionalNumber(metrics.step)},
        {"l1Displacement",optionalNumber(metrics.l1Displacement)},{"macroDisplacement",displacementJson(metrics.macroDisplacement)},
        {"standardDisplacement",displacementJson(metrics.standardDisplacement)},{"fixedDisplacement",displacementJson(metrics.fixedDisplacement)}};
}
/** @brief 编码快照身份。 @param snapshot 源。 @return 不含坐标的身份对象。 */
Json identityJson(const Snapshot& snapshot) {
    return {{"datasetId",snapshot.datasetId},{"runId",snapshot.runId},{"stage",snapshot.stage},{"iteration",snapshot.iteration},{"revision",snapshot.revision}};
}
/** @brief 解析JSON，统一错误语义。 @param text 原文。 @return JSON对象；失败抛INPUT_INVALID。 */
Json parse(const std::string& text) {
    try {return Json::parse(text);}catch(const Json::exception& error) {Throw("INPUT_INVALID",std::string("JSON解析失败：")+error.what());} // 原始解析错误定位。
}
/** @brief 校验根版本。 @param value 根对象。 无返回值，版本未知拒绝。 */
void checkVersion(const Json& value) {
    if(!value.is_object()||value.value("schemaVersion",1)!=1) Throw("INPUT_INVALID","仅支持schemaVersion=1对象");
}
} // namespace

/** @brief 严格读取配置，参数契约见result.hpp。 @param text JSON。 @param base 基线。 @return 验证配置。 */
Config ParseConfig(const std::string& text,const Config& base) {
    const Json object=parse(text); // 用户给出的字段集合。
    checkVersion(object);
    Config result=base; // 在独占副本上应用，失败不修改调用者。
    try {
        for(auto field=object.begin();field!=object.end();++field) { // 当前JSON键值迭代器。
            const std::string key=field.key(); // 当前snake_case键。
            const Json& value=field.value(); // 当前字段只读值。
            if(key=="schemaVersion") continue;
            if((key=="grid_nx"||key=="grid_ny")&&value.is_string()&&value=="auto") {if(key=="grid_nx") result.gridNx=0;else result.gridNy=0;continue;}
            bool known=false; // 本键是否有共享配置定义。
            /** @brief 严格赋值一项配置；拒绝浮点值截断为整数。 */
            #define READ_CONFIG(name,member) if(key==#name) {result.member=configValue<decltype(result.member)>(value,key);known=true;}
            PLACEMENT_CONFIG_FIELDS(READ_CONFIG)
            #undef READ_CONFIG
            if(key=="macro_seed") {
                if(value.is_null() || (value.is_string() && value=="derived")) result.macroSeed.reset();
                else {if(!value.is_number_unsigned() && (!value.is_number_integer()||value.get<long double>()<0)) Throw("INPUT_INVALID","macro_seed应为uint64");result.macroSeed=value.get<std::uint64_t>();}
                known=true;
            }
            if(key=="microns_per_du") {if(value.is_null()) result.micronsPerDu.reset();else result.micronsPerDu=value.get<double>();known=true;}
            if(key=="abacus_candidate_rows") {if(value!="all") Throw("UNSUPPORTED","本版仅支持all候选行");known=true;}
            if(!known) Throw("INPUT_INVALID","未知配置字段",key,true);
        }
    } catch(const Json::exception& error) {Throw("INPUT_INVALID",std::string("配置类型错误：")+error.what());} // 统一第三方异常。
    validateConfig(result);return result;
}
/** @brief 解析显式CLI覆盖，详见result.hpp。 @param base 已加载配置。 @param values 字符串键值。 @return 配置。 */
Config OverrideConfig(const Config& base,const std::map<std::string,std::string>& values) {
    Json overrides=Json::object(); // 只包含CLI实际提供的键，避免默认覆盖文件。
    for(const auto& item:values) { // item.first是配置键，item.second是原始文本。
        if(item.first=="initial_mode" || item.first=="abacus_candidate_rows" || (item.first=="macro_seed"&&item.second=="derived")) overrides[item.first]=item.second;
        else if((item.first=="grid_nx"||item.first=="grid_ny")&&item.second=="auto") overrides[item.first]=0;
        else overrides[item.first]=parse(item.second);
    }
    return ParseConfig(overrides.dump(),base);
}
/** @brief 序列化有效配置。 @param config 配置。 @return UTF-8 JSON。 */
std::string SerializeConfig(const Config& config) {validateConfig(config);return configJson(config).dump(2);}
/** @brief 序列化输入审计。 @param database 只读数据库。 @return JSON。 */
std::string SerializeAudit(const DesignDatabase& database) {
    Json files=Json::array(); // 按规范输入角色顺序保存指纹。
    for(const InputFile& file:database.inputFiles) files.push_back({{"path",file.path},{"sha256",file.sha256},{"size",file.size}}); // 当前输入文件。
    std::size_t fixed=0; // 输入固定对象数，非优化锁定集合。
    std::size_t macros=0; // 真实宏单元数。
    for(const Node& node:database.nodes) {if(node.fixed) ++fixed;if(node.kind==NodeKind::Macro) ++macros;} // 当前真实节点。
    return Json{{"schemaVersion",1},{"datasetId",database.datasetId},{"unit",database.unit},{"microns_per_du",database.micronsPerDu},
        {"physical_scale_known",database.physicalScaleKnown},{"files",files},{"nodes",database.nodes.size()},{"pins",database.pins.size()},
        {"nets",database.nets.size()},{"rows",database.rows.size()},{"fixed",fixed},{"macros",macros},{"warnings",database.auditWarnings}}.dump(2);
}
/** @brief 序列化快照。 @param snapshot 完整源。 @return JSON。 */
std::string SerializeSnapshot(const Snapshot& snapshot) {
    if(!snapshot.database||snapshot.datasetId!=snapshot.database->datasetId) Throw("INPUT_INVALID","快照身份不符");
    validatePositions(*snapshot.database,snapshot.positions);
    Json result=identityJson(snapshot); // 快照根身份。
    result["schemaVersion"]=1;result["termination"]=snapshot.termination;result["legalitySummary"]=snapshot.legalitySummary;
    result["globalTermination"]=snapshot.globalTermination;
    result["metrics"]=metricsJson(snapshot.metrics);result["filler_capped"]=snapshot.fillerCapped;
    result["grid_nx"]=snapshot.gridNx;result["grid_ny"]=snapshot.gridNy;
    result["initial_adjustments"]=snapshot.adjustments;
    result["unit"]=snapshot.database->unit;result["physical_scale_known"]=snapshot.database->physicalScaleKnown;
    result["microns_per_du"]=snapshot.database->micronsPerDu;result["nodes"]=Json::array();
    for(const Node& node:snapshot.database->nodes) { // 原始实例，无filler。
        const Position& position=snapshot.positions[node.id]; // 同NodeId位置。
        result["nodes"].push_back({{"nodeId",node.id},{"name",node.name},{"llx",position.x},{"lly",position.y},{"orientation",orientationName(position.orientation)},
            {"width",node.width},{"height",node.height},{"fixed",node.fixed},{"kind",node.kind==NodeKind::Macro?"macro":node.kind==NodeKind::Terminal?"terminal":"standard"}});
    }
    result["obstacles"]=Json::array();
    for(const Obstacle& obstacle:snapshot.database->obstacles) result["obstacles"].push_back({obstacle.rect.lx,obstacle.rect.ly,obstacle.rect.ux,obstacle.rect.uy}); // 导出几何的独立外部回读依据。
    return result.dump(2);
}
/** @brief 序列化违规。 @param snapshot 检查源。 @param report 独立结果。 @return JSON。 */
std::string SerializeViolations(const Snapshot& snapshot,const ViolationReport& report) {
    Json records=Json::array(); // 已限长的详细记录。
    double overlapArea=0; // 在详情完整时可复核的逐对重叠面积，DU²。
    for(const Violation& violation:report.records) if(violation.kind=="OVERLAP") overlapArea+=violation.magnitude; // 完整记录中的当前相交对。
    for(const Violation& violation:report.records) // 当前独立违规。
        records.push_back({{"kind",violation.kind},{"objectIds",violation.objectIds},{"region",{violation.region.lx,violation.region.ly,violation.region.ux,violation.region.uy}},{"magnitude",violation.magnitude},{"message",violation.message}});
    return Json{{"schemaVersion",1},{"checkedSnapshotIdentity",identityJson(snapshot)},{"scope",report.scope},{"countByType",report.countByType},
        {"records",records},{"totalCount",report.totalCount},{"truncated",report.truncated},
        {"pairwise_overlap_area",report.truncated?Json(nullptr):Json(overlapArea)},{"overlap_area_complete",!report.truncated}}.dump(2);
}
/** @brief 读取小文本。 @param path 路径。 @return 内容。 */
std::string ReadText(const std::string& path) {
    std::ifstream stream(path,std::ios::binary); // RAII文本输入。
    if(!stream) Throw("IO_FAILURE","无法读取文件",path,true);
    std::ostringstream text; // 完整JSON/配置文本缓冲。
    text<<stream.rdbuf();if(stream.bad()) Throw("IO_FAILURE","读取文件失败",path);return text.str();
}
/** @brief 重载结果，不恢复动量。 @param database 输入拓扑。 @param path placement。 @param control 取消。 @return 快照。 */
Snapshot LoadSnapshot(std::shared_ptr<const DesignDatabase> database,const std::string& path,ControlToken& control) {
    control.checkCancelled();const Json value=parse(ReadText(path)); // 读取后待验证的结果。
    checkVersion(value);
    if(!database || value.value("datasetId",std::string{})!=database->datasetId) Throw("INPUT_INVALID","输入指纹与快照不匹配",path,true);
    Snapshot result; // 仅验证成功后发布的快照。
    try {
        result.database=database;result.datasetId=database->datasetId;result.runId=value.at("runId").get<std::string>();
        result.stage=value.at("stage").get<std::string>();result.iteration=value.at("iteration");result.revision=value.at("revision");
        if(result.stage!="initial"&&result.stage!="global"&&result.stage!="macro_legal"&&result.stage!="standard_legal"&&result.stage!="preview"&&result.stage!="diagnostic") Throw("INPUT_INVALID","未知快照阶段",result.stage);
        result.termination=value.at("termination").get<std::string>();result.legalitySummary=value.value("legalitySummary",std::string{});
        result.globalTermination=value.value("globalTermination",result.stage=="global"?result.termination:std::string{});
        result.fillerCapped=value.value("filler_capped",false);result.gridNx=value.value("grid_nx",0);result.gridNy=value.value("grid_ny",0);
        result.positions.resize(database->nodes.size());
        std::vector<bool> seen(database->nodes.size(),false); // 重复/缺失ID检测。
        for(const Json& item:value.at("nodes")) { // 一条真实实例结果。
            control.checkCancelled();const NodeId id=item.at("nodeId"); // 稳定实例ID。
            if(id>=seen.size()||seen[id]||item.at("name")!=database->nodes[id].name) Throw("INPUT_INVALID","快照对象身份重复或不符",path);
            seen[id]=true;result.positions[id]={item.at("llx").get<double>(),item.at("lly").get<double>(),parseOrientation(item.at("orientation"))};
        }
        if(std::find(seen.begin(),seen.end(),false)!=seen.end()) Throw("INPUT_INVALID","快照缺少实例",path);
        if(value.contains("metrics")) {
            const Json& metrics=value.at("metrics"); // 可空数值对象。
            /** @brief 读取一个可空指标；key为JSON键，member为Metrics字段。 */
            #define READ_METRIC(key,member) if(metrics.contains(key)&&!metrics.at(key).is_null()) result.metrics.member=metrics.at(key).get<double>();
            READ_METRIC("hpwl",hpwl) READ_METRIC("weightedHpwl",weightedHpwl) READ_METRIC("overflow",overflow)
            READ_METRIC("densityEnergy",densityEnergy) READ_METRIC("elapsedMs",elapsedMs) READ_METRIC("lambda",lambda) READ_METRIC("gamma",gamma) READ_METRIC("step",step)
            #undef READ_METRIC
        }
    }catch(const Json::exception& error) {Throw("INPUT_INVALID",std::string("快照字段错误：")+error.what(),path);}
    validatePositions(*database,result.positions);
    const ViolationReport report=CheckLegality(result,result.stage=="macro_legal"?"macro":"all",0,&control); // 不信任外部合法标签。
    if((result.stage=="macro_legal"||result.stage=="standard_legal")&&report.totalCount) Throw("INPUT_INVALID","成功阶段快照未通过独立检查",path);
    result.legalitySummary=report.totalCount?"violations="+std::to_string(report.totalCount):"legal";
    // 没有优化器与有效配置上下文时，密度目标值不再被当作已验证指标。
    result.metrics.overflow.reset();result.metrics.densityEnergy.reset();result.metrics.lambda.reset();result.metrics.gamma.reset();result.metrics.step.reset();
    result.metrics=ComputeMetrics(result);return result;
}
/** @brief 原子输出；取消/失败均不发布临时文件。 @param path 目标。 @param writer 临时写出器。 @param control 取消。 @param overwrite 覆盖许可。 无返回值。 */
void AtomicWrite(const std::string& path,const std::function<void(const std::string&)>& writer,ControlToken& control,bool overwrite) {
    static std::atomic<std::uint64_t> serial{0}; // 进程内临时文件唯一序号。
    const std::filesystem::path target=std::filesystem::absolute(path); // 目标绝对路径，临时文件与其同目录。
    std::error_code filesystemError; // 不向外泄漏filesystem异常的诊断。
    if(!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(),filesystemError);
    if(filesystemError) Throw("IO_FAILURE",filesystemError.message(),path,true);
    control.checkCancelled();
    if(!overwrite&&std::filesystem::exists(target)) Throw("OUTPUT_EXISTS","目标已存在，请显式选择覆盖",path,true);
    const std::string temporary=target.string()+".tmp-"+std::to_string(::getpid())+"-"+std::to_string(serial.fetch_add(1)); // 当前写出专属临时路径。
    const int descriptor=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); // 原子占用临时路径。
    if(descriptor<0) Throw("IO_FAILURE","无法创建临时输出",temporary,true);
    ::close(descriptor);
    try {
        writer(temporary);control.checkCancelled();
        if(!std::filesystem::exists(temporary)||std::filesystem::file_size(temporary)==0) Throw("IO_FAILURE","写出器没有生成完整文件",temporary);
        // Linux renameat2保证默认不覆盖，即使另一进程在预检后创建同名文件也安全。
        const int status=static_cast<int>(::syscall(SYS_renameat2,AT_FDCWD,temporary.c_str(),AT_FDCWD,target.c_str(),overwrite?0:RENAME_NOREPLACE)); // 原子提交返回码。
        if(status!=0) Throw(errno==EEXIST?"OUTPUT_EXISTS":"IO_FAILURE","无法原子提交输出："+std::string(std::strerror(errno)),path,true);
    } catch(const Error&) {std::filesystem::remove(temporary,filesystemError);throw;}
    catch(const std::exception& error) {std::filesystem::remove(temporary,filesystemError);Throw("IO_FAILURE",error.what(),path,true);} // 保留已有成功目标。
}
/** @brief 文本原子输出。 @param path 目标。 @param text 内容。 @param control 取消。 @param overwrite 覆盖。 无返回值。 */
void WriteTextAtomic(const std::string& path,const std::string& text,ControlToken& control,bool overwrite) {
    /** @brief 将捕获text完整写入temporary并关闭；失败抛异常交由AtomicWrite清理。 @param temporary 临时路径。 无返回值。 */
    AtomicWrite(path,[&text](const std::string& temporary) {std::ofstream stream(temporary,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<text;stream.close();},control,overwrite);
}
} // namespace placement
