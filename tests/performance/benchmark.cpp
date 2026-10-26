#include "canvas.hpp"
#include <placement/application/global.hpp>
#include <placement/io/result.hpp>
#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsScene>
#include <cmath>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <future>
#include <sys/resource.h>

/** @brief 实际基准执行器，手动指定外部数据，不随普通CTest运行。
 * @param argc 命令参数数。 @param argv `AUX OUTPUT_JSON [GP_ITERATIONS] -platform offscreen`。
 * @return 成功记录为0；失败也保存具体诊断，不伪报优化收敛。 */
int main(int argc,char** argv) {
    QApplication application(argc,argv); // Qt主线程，offscreen也执行真实绘制与命中。
    if(argc<3) {std::cerr<<"usage: placement_benchmark AUX OUTPUT_JSON [GP_ITERATIONS] -platform offscreen\n";return 2;}
    nlohmann::json report={{"schemaVersion",1},{"qt",qVersion()},{"platform",QApplication::platformName().toStdString()},{"compiler",__VERSION__},{"viewport",{1024,720}},{"samples",100}}; // 精确环境与原始样本输出。
    placement::ControlToken control; // 本次串行基准取消令牌。
    placement::Config config; // 实际有效配置记录，算法性能单独限制预算。
    try {
        config.maxIterations=argc>3?std::stoi(argv[3]):1;config.gridNx=32;config.gridNy=32;config.overflowStop=0;
        const auto start=std::chrono::steady_clock::now(); // 包含加载准备和首次可交互的起始点。
        placement::InputSpec input; // 外部只读AUX来源。
        input.auxPath=argv[1];
        const auto database=placement::LoadDesign(input,config,control); // 完整数据审计，计时含SHA-256。
        const auto loaded=std::chrono::steady_clock::now(); // 纯加载完成时刻。
        const auto initial=std::make_shared<const placement::Snapshot>(placement::PrepareInitial(database,config,placement::NewRunId(),control)); // 默认auto准备，保留固定输入。
        const auto packet=placement::gui::BuildRenderPacket(initial,control,true); // 真实独立检查与分块空间索引。
        placement::gui::Canvas canvas; // 主线程单画布，不创建逐单元/逐引脚图元。
        canvas.resize(1024,720);canvas.show();canvas.setPacket(packet);application.processEvents();canvas.viewport()->repaint();
        const auto interactive=std::chrono::steady_clock::now(); // 首次真实可绘制时刻。
        /** @brief 后台算法的独立测量，禁止工作线程访问画布。 */
        struct AlgorithmMeasurement {
            std::optional<placement::Snapshot> result; ///< 完整global，失败时空。
            std::string error; ///< 稳定错误码或空。
            std::string message; ///< 数值/容量诊断。
            double elapsedMs=0; ///< 仅后台算法耗时，毫秒。
            double startedMs=0; ///< 相对导航起点的后台开始时刻，毫秒。
            double finishedMs=0; ///< 相对导航起点的后台结束时刻，毫秒。
        };
        const auto navigationStart=std::chrono::steady_clock::now(); // 两线程共享的单调时钟基准。
        /** @brief 与导航并发执行真实全局预算；捕获initial/config只读值、control共享取消。
         * 无输入，返回完整测量，异常转为诊断。 */
        auto algorithm=std::async(std::launch::async,[initial,config,&control,navigationStart]{
            AlgorithmMeasurement measured; // 工作线程独占测量。
            const auto start=std::chrono::steady_clock::now(); // 后台实际起始点。
            measured.startedMs=std::chrono::duration<double,std::milli>(start-navigationStart).count();
            try {measured.result=placement::RunGlobal(*initial,config,control);}
            catch(const placement::Error& error) {measured.error=error.code;measured.message=error.what();}
            measured.finishedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-navigationStart).count();measured.elapsedMs=measured.finishedMs-measured.startedMs;return measured;
        }); // 后台future在离开作用域前完成，源数据始终只读。
        std::vector<double> frames; // 100次同步平移/缩放耗时，毫秒。
        std::vector<double> hits; // 100次精确空间命中耗时，毫秒。
        nlohmann::json rendering=nlohmann::json::array(); // 每样本的策略、逐实体工作量和真实线程时段，避免隐藏自动粗化。
        const auto originalCamera=canvas.camera(); // 相机基准，DU/px。
        for(int sample=0;sample<100;++sample) { // 基准样本序号，不使用任意sleep。
            QElapsedTimer timer; // 本次同步交互计时器。
            const double began=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-navigationStart).count(); // 当前交互开始时刻，毫秒。
            timer.start();
            auto camera=originalCamera; // 围绕基准做可重现导航。
            camera.center.x+=(sample%11-5)*originalCamera.duPerPixel*4;
            camera.duPerPixel*=1+(sample%7)*.015;
            canvas.setCamera(camera);canvas.viewport()->repaint();application.processEvents();frames.push_back(timer.nsecsElapsed()/1e6);
            timer.restart();canvas.hits({packet->core.lx+(sample%10+.5)*packet->core.width()/10,packet->core.ly+(sample/10+.5)*packet->core.height()/10});hits.push_back(timer.nsecsElapsed()/1e6);
            const placement::gui::RenderStatistics stats=canvas.renderStatistics(); // 最近一次真实画帧的策略与数量。
            rendering.push_back({{"sample",sample},{"started_ms",began},{"finished_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-navigationStart).count()},{"du_per_pixel",canvas.camera().duPerPixel},{"coarse",stats.coarse},{"coarse_transitions",stats.coarseTransitions},{"aggregated_blocks",stats.aggregatedBlocks},{"entity_visits",stats.entityVisits},{"paint_count",stats.paintCount}});
        }
        /** @brief 独立最近秩分位数。 @param values 原始毫秒样本。 @param quantile [0,1]。 @return 毫秒。 */
        const auto percentile=[](std::vector<double> values,double quantile) {std::sort(values.begin(),values.end());return values[static_cast<std::size_t>(std::ceil(quantile*values.size()))-1];};
        report["datasetId"]=database->datasetId;report["input_audit"]=nlohmann::json::parse(placement::SerializeAudit(*database));report["effective_config"]=nlohmann::json::parse(placement::SerializeConfig(config));
        report["load_ms"]=std::chrono::duration<double,std::milli>(loaded-start).count();report["first_interactive_ms"]=std::chrono::duration<double,std::milli>(interactive-start).count();
        report["frames_ms"]=frames;report["hit_ms"]=hits;report["frame_p50_ms"]=percentile(frames,.5);report["frame_p95_ms"]=percentile(frames,.95);report["hit_p95_ms"]=percentile(hits,.95);
        report["scene_items"]=canvas.scene()->items().size();report["view_target_met"]=percentile(frames,.95)<=200;report["input_target_met"]=percentile(hits,.95)<=100;
        const AlgorithmMeasurement measured=algorithm.get(); // 同步收回后台结果，导航计时已经独立完成。
        if(measured.result) {
            report["global_termination"]=measured.result->termination;report["global_iterations"]=measured.result->iteration;
            report["hpwl"]=measured.result->metrics.hpwl.value_or(0);report["overflow"]=measured.result->metrics.overflow.value_or(0);
        }else {report["global_termination"]=measured.error;report["global_error"]=measured.message;}
        report["global_ms"]=measured.elapsedMs;report["global_concurrent_with_navigation"]=true;
        report["rendering_samples"]=rendering;report["global_started_ms"]=measured.startedMs;report["global_finished_ms"]=measured.finishedMs;
        /** @brief 按单调时钟区间确认线程重叠。 @param sample 当前交互时段。 @return 是否与真实算法时段相交。 */
        report["overlapping_navigation_samples"]=std::count_if(rendering.begin(),rendering.end(),[&measured](const auto& sample){return sample.at("started_ms").template get<double>()<measured.finishedMs&&sample.at("finished_ms").template get<double>()>measured.startedMs;});
        report["lod_policy"]="subpixel standard-cell block aggregation; exact macro/fixed geometry; automatic coarse mode after ten slow frames";
        canvas.grab().save(QString::fromStdString(std::string(argv[2])+".png")); // 实际offscreen画布截图，不能冒充桌面人工验收。
        struct rusage resources{}; // Linux进程峰值内存与系统资源。
        getrusage(RUSAGE_SELF,&resources);report["peak_rss_kib"]=resources.ru_maxrss;
        placement::WriteTextAtomic(argv[2],report.dump(2),control,true);return 0;
    }catch(const std::exception& error) {report["failure"]=error.what();try{placement::WriteTextAtomic(argv[2],report.dump(2),control,true);}catch(...){std::cerr<<report.dump(2)<<'\n';}return 4;}
}
