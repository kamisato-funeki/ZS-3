#include <placement/application/service.hpp>
#include <placement/io/visualization.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace placement {
namespace {
/** @brief 取得人工AUX路径，无输入无副作用。 @return 绝对路径。 */
std::string tinyInput() {return std::string(PLACEMENT_SOURCE_DIR)+"/tests/fixtures/tiny/tiny.aux";}
/** @brief 为每项测试分配唯一临时目录，析构只删除本测试创建内容。 */
struct TemporaryDirectory {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("placement-test-"+NewRunId()); ///< 本测试专属目录。
    /** @brief 创建新目录，无输入无返回值。 */
    TemporaryDirectory() {std::filesystem::create_directory(path);}
    /** @brief 删除本测试临时文件，无输入无返回值，不抛异常。 */
    ~TemporaryDirectory() {std::error_code error; /* error为清理失败诊断。 */ std::filesystem::remove_all(path,error);}
};
/** @brief 加载真实小夹具。 @param control 取消令牌。 @return 已验证拓扑。 */
std::shared_ptr<const DesignDatabase> tinyDatabase(ControlToken& control) {InputSpec input; /* input为AUX请求。 */ input.auxPath=tinyInput();return LoadDesign(input,Config{},control);}
/** @brief TC-UI-02/NF-02：共享配置未知字段、范围、覆盖和往返；无输入，无返回值。 */
TEST(EffectiveConfiguration,StrictTypesPrecedenceAndRoundtrip) {
    const Config file=ParseConfig(R"({"seed":7,"target_density":0.7,"grid_nx":"auto","grid_ny":"auto"})"); // 文件配置。
    const Config merged=OverrideConfig(file,{{"seed","42"}}); // 显式CLI只覆盖seed。
    EXPECT_EQ(merged.seed,42U);
    EXPECT_DOUBLE_EQ(merged.targetDensity,.7);
    EXPECT_EQ(SerializeConfig(ParseConfig(SerializeConfig(merged))),SerializeConfig(merged));
    EXPECT_THROW(ParseConfig(R"({"typo":1})"),Error);
    EXPECT_THROW(ParseConfig(R"({"max_iterations":1.5})"),Error);
    EXPECT_THROW(ParseConfig(R"({"target_density":0})"),Error);
    EXPECT_THROW(ParseConfig(R"({"grid_nx":30,"grid_ny":32})"),Error);
    EXPECT_THROW(ParseConfig(R"({"convergence_window":1})"),Error);
    EXPECT_THROW(ParseConfig(R"({"gds_dbu_um":0})"),Error);
    EXPECT_THROW(ParseConfig(R"({"target_density":NaN})"),Error);
}
/** @brief TC-VIS-11：写失败/取消/目标竞争均不破坏旧文件；无输入，无返回值。 */
TEST(AtomicOutput,PreservesExistingAndCleansFailedWrites) {
    TemporaryDirectory directory; // 本测试拥有的目录。
    ControlToken control; // 未取消写出令牌。
    const std::string target=(directory.path/"result.json").string(); // 已有成功输出。
    WriteTextAtomic(target,"old",control);
    EXPECT_THROW(WriteTextAtomic(target,"new",control),Error);
    EXPECT_EQ(ReadText(target),"old");
    /** @brief 模拟写出中途失败。 @param path 临时路径。 无返回值，必抛业务错误。 */
    EXPECT_THROW(AtomicWrite(target,[](const std::string& path) {std::ofstream file(path); /* file是半写流。 */ file<<"partial";Throw("IO_FAILURE","injected");},control,true),Error);
    EXPECT_EQ(ReadText(target),"old");
    ControlToken cancelled; // 独立导出取消令牌。
    /** @brief 完成临时写后注入取消。 @param path 临时路径。 无返回值，只取消本任务。 */
    EXPECT_THROW(AtomicWrite(target,[&cancelled](const std::string& path) {std::ofstream file(path); /* file是尚未发布的完整流。 */ file<<"new";file.close();cancelled.cancel();},cancelled,true),Error);
    EXPECT_EQ(ReadText(target),"old");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),std::filesystem::directory_iterator{}),1);
}
/** @brief TC-IN-10/11：auto可复现且不修改原始输入；无输入，无返回值。 */
TEST(InitialPreparation,RandomPreservesFixedAndOriginalBaseline) {
    ControlToken control; // 初始准备令牌。
    const auto database=tinyDatabase(control); // 原始只读拓扑。
    Config config; // 显式random配置。
    config.initialMode="random";
    const Snapshot first=PrepareInitial(database,config,"first",control); // 第一次随机结果。
    const Snapshot second=PrepareInitial(database,config,"second",control); // 同种子第二次结果。
    EXPECT_EQ(first.positions[0].x,second.positions[0].x);
    EXPECT_EQ(first.positions[0].y,second.positions[0].y);
    EXPECT_EQ(first.positions[2].x,-1);
    EXPECT_EQ(database->originalPositions[0].x,1);
    EXPECT_FALSE(first.adjustments.empty());
}
/** @brief TC-INT-03/LG-16：快照指纹和伪合法标签独立复查；无输入，无返回值。 */
TEST(ResultReload,RejectsForgedLegalityAndRecomputesMetrics) {
    TemporaryDirectory directory; // 结果测试目录。
    ControlToken control; // 输入及保存令牌。
    const auto database=tinyDatabase(control); // 原始输入。
    Snapshot snapshot=PrepareInitial(database,Config{},"reload",control); // 故意重叠的initial。
    snapshot.stage="standard_legal";snapshot.legalitySummary="legal";
    const std::string path=(directory.path/"placement.json").string(); // 伪造快照。
    WriteTextAtomic(path,SerializeSnapshot(snapshot),control);
    EXPECT_THROW(LoadSnapshot(database,path,control),Error);
    snapshot.stage="initial";snapshot.metrics.hpwl=999999;
    WriteTextAtomic(path,SerializeSnapshot(snapshot),control,true);
    const Snapshot loaded=LoadSnapshot(database,path,control); // 真实HPWL重算而非信任文件。
    EXPECT_NE(*loaded.metrics.hpwl,999999);
    EXPECT_NE(loaded.legalitySummary,"legal");
    auto changed=std::make_shared<DesignDatabase>(*database); // 模拟被更改的输入指纹。
    changed->datasetId="different";
    EXPECT_THROW(LoadSnapshot(changed,path,control),Error);
}
/** @brief TC-VIS-01：F-G角点与单次Y翻转手算；无输入，无返回值。 */
TEST(ImageGeometry,IndependentCornerMapping) {
    const ImageTransform transform=MakeImageTransform({-10,-5,10,5},200,10); // F-G指定有效区和边距。
    const Vec2 upper=transform.map({-8,-2}); // 对象左上数学点。
    const Vec2 lower=transform.map({-6,-4}); // 对象右下数学点。
    EXPECT_DOUBLE_EQ(transform.scale,10);
    EXPECT_DOUBLE_EQ(upper.x,30);
    EXPECT_DOUBLE_EQ(upper.y,80);
    EXPECT_DOUBLE_EQ(lower.x,50);
    EXPECT_DOUBLE_EQ(lower.y,100);
}
/** @brief TC-VIS-03/04/06/11：无窗口双导出及元数据失败保护；无输入，无返回值。 */
TEST(ExportFormats,RealBmpGdsAndMetadataFailureProtection) {
    TemporaryDirectory directory; // 独占导出目录。
    ControlToken control; // 导出取消令牌。
    const auto database=tinyDatabase(control); // 人工几何。
    const Snapshot snapshot=PrepareInitial(database,Config{},"export",control); // 明确可含重叠，不阻止可视化。
    Config config; // 小型测试图配置。
    config.imageWidth=320;
    const std::string bmp=(directory.path/"layout.bmp").string(); // BMP目标。
    const std::string gds=(directory.path/"layout.gds").string(); // GDS目标。
    ExportBmp(snapshot,bmp,config,control);ExportGds(snapshot,gds,config,control);
    EXPECT_EQ(ReadText(bmp).substr(0,2),"BM");
    EXPECT_GT(std::filesystem::file_size(gds),100U);
    EXPECT_TRUE(std::filesystem::exists(gds+".metadata.json"));
    const std::string old=ReadText(gds); // 已成功GDS字节基线。
    std::filesystem::remove(gds+".metadata.json");std::filesystem::create_directory(gds+".metadata.json");
    EXPECT_THROW(ExportGds(snapshot,gds,config,control,true),Error);
    EXPECT_EQ(ReadText(gds),old);
    config.gdsDbuUm=1000;
    EXPECT_THROW(ExportGds(snapshot,(directory.path/"collapsed.gds").string(),config,control),Error);
}
/** @brief TC-INT-02/04：完整四阶段、独立最终合法和可重载；无输入，无返回值。 */
TEST(ApplicationPipeline,CompleteStagesUniqueTerminalAndReproducibility) {
    TemporaryDirectory directory; // 两次运行的共同父目录。
    RunRequest request; // 完整流水线共享请求。
    request.input.auxPath=tinyInput();request.config.overflowStop=.9;request.config.imageWidth=320;request.runId="first";request.outputDirectory=(directory.path/"first").string();
    ControlToken firstControl; // 首次运行令牌。
    int terminalCount=0; // 终态通知次数，必须恰为1。
    /** @brief 累计唯一终态。 @param event 不可变应用事件。 无返回值。 */
    const RunResult first=RunPipeline(request,firstControl,[&terminalCount](const Event& event){if(event.kind=="terminal") ++terminalCount;}); // 首次独立运行。
    ASSERT_EQ(first.exitCode,0)<<first.message;
    ASSERT_EQ(first.stages.size(),4U);
    EXPECT_EQ(terminalCount,1);
    EXPECT_EQ(CheckLegality(*first.stages.back()).totalCount,0U);
    ControlToken secondControl; // 同种子重跑令牌。
    request.runId="second";request.outputDirectory=(directory.path/"second").string();
    const RunResult second=RunPipeline(request,secondControl); // 第二次独立运行。
    ASSERT_EQ(second.exitCode,0)<<second.message;
    for(std::size_t node=0;node<first.stages.back()->positions.size();++node) { // 按稳定ID核对所有真实实例。
        EXPECT_DOUBLE_EQ(first.stages.back()->positions[node].x,second.stages.back()->positions[node].x);
    EXPECT_DOUBLE_EQ(first.stages.back()->positions[node].y,second.stages.back()->positions[node].y);
    }
    const Snapshot loaded=LoadSnapshot(first.stages.back()->database,(directory.path/"first/stages/standard_legal/placement.json").string(),firstControl); // 最终结果回读。
    EXPECT_EQ(CheckLegality(loaded).totalCount,0U);
}
/** @brief TC-INT-06：分开运行宏/标准也保留全局未收敛；无输入，无返回值。 */
TEST(ApplicationPipeline,PreservesUnconvergedAcrossSeparateRequests) {
    TemporaryDirectory directory; // 分阶段测试目录。
    ControlToken control; // 全部独立任务前的加载令牌。
    const auto database=tinyDatabase(control); // 人工输入。
    Snapshot global=PrepareInitial(database,Config{},"original",control); // 作为前置global的可用有限几何。
    global.stage="global";global.termination="max_iterations";global.globalTermination="max_iterations";
    RunRequest request; // 显式继续宏阶段。
    request.database=database;request.startingSnapshot=std::make_shared<const Snapshot>(global);request.stage="macro";request.config.imageWidth=320;request.outputDirectory=(directory.path/"macro").string();
    ControlToken macroControl; // 宏任务令牌。
    const RunResult macro=RunPipeline(request,macroControl); // 宏可合法但全局状态保留。
    ASSERT_EQ(macro.exitCode,3)<<macro.message;
    ASSERT_FALSE(macro.stages.empty());
    EXPECT_EQ(macro.stages.back()->globalTermination,"max_iterations");
    request.startingSnapshot=macro.stages.back();request.stage="standard";request.outputDirectory=(directory.path/"standard").string();
    ControlToken standardControl; // 标准任务令牌。
    const RunResult standard=RunPipeline(request,standardControl); // 跨第二个新run仍应保留上游结论。
    EXPECT_EQ(standard.exitCode,3)<<standard.message;
    ASSERT_FALSE(standard.stages.empty());
    EXPECT_EQ(standard.stages.back()->stage,"standard_legal");
}
/** @brief TC-GP-14/INT-06：目标密度不足为配置失败，不是内部失败；无输入，无返回值。 */
TEST(ApplicationPipeline,CapacityUsesInputErrorExitCode) {
    TemporaryDirectory directory; // 失败运行目录。
    RunRequest request; // 不可行目标密度任务。
    request.input.auxPath=tinyInput();request.config.targetDensity=.01;request.config.imageWidth=320;request.outputDirectory=(directory.path/"capacity").string();
    ControlToken control; // 当前失败任务令牌。
    const RunResult result=RunPipeline(request,control); // 容量预检结果。
    EXPECT_EQ(result.exitCode,2);
    EXPECT_EQ(result.termination,"CAPACITY_INFEASIBLE");
}
/** @brief 实际算法取消保留初始阶段及最后完整诊断，不发布伪global；无输入，无返回值。 */
TEST(ApplicationPipeline,CancelPersistsCompleteDiagnosticOnly) {
    TemporaryDirectory directory; // 取消任务独占输出。
    RunRequest request; // 真实全局任务，确保初始overflow未达0阈值。
    request.input.auxPath=tinyInput();request.stage="global";request.config.overflowStop=0;request.config.imageWidth=320;request.outputDirectory=(directory.path/"cancel").string();
    ControlToken control; // 在完整算法指标事件上确定性取消。
    int terminal=0; // 唯一终态计数。
    /** @brief 首次完整指标触发取消，不依赖时序睡眠。 @param event 共享事件。 无返回值。 */
    const RunResult result=RunPipeline(request,control,[&](const Event& event){if(event.kind=="metrics")control.cancel();if(event.kind=="terminal")++terminal;}); // 真实取消返回值。
    EXPECT_EQ(result.exitCode,130);
    EXPECT_EQ(terminal,1);
    ASSERT_EQ(result.stages.size(),1U);
    EXPECT_EQ(result.stages.front()->stage,"initial");
    EXPECT_TRUE(std::filesystem::exists(directory.path/"cancel/diagnostics/candidate.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path/"cancel/stages/global/placement.json"));
}
} // namespace
} // namespace placement
