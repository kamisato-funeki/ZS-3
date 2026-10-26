#include <placement/application/service.hpp>
#include <placement/io/visualization.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace placement {
namespace {
/** @brief 独立导出精度夹具；节点相离，坐标跨正负半DBU，不复用被测量化代码。 @return 完整只读源。 */
Snapshot precisionFixture() {
    auto db=std::make_shared<DesignDatabase>(); // 拥有两个小宏的只读拓扑源。
    db->datasetId="halfway-precision";db->physicalScaleKnown=true;db->micronsPerDu=1;
    RowSegment row; // 20×20 DU连续可放置域，不引入站点约束。
    row.height=20;row.y=-10;row.xOrigin=-10;row.numSites=20;row.siteWidth=1;row.sitePitch=1;db->rows.push_back(row);
    for(int index=0;index<2;++index) { // 两个正负中点样本索引。
        Node node; // 2 DBU尺寸，防止被量化坍缩检查拒绝。
        node.id=index;node.name="half"+std::to_string(index);node.kind=NodeKind::Macro;node.width=.002;node.height=.002;db->nodes.push_back(node);
        db->originalPositions.push_back({index==0?1.2345:-1.2345,index==0?1.2345:-1.2345,Orientation::N});
    }
    Snapshot result; // 仅包含真实实例的源快照。
    result.database=db;result.datasetId=db->datasetId;result.runId="precision";result.stage="initial";result.positions=db->originalPositions;result.termination="prepared";return result;
}
/** @brief TC-VIS-05/06：独立读取二进制XY记录验证正负中点，超int32时不能破坏已有文件；无输入，无返回值。 */
TEST(GdsPrecision,HalfwayCoordinatesAndOverflowPreserveExistingFile) {
    const auto directory=std::filesystem::temp_directory_path()/("gds-precision-"+NewRunId()); // 本测试独占临时目录。
    std::filesystem::create_directory(directory);
    /** @brief RAII清理仅本测试目录，无输入、无返回值且不抛异常。 */
    struct Cleanup {
        std::filesystem::path directory; ///< 本测试拥有的目录。
        /** @brief 删除本测试目录，无输入、无返回值。 */
        ~Cleanup(){std::error_code error; /* error为清理诊断，析构不抛出。 */std::filesystem::remove_all(directory,error);}
    } cleanup{directory}; // 在任何断言返回路径都清理。
    Snapshot snapshot=precisionFixture(); // 原始合法几何，尺度μm/DU=1。
    ControlToken token; // 独立导出令牌。
    Config config; // 默认DBU=.001 μm。
    const std::string path=(directory/"precision.gds").string(); // 唯一成功输出。
    ExportGds(snapshot,path,config,token);
    std::ifstream stream(path,std::ios::binary); // 不经过Limbo的原始记录读取器。
    int layer=-1; // 最近LAYER记录中的整数层号。
    std::set<std::array<std::int64_t,2>> firstPoints; // 层2全部XY的首点，整数DBU。
    while(stream.peek()!=EOF) {
        std::array<unsigned char,4> header{}; // 大端长度/type/dataType。
        stream.read(reinterpret_cast<char*>(header.data()),header.size());ASSERT_TRUE(stream);
        const int length=(header[0]<<8)|header[1]; // 独立记录长度解码，字节。
        ASSERT_GE(length,4);
        std::vector<unsigned char> payload(static_cast<std::size_t>(length-4)); // 当前记录原始内容。
        stream.read(reinterpret_cast<char*>(payload.data()),static_cast<std::streamsize>(payload.size()));ASSERT_TRUE(stream);
        if(header[2]==0x0d&&payload.size()==2)layer=(payload[0]<<8)|payload[1];
        if(header[2]==0x10&&layer==2) {
            ASSERT_GE(payload.size(),8U);
            std::array<std::int64_t,2> point{}; // 解码的首个x/y DBU。
            for(int axis=0;axis<2;++axis) { // XY首点的两个坐标。
                std::uint64_t unsignedValue=0; // 独立大端无符号累积，避免有符号移位溢出。
                for(int byte=0;byte<4;++byte)unsignedValue=(unsignedValue<<8)|payload[axis*4+byte]; // byte为坐标字节序号。
                point[axis]=unsignedValue>=0x80000000ULL?static_cast<std::int64_t>(unsignedValue)-0x100000000LL:static_cast<std::int64_t>(unsignedValue);
            }
            firstPoints.insert(point);
        }
    }
    EXPECT_EQ(firstPoints,(std::set<std::array<std::int64_t,2>>{{1235,1235},{-1235,-1235}}));
    const std::string previous=ReadText(path); // 二进制也通过完整字节字符串比较。
    const std::string metadata=ReadText(path+".metadata.json"); // 原成功单位元数据。
    snapshot.positions[0].x=3e9;
    EXPECT_THROW(ExportGds(snapshot,path,config,token,true),Error);
    EXPECT_EQ(ReadText(path),previous);EXPECT_EQ(ReadText(path+".metadata.json"),metadata);
}
/** @brief TC-IN-10/11：越界auto、preserve拒绝、聚集扰动均不修改原始固定基线；无输入，无返回值。 */
TEST(InitialPreparation,AutoRepairsAndPreserveRejectsWithoutChangingInput) {
    auto db=std::make_shared<DesignDatabase>(*precisionFixture().database); // 可构造异常初始位置的独占数据库。
    db->nodes[0].width=2;db->nodes[0].height=2;db->nodes[1].width=2;db->nodes[1].height=2;
    db->originalPositions={{-100,100,Orientation::N},{2,2,Orientation::N}};
    Config config; // 默认auto策略及固定seed。
    ControlToken token; // 初始准备令牌。
    const Snapshot repaired=PrepareInitial(db,config,"auto",token); // 可审计修复结果。
    EXPECT_GE(repaired.positions[0].x,-10);EXPECT_LE(repaired.positions[0].x,8);EXPECT_GE(repaired.positions[0].y,-10);EXPECT_LE(repaired.positions[0].y,8);
    EXPECT_DOUBLE_EQ(db->originalPositions[0].x,-100);EXPECT_FALSE(repaired.adjustments.empty());
    config.initialMode="preserve";EXPECT_THROW(PrepareInitial(db,config,"preserve",token),Error);
    db->originalPositions={{0,0,Orientation::N},{0,0,Orientation::N}};config.initialMode="auto";
    const Snapshot first=PrepareInitial(db,config,"cluster1",token); // 第一次同坐标扰动。
    const Snapshot second=PrepareInitial(db,config,"cluster2",token); // 相同种子重现。
    EXPECT_DOUBLE_EQ(first.positions[0].x,second.positions[0].x);EXPECT_DOUBLE_EQ(first.positions[1].y,second.positions[1].y);
    EXPECT_NE(first.positions[0].x,first.positions[1].x);EXPECT_DOUBLE_EQ(db->originalPositions[0].x,0);
}
} // namespace
} // namespace placement
