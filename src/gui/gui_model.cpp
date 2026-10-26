#include "gui_model.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <map>
#include <numeric>

namespace placement::gui {
namespace {
/** @brief 跨渲染包复用同数据集索引，弱缓存不额外保留数据库。 @param db 拓扑。 @param control 取消。 @return 完整只读索引。 */
std::shared_ptr<const TopologyIndex> topologyIndex(const DesignDatabase& db,ControlToken& control) {
    static std::mutex mutex; ///< 保护弱缓存，索引构建时不持锁。
    static std::map<std::string,std::weak_ptr<const TopologyIndex>> cache; ///< 按数据集指纹的弱引用。
    {
        std::lock_guard<std::mutex> lock(mutex); ///< 缓存读锁。
        const auto found=cache.find(db.datasetId); ///< 已有索引引用。
        if(found!=cache.end())if(auto existing=found->second.lock())return existing;
    }
    auto result=std::make_shared<TopologyIndex>(); ///< 后台独占索引构建缓冲。
    result->names.resize(db.nodes.size());std::iota(result->names.begin(),result->names.end(),NodeId{0});result->nodeNets.resize(db.nodes.size());
    /** @brief 按原始UTF-8名称排序；捕获db只读拓扑。 @param a 左ID。 @param b 右ID。 @return 严格字典序。 */
    std::sort(result->names.begin(),result->names.end(),[&db](NodeId a,NodeId b){return db.nodes[a].name<db.nodes[b].name;});
    for(const Pin& pin:db.pins){if(pin.id%4096==0)control.checkCancelled();result->nodeNets.at(pin.nodeId).push_back(pin.netId);} // pin 为双向验证的拓扑引脚。
    for(auto& nets:result->nodeNets){std::sort(nets.begin(),nets.end());nets.erase(std::unique(nets.begin(),nets.end()),nets.end());} // nets 为单节点所属网络。
    control.checkCancelled();
    {std::lock_guard<std::mutex> lock(mutex); /* lock 为弱缓存写锁。 */for(auto entry=cache.begin();entry!=cache.end();)if(entry->second.expired())entry=cache.erase(entry);else ++entry; /* entry 为可清理过期弱引用。 */cache[db.datasetId]=result;}
    return result;
}
/** @brief 映射数学坐标到64格索引，域外点截断。
 * @param coordinate 数学坐标DU。 @param origin 域原点DU。 @param length 正跨度DU。 @return [0,63]。 */
int block(double coordinate,double origin,double length) {return static_cast<int>(std::clamp(std::floor(64*(coordinate-origin)/std::max(length,1e-20)),0.0,63.0));}
/** @brief 扩展可空包围盒。 @param bounds 可变包围盒。 @param rect 待包含矩形。 @param first 是否首个。 无返回值。 */
void include(Rect& bounds,Rect rect,bool first) {if(first) bounds=rect;else{bounds.lx=std::min(bounds.lx,rect.lx);bounds.ly=std::min(bounds.ly,rect.ly);bounds.ux=std::max(bounds.ux,rect.ux);bounds.uy=std::max(bounds.uy,rect.uy);}}
}
/** @brief 构建后台空间包，参数契约见头文件。 */
std::shared_ptr<const RenderPacket> BuildRenderPacket(std::shared_ptr<const Snapshot> snapshot,ControlToken& control,bool inspect) {
    if(!snapshot||!snapshot->database) Throw("INPUT_INVALID","绘图缺少快照","gui");
    auto packet=std::make_shared<RenderPacket>(); ///< 后台独占构建后发布的包。
    packet->snapshot=std::move(snapshot);packet->core=coreBounds(*packet->snapshot->database);packet->chip=chipBounds(*packet->snapshot->database,packet->snapshot->positions);
    packet->boxes.resize(packet->snapshot->database->nodes.size());packet->topology=topologyIndex(*packet->snapshot->database,control);
    for(const Node& node:packet->snapshot->database->nodes) { // node 为真实实体。
        if(node.id%4096==0) control.checkCancelled();
        const Rect rect=nodeRect(node,packet->snapshot->positions.at(node.id)); ///< 当前物理矩形。
        packet->boxes[node.id]=rect;
        const int x0=block(rect.lx,packet->core.lx,packet->core.width()); ///< 左列索引。
        const int x1=block(rect.ux,packet->core.lx,packet->core.width()); ///< 右列索引。
        const int y0=block(rect.ly,packet->core.ly,packet->core.height()); ///< 下行索引。
        const int y1=block(rect.uy,packet->core.ly,packet->core.height()); ///< 上行索引。
        if((x1-x0+1)*(y1-y0+1)>16) packet->large.push_back(node.id);
        else {
            const int index=block((rect.lx+rect.ux)/2,packet->core.lx,packet->core.width())+64*block((rect.ly+rect.uy)/2,packet->core.ly,packet->core.height()); ///< 中心所属绘制块。
            include(packet->blockBounds[index],rect,packet->drawBlocks[index].empty());packet->drawBlocks[index].push_back(node.id);
            BlockStatistics& statistics=packet->statistics[index]; // 当前块仅用于LOD显示的聚合量。
            if(!node.fixed&&node.kind==NodeKind::Standard) {
                include(statistics.standardBounds,rect,statistics.areaByKind[0]==0);
                statistics.maximumMinExtent=std::max(statistics.maximumMinExtent,std::min(rect.width(),rect.height()));
            }else packet->detailBlocks[index].push_back(node.id);
            statistics.areaByKind[node.fixed?2:node.kind==NodeKind::Macro?1:0]+=rect.area();
            for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) packet->hitBlocks[y*64+x].push_back(node.id); // y/x 为相交命中块。
        }
        if(rect.area()==0) packet->ports.push_back(node.id);
    }
    if(inspect) packet->violations=CheckLegality(*packet->snapshot,"all",1000,&control);
    control.checkCancelled();return packet;
}
/** @brief 仅翻转场景y，参数契约见头文件。 */
QRectF SceneRect(Rect rect) {return {rect.lx,-rect.uy,rect.width(),rect.height()};}
/** @brief 判断当前图层的实体可见性，参数契约见头文件。 */
bool Visible(const Node& node,const Layers& layers) {return node.fixed?layers.fixed:node.kind==NodeKind::Macro?layers.macro:layers.standard;}
/** @brief 精确空间命中，参数及排序契约见头文件。 */
std::vector<NodeId> HitTest(const RenderPacket& packet,Vec2 point,double tolerance,const Layers& layers) {
    const int index=64*block(point.y,packet.core.ly,packet.core.height())+block(point.x,packet.core.lx,packet.core.width()); ///< 查询点空间块。
    std::vector<NodeId> candidates=packet.hitBlocks[index]; ///< 小实体局部候选。
    candidates.insert(candidates.end(),packet.large.begin(),packet.large.end());candidates.insert(candidates.end(),packet.ports.begin(),packet.ports.end());
    std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
    std::vector<NodeId> result; ///< 精确命中且图层可见的对象。
    for(NodeId id:candidates) { // id 为候选稳定编号。
        const Rect rect=packet.boxes.at(id); ///< 候选真实矩形。
        if(!Visible(packet.snapshot->database->nodes.at(id),layers)) continue;
        const double padding=rect.area()==0?tolerance:0; ///< 仅端口采用像素转换容差，DU。
        if(point.x>=rect.lx-padding&&point.x<=rect.ux+padding&&point.y>=rect.ly-padding&&point.y<=rect.uy+padding) result.push_back(id);
    }
    /** @brief 顶层类别、面积、ID排序。 @param a 左ID。 @param b 右ID。 @return 排序关系；捕获packet借用只读包。 */
    std::sort(result.begin(),result.end(),[&packet](NodeId a,NodeId b){
        const Node& na=packet.snapshot->database->nodes[a]; ///< 左节点。
        const Node& nb=packet.snapshot->database->nodes[b]; ///< 右节点。
        const int za=na.fixed?2:na.kind==NodeKind::Macro?0:1; ///< 左实体可视堆叠层。
        const int zb=nb.fixed?2:nb.kind==NodeKind::Macro?0:1; ///< 右实体可视堆叠层。
        if(za!=zb) return za>zb;
        if(packet.boxes[a].area()!=packet.boxes[b].area()) return packet.boxes[a].area()<packet.boxes[b].area();
        return a<b;
    });return result;
}
/** @brief 压缩曲线旧点；先按成对时间桶保留HPWL/overflow偏离均值更大的点，最新点不丢。 */
void CompactMetrics(std::deque<MetricPoint>& points) {
    while(points.size()>5000) {
        std::deque<MetricPoint> compressed; ///< 压缩后的时间桶极值。
        const std::size_t boundary=points.size()/2; ///< 近期一半完整保留。
        for(std::size_t begin=0;begin<boundary;begin+=8){ // begin 为旧段八点桶起始。
            const std::size_t end=std::min(boundary,begin+8); ///< 桶末端开区间。
            std::set<std::size_t> keep{begin,end-1}; ///< 保留桶两端和两个单位各自极值。
            for(int metric=0;metric<2;++metric){ // metric 为HPWL或overflow。
                std::size_t minimum=begin; ///< 最小指标位置。
                std::size_t maximum=begin; ///< 最大指标位置。
                for(std::size_t i=begin+1;i<end;++i){ // i 为桶内点序号。
                    const double value=metric==0?points[i].metrics.hpwl.value_or(0):points[i].metrics.overflow.value_or(0); ///< 当前单位的值。
                    const double low=metric==0?points[minimum].metrics.hpwl.value_or(0):points[minimum].metrics.overflow.value_or(0); ///< 当前最小值。
                    const double high=metric==0?points[maximum].metrics.hpwl.value_or(0):points[maximum].metrics.overflow.value_or(0); ///< 当前最大值。
                    if(value<low)minimum=i;
                    if(value>high)maximum=i;
                }
                keep.insert(minimum);keep.insert(maximum);
            }
            for(std::size_t i:keep)compressed.push_back(points[i]); // i 为按时间顺序保留的极值。
        }
        compressed.insert(compressed.end(),points.begin()+static_cast<std::ptrdiff_t>(boundary),points.end());points=std::move(compressed);
    }
}
/** @brief 重置邮箱身份，参数契约见头文件。 */
void Mailbox::reset(std::string dataset,std::string run) {std::lock_guard<std::mutex> lock(mutex_); /* lock 为邮箱互斥作用域。 */ dataset_=std::move(dataset);run_=std::move(run);sequence_=0;haveSequence_=false;terminated_=false;batch_=MailBatch{};}
/** @brief 接纳小型事件并保持有界，参数契约见头文件。 */
bool Mailbox::accept(const Event& event) {
    std::lock_guard<std::mutex> lock(mutex_); ///< 串行邮箱访问。
    if(event.runId!=run_||(!dataset_.empty()&&event.datasetId!=dataset_)||(haveSequence_&&event.sequence<=sequence_)||terminated_) return false;
    if(dataset_.empty()&&!event.datasetId.empty()) dataset_=event.datasetId;
    sequence_=event.sequence;haveSequence_=true;
    if(event.kind=="metrics") {batch_.metrics.push_back({event.iteration,event.metrics,event.stage});CompactMetrics(batch_.metrics);}
    if(!event.message.empty()) {
        batch_.logs.push_back(QString::fromStdString(event.stage+" · "+event.kind+" · "+event.message));
        if(batch_.logs.size()>5000){batch_.logs.pop_front();++batch_.droppedLogs;}
    }
    return true;
}
/** @brief 放入完整空间包；接纳事件后调用，身份仍在此检查。 */
void Mailbox::packet(std::shared_ptr<const RenderPacket> packet,bool stage) {
    std::lock_guard<std::mutex> lock(mutex_); ///< 保护槽位及完成阶段。
    if(!packet||packet->snapshot->runId!=run_||(!dataset_.empty()&&packet->snapshot->datasetId!=dataset_)) return;
    if(dataset_.empty()) dataset_=packet->snapshot->datasetId;
    if(!stage) {batch_.preview=std::move(packet);return;}
    for(auto& existing:batch_.stages) if(existing->snapshot->stage==packet->snapshot->stage){existing=std::move(packet);return;} // existing 为同阶段完成包。
    if(batch_.stages.size()<4) batch_.stages.push_back(std::move(packet));
}
/** @brief 写唯一可靠终态。 */
void Mailbox::finish(RunResult result) {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */if(terminated_||result.runId!=run_) return;terminated_=true;batch_.result=std::move(result);}
/** @brief 写非流水线任务结果。 */
void Mailbox::finishTask(QString error,bool success) {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */if(terminated_) return;terminated_=true;batch_.error=std::move(error);batch_.success=success;}
/** @brief 放入完整热图。 */
void Mailbox::heat(std::shared_ptr<const Heatmap> heat) {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */if(batch_.heatmaps.size()<2)batch_.heatmaps.push_back(std::move(heat));}
/** @brief 放入后台网络示意。 */
void Mailbox::network(std::shared_ptr<const NetworkOverlay> network) {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */if(batch_.networks.size()<2)batch_.networks.push_back(std::move(network));}
/** @brief 一次消费并清空缓冲。 */
MailBatch Mailbox::take() {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */MailBatch result=std::move(batch_); /* result 为独占消费批次。 */batch_=MailBatch{};return result;}
/** @brief 读取容量诊断。 */
std::size_t Mailbox::previewCount() const {std::lock_guard<std::mutex> lock(mutex_); /* lock 为互斥作用域。 */return batch_.preview?1:0;}
} // namespace placement::gui
