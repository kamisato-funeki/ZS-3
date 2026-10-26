#include "placement/application/global.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace placement {
namespace {
/** @brief 生成独立小型平面：32×32 行，四个可移动方块及一个固定端口。
 * @return 完整、可数值验证的数据库，单位 DU；无外部副作用。 */
DesignDatabase fixture() {
    DesignDatabase db; ///< 小型测试拓扑。
    db.datasetId="global-test";
    for(int i=0;i<4;++i) { // i 为可移动对象 ID。
        Node node; ///< 2×2 移动标准单元。
        node.id=i;node.name="n"+std::to_string(i);node.width=2;node.height=2;
        db.nodes.push_back(node);db.originalPositions.push_back({3.17+i*0.3,4.23+i*0.4,Orientation::N});
    }
    Node terminal; ///< 固定零面积引脚载体。
    terminal.id=4;terminal.name="port";terminal.kind=NodeKind::Terminal;terminal.fixed=true;
    db.nodes.push_back(terminal);db.originalPositions.push_back({29,28,Orientation::N});
    for(int i=0;i<32;++i) { // i 为行索引。
        RowSegment row; ///< 横跨 core 的一 DU 高行。
        row.id=i;row.y=i;row.height=1;row.xOrigin=0;row.siteWidth=1;row.sitePitch=1;row.numSites=32;db.rows.push_back(row);
    }
    Net net; ///< 包含全部节点的测试网络。
    net.id=0;net.name="net";net.weight=1.7;
    for(std::size_t i=0;i<db.nodes.size();++i) { // i 为引脚及节点 ID。
        Pin pin; ///< 以中心为参考的测试引脚。
        pin.id=i;pin.nodeId=i;pin.netId=0;pin.offset={0.13,-0.21};db.pins.push_back(pin);net.pinIds.push_back(i);
    }
    db.nets.push_back(net);return db;
}
/** @brief 默认快速数值配置，显式矩形网格及小 filler 上限。
 * @return 32×64 网格配置。 */
Config configuration() { Config config; /* config 为测试有效配置。 */ config.gridNx=32;config.gridNy=64;config.fillerLimit=64;return config; }
/** @brief 求数组总和。 @param values bin 电荷。 @return 总电荷 DU²。 */
double sum(const std::vector<double>& values) {return std::accumulate(values.begin(),values.end(),0.0);}
/** @brief TC-GP：WA 用独立中心差分核对全部移动坐标及固定梯度。 */
TEST(GlobalWirelength,IndependentFiniteDifference) {
    const auto db=fixture(); ///< 完整测试拓扑。
    const double step=1e-5; ///< 差分步长 DU。
    for(double gamma:{0.3,2.0,20.0}) { // gamma 为不同平滑长度，DU。
        const auto analytic=EvaluateWirelength(db,db.originalPositions,gamma); ///< 解析目标与梯度。
        for(NodeId id=0;id<4;++id) for(int axis=0;axis<2;++axis) { // id/axis 为待验证分量。
            for(double h:{step,step/2}) { // h 为复核差分步长，DU。
                auto plus=db.originalPositions; ///< 正向移动后的完整布局。
                auto minus=plus; ///< 负向移动后的完整布局。
                if(axis==0){plus[id].x+=h;minus[id].x-=h;}else{plus[id].y+=h;minus[id].y-=h;}
                const double numerical=(EvaluateWirelength(db,plus,gamma).value-EvaluateWirelength(db,minus,gamma).value)/(2*h); ///< 独立中心差分。
                EXPECT_NEAR(numerical,axis==0?analytic.gradient[id].x:analytic.gradient[id].y,1e-5);
            }
        }
        EXPECT_DOUBLE_EQ(analytic.gradient[4].x,0);EXPECT_DOUBLE_EQ(analytic.gradient[4].y,0);
    }
}
/** @brief TC-GP：矩形 32×64 DCT 随机场往返与常量场去直流。 */
TEST(GlobalSpectral,RectangularRoundtripAndConstant) {
    const int nx=32; ///< 横向 bin 数。
    const int ny=64; ///< 纵向 bin 数。
    std::vector<double> field(nx*ny); ///< 无量纲确定性非周期场。
    for(std::size_t i=0;i<field.size();++i) field[i]=std::sin(i*1.234)+std::cos(i*0.127); // i 为 bin 索引。
    const auto coefficients=OrthonormalDct(field,nx,ny); ///< 正交系数，用于独立 Parseval 判据。
    EXPECT_NEAR(std::inner_product(coefficients.begin(),coefficients.end(),coefficients.begin(),0.0),std::inner_product(field.begin(),field.end(),field.begin(),0.0),1e-9);
    const auto dc=OrthonormalDct(std::vector<double>(nx*ny,0.8),nx,ny); ///< 常量场正交频谱。
    EXPECT_NEAR(dc.front(),0.8*std::sqrt(nx*ny),1e-10);
    const auto restored=OrthonormalDct(coefficients,nx,ny,true); ///< 正交往返值。
    double error=0; ///< L2 误差平方和。
    double scale=0; ///< 原场 L2 平方和。
    for(std::size_t i=0;i<field.size();++i){error+=std::pow(restored[i]-field[i],2);scale+=field[i]*field[i];} // i 为 bin 索引。
    EXPECT_LE(std::sqrt(error/scale),1e-10);
    const auto constant=SolvePoisson(std::vector<double>(nx*ny,0.8),nx,ny,0.7,1.3); ///< 常量去均值逆解。
    for(double value:constant) EXPECT_NEAR(value,0,1e-10); // value 为单 bin 电势。
}
/** @brief TC-GP：用独立有限差分 Neumann 算子检验单余弦模式，避免只复验频域除法。 */
TEST(GlobalSpectral,DiscreteNeumannSingleMode) {
    const int nx=32; ///< 横向 bin 数。
    const int ny=64; ///< 纵向 bin 数。
    const double dx=0.7; ///< 横向物理步长 DU。
    const double dy=1.3; ///< 纵向物理步长 DU。
    const double pi=std::acos(-1.0); ///< 圆周率。
    std::vector<double> field(nx*ny); ///< 单余弦密度模式。
    for(int y=0;y<ny;++y) for(int x=0;x<nx;++x) field[y*nx+x]=std::cos(pi*3*(x+0.5)/nx)*std::cos(pi*5*(y+0.5)/ny); // y/x 为空间 bin 索引。
    const auto potential=SolvePoisson(field,nx,ny,dx,dy); ///< 离散泊松电势。
    for(int y=0;y<ny;++y) for(int x=0;x<nx;++x) { // y/x 为独立离散算子空间坐标。
        const double center=potential[y*nx+x]; ///< 当前电势 DU²。
        const double left=potential[y*nx+std::max(0,x-1)]; ///< 左邻，边缘 Neumann 镜像。
        const double right=potential[y*nx+std::min(nx-1,x+1)]; ///< 右邻，边缘 Neumann 镜像。
        const double below=potential[std::max(0,y-1)*nx+x]; ///< 下邻，边缘 Neumann 镜像。
        const double above=potential[std::min(ny-1,y+1)*nx+x]; ///< 上邻，边缘 Neumann 镜像。
        EXPECT_NEAR((2*center-left-right)/(dx*dx)+(2*center-below-above)/(dy*dy),field[y*nx+x],1e-10);
    }
}
/** @brief TC-GP：密度真实/filler 解析梯度用独立差分复核，覆盖 core 边界截断分母。 */
TEST(GlobalDensity,ExactConservativeKernelFiniteDifference) {
    const auto db=fixture(); ///< 拓扑及位置。
    const auto config=configuration(); ///< 数值配置。
    DensityModel model(db,db.originalPositions,config); ///< 精确核密度模型。
    auto positions=db.originalPositions; ///< 可变差分基点。
    positions[0].x=0.03;positions[0].y=0.02;
    const auto fillers=model.fillerPositions(); ///< 保持固定的 filler 基点。
    const auto analytic=model.evaluate(positions,fillers); ///< 解析密度及导数。
    EXPECT_NEAR(sum(analytic.totalCharge),config.targetDensity*1024,1e-8);
    for(double h:{1e-5,5e-6}) { // h 为独立中心差分步长 DU。
        for(int axis=0;axis<2;++axis) { // axis 为 x/y 方向。
            auto plus=positions; ///< 正向真实对象布局。
            auto minus=positions; ///< 负向真实对象布局。
            if(axis==0){plus[0].x+=h;minus[0].x-=h;}else{plus[0].y+=h;minus[0].y-=h;}
            const double derivative=(model.evaluate(plus,fillers).energy-model.evaluate(minus,fillers).energy)/(2*h); ///< 独立密度中心差分。
            EXPECT_NEAR(derivative,axis==0?analytic.gradient[0].x:analytic.gradient[0].y,1e-5*std::max(1.0,std::abs(derivative)));
            auto fplus=fillers; ///< 正向 filler 中心。
            auto fminus=fillers; ///< 负向 filler 中心。
            if(axis==0){fplus[0].x+=h;fminus[0].x-=h;}else{fplus[0].y+=h;fminus[0].y-=h;}
            const double fd=(model.evaluate(positions,fplus).energy-model.evaluate(positions,fminus).energy)/(2*h); ///< filler 独立差分。
            EXPECT_NEAR(fd,axis==0?analytic.fillerGradient[0].x:analytic.fillerGradient[0].y,1e-5*std::max(1.0,std::abs(fd)));
        }
    }
}
/** @brief TC-GP：重合固定体/硬阻挡与暗区不得重复计数，filler 不影响 overflow。 */
TEST(GlobalDensity,StaticUnionFillerCapAndOverflowExclusion) {
    auto db=fixture(); ///< 带静态重叠的拓扑。
    db.rows.erase(db.rows.begin()+16,db.rows.end());
    for(std::size_t i=0;i<db.rows.size();++i) db.rows[i].id=i; // i 为缩减后的行 ID。
    db.obstacles.push_back({{10,0,14,16},"a"});db.obstacles.push_back({{12,0,16,16},"b"});
    db.nodes[4].width=4;db.nodes[4].height=16;db.originalPositions[4]={12,0,Orientation::N};
    auto config=configuration(); ///< 将 filler 上限设为 2 以触发放大。
    config.fillerLimit=2;
    DensityModel model(db,db.originalPositions,config); ///< 联合静态区域模型。
    EXPECT_DOUBLE_EQ(model.availableArea(),416);EXPECT_TRUE(model.fillerCapped());EXPECT_LE(model.fillerPositions().size(),2u);
    const auto first=model.evaluate(db.originalPositions,model.fillerPositions()); ///< 初始 filler 分布的密度。
    auto shifted=model.fillerPositions(); ///< 全部移动至同点的 filler。
    for(Vec2& point:shifted) point={16,8}; // point 为可变 filler 中心。
    const auto second=model.evaluate(db.originalPositions,shifted); ///< 改变 filler 后的密度。
    EXPECT_NEAR(sum(first.staticCharge),0.8*96,1e-10);
    EXPECT_NEAR(sum(first.totalCharge),0.8*512,1e-8);EXPECT_DOUBLE_EQ(first.overflow,second.overflow);
}
/** @brief TC-GP：容量预检、配置改变 filler 数与网格确实生效。 */
TEST(GlobalDensity,ConfigurationAndInfeasibleCapacity) {
    auto db=fixture(); ///< 测试数据库。
    auto config=configuration(); ///< 有效显式网格。
    config.fillerLimit=8;
    DensityModel model(db,db.originalPositions,config); ///< 上限为八的模型。
    EXPECT_EQ(model.gridNx(),32);EXPECT_EQ(model.gridNy(),64);EXPECT_EQ(model.fillerPositions().size(),8u);
    config.targetDensity=0.01;
    try {DensityModel invalid(db,db.originalPositions,config);FAIL()<<"应拒绝容量";}catch(const Error& error){EXPECT_EQ(error.code,"CAPACITY_INFEASIBLE");} // error 为预期业务错误。
}
/** @brief TC-GP：零 filler 电荷分支与极端宽高比的上限错误均显式处理。 */
TEST(GlobalDensity,ZeroFillAndUnrepresentableFiller) {
    auto db=fixture(); ///< 方形对象拓扑。
    auto config=configuration(); ///< 可调整目标密度配置。
    config.targetDensity=16.0/1024;
    DensityModel zero(db,db.originalPositions,config); ///< 恰好无需 filler 的模型。
    EXPECT_TRUE(zero.fillerPositions().empty());
    EXPECT_NEAR(sum(zero.evaluate(db.originalPositions,{}).totalCharge),16,1e-10);
    for(Node& node:db.nodes) { // node 为改变宽高比而保持面积的移动节点。
        if(!node.fixed){node.width=16;node.height=0.25;}
    }
    config.targetDensity=0.8;config.fillerLimit=1;
    EXPECT_THROW(DensityModel invalid(db,db.originalPositions,config),Error);
}
/** @brief TC-GP：非正方形显式网格和混合尺寸宏电荷读取配置。 */
TEST(GlobalDensity,MacroChargeAndGridConfiguration) {
    auto db=fixture(); ///< 含一个移动宏的拓扑。
    db.nodes[0].kind=NodeKind::Macro;
    auto config=configuration(); ///< 横纵交换的显式网格配置。
    config.gridNx=64;config.gridNy=32;config.targetDensity=0.5;
    DensityModel model(db,db.originalPositions,config); ///< 混合尺寸密度模型。
    const auto result=model.evaluate(db.originalPositions,model.fillerPositions()); ///< 当前电荷结果。
    EXPECT_EQ(model.gridNx(),64);EXPECT_EQ(model.gridNy(),32);
    EXPECT_NEAR(sum(result.realCharge),14,1e-10);
    EXPECT_NEAR(sum(result.totalCharge),512,1e-8);
}
/** @brief TC-GP：最大接受迭代、固定节点/身份保持及 sink 指标一致。 */
TEST(GlobalOptimizer,MaximumIterationsAndIdentity) {
    auto db=std::make_shared<DesignDatabase>(fixture()); ///< 跨快照共享的只读拓扑源。
    Snapshot initial; ///< 已准备的初始快照。
    initial.database=db;initial.datasetId=db->datasetId;initial.runId="run";initial.positions=db->originalPositions;
    auto config=configuration(); ///< 强制一轮且不允许初始 overflow 收敛。
    config.maxIterations=1;config.overflowStop=0;config.fillerLimit=16;
    ControlToken token; ///< 未取消的运行控制。
    int seen=0; ///< 已发布完整快照数。
    /** @brief 核验发布点固定性。 @param snapshot 完整安全快照。 无返回值。 */
    const auto sink=[&](const Snapshot& snapshot){++seen;EXPECT_EQ(snapshot.stage,"global");EXPECT_DOUBLE_EQ(snapshot.positions[4].x,29);EXPECT_TRUE(snapshot.metrics.hpwl.has_value());};
    const auto result=RunGlobal(initial,config,token,sink); ///< 单轮终态。
    EXPECT_EQ(result.termination,"max_iterations");EXPECT_EQ(result.iteration,1u);EXPECT_EQ(result.runId,"run");EXPECT_EQ(result.datasetId,db->datasetId);EXPECT_EQ(result.positions.size(),5u);EXPECT_GE(seen,2);
    EXPECT_EQ(result.gridNx,32);EXPECT_EQ(result.gridNy,64);EXPECT_TRUE(result.fillerCapped);
    ASSERT_TRUE(result.metrics.lambda.has_value());ASSERT_TRUE(result.metrics.gamma.has_value());ASSERT_TRUE(result.metrics.step.has_value());
    EXPECT_GT(*result.metrics.lambda,0);EXPECT_GT(*result.metrics.gamma,0);EXPECT_GT(*result.metrics.step,0);
}
/** @brief TC-GP-13：真实轨迹首次跨阈值后必须连续五轮，不能一次达标提前成功；无输入，无返回值。 */
TEST(GlobalOptimizer,ConvergenceRequiresFiveAcceptedRounds) {
    auto db=std::make_shared<DesignDatabase>(fixture()); // 本测试只读拓扑。
    Snapshot initial; // 不满足初始阈值例外的聚集布局。
    initial.database=db;initial.datasetId=db->datasetId;initial.runId="five-rounds";initial.stage="initial";initial.positions=db->originalPositions;
    Config config=configuration(); // 先以固定预算测量可到达overflow范围，不复用收敛计数实现。
    config.maxIterations=25;config.overflowStop=0;config.fillerLimit=16;
    ControlToken probeToken; // 只用于建立可达阈值的独立试运行。
    double firstOverflow=-1; // 初始真实overflow，首次回调设置。
    double minimumOverflow=1; // 固定25轮中观测到的最小值。
    /** @brief 收集真实溢出范围，不参与优化器决策。 @param snapshot 完整接受轮。 无返回值。 */
    RunGlobal(initial,config,probeToken,[&](const Snapshot& snapshot){if(firstOverflow<0)firstOverflow=*snapshot.metrics.overflow;minimumOverflow=std::min(minimumOverflow,*snapshot.metrics.overflow);});
    ASSERT_LT(minimumOverflow,firstOverflow);
    config.overflowStop=(firstOverflow+minimumOverflow)/2;config.maxIterations=100;
    ControlToken token; // 真正收敛测试的独立控制。
    int consecutive=0; // 外部逐事件判定器的连续达标计数。
    std::uint64_t firstFive=0; // 第一次达到五轮的接受迭代编号，0表示尚未达到。
    /** @brief 独立观察已发布指标并检查末轮标记。 @param snapshot 不可变完整轮。 无返回值。 */
    const Snapshot result=RunGlobal(initial,config,token,[&](const Snapshot& snapshot){if(snapshot.iteration==0){EXPECT_GT(*snapshot.metrics.overflow,config.overflowStop);return;}consecutive=*snapshot.metrics.overflow<=config.overflowStop?consecutive+1:0;if(consecutive==5&&firstFive==0)firstFive=snapshot.iteration;if(consecutive<5)EXPECT_NE(snapshot.termination,"converged");});
    EXPECT_EQ(result.termination,"converged");EXPECT_GE(firstFive,5U);EXPECT_EQ(result.iteration,firstFive);
}
/** @brief TC-GP：多轮变参数调度及投影均保持完整有限发布点，sink 中取消会停止。 */
TEST(GlobalOptimizer,MultipleIterationsAndCancellationAtCommit) {
    auto db=std::make_shared<DesignDatabase>(fixture()); ///< 共享数值拓扑。
    Snapshot initial; ///< 初始完整状态。
    initial.database=db;initial.datasetId=db->datasetId;initial.runId="multi";initial.positions=db->originalPositions;
    auto config=configuration(); ///< 强制执行二十五次接受步骤的配置。
    config.maxIterations=25;config.overflowStop=0;config.fillerLimit=16;
    ControlToken token; ///< 正常控制令牌。
    const auto result=RunGlobal(initial,config,token); ///< 多轮终态。
    EXPECT_EQ(result.termination,"max_iterations");EXPECT_EQ(result.iteration,25u);
    for(const Node& node:db->nodes) { // node 为已接受终态的真实节点。
        const Rect rect=nodeRect(node,result.positions[node.id]); ///< 当前物理矩形。
        EXPECT_TRUE(std::isfinite(rect.lx));EXPECT_TRUE(std::isfinite(rect.ly));
        if(!node.fixed){EXPECT_GE(rect.lx,0);EXPECT_GE(rect.ly,0);EXPECT_LE(rect.ux,32);EXPECT_LE(rect.uy,32);}
    }
    ControlToken cancelled; ///< 在完整提交回调中取消的令牌。
    int commits=0; ///< 取消前发布计数。
    /** @brief 在第一次完整迭代提交时取消；无返回值。 @param snapshot 完整快照。 */
    const auto sink=[&](const Snapshot& snapshot){++commits;if(snapshot.iteration==1) cancelled.cancel();};
    try {RunGlobal(initial,config,cancelled,sink);FAIL()<<"应响应取消";}catch(const Error& error){EXPECT_EQ(error.code,"CANCELLED");} // error 为取消业务异常。
    EXPECT_EQ(commits,2);
}
/** @brief TC-GP：空设计、固定设计、取消和初始阈值达标的退化处理。 */
TEST(GlobalOptimizer,DegenerateAndCancelledCases) {
    auto db=std::make_shared<DesignDatabase>(); ///< 空设计拓扑。
    db->datasetId="empty";
    Snapshot initial; ///< 空设计合法初始快照。
    initial.database=db;initial.datasetId=db->datasetId;
    Config config; ///< 默认有效配置。
    ControlToken token; ///< 正常运行令牌。
    EXPECT_EQ(RunGlobal(initial,config,token).termination,"converged");
    ControlToken cancelled; ///< 已取消令牌。
    cancelled.cancel();EXPECT_THROW(RunGlobal(initial,config,cancelled),Error);
    auto fixed=std::make_shared<DesignDatabase>(fixture()); ///< 全部固定测试拓扑。
    for(Node& node:fixed->nodes) node.fixed=true; // node 为要保持不动的实体。
    initial.database=fixed;initial.datasetId=fixed->datasetId;initial.positions=fixed->originalPositions;
    EXPECT_EQ(RunGlobal(initial,config,token).iteration,0u);
    auto spread=std::make_shared<DesignDatabase>(fixture()); ///< 使用较松阈值的初始收敛拓扑。
    initial.database=spread;initial.datasetId=spread->datasetId;initial.positions=spread->originalPositions;
    config.overflowStop=0.99;config.fillerLimit=16;
    EXPECT_EQ(RunGlobal(initial,config,token).termination,"converged");
}
/** @brief 构造无节点、无网络、以整行完全铺满矩形core的独立测试拓扑。
 * @param width core宽度，正整数DU。 @param height core高度，正整数DU。
 * @param rowHeight 行高，正整数DU，测试调用保证整除height。
 * @return 新数据库值，ID等于向量索引，不读取外部数据。 */
DesignDatabase densityDatabase(int width,int height,int rowHeight) {
    DesignDatabase database; // 当前测试独占的矩形区域拓扑。
    database.datasetId="direct-density-"+std::to_string(width)+"-"+std::to_string(height);
    for(int y=0;y<height;y+=rowHeight) { // 当前水平行底边，DU。
        RowSegment row; // 当前原始行，站点间距和宽度均为1 DU。
        row.id=database.rows.size();row.y=y;row.height=rowHeight;
        row.siteWidth=1;row.sitePitch=1;row.numSites=static_cast<std::uint64_t>(width);
        database.rows.push_back(row);
    }
    return database;
}
/** @brief 为独立密度夹具追加实体及输入基线。
 * @param database 独占目标拓扑。 @param name 唯一对象名称。 @param kind 物理类别。
 * @param width 未旋转宽度，DU。 @param height 未旋转高度，DU。
 * @param x 输入左下角横坐标，DU。 @param y 输入左下角纵坐标，DU。
 * @param fixed 输入固定标记。 无返回值，只追加节点和对应基线。 */
void densityNode(DesignDatabase& database,const std::string& name,NodeKind kind,double width,double height,
                 double x,double y,bool fixed=false) {
    Node node; // 当前追加对象，稳定ID由原节点数量确定。
    node.id=database.nodes.size();node.name=name;node.kind=kind;
    node.width=width;node.height=height;node.fixed=fixed;
    database.nodes.push_back(node);database.originalPositions.push_back({x,y,Orientation::N});
}
/** @brief TC-GP-01：F-A两引脚在多个递减gamma下趋近手算HPWL=10 DU。
 * 无输入、无返回值；两轴间距均为5 DU，独立闭式WA=10*tanh(2.5/gamma)。 */
TEST(GlobalWirelength,DecreasingGammaApproachesHandComputedHpwl) {
    DesignDatabase database=densityDatabase(10,10,2); // F-A几何域，指标核对不要求初始位置合法。
    densityNode(database,"A",NodeKind::Standard,2,2,1,1);
    densityNode(database,"B",NodeKind::Standard,2,2,7,5);
    database.pins={{0,0,0,{0.5,-0.5}},{1,1,0,{-0.5,0.5}}};
    database.nets={{0,"F-A",{0,1},1.0}};
    double previousError=std::numeric_limits<double>::infinity(); // 上一个较大gamma的HPWL近似绝对误差，DU。
    for(const double gamma:{5.0,2.0,1.0,0.5,0.25,0.1}) { // 当前平滑尺度，DU，严格递减。
        const WirelengthEvaluation evaluation=EvaluateWirelength(database,database.originalPositions,gamma); // 待验证的真实解析WA结果。
        const double closedForm=10.0*std::tanh(2.5/gamma); // 独立二引脚logistic闭式真值，DU。
        const double error=std::abs(10.0-evaluation.value); // 相对纸面HPWL的绝对误差，DU。
        EXPECT_NEAR(evaluation.value,closedForm,1e-12);
        EXPECT_GE(evaluation.value,0.0);EXPECT_LE(evaluation.value,10.0+1e-12);
        EXPECT_LE(error,previousError+1e-12);
        previousError=error;
    }
    EXPECT_LE(previousError,1e-12);
}
/** @brief TC-GP-03/F-E：core100 DU²、标准面积20、宏面积20、d=.8给Q36及filler44。
 * 无输入、无返回值；分项及总电荷对独立面积公式核对，不用模型自身的平均值当真值。 */
TEST(GlobalDensity,SpecifiedMixedChargeAndFillerConservation) {
    DesignDatabase database=densityDatabase(10,10,2); // 五行形成完整10×10 DU域。
    densityNode(database,"standard",NodeKind::Standard,10,2,0,0);
    densityNode(database,"macro",NodeKind::Macro,5,4,0,2);
    Config config; // 保留设计默认密度.8与filler上限，仅显式小网格加速直接判据。
    config.gridNx=32;config.gridNy=32;
    DensityModel model(database,database.originalPositions,config); // F-E混合尺寸电荷模型。
    const DensityEvaluation evaluation=model.evaluate(database.originalPositions,model.fillerPositions()); // 固定同组filler位置的完整电荷。
    EXPECT_DOUBLE_EQ(model.availableArea(),100.0);
    EXPECT_FALSE(model.fillerCapped());
    ASSERT_EQ(model.fillerSizes().size(),3U);
    EXPECT_NEAR(sum(evaluation.staticCharge),0.0,1e-12);
    EXPECT_NEAR(sum(evaluation.realCharge),36.0,1e-10); // Q=20+.8×20。
    EXPECT_NEAR(sum(evaluation.fillerCharge),44.0,1e-10); // .8×100-Q。
    EXPECT_NEAR(sum(evaluation.totalCharge),80.0,1e-10);
    double fillerArea=0; // 独立从公开宽高累加的filler物理面积，DU²。
    for(const Vec2& size:model.fillerSizes()) { // 当前临时filler宽高，DU。
        EXPECT_GT(size.x,0.0);EXPECT_GT(size.y,0.0);
        EXPECT_LE(size.x,10.0);EXPECT_LE(size.y,10.0);
        fillerArea+=size.x*size.y;
    }
    EXPECT_NEAR(fillerArea,44.0,1e-10);
    EXPECT_EQ(database.nodes.size(),2U);
}
/** @brief TC-GP-08：整个core被同一固定矩形均匀覆盖且没有网络时，去DC后电势/能量/梯度为零。
 * 无输入、无返回值；每bin静态密度独立预期为d=.8，不借随机filler近似均匀场。 */
TEST(GlobalDensity,UniformStaticNoNetFieldHasZeroPotentialAndGradient) {
    DesignDatabase database=densityDatabase(32,32,1); // 无暗区的32×32 DU core。
    densityNode(database,"fixed-cover",NodeKind::Macro,32,32,0,0,true);
    Config config; // 固定32×32网格使每bin恰为1 DU²。
    config.gridNx=32;config.gridNy=32;
    DensityModel model(database,database.originalPositions,config); // 无移动电荷，禁止用随机filler制造均匀假设。
    EXPECT_TRUE(database.nets.empty());EXPECT_TRUE(database.pins.empty());
    EXPECT_TRUE(model.fillerPositions().empty());EXPECT_DOUBLE_EQ(model.availableArea(),0.0);
    const DensityEvaluation evaluation=model.evaluate(database.originalPositions,{}); // 仅含均匀静态电荷的完整场。
    ASSERT_EQ(evaluation.totalCharge.size(),1024U);
    for(const double charge:evaluation.staticCharge) EXPECT_NEAR(charge,0.8,1e-12); // 当前bin静态电荷，DU²。
    for(const double charge:evaluation.realCharge) EXPECT_DOUBLE_EQ(charge,0.0); // 当前bin真实移动电荷，DU²。
    for(const double potential:evaluation.potential) EXPECT_NEAR(potential,0.0,1e-10); // 当前去直流电势，DU²。
    EXPECT_NEAR(evaluation.energy,0.0,1e-10);EXPECT_DOUBLE_EQ(evaluation.overflow,0.0);
    ASSERT_EQ(evaluation.gradient.size(),1U);
    EXPECT_DOUBLE_EQ(evaluation.gradient[0].x,0.0);EXPECT_DOUBLE_EQ(evaluation.gradient[0].y,0.0);
    EXPECT_TRUE(evaluation.fillerGradient.empty());
    EXPECT_NEAR(sum(evaluation.totalCharge),819.2,1e-10);
}
/** @brief TC-GP-09：对象真实贴core四个边界时只向可行域内做二阶单侧差分。
 * 无输入、无返回值；h及h/2均满足相对误差<=1e-5，不使用越界负向样本。 */
TEST(GlobalDensity,CoreBoundaryUsesFeasibleOneSidedDifferences) {
    const DesignDatabase database=fixture(); // 2×2对象与32×32 core的独立数值拓扑。
    const Config config=configuration(); // 既有32×64矩形网格，保留核截断及分母导数。
    DensityModel model(database,database.originalPositions,config); // 全部差分共用相同filler和静态场。
    const std::vector<Vec2> fillers=model.fillerPositions(); // 固定的临时filler中心，DU。
    for(const double edge:{0.0,30.0}) { // 2 DU宽高对象的左下/右上可行边界，DU。
        PositionState positions=database.originalPositions; // 当前贴边、仍完全可行的基点。
        positions[0].x=edge;positions[0].y=edge;
        const DensityEvaluation analytic=model.evaluate(positions,fillers); // 边界基点的能量与解析中心梯度。
        const double inward=edge==0.0?1.0:-1.0; // 内向采样方向，保证没有任何越界差分点。
        for(int axis=0;axis<2;++axis) { // 当前核对的横向/纵向分量。
            for(const double h:{1e-4,5e-5}) { // 当前可行单侧差分步长，DU，第二值为减半复核。
                PositionState first=positions; // 沿可行方向移动h的完整真实位置。
                PositionState second=positions; // 沿可行方向移动2h的完整真实位置。
                if(axis==0) { first[0].x+=inward*h;second[0].x+=inward*2*h; }
                else { first[0].y+=inward*h;second[0].y+=inward*2*h; }
                const Rect firstRect=nodeRect(database.nodes[0],first[0]); // 首差分样本物理框，DU。
                const Rect secondRect=nodeRect(database.nodes[0],second[0]); // 次差分样本物理框，DU。
                EXPECT_GE(firstRect.lx,0.0);EXPECT_GE(firstRect.ly,0.0);EXPECT_LE(firstRect.ux,32.0);EXPECT_LE(firstRect.uy,32.0);
                EXPECT_GE(secondRect.lx,0.0);EXPECT_GE(secondRect.ly,0.0);EXPECT_LE(secondRect.ux,32.0);EXPECT_LE(secondRect.uy,32.0);
                const double firstEnergy=model.evaluate(first,fillers).energy; // E(z+s*h)，DU⁴。
                const double secondEnergy=model.evaluate(second,fillers).energy; // E(z+2s*h)，DU⁴。
                const double numerical=inward*(-3*analytic.energy+4*firstEnergy-secondEnergy)/(2*h); // 对原x/y坐标的二阶可行单侧导数，DU³。
                const double derivative=axis==0?analytic.gradient[0].x:analytic.gradient[0].y; // 待独立核对的解析分量，DU³。
                const double relativeError=std::abs(numerical-derivative)/std::max({1.0,std::abs(numerical),std::abs(derivative)}); // 设计05规定的相对差分误差。
                EXPECT_LE(relativeError,1e-5)<<"edge="<<edge<<", axis="<<axis<<", h="<<h;
            }
        }
    }
}
/** @brief TC-GP-17：具有非零权重和非零偏移的单引脚网络对所有节点的WA目标/梯度都为零。
 * 无输入、无返回值；覆盖不同gamma，避免把零权网络误当作单引脚判据。 */
TEST(GlobalWirelength,SinglePinNetworkHasZeroObjectiveAndGradient) {
    DesignDatabase database=fixture(); // 保留移动实体和固定端口，仅替换网络连接。
    database.pins={{0,0,0,{0.13,-0.21}}};
    database.nets={{0,"single-pin",{0},1.7}};
    for(const double gamma:{0.05,1.0,8.0}) { // 当前正平滑尺度，DU。
        const WirelengthEvaluation evaluation=EvaluateWirelength(database,database.originalPositions,gamma); // 单引脚目标及按NodeId累积梯度。
        EXPECT_DOUBLE_EQ(evaluation.value,0.0);
        ASSERT_EQ(evaluation.gradient.size(),database.nodes.size());
        for(const Vec2& gradient:evaluation.gradient) { // 包括无连接移动对象和固定端口在内的完整梯度。
            EXPECT_DOUBLE_EQ(gradient.x,0.0);EXPECT_DOUBLE_EQ(gradient.y,0.0);
        }
    }
}
/** @brief TC-GP-18：默认上限20000必须真正触发，并保持每个filler尺寸可容纳及总面积/电荷守恒。
 * 无输入、无返回值；core面积1e6、真实电荷1、d=.8，独立所需filler面积799999 DU²。 */
TEST(GlobalDensity,DefaultTwentyThousandFillerCapIsActuallyTriggered) {
    DesignDatabase database=densityDatabase(1000,1000,1); // 稀疏大core，用默认网格公式和默认filler上限。
    densityNode(database,"one-cell",NodeKind::Standard,1,1,1.125,2.25);
    const Config config; // 完全未改写的默认配置，fillerLimit应为20000。
    ASSERT_EQ(config.fillerLimit,20000U);
    DensityModel model(database,database.originalPositions,config); // 未截断数量ceil799999，实际必须触顶20000。
    EXPECT_TRUE(model.fillerCapped());
    ASSERT_EQ(model.fillerPositions().size(),20000U);ASSERT_EQ(model.fillerSizes().size(),20000U);
    double fillerArea=0; // 独立累加公开临时矩形尺寸得到的总面积，DU²。
    for(std::size_t index=0;index<model.fillerSizes().size();++index) { // 当前真实触顶filler编号。
        const Vec2& size=model.fillerSizes()[index]; // 当前filler正宽高，DU。
        const Vec2& center=model.fillerPositions()[index]; // 当前按允许区域抽样的中心，DU。
        EXPECT_GT(size.x,0.0);EXPECT_GT(size.y,0.0);EXPECT_LE(size.x,1000.0);EXPECT_LE(size.y,1000.0);
        EXPECT_NEAR(size.x,size.y,1e-12); // 单位方块样本推导的宽高比必须为1。
        EXPECT_TRUE(std::isfinite(center.x));EXPECT_TRUE(std::isfinite(center.y));
        EXPECT_GE(center.x,0.0);EXPECT_LE(center.x,1000.0);EXPECT_GE(center.y,0.0);EXPECT_LE(center.y,1000.0);
        fillerArea+=size.x*size.y;
    }
    EXPECT_NEAR(fillerArea,799999.0,799999.0e-10);
    const DensityEvaluation evaluation=model.evaluate(database.originalPositions,model.fillerPositions()); // 默认512级网格中的实际沉积与守恒结果。
    EXPECT_NEAR(sum(evaluation.realCharge),1.0,1e-10);
    EXPECT_NEAR(sum(evaluation.fillerCharge),799999.0,799999.0e-10);
    EXPECT_NEAR(sum(evaluation.totalCharge),800000.0,800000.0e-10);
    EXPECT_EQ(database.nodes.size(),1U);EXPECT_TRUE(database.pins.empty()); // filler不得写入拓扑或真实计数。
    EXPECT_DOUBLE_EQ(database.originalPositions[0].x,1.125);EXPECT_DOUBLE_EQ(database.originalPositions[0].y,2.25);
}
/** @brief TC-GP-15：有限巨大合法权重诱发数值失败，诊断必须保持最后完整发布点和原输入固定性。
 * 无输入、无返回值；不修改const模型、不注入NaN、不依赖不安全内存或生产代码钩子。 */
TEST(GlobalOptimizer,FiniteHugeWeightFailureRetainsLastCompleteDiagnostic) {
    auto database=std::make_shared<DesignDatabase>(fixture()); // 尚未发布为const前构造可审计输入。
    database->nets[0].weight=1e200; // 合法有限正权重；导数平方范数可能溢出，必须转为NUMERIC_FAILURE。
    EXPECT_TRUE(std::isfinite(database->nets[0].weight));
    ASSERT_NO_THROW(validateDatabase(*database));
    Snapshot initial; // 明确绑定输入身份的完整初始快照。
    initial.database=database;initial.datasetId=database->datasetId;initial.runId="finite-weight-failure";
    initial.revision=17;initial.positions=database->originalPositions;
    Config config=configuration(); // 强制进入优化；只需有限轮数触达导数差范数的数值检查。
    config.maxIterations=4;config.overflowStop=0;config.fillerLimit=16;
    ControlToken control; // 正常运行令牌，失败必须来自数值检查而非取消。
    std::optional<Snapshot> lastComplete; // 仅由同步完整发布回调复制的最后一次一致几何。
    /** @brief 保存完整提交，便于与异常携带诊断独立比较。
     * @param snapshot 同轮有限坐标和指标的发布点。 无返回值；只复制测试局部状态。 */
    const auto sink=[&lastComplete](const Snapshot& snapshot) {
        lastComplete=snapshot;
        for(const Position& position:snapshot.positions) { // 每个实际发布节点位置，DU。
            EXPECT_TRUE(std::isfinite(position.x));EXPECT_TRUE(std::isfinite(position.y));
        }
        if(snapshot.metrics.hpwl) EXPECT_TRUE(std::isfinite(*snapshot.metrics.hpwl));
        if(snapshot.metrics.weightedHpwl) EXPECT_TRUE(std::isfinite(*snapshot.metrics.weightedHpwl));
        if(snapshot.metrics.densityEnergy) EXPECT_TRUE(std::isfinite(*snapshot.metrics.densityEnergy));
    }; // 捕获的lastComplete只由当前工作线程在同步回调内修改。
    try { RunGlobal(initial,config,control,sink); FAIL()<<"有限巨大权重的数值溢出必须报告而非发布非有限结果"; }
    catch(const Error& error) { // 稳定错误及不可变诊断必须保留最后完整布局，不泄漏半步候选。
        EXPECT_EQ(error.code,"NUMERIC_FAILURE");
        ASSERT_TRUE(error.diagnosticSnapshot);
        const Snapshot& diagnostic=*error.diagnosticSnapshot; // 异常持有的只读完整诊断。
        EXPECT_EQ(diagnostic.stage,"preview");EXPECT_EQ(diagnostic.termination,"NUMERIC_FAILURE");
        EXPECT_EQ(diagnostic.datasetId,initial.datasetId);EXPECT_EQ(diagnostic.runId,initial.runId);
        ASSERT_TRUE(lastComplete.has_value());
        EXPECT_EQ(diagnostic.iteration,lastComplete->iteration);EXPECT_EQ(diagnostic.revision,lastComplete->revision);
        ASSERT_EQ(diagnostic.positions.size(),lastComplete->positions.size());
        for(std::size_t id=0;id<diagnostic.positions.size();++id) { // 按稳定NodeId逐项核对最后完整点。
            EXPECT_DOUBLE_EQ(diagnostic.positions[id].x,lastComplete->positions[id].x);
            EXPECT_DOUBLE_EQ(diagnostic.positions[id].y,lastComplete->positions[id].y);
            EXPECT_EQ(diagnostic.positions[id].orientation,lastComplete->positions[id].orientation);
        }
        EXPECT_NO_THROW(validatePositions(*database,diagnostic.positions));
        EXPECT_EQ(diagnostic.metrics.hpwl,lastComplete->metrics.hpwl);
        EXPECT_EQ(diagnostic.metrics.weightedHpwl,lastComplete->metrics.weightedHpwl);
        EXPECT_EQ(diagnostic.metrics.densityEnergy,lastComplete->metrics.densityEnergy);
        EXPECT_DOUBLE_EQ(diagnostic.positions[4].x,29);EXPECT_DOUBLE_EQ(diagnostic.positions[4].y,28);
    }
    EXPECT_DOUBLE_EQ(database->originalPositions[0].x,initial.positions[0].x);
    EXPECT_DOUBLE_EQ(database->originalPositions[4].x,29);EXPECT_DOUBLE_EQ(database->originalPositions[4].y,28);
}
}
} // namespace placement
