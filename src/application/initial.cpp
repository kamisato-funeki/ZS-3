#include <placement/application/service.hpp>
#include <algorithm>
#include <cmath>
#include <random>

namespace placement {
/** @brief 初始位置准备，保留原拓扑与固定基线。 @param database 输入。 @param config 策略。 @param runId 身份。 @param control 取消。 @return 初始快照。 */
Snapshot PrepareInitial(std::shared_ptr<const DesignDatabase> database,const Config& config,const std::string& runId,ControlToken& control) {
    validateConfig(config);if(!database) Throw("INPUT_INVALID","初始准备缺少数据库");validateDatabase(*database,false);
    Snapshot result; // 独占构建初始几何，发布后只读。
    result.database=database;result.datasetId=database->datasetId;result.runId=runId;result.stage="initial";result.positions=database->originalPositions;
    const Rect core=coreBounds(*database); // 连续初始放置允许的包围盒。
    std::mt19937_64 random(config.seed); // 同版本单线程确定性随机源。
    std::vector<double> rowAreas; // 中心采样行的面积权重，DU²。
    for(const RowSegment& row:database->rows) rowAreas.push_back(rowRect(row).area()); // 当前可放置行段。
    std::discrete_distribution<std::size_t> chooseRow(rowAreas.begin(),rowAreas.end()); // 按行面积而非行数量选择采样区域。
    std::vector<NodeId> movable; // 初始准备可变对象，固定从不投影。
    bool clustered=true; // 是否所有可移动对象完全聚集且有效。
    std::optional<Vec2> common; // 首个可移动原始左下角。
    for(const Node& node:database->nodes) { // 当前真实节点。
        if(node.fixed) continue;
        movable.push_back(node.id);
        const Position& position=result.positions[node.id]; // 原始可移动位置。
        if(!std::isfinite(position.x)||!std::isfinite(position.y)) clustered=false;
        else if(!common) common=Vec2{position.x,position.y};
        else if(position.x!=common->x||position.y!=common->y) clustered=false;
    }
    for(const NodeId id:movable) { // 当前需准备的稳定ID。
        if(id%4096==0) control.checkCancelled();
        const Node& node=database->nodes[id]; // 尺寸与输入方向。
        Position& position=result.positions[id]; // 当前任务独占位置。
        const Rect size=nodeRect(node,{0,0,node.orientation}); // 不受NaN位置影响的方向后尺寸框。
        if(core.width()<size.width()||core.height()<size.height()||database->rows.empty()) Throw("CAPACITY_INFEASIBLE","对象无法容纳于core",node.name,true);
        const bool valid=std::isfinite(position.x)&&std::isfinite(position.y)&&position.x>=core.lx&&position.y>=core.ly&&position.x+size.width()<=core.ux&&position.y+size.height()<=core.uy; // preserve的物理bbox判据。
        if(config.initialMode=="preserve"&&!valid) Throw("INPUT_INVALID","preserve遇到缺失或越界位置",node.name,true);
        if(config.initialMode=="random"||!valid) {
            bool placed=false; // 是否找到中心落在行并集且bbox落在core的样本。
            for(int attempt=0;attempt<10000&&!placed;++attempt) { // 有界拒绝采样，避免几何不可采样时死循环。
                const RowSegment& row=database->rows[chooseRow(random)]; // 当前按面积抽到的行段。
                const Rect area=rowRect(row); // 中心可采样范围。
                const double lx=std::max(area.lx,core.lx+size.width()/2); // 中心x下界，DU。
                const double ux=std::min(area.ux,core.ux-size.width()/2); // 中心x上界，DU。
                const double ly=std::max(area.ly,core.ly+size.height()/2); // 中心y下界，DU。
                const double uy=std::min(area.uy,core.uy-size.height()/2); // 中心y上界，DU。
                if(lx>ux||ly>uy) continue;
                std::uniform_real_distribution<double> x(lx,ux); // 中心x均匀采样。
                std::uniform_real_distribution<double> y(ly,uy); // 中心y均匀采样。
                position={x(random)-size.width()/2,y(random)-size.height()/2,node.orientation};placed=true;
            }
            if(!placed) Throw("CAPACITY_INFEASIBLE","初始位置中心无法在行并集内采样",node.name,true);
            result.adjustments.push_back(node.name+": "+(config.initialMode=="random"?"random":"auto修复缺失/越界"));
        }
    }
    if(config.initialMode=="auto"&&clustered&&movable.size()>1) {
        double rowHeight=database->rows.front().height; // 最小行高，DU，扰动尺度来源。
        for(const RowSegment& row:database->rows) rowHeight=std::min(rowHeight,row.height); // 当前行高。
        std::uniform_real_distribution<double> perturb(-.01*rowHeight,.01*rowHeight); // 文档规定1%行高确定性微扰。
        for(const NodeId id:movable) { // 聚集对象稳定ID。
            Position& position=result.positions[id]; // 独占位置。
            const Rect box=nodeRect(database->nodes[id],position); // 当前朝向尺寸。
            position.x=std::clamp(position.x+perturb(random),core.lx,core.ux-box.width());
            position.y=std::clamp(position.y+perturb(random),core.ly,core.uy-box.height());
            result.adjustments.push_back(database->nodes[id].name+": auto聚集扰动");
        }
    }
    control.checkCancelled();validatePositions(*database,result.positions);result.metrics=ComputeMetrics(result);
    const ViolationReport report=CheckLegality(result,"all",0,&control); // 初始可重叠，明确记录合法性。
    result.legalitySummary=report.totalCount==0?"legal":"violations="+std::to_string(report.totalCount);result.termination="prepared";
    return result;
}
} // namespace placement
