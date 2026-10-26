#include "placement/application/global.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace placement {
namespace {
/** @brief 优化向量状态，真实节点位置仍按 NodeId 排序，固定不参加运算。 */
struct State { PositionState positions; ///< 真实节点左下角及方向，DU。
               std::vector<Vec2> fillers; ///< 临时对象中心，DU。
};
/** @brief 冻结参数目标评估。 */
struct Objective { double value=0; ///< W+λD。
                   WirelengthEvaluation wire; ///< 当前 WA 与真实对象梯度。
                   DensityEvaluation density; ///< 当前完整密度值与梯度。
                   std::vector<Vec2> gradient; ///< 移动真实对象后接 filler 的 2D 梯度。
};
/** @brief 计算真实无权及加权 HPWL。 */
struct Hpwl { double plain=0; ///< 无权 HPWL，DU。
              double weighted=0; ///< 加权 HPWL，DU。
};
/** @brief 计算真实引脚 HPWL。 @param db 拓扑。 @param positions 当前布局。 @param control 取消令牌。 @return 双口径 HPWL。 */
Hpwl hpwl(const DesignDatabase& db,const PositionState& positions,ControlToken& control) {
    Hpwl result; ///< 两类真实线长和。
    std::size_t processed=0; ///< 已处理引脚数。
    for(const Net& net:db.nets) { // net 为当前超边。
        control.checkCancelled();
        if(net.pinIds.size()<2) continue;
        Rect bounds{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}; ///< 引脚点包围盒，DU。
        for(PinId id:net.pinIds) { // id 为当前网络的引脚稳定编号。
            if(++processed%4096==0) control.checkCancelled();
            const Vec2 point=pinPosition(db,positions,db.pins.at(id)); ///< 当前实际引脚，DU。
            bounds.lx=std::min(bounds.lx,point.x);bounds.ux=std::max(bounds.ux,point.x);
            bounds.ly=std::min(bounds.ly,point.y);bounds.uy=std::max(bounds.uy,point.y);
        }
        result.plain+=bounds.width()+bounds.height();result.weighted+=net.weight*(bounds.width()+bounds.height());
    }
    if(!std::isfinite(result.plain)||!std::isfinite(result.weighted)) Throw("NUMERIC_FAILURE","真实 HPWL 非有限","global");
    return result;
}
/** @brief 对可移动真实对象及 filler 投影；固定值不写入。
 * @param state 可变优化状态。 @param db 拓扑。 @param model filler 尺寸。 @param movable 优化中的真实节点 ID。 无返回值。 */
void project(State& state,const DesignDatabase& db,const DensityModel& model,const std::vector<NodeId>& movable) {
    const Rect core=coreBounds(db); ///< 连续优化矩形域，DU。
    for(NodeId id:movable) { // id 为可移动节点稳定编号。
        const Rect r=nodeRect(db.nodes[id],state.positions[id]); ///< 当前方向后矩形。
        state.positions[id].x=std::clamp(state.positions[id].x,core.lx,core.ux-r.width());
        state.positions[id].y=std::clamp(state.positions[id].y,core.ly,core.uy-r.height());
    }
    for(std::size_t i=0;i<state.fillers.size();++i) { // i 为 filler 编号。
        const Vec2 size=model.fillerSizes()[i]; ///< filler 宽高，DU。
        state.fillers[i].x=std::clamp(state.fillers[i].x,core.lx+size.x/2,core.ux-size.x/2);
        state.fillers[i].y=std::clamp(state.fillers[i].y,core.ly+size.y/2,core.uy-size.y/2);
    }
}
/** @brief 取优化向量中的中心；真实节点的平移差等于中心差。
 * @param state 优化状态。 @param movable 可移动 ID。 @return 真实左下角后接 filler 中心，DU；仅向量差参与公式。 */
std::vector<Vec2> vectorOf(const State& state,const std::vector<NodeId>& movable) {
    std::vector<Vec2> result; ///< 连续向量的成对坐标。
    result.reserve(movable.size()+state.fillers.size());
    for(NodeId id:movable) result.push_back({state.positions[id].x,state.positions[id].y}); // id 为移动节点编号。
    result.insert(result.end(),state.fillers.begin(),state.fillers.end());return result;
}
/** @brief 给状态添加向量增量。 @param state 可变状态。 @param movable 真实移动 ID。 @param delta 同维增量。 @param factor 增量倍率。 无返回值。 */
void add(State& state,const std::vector<NodeId>& movable,const std::vector<Vec2>& delta,double factor) {
    for(std::size_t i=0;i<movable.size();++i) { // i 为优化真实对象索引。
        state.positions[movable[i]].x+=factor*delta[i].x;state.positions[movable[i]].y+=factor*delta[i].y;
    }
    for(std::size_t i=0;i<state.fillers.size();++i) { // i 为 filler 索引。
        state.fillers[i].x+=factor*delta[i+movable.size()].x;state.fillers[i].y+=factor*delta[i+movable.size()].y;
    }
}
/** @brief 向量差。 @param a 被减向量。 @param b 减向量。 @return a-b。 */
std::vector<Vec2> difference(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
    std::vector<Vec2> result(a.size()); ///< 同维差向量。
    for(std::size_t i=0;i<a.size();++i) result[i]={a[i].x-b[i].x,a[i].y-b[i].y}; // i 为对象维度索引。
    return result;
}
/** @brief 欧氏平方范数。 @param vector 成对坐标向量。 @return 所有分量平方和。 */
double normSquared(const std::vector<Vec2>& vector) { double sum=0; /* sum 为平方和。 */ for(Vec2 v:vector) sum+=v.x*v.x+v.y*v.y; /* v 为当前对象向量。 */ return sum; }
/** @brief L1 范数。 @param vector 成对坐标向量。 @return 分量绝对值之和。 */
double normOne(const std::vector<Vec2>& vector) { double sum=0; /* sum 为绝对值和。 */ for(Vec2 v:vector) sum+=std::abs(v.x)+std::abs(v.y); /* v 为当前对象向量。 */ return sum; }
/** @brief 点积。 @param a 左向量。 @param b 右向量。 @return 所有分量积之和。 */
double dot(const std::vector<Vec2>& a,const std::vector<Vec2>& b) { double sum=0; /* sum 为内积。 */ for(std::size_t i=0;i<a.size();++i) sum+=a[i].x*b[i].x+a[i].y*b[i].y; /* i 为对象索引。 */ return sum; }
/** @brief 在同一个冻结目标下评估值与导数。
 * @param state 状态。 @param db 拓扑。 @param model 密度模型。 @param movable 真实优化 ID。
 * @param gamma 平滑长度 DU。 @param lambda 密度系数。 @param control 取消令牌。 @return 完整评估；非有限抛 Error。 */
Objective evaluate(const State& state,const DesignDatabase& db,const DensityModel& model,const std::vector<NodeId>& movable,double gamma,double lambda,ControlToken& control) {
    Objective result; ///< 当前冻结目标完整评估。
    result.wire=EvaluateWirelength(db,state.positions,gamma,&control);result.density=model.evaluate(state.positions,state.fillers);
    result.value=result.wire.value+lambda*result.density.energy;
    for(NodeId id:movable) result.gradient.push_back({result.wire.gradient[id].x+lambda*result.density.gradient[id].x,result.wire.gradient[id].y+lambda*result.density.gradient[id].y}); // id 为优化真实对象。
    for(Vec2 g:result.density.fillerGradient) result.gradient.push_back({lambda*g.x,lambda*g.y}); // g 为 filler 密度导数。
    if(!std::isfinite(result.value)) Throw("NUMERIC_FAILURE","全局目标非有限","global");
    for(Vec2 g:result.gradient) if(!std::isfinite(g.x)||!std::isfinite(g.y)) Throw("NUMERIC_FAILURE","全局梯度非有限","global"); // g 为优化对象导数。
    return result;
}
/** @brief 按 overflow 调度平滑尺度。 @param overflow 当前真实溢出。 @param binWidth 几何平均 bin 宽 DU。 @return gamma，DU。 */
double smoothing(double overflow,double binWidth) {return 8*binWidth*std::pow(10.0,(20.0/9)*(std::clamp(overflow,0.1,1.0)-0.1)-1);}
}
/** @brief 实现投影 Nesterov 全局运行；详细输入和控制契约见 global.hpp。 */
Snapshot RunGlobal(const Snapshot& initial,const Config& config,ControlToken& control,const std::function<void(const Snapshot&)>& sink) {
    validateConfig(config);
    if(!initial.database) Throw("INPUT_INVALID","全局布局缺少数据库","global");
    const DesignDatabase& db=*initial.database; ///< 共享只读拓扑。
    validatePositions(db,initial.positions);
    if(initial.datasetId!=db.datasetId) Throw("INPUT_INVALID","快照数据集身份不匹配","global");
    const auto start=std::chrono::steady_clock::now(); ///< 当前任务起始单调时间。
    Snapshot snapshot=initial; ///< 唯一可发布的完整真实布局。
    snapshot.stage="global";snapshot.iteration=0;snapshot.revision=initial.revision+1;snapshot.termination.clear();snapshot.legalitySummary.clear();snapshot.metrics=Metrics{};snapshot.fillerCapped=false;snapshot.gridNx=0;snapshot.gridNy=0;
    std::vector<NodeId> movable; ///< 明确排除固定节点的优化真实对象 ID。
    for(const Node& node:db.nodes) if(!node.fixed) movable.push_back(node.id); // node 为拓扑实体。
    control.safePoint();
    bool positiveCharge=false; ///< 是否存在正面积真实移动电荷，Q=0 时跳过优化。
    for(NodeId id:movable) if(nodeRect(db.nodes[id],initial.positions[id]).area()>0) positiveCharge=true; // id 为真实移动节点。
    if(movable.empty()||!positiveCharge) {
        const Hpwl h=hpwl(db,snapshot.positions,control); ///< 固定/空设计真实线长。
        snapshot.metrics.hpwl=h.plain;snapshot.metrics.weightedHpwl=h.weighted;snapshot.metrics.overflow=0;snapshot.metrics.densityEnergy=0;snapshot.metrics.elapsedMs=0;snapshot.termination="converged";
        if(sink) sink(snapshot);
        return snapshot;
    }
    DensityModel model(db,initial.positions,config,&control); ///< 当前任务独占的密度与 filler 状态。
    snapshot.fillerCapped=model.fillerCapped();snapshot.gridNx=model.gridNx();snapshot.gridNy=model.gridNy();
    State accepted{initial.positions,model.fillerPositions()}; ///< 上一个完整接受点 u。
    project(accepted,db,model,movable);
    State lookahead=accepted; ///< Nesterov 前瞻点 v。
    State previousLookahead=lookahead; ///< 上轮实际用于求导的 v，供冻结参数差分。
    bool havePrevious=false; ///< 是否存在可用前轮前瞻点。
    double momentum=1; ///< Nesterov 动量变量 a。
    const double binWidth=std::sqrt(model.binWidth()*model.binHeight()); ///< gamma/Href 使用的网格尺度，DU。
    double rowHeight=db.rows.empty()?binWidth:db.rows.front().height; ///< 第一轮步长和微扰的行高尺度，DU。
    for(const RowSegment& row:db.rows) rowHeight=std::min(rowHeight,row.height); // row 为有效行。
    auto density=model.evaluate(accepted.positions,accepted.fillers); ///< 当前接受点初始密度。
    double gamma=smoothing(density.overflow,binWidth); ///< 当前冻结目标 WA 平滑长度，DU。
    double lambda=1; ///< 当前密度系数，初始化后受配置上下界约束。
    double acceptedStep=0; ///< 当前完整快照的实际接受步长；初始点为零。
    Objective current=evaluate(accepted,db,model,movable,gamma,lambda,control); ///< 初始目标及分离梯度。
    /** @brief 发布同轮坐标指标；只在完整接受点调用。
     * @param evaluation 当前接受点目标。 @param termination 空串或终态原因。 @return 当前真实 HPWL；有同步 sink 副作用。
     * 捕获 control/db 为取消和拓扑只读引用，accepted 为完整点，snapshot 为发布输出，start 为时钟基线；
     * lambda/gamma/acceptedStep 为当前冻结参数，sink 为同步回调引用，均不逃逸本函数。 */
    const auto publish=[&](const Objective& evaluation,const std::string& termination) {
        control.checkCancelled();
        const Hpwl h=hpwl(db,accepted.positions,control); ///< 当前真实加权与无权线长。
        snapshot.positions=accepted.positions;snapshot.metrics.hpwl=h.plain;snapshot.metrics.weightedHpwl=h.weighted;
        snapshot.metrics.overflow=evaluation.density.overflow;snapshot.metrics.densityEnergy=evaluation.density.energy;
        snapshot.metrics.lambda=lambda;snapshot.metrics.gamma=gamma;snapshot.metrics.step=acceptedStep;
        snapshot.metrics.elapsedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        snapshot.termination=termination;validatePositions(db,snapshot.positions);
        if(sink) sink(snapshot);
        control.safePoint();return h;
    };
    if(current.density.overflow<=config.overflowStop) { publish(current,"converged");return snapshot; }
    double wireNorm=normOne(current.wire.gradient); ///< 初始化 WA 梯度 L1 范数。
    double densityNorm=normOne(current.density.gradient)+normOne(current.density.fillerGradient); ///< 全部优化对象密度梯度 L1 范数。
    if(densityNorm<=1e-12) {
        std::vector<Vec2> perturbation(movable.size()+accepted.fillers.size()); ///< 确定性一次微扰，DU。
        for(std::size_t i=0;i<perturbation.size();++i) { // i 为优化对象索引。
            const double phase=static_cast<double>((config.seed%1000003)+i*2+1); ///< 种子与索引确定的相位。
            perturbation[i]={1e-3*rowHeight*std::sin(phase),1e-3*rowHeight*std::cos(phase)};
        }
        add(accepted,movable,perturbation,1);project(accepted,db,model,movable);lookahead=accepted;previousLookahead=accepted;
        current=evaluate(accepted,db,model,movable,gamma,1,control);
        wireNorm=normOne(current.wire.gradient);densityNorm=normOne(current.density.gradient)+normOne(current.density.fillerGradient);
        if(densityNorm<=1e-12 && current.density.overflow>config.overflowStop) Throw("NUMERIC_FAILURE","微扰后密度梯度仍为零且溢出未达标","global");
    }
    lambda=std::clamp(wireNorm<=1e-12?1:wireNorm/std::max(densityNorm,1e-12),config.lambdaMin,config.lambdaMax);
    current=evaluate(accepted,db,model,movable,gamma,lambda,control);
    Hpwl previousHpwl=publish(current,""); ///< 前个接受点真实 HPWL，用于 λ 调度。
    const double reference=std::max(1e-3*previousHpwl.weighted,binWidth); ///< λ 调度线长参考尺度，DU。
    int consecutive=0; ///< 连续满足 overflow 阈值的已接受轮数。
    int stalled=0; ///< λ 触顶且 overflow 无改善的连续轮数。
    double bestOverflow=current.density.overflow; ///< 迄今最优真实 overflow。
    for(int iteration=1;iteration<=config.maxIterations;++iteration) { // iteration 为将要提交的接受轮编号。
        control.checkCancelled();
        project(lookahead,db,model,movable);
        const State oldAccepted=accepted; ///< 本轮开始的完整点 uold。
        const Objective oldObjective=evaluate(oldAccepted,db,model,movable,gamma,lambda,control); ///< 以当前冻结参数重算旧接受点。
        Objective base=evaluate(lookahead,db,model,movable,gamma,lambda,control); ///< 当前冻结参数的前瞻点评估。
        double maximum=0; ///< 梯度无穷范数。
        for(Vec2 gradient:base.gradient) maximum=std::max({maximum,std::abs(gradient.x),std::abs(gradient.y)}); // gradient 为当前对象导数。
        double alpha=1/std::max(1.0,maximum/rowHeight); ///< 第一轮或无差分时步长。
        if(havePrevious) {
            const Objective previous=evaluate(previousLookahead,db,model,movable,gamma,lambda,control); ///< 差分另一端使用同一冻结参数。
            const double distance=std::sqrt(normSquared(difference(vectorOf(lookahead,movable),vectorOf(previousLookahead,movable)))); ///< 前瞻点位移范数，DU。
            const double gradientDistance=std::sqrt(normSquared(difference(base.gradient,previous.gradient))); ///< 同目标导数差范数。
            if(distance>0) alpha=distance/std::max(gradientDistance,1e-12);
        }
        if(!(alpha>0&&std::isfinite(alpha))) Throw("NUMERIC_FAILURE","回溯初始步长非正有限","global");
        bool found=false; ///< 是否找到满足二次上界的候选点。
        Objective candidateObjective; ///< 被接受候选的完整评估。
        State usedLookahead=lookahead; ///< 本轮实际求导点，重启时更新。
        for(int attempt=0;attempt<2&&!found;++attempt) { // attempt=1 为动量重启后的唯一重试。
            if(attempt==1) {
                lookahead=oldAccepted;usedLookahead=lookahead;momentum=1;havePrevious=false;
                base=evaluate(lookahead,db,model,movable,gamma,lambda,control);
                maximum=0;for(Vec2 gradient:base.gradient) maximum=std::max({maximum,std::abs(gradient.x),std::abs(gradient.y)}); // gradient 为重启后导数。
                alpha=1/std::max(1.0,maximum/rowHeight);
            }
            const auto origin=vectorOf(lookahead,movable); ///< 当前前瞻优化向量，DU。
            for(int trial=0;trial<config.backtrackLimit;++trial) { // trial 为冻结目标回溯次数。
                control.checkCancelled();
                State candidate=lookahead; ///< 本次投影梯度候选。
                add(candidate,movable,base.gradient,-alpha);project(candidate,db,model,movable);
                Objective proposal=evaluate(candidate,db,model,movable,gamma,lambda,control); ///< 当前候选的同参数目标。
                const auto delta=difference(vectorOf(candidate,movable),origin); ///< 投影后的实际步，DU。
                const double bound=base.value+dot(base.gradient,delta)+normSquared(delta)/(2*alpha)+1e-12*std::max(1.0,std::abs(base.value)); ///< 文档05的二次上界。
                if(proposal.value<=bound) { accepted=std::move(candidate);candidateObjective=std::move(proposal);found=true;break; }
                alpha*=config.shrink;
            }
        }
        if(!found) Throw("NUMERIC_FAILURE","Nesterov 回溯及动量重启均未找到可接受步长","global");
        current=std::move(candidateObjective);acceptedStep=alpha;
        control.checkCancelled();snapshot.iteration=iteration;++snapshot.revision;
        consecutive=current.density.overflow<=config.overflowStop?consecutive+1:0;
        const bool converged=consecutive>=config.convergenceWindow; ///< 连续阈值终止判据。
        const bool last=iteration==config.maxIterations; ///< 最大迭代终止判据。
        const Hpwl now=publish(current,converged?"converged":last?"max_iterations":""); ///< 同轮真实指标和完整安全点。
        if(converged||last) return snapshot;
        const double newMomentum=(1+std::sqrt(1+4*momentum*momentum))/2; ///< Nesterov a 新值。
        lookahead=accepted;
        if(current.value>oldObjective.value) momentum=1;
        else {
            add(lookahead,movable,difference(vectorOf(accepted,movable),vectorOf(oldAccepted,movable)),(momentum-1)/newMomentum);
            project(lookahead,db,model,movable);momentum=newMomentum;
        }
        previousLookahead=usedLookahead;havePrevious=true;
        const double exponent=1-(now.weighted-previousHpwl.weighted)/reference; ///< 尺度适配 HPWL 调度指数。
        const double multiplier=std::exp(std::clamp(exponent*std::log(1.1),std::log(config.lambdaUpdateMin),std::log(config.lambdaUpdateMax))); ///< 受有效配置约束的 λ 倍率。
        lambda=std::clamp(lambda*multiplier,config.lambdaMin,config.lambdaMax);
        if(current.density.overflow<bestOverflow-1e-12) {bestOverflow=current.density.overflow;stalled=0;}
        else if(lambda>=config.lambdaMax) ++stalled;else stalled=0;
        if(stalled>=config.stagnationWindow) Throw("NUMERIC_FAILURE","密度系数连续触顶且溢出停滞","global");
        gamma=smoothing(current.density.overflow,binWidth);previousHpwl=now;
    }
    Throw("NUMERIC_FAILURE","全局迭代意外退出","global");
}
} // namespace placement
