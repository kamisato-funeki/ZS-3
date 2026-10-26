#include "placement/application/global.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace placement {
namespace {
/** @brief 精确区域分解的一维区间，采用 DU。 */
struct Interval { double low; ///< 左/下界，DU。
                  double high; ///< 右/上界，DU。
};
/** @brief 求矩形交集。 @param a 第一矩形。 @param b 第二矩形。 @return 非负交集矩形，DU。 */
Rect intersect(Rect a,Rect b) { return {std::max(a.lx,b.lx),std::max(a.ly,b.ly),std::max(std::max(a.lx,b.lx),std::min(a.ux,b.ux)),std::max(std::max(a.ly,b.ly),std::min(a.uy,b.uy))}; }
/** @brief 合并一维区间。 @param input 可重叠区间。 @return 不相交升序区间。 */
std::vector<Interval> merge(std::vector<Interval> input) {
    /** @brief 按下界排序。 @param a 左区间。 @param b 右区间。 @return 排序关系。 */
    std::sort(input.begin(),input.end(),[](const Interval& a,const Interval& b){return a.low<b.low;});
    std::vector<Interval> result; ///< 合并后的区间。
    for (const Interval& interval:input) { // interval 为待加入区间。
        if (interval.high<=interval.low) continue;
        if (result.empty()||interval.low>result.back().high) result.push_back(interval);
        else result.back().high=std::max(result.back().high,interval.high);
    }
    return result;
}
/** @brief 用 x 扫描线构造 P\F 的不相交矩形，静态区域取其补集。
 * @param core 包围盒。 @param rows 行矩形。 @param blockers 固定及硬阻挡。 @param control 可空取消令牌。
 * @return 有效区域的不相交矩形；DU。 */
std::vector<Rect> freeRegion(Rect core,const std::vector<Rect>& rows,const std::vector<Rect>& blockers,ControlToken* control) {
    std::vector<double> cuts{core.lx,core.ux}; ///< 所有 x 拓扑变化边界，DU。
    for(const Rect& r:rows) { cuts.push_back(r.lx);cuts.push_back(r.ux); } // r 为裁剪后的行。
    for(const Rect& r:blockers) { cuts.push_back(r.lx);cuts.push_back(r.ux); } // r 为裁剪后的障碍。
    std::sort(cuts.begin(),cuts.end()); cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
    std::vector<Rect> free; ///< 精确无重叠自由区域分解。
    for(std::size_t slab=1;slab<cuts.size();++slab) { // slab 为相邻 x 边界编号。
        if(control) control->checkCancelled();
        const double mid=(cuts[slab-1]+cuts[slab])/2; ///< 当前开区间代表 x，DU。
        std::vector<Interval> allowed; ///< 当前 x 上可放置行的 y 区间。
        std::vector<Interval> blocked; ///< 当前 x 上固定阻挡 y 区间。
        for(const Rect& r:rows) if(mid>=r.lx&&mid<r.ux) allowed.push_back({r.ly,r.uy}); // r 为当前行矩形。
        for(const Rect& r:blockers) if(mid>=r.lx&&mid<r.ux) blocked.push_back({r.ly,r.uy}); // r 为当前阻挡矩形。
        allowed=merge(std::move(allowed)); blocked=merge(std::move(blocked));
        for(const Interval& a:allowed) { // a 为并集后互不相交行区间。
            double cursor=a.low; ///< 下一个尚未判定的 y 下界，DU。
            for(const Interval& b:blocked) { // b 为并集后阻挡区间。
                if(b.high<=cursor||b.low>=a.high) continue;
                if(b.low>cursor) free.push_back({cuts[slab-1],cursor,cuts[slab],std::min(b.low,a.high)});
                cursor=std::max(cursor,b.high); if(cursor>=a.high) break;
            }
            if(cursor<a.high) free.push_back({cuts[slab-1],cursor,cuts[slab],a.high});
        }
    }
    return free;
}
/** @brief 三角核二阶原函数，避免远区立方相消。 @param t 相对距离 DU。 @param h 核宽 DU。 @return DU。 */
double primitive(double t,double h) {
    if(t<=-h) return 0;
    if(t>=h) return t;
    if(t<0) return std::pow(t+h,3)/(6*h*h);
    return t+std::pow(h-t,3)/(6*h*h);
}
/** @brief 三角核累积分布。 @param t 相对距离 DU。 @param h 核宽 DU。 @return 无量纲。 */
double cumulative(double t,double h) {
    if(t<=-h) return 0;
    if(t>=h) return 1;
    if(t<0) return (t+h)*(t+h)/(2*h*h);
    return 1-(h-t)*(h-t)/(2*h*h);
}
/** @brief 一个轴在某 bin 上的归一化分配与中心导数。 */
struct AxisWeight { int index; ///< 轴向 bin 索引。
                    double weight; ///< 归一化分配，无量纲。
                    double derivative; ///< 对中心的导数，DU⁻¹。
};
/** @brief 对矩形和 bin 积分三角卷积，并对截断分母求导。
 * @param center 对象中心 DU。 @param size 对象轴尺寸 DU。 @param origin 域起点 DU。
 * @param h bin 宽与核宽 DU。 @param count bin 数。 @return 有限支撑内的归一化权重与导数。 */
std::vector<AxisWeight> axisWeights(double center,double size,double origin,double h,int count) {
    const double low=center-size/2-origin; ///< 相对域原点的对象左界，DU。
    const double high=center+size/2-origin; ///< 相对域原点的对象右界，DU。
    const int begin=static_cast<int>(std::clamp(std::floor((low-h)/h),0.0,static_cast<double>(count))); ///< 支撑首 bin。
    const int end=static_cast<int>(std::clamp(std::floor((high+h)/h),-1.0,static_cast<double>(count-1))); ///< 支撑末 bin。
    std::vector<AxisWeight> weights; ///< 尚未归一化的一维积分。
    double sum=0; ///< 域内轴向积分总量，DU。
    double derivativeSum=0; ///< 轴向积分总量的中心导数。
    for(int index=begin;index<=end;++index) { // index 为支撑中的轴向 bin。
        const double a=index*h; ///< bin 左界，DU。
        const double b=(index+1)*h; ///< bin 右界，DU。
        const double value=primitive(b-low,h)-primitive(a-low,h)-primitive(b-high,h)+primitive(a-high,h); ///< 精确双重积分，DU。
        const double derivative=-cumulative(b-low,h)+cumulative(a-low,h)+cumulative(b-high,h)-cumulative(a-high,h); ///< 对中心的精确积分导数。
        weights.push_back({index,std::max(0.0,value),derivative}); sum+=std::max(0.0,value); derivativeSum+=derivative;
    }
    if(!(sum>0&&std::isfinite(sum))) Throw("NUMERIC_FAILURE","密度核支撑不与 core 相交","global");
    for(AxisWeight& weight:weights) { // weight 为当前轴 bin 的积分项。
        weight.derivative=(weight.derivative*sum-weight.weight*derivativeSum)/(sum*sum); weight.weight/=sum;
    }
    return weights;
}
/** @brief 向上取 2 次幂并截断自动网格。 @param value 目标 bin 数。 @return [32,512] 的 2 次幂。 */
int powerGrid(double value) { int count=32; /* count 为当前候选网格长度。 */ while(count<512&&count<value) count*=2; return count; }
}
/** @brief 密度模型私有资源；单任务拥有，数据库只借用。 */
struct DensityModel::Impl {
    const DesignDatabase& db; ///< 借用的已验证数据库。
    Config config; ///< 有效配置副本。
    ControlToken* control; ///< 借用可空取消令牌。
    Rect core; ///< 优化矩形域，DU。
    int nx=32; ///< 横向 bin 数。
    int ny=32; ///< 纵向 bin 数。
    double dx=0; ///< bin 宽，DU。
    double dy=0; ///< bin 高，DU。
    double area=0; ///< P\F 面积，DU²。
    double realTotal=0; ///< S+dM，DU²。
    bool capped=false; ///< filler 是否因上限变粗。
    std::vector<double> staticBins; ///< 每 bin 固定电荷，DU²。
    std::vector<Vec2> centers; ///< 初始 filler 中心，DU。
    std::vector<Vec2> sizes; ///< filler 宽高，DU。
    /** @brief 初始化模型存储。 @param database 借用拓扑。 @param cfg 有效配置。 @param token 借用取消令牌。 */
    Impl(const DesignDatabase& database,const Config& cfg,ControlToken* token):db(database),config(cfg),control(token),core(coreBounds(db)) {}
    /** @brief 计算单个对象分配并可选择收集能量梯度。
     * @param center 中心 DU。 @param size 宽高 DU。 @param charge 电荷 DU²。
     * @param bins 可变电荷累积。 @param potential 可空电势。 @return 对中心梯度 DU³。 */
    Vec2 deposit(Vec2 center,Vec2 size,double charge,std::vector<double>& bins,const std::vector<double>* potential=nullptr) const {
        const auto wx=axisWeights(center.x,size.x,core.lx,dx,nx); ///< 横向归一化积分。
        const auto wy=axisWeights(center.y,size.y,core.ly,dy,ny); ///< 纵向归一化积分。
        Vec2 gradient; ///< 当前对象密度梯度，DU³。
        for(const AxisWeight& y:wy) { // y 为纵向 bin 权重。
            for(const AxisWeight& x:wx) { // x 为横向 bin 权重。
                const std::size_t index=static_cast<std::size_t>(y.index)*nx+x.index; ///< 网格扁平索引。
                if(potential) { gradient.x+=charge*(*potential)[index]*x.derivative*y.weight; gradient.y+=charge*(*potential)[index]*x.weight*y.derivative; }
                else bins[index]+=charge*x.weight*y.weight;
            }
        }
        return gradient;
    }
};
/** @brief 构造静态区域、容量、网格及 filler；参数契约见 global.hpp。 */
DensityModel::DensityModel(const DesignDatabase& database,const PositionState& referencePositions,const Config& config,ControlToken* control):impl_(std::make_unique<Impl>(database,config,control)) {
    validateConfig(config); validatePositions(database,referencePositions);
    Impl& m=*impl_; ///< 当前任务独占的模型。
    if(m.core.area()<=0) Throw("INPUT_INVALID","密度域需要非退化可放置行","global");
    std::vector<Rect> rows; ///< 裁剪至 core 的行矩形。
    std::vector<Rect> blockers; ///< 裁剪至 core 的固定和硬阻挡。
    std::vector<Vec2> sample; ///< 真实移动对象的方向后宽高样本，DU。
    double standard=0; ///< 移动标准单元面积 S，DU²。
    double macro=0; ///< 移动宏面积 M，DU²。
    for(const RowSegment& row:database.rows) rows.push_back(intersect(rowRect(row),m.core)); // row 为输入可放置行。
    for(const Obstacle& obstacle:database.obstacles) blockers.push_back(intersect(obstacle.rect,m.core)); // obstacle 为硬阻挡。
    for(const Node& node:database.nodes) { // node 为当前实体。
        if(control && node.id%4096==0) control->checkCancelled();
        const Rect r=nodeRect(node,referencePositions.at(node.id)); ///< 方向后物理矩形。
        if(node.fixed) { if(r.area()>0) blockers.push_back(intersect(r,m.core)); continue; }
        if(r.area()<=0) continue;
        if(r.width()>m.core.width()||r.height()>m.core.height()) Throw("INFEASIBLE","移动对象尺寸超过 core",node.name);
        sample.push_back({r.width(),r.height()});
        if(node.kind==NodeKind::Macro) macro+=r.area(); else standard+=r.area();
    }
    const auto free=freeRegion(m.core,rows,blockers,control); ///< 无重叠自由区域分解。
    for(const Rect& r:free) m.area+=r.area(); // r 为一块有效可放置矩形。
    m.realTotal=standard+config.targetDensity*macro;
    const double tolerance=1e-10*std::max(1.0,m.core.area()); ///< 仅用于面积浮点预检容差，DU²。
    if(standard+macro>m.area+tolerance) Throw("INFEASIBLE","真实物理面积超过有效区域","global");
    if(m.realTotal>config.targetDensity*m.area+tolerance) {
        const std::string minimum=m.area>macro?std::to_string(standard/(m.area-macro)):"不存在有限可行目标"; ///< 最小目标密度诊断。
        Throw("TARGET_DENSITY_INFEASIBLE","目标密度不可行，最小密度估计="+minimum,"global",true);
    }
    double average=1; ///< 剪裁后面积样本均值，DU²；空样本不生成 filler。
    double ratio=1; ///< 剪裁后宽高比均值。
    if(!sample.empty()) {
        /** @brief 按面积排序。 @param a 左样本。 @param b 右样本。 @return 面积排序。 */
        std::sort(sample.begin(),sample.end(),[](Vec2 a,Vec2 b){return a.x*a.y<b.x*b.y;});
        const std::size_t trim=sample.size()<10?0:sample.size()/10; ///< 两端各丢弃的对象数。
        average=0;ratio=0;
        for(std::size_t i=trim;i<sample.size()-trim;++i) { average+=sample[i].x*sample[i].y;ratio+=sample[i].x/sample[i].y; } // i 为保留样本索引。
        average/=sample.size()-2*trim;ratio/=sample.size()-2*trim;
    }
    const double allAverage=sample.empty()?1:(standard+macro)/sample.size(); ///< 网格公式使用的真实对象平均面积，DU²。
    if(config.gridNx) { m.nx=config.gridNx;m.ny=config.gridNy; }
    else { const double count=std::ceil(m.area/std::max(4*allAverage/config.targetDensity,tolerance)); /* count 为目标总 bin 数。 */
        m.nx=powerGrid(std::sqrt(count*m.core.width()/m.core.height()));m.ny=powerGrid(count/m.nx); }
    m.dx=m.core.width()/m.nx;m.dy=m.core.height()/m.ny;
    m.staticBins.assign(static_cast<std::size_t>(m.nx)*m.ny,config.targetDensity*m.dx*m.dy);
    for(const Rect& r:free) { // r 为不相交自由矩形，逐 bin 扣除其容量电荷。
        const int x0=std::max(0,static_cast<int>((r.lx-m.core.lx)/m.dx)); ///< 相交首列。
        const int x1=std::min(m.nx-1,static_cast<int>((r.ux-m.core.lx)/m.dx)); ///< 相交末列。
        const int y0=std::max(0,static_cast<int>((r.ly-m.core.ly)/m.dy)); ///< 相交首行。
        const int y1=std::min(m.ny-1,static_cast<int>((r.uy-m.core.ly)/m.dy)); ///< 相交末行。
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) { // y/x 为相交 bin 行列。
            const Rect bin{m.core.lx+x*m.dx,m.core.ly+y*m.dy,m.core.lx+(x+1)*m.dx,m.core.ly+(y+1)*m.dy}; ///< 当前 bin 物理矩形。
            m.staticBins[static_cast<std::size_t>(y)*m.nx+x]-=config.targetDensity*intersect(r,bin).area();
        }
    }
    for(double& charge:m.staticBins) charge=std::max(0.0,charge); // charge 为仅受舍入影响可能微负的静态 bin 电荷。
    const double fill=std::max(0.0,config.targetDensity*m.area-m.realTotal); ///< 守恒所需临时电荷，DU²。
    if(sample.empty()||fill==0) return;
    const double maximum=std::min(m.core.width()*m.core.width()/ratio,m.core.height()*m.core.height()*ratio); ///< 保持样本宽高比能进入 R 的最大 filler 面积，DU²。
    const double desired=std::ceil(fill/average); ///< 按基准面积生成的期望数量。
    const double required=std::ceil(fill/maximum); ///< 尺寸限制所需最少数量。
    if(required>static_cast<double>(config.fillerLimit)) Throw("INPUT_INVALID","filler 数量上限不足以容纳守恒电荷","global",true);
    const std::size_t count=static_cast<std::size_t>(std::max(required,std::min(desired,static_cast<double>(config.fillerLimit)))); ///< 实际 filler 数。
    m.capped=desired>static_cast<double>(config.fillerLimit);
    const double piece=fill/count; ///< 均匀 filler 面积，末个用余量修正，DU²。
    std::vector<double> cumulativeArea; ///< 自由矩形面积累积，用于无拒绝抽样。
    double sum=0; ///< 累积自由面积，DU²。
    for(const Rect& r:free) {sum+=r.area();cumulativeArea.push_back(sum);} // r 为可抽样自由矩形。
    std::mt19937_64 random(config.seed); ///< 确定性 filler 随机序列。
    std::uniform_real_distribution<double> unit(0,1); ///< 单位区间均匀分布。
    double assigned=0; ///< 已分配 filler 电荷，DU²。
    for(std::size_t i=0;i<count;++i) { // i 为 filler 顺序编号。
        if(control && i%4096==0) control->checkCancelled();
        const double charge=i+1==count?fill-assigned:piece; ///< 最后一个精确剩余电荷，DU²。
        assigned+=charge;
        const Vec2 size{std::sqrt(charge*ratio),std::sqrt(charge/ratio)}; ///< 保持平均宽高比的尺寸，DU。
        const double draw=unit(random)*sum; ///< 自由区域面积抽样值，DU²。
        const std::size_t index=std::min(free.size()-1,static_cast<std::size_t>(std::upper_bound(cumulativeArea.begin(),cumulativeArea.end(),draw)-cumulativeArea.begin())); ///< 选中自由矩形索引。
        const Rect& region=free[index]; ///< 选中的自由矩形。
        Vec2 center{region.lx+unit(random)*region.width(),region.ly+unit(random)*region.height()}; ///< 位于 P\F 的初始中心，DU。
        // 中心抽样保持 P\F；优化首次投影负责保证完整矩形容纳于 R。
        m.centers.push_back(center);m.sizes.push_back(size);
    }
}
/** @brief 默认销毁 PIMPL；无参数、无返回值。 */
DensityModel::~DensityModel()=default;
/** @brief 实现完整密度与解析梯度计算；契约见 global.hpp。 */
DensityEvaluation DensityModel::evaluate(const PositionState& positions,const std::vector<Vec2>& fillerCenters) const {
    const Impl& m=*impl_; ///< 当前只读任务模型。
    validatePositions(m.db,positions);
    if(fillerCenters.size()!=m.sizes.size()) Throw("INPUT_INVALID","filler 位置数量错误","global");
    DensityEvaluation result; ///< 当前评估的所有电荷、能量、梯度。
    result.staticCharge=m.staticBins;
    result.realCharge.assign(m.staticBins.size(),0);result.fillerCharge=result.realCharge;
    result.gradient.resize(m.db.nodes.size());result.fillerGradient.resize(fillerCenters.size());
    for(const Node& node:m.db.nodes) { // node 为待沉积真实移动对象。
        if(m.control && node.id%4096==0) m.control->checkCancelled();
        if(node.fixed) continue;
        const Rect rect=nodeRect(node,positions.at(node.id)); ///< 当前物理包围盒，DU。
        if(rect.area()==0) continue;
        const double charge=rect.area()*(node.kind==NodeKind::Macro?m.config.targetDensity:1); ///< 混合尺寸真实电荷，DU²。
        m.deposit({(rect.lx+rect.ux)/2,(rect.ly+rect.uy)/2},{rect.width(),rect.height()},charge,result.realCharge);
    }
    for(std::size_t i=0;i<fillerCenters.size();++i) { // i 为 filler 编号。
        if(m.control && i%4096==0) m.control->checkCancelled();
        if(!std::isfinite(fillerCenters[i].x)||!std::isfinite(fillerCenters[i].y)) Throw("NUMERIC_FAILURE","filler 坐标非有限","global");
        m.deposit(fillerCenters[i],m.sizes[i],m.sizes[i].x*m.sizes[i].y,result.fillerCharge);
    }
    result.totalCharge.resize(m.staticBins.size());
    std::vector<double> density(m.staticBins.size()); ///< 无量纲总密度。
    for(std::size_t i=0;i<density.size();++i) { // i 为 bin 索引。
        result.totalCharge[i]=result.staticCharge[i]+result.realCharge[i]+result.fillerCharge[i];
        density[i]=result.totalCharge[i]/(m.dx*m.dy);
        result.overflow+=std::max(0.0,result.staticCharge[i]+result.realCharge[i]-m.config.targetDensity*m.dx*m.dy);
    }
    result.overflow=m.realTotal>0?result.overflow/m.realTotal:0;
    result.potential=SolvePoisson(density,m.nx,m.ny,m.dx,m.dy,m.control);
    const double mean=std::accumulate(density.begin(),density.end(),0.0)/density.size(); ///< 密度直流，无量纲。
    for(std::size_t i=0;i<density.size();++i) result.energy+=0.5*m.dx*m.dy*(density[i]-mean)*result.potential[i]; // i 为 bin 索引。
    for(const Node& node:m.db.nodes) { // node 为求导真实对象。
        if(m.control && node.id%4096==0) m.control->checkCancelled();
        if(node.fixed) continue;
        const Rect rect=nodeRect(node,positions.at(node.id)); ///< 当前方向后矩形，DU。
        if(rect.area()==0) continue;
        result.gradient[node.id]=m.deposit({(rect.lx+rect.ux)/2,(rect.ly+rect.uy)/2},{rect.width(),rect.height()},rect.area()*(node.kind==NodeKind::Macro?m.config.targetDensity:1),result.realCharge,&result.potential);
    }
    for(std::size_t i=0;i<fillerCenters.size();++i) { // i 为 filler 编号。
        if(m.control && i%4096==0) m.control->checkCancelled();
        result.fillerGradient[i]=m.deposit(fillerCenters[i],m.sizes[i],m.sizes[i].x*m.sizes[i].y,result.fillerCharge,&result.potential);
    }
    for(const Vec2& gradient:result.gradient) { // gradient 为真实节点的完整解析梯度。
        if(!std::isfinite(gradient.x)||!std::isfinite(gradient.y)) Throw("NUMERIC_FAILURE","真实密度梯度非有限","global");
    }
    for(const Vec2& gradient:result.fillerGradient) { // gradient 为 filler 完整解析梯度。
        if(!std::isfinite(gradient.x)||!std::isfinite(gradient.y)) Throw("NUMERIC_FAILURE","filler 密度梯度非有限","global");
    }
    if(!std::isfinite(result.energy)||!std::isfinite(result.overflow)) Throw("NUMERIC_FAILURE","密度目标非有限","global");
    return result;
}
/** @brief 读取初始 filler 中心；返回借用引用。 */
const std::vector<Vec2>& DensityModel::fillerPositions() const{return impl_->centers;}
/** @brief 读取 filler 宽高；返回借用引用。 */
const std::vector<Vec2>& DensityModel::fillerSizes() const{return impl_->sizes;}
/** @brief 读取横向 bin 数。 @return 正整数。 */
int DensityModel::gridNx() const{return impl_->nx;}
/** @brief 读取纵向 bin 数。 @return 正整数。 */
int DensityModel::gridNy() const{return impl_->ny;}
/** @brief 读取 bin 宽。 @return DU。 */
double DensityModel::binWidth() const{return impl_->dx;}
/** @brief 读取 bin 高。 @return DU。 */
double DensityModel::binHeight() const{return impl_->dy;}
/** @brief 读取 filler 上限标记。 @return 是否放大 filler。 */
bool DensityModel::fillerCapped() const{return impl_->capped;}
/** @brief 读取有效面积。 @return DU²。 */
double DensityModel::availableArea() const{return impl_->area;}
} // namespace placement
