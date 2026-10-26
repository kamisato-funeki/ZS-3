#include "placement/application/global.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
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
}
} // namespace placement
