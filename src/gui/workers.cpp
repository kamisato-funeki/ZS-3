#include "workers.hpp"
#include <placement/application/global.hpp>
#include <utility>

namespace placement::gui {
/** @brief 构造工作线程资源；参数契约见头文件。 */
TaskWorker::TaskWorker(BackgroundJob job,std::shared_ptr<ControlToken> token,std::shared_ptr<Mailbox> mailbox):job_(std::move(job)),token_(std::move(token)),mailbox_(std::move(mailbox)){}
/** @brief 异常边界保证后台线程正常退出。 */
void TaskWorker::execute(){try{job_(*token_,*mailbox_);mailbox_->finishTask({},true);}catch(const Error& error){mailbox_->finishTask(QString::fromStdString(error.code+": "+error.what()+" ["+error.context+"]"),false);}catch(const std::exception& error){mailbox_->finishTask(QString::fromUtf8(error.what()),false);}catch(...){mailbox_->finishTask(tr("未知后台异常"),false);}emit finished();}
/** @brief 创建空闲主线程控制器。 */
TaskController::TaskController(QObject* parent):QObject(parent),mailbox_(std::make_shared<Mailbox>()){}
/** @brief moveToThread模式启动独占后台函数；连接只捕获主线程this及本次thread。 */
bool TaskController::start(BackgroundJob job,std::string dataset,std::string run){
    if(thread_)return false;
    mailbox_->reset(std::move(dataset),std::move(run));token_=std::make_shared<ControlToken>();thread_=new QThread(this);
    auto* worker=new TaskWorker(std::move(job),token_,mailbox_); ///< 工作线程执行对象，退出时deleteLater。
    worker->moveToThread(thread_);
    connect(thread_,&QThread::started,worker,&TaskWorker::execute);
    connect(worker,&TaskWorker::finished,thread_,&QThread::quit,Qt::DirectConnection);
    connect(worker,&TaskWorker::finished,worker,&QObject::deleteLater);
    QThread* completed=thread_; ///< 捕获本次线程身份，避免旧finished影响后续任务。
    /** @brief 在线程已结束后清理句柄并通知；捕获this主线程所有者、completed本次线程。无参数、无返回值。 */
    connect(thread_,&QThread::finished,this,[this,completed]{if(thread_!=completed)return;thread_=nullptr;completed->deleteLater();emit idle();});
    thread_->start();return true;
}
/** @brief 无排队slot直接跨线程取消。 */
void TaskController::cancel(){if(token_)token_->cancel();}
/** @brief 无排队slot请求暂停。 */
void TaskController::pause(){if(token_)token_->requestPause();}
/** @brief 无排队slot唤醒暂停。 */
void TaskController::resume(){if(token_)token_->resume();}
/** @brief 获取活动线程状态。 */
bool TaskController::running()const{return thread_!=nullptr;}
/** @brief 获取核心安全点确认。 */
bool TaskController::paused()const{return token_&&token_->isPaused();}
/** @brief 主线程定时消费，缓冲始终有界。 */
MailBatch TaskController::take(){return mailbox_->take();}
/** @brief 事务导入后台函数；捕获input/config/run为独占值，无GUI指针。 */
BackgroundJob LoadJob(InputSpec input,Config config,std::string run){
    /** @brief 加载准备后方发布完成initial。 @param token 取消。 @param mailbox 有界结果。 无返回值。 */
    return [input=std::move(input),config,run=std::move(run)](ControlToken& token,Mailbox& mailbox){
        auto database=LoadDesign(input,config,token); ///< 加载成功的只读拓扑。
        auto initial=std::make_shared<Snapshot>(PrepareInitial(database,config,run,token)); ///< 准备后完整initial。
        initial->metrics=ComputeMetrics(*initial);mailbox.packet(BuildRenderPacket(initial,token),true);mailbox.finishTask({},true);
    };
}
/** @brief placement重载后台函数，捕获database/path/run值维持源身份。 */
BackgroundJob ReloadJob(std::shared_ptr<const DesignDatabase> database,std::string path,std::string run){
    /** @brief 核对输入指纹后发布载入阶段。 @param token 取消。 @param mailbox 有界结果。 无返回值。 */
    return [database=std::move(database),path=std::move(path),run=std::move(run)](ControlToken& token,Mailbox& mailbox){auto snapshot=std::make_shared<Snapshot>(LoadSnapshot(database,path,token)); /* snapshot 为指纹匹配的完整位置。 */snapshot->runId=run;mailbox.packet(BuildRenderPacket(snapshot,token),true);mailbox.finishTask({},true);};
}
/** @brief 流水线后台函数，捕获完整request值，回调仅写有界邮箱。 */
BackgroundJob PipelineJob(RunRequest request){
    /** @brief 调用唯一应用流水线。 @param token 算法令牌。 @param mailbox GUI有界传递器。 无返回值。 */
    return [request=std::move(request)](ControlToken& token,Mailbox& mailbox){
        std::map<std::string,std::shared_ptr<const RenderPacket>> completed; ///< 最多四个可复用阶段包，避免终态重建。
        /** @brief 聚合小事件并按应用节流后的快照建立包；捕获token/mailbox借用本次后台资源。
         * @param event 应用安全点事件。 无返回值。 */
        const auto sink=[&token,&mailbox,&completed](const Event& event){if(!mailbox.accept(event))return;if(event.snapshot&&(event.kind=="preview"||event.kind=="stage")){const auto packet=BuildRenderPacket(event.snapshot,token,event.kind=="stage"); /* packet 为本次修订不可变空间包。 */if(event.kind=="stage")completed[event.snapshot->stage]=packet;mailbox.packet(packet,event.kind=="stage");}};
        RunResult result=RunPipeline(request,token,sink); ///< 应用层唯一终态，完整成功阶段保留。
        // 终态可靠重装最后成功阶段；取消后不再做可取消检查，复用已建包或以独立短任务令牌构建。
        ControlToken finalToken; ///< 仅用于安装已完成几何，不继承取消标志。
        for(const auto& stage:result.stages) {const auto found=completed.find(stage->stage); /* found 为此前构建的阶段包。 */mailbox.packet(found!=completed.end()?found->second:BuildRenderPacket(stage,finalToken,false),true);} // stage 为最多四份完整阶段。
        mailbox.finish(std::move(result));
    };
}
/** @brief 独立导出后台函数；捕获源快照和全部导出参数值。 */
BackgroundJob ExportJob(std::shared_ptr<const Snapshot> snapshot,std::string format,std::string path,Config config,bool overwrite){
    /** @brief 执行统一原子导出。 @param token 独立导出取消。 @param mailbox 独立结果。 无返回值。 */
    return [snapshot=std::move(snapshot),format=std::move(format),path=std::move(path),config,overwrite](ControlToken& token,Mailbox& mailbox){if(snapshot->termination.empty()){Snapshot preview=*snapshot; /* preview 为导出线程独占的非最终副本。 */preview.stage="preview";ExportSnapshot(preview,format,path,config,token,overwrite);}else ExportSnapshot(*snapshot,format,path,config,token,overwrite);mailbox.finishTask({},true);};
}
/** @brief 同轮真实与静态电荷密度；捕获snapshot/config值，主线程不参与FFT，不重建迭代filler。 */
BackgroundJob HeatJob(std::shared_ptr<const Snapshot> snapshot,Config config){
    /** @brief 按源快照构造独立模型并求网格。 @param token 取消。 @param mailbox 热图结果。 无返回值。 */
    return [snapshot=std::move(snapshot),config](ControlToken& token,Mailbox& mailbox){
        DensityModel model(*snapshot->database,snapshot->positions,config,&token); ///< 独立后台密度模型。
        const auto evaluation=model.evaluate(snapshot->positions,model.fillerPositions()); ///< 仅采用与filler坐标无关的真实/静态网格。
        auto heat=std::make_shared<Heatmap>(); ///< 后台构造后不可变网格。
        heat->dataset=snapshot->datasetId;heat->run=snapshot->runId;heat->stage=snapshot->stage;heat->revision=snapshot->revision;heat->nx=model.gridNx();heat->ny=model.gridNy();heat->target=config.targetDensity;heat->density=evaluation.realCharge;
        // Snapshot不含优化任务私有的filler；不能用初始filler冒充源revision的总密度。
        for(std::size_t bin=0;bin<heat->density.size();++bin)heat->density[bin]=(heat->density[bin]+evaluation.staticCharge[bin])/(model.binWidth()*model.binHeight()); // bin为同轮网格编号，转为无量纲密度。
        mailbox.heat(heat);
    };
}
/** @brief 构造后台网络平均与确定性等距抽样，捕获snapshot/net值。 */
BackgroundJob NetworkJob(std::shared_ptr<const Snapshot> snapshot,NetId net,std::uint64_t requestId){
    /** @brief 全部引脚计算平均，仅保留5000抽样。 @param token 取消。 @param mailbox 网络结果。 无返回值。 */
    return [snapshot=std::move(snapshot),net,requestId](ControlToken& token,Mailbox& mailbox){
        auto overlay=std::make_shared<NetworkOverlay>(); ///< 有界高亮值。
        overlay->dataset=snapshot->datasetId;overlay->run=snapshot->runId;overlay->stage=snapshot->stage;overlay->revision=snapshot->revision;
        overlay->netId=net;overlay->requestId=requestId;
        const auto& ids=snapshot->database->nets.at(net).pinIds; ///< 当前网络全部引脚编号。
        overlay->originalCount=ids.size();
        const std::size_t count=std::min<std::size_t>(5000,ids.size()); ///< 实际抽样数。
        std::size_t next=0; ///< 下一个应抽样的序号。
        for(std::size_t i=0;i<ids.size();++i){ // i 为全部引脚序号。
            if(i%4096==0)token.checkCancelled();
            const Vec2 point=pinPosition(*snapshot->database,snapshot->positions,snapshot->database->pins[ids[i]]); ///< 实际引脚DU点。
            // 在线均值避免巨量坐标求和溢出。
            overlay->mean.x+=(point.x-overlay->mean.x)/(i+1);overlay->mean.y+=(point.y-overlay->mean.y)/(i+1);
            if(next<count&&i==next*ids.size()/count){overlay->pins.push_back(point);++next;}
        }
        mailbox.network(overlay);
    };
}
} // namespace placement::gui
