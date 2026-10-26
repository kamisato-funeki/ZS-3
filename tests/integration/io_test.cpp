#include <placement/io/design.hpp>
#include <placement/application/service.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <array>
#include <cmath>
#include <unistd.h>

namespace placement {
namespace {
/** @brief 获取仓库夹具路径。 @param relative 相对fixtures路径。 @return 绝对路径。 */
std::string fixture(const std::string& relative) {return std::string(PLACEMENT_SOURCE_DIR)+"/tests/fixtures/"+relative;}
/** @brief 一个可修改的独占小夹具副本，不触碰原始参考数据。 */
struct FixtureCopy {
    std::filesystem::path path; ///< 本测试临时副本目录。
    /** @brief 复制指定小型夹具。 @param source fixtures下目录。 无返回值。 */
    explicit FixtureCopy(const std::string& source) {
        static std::atomic<unsigned> serial{0}; // 本进程夹具副本序号。
        path=std::filesystem::temp_directory_path()/("placement-io-"+std::to_string(::getpid())+"-"+std::to_string(serial++));
        std::filesystem::create_directory(path);std::filesystem::copy(fixture(source),path,std::filesystem::copy_options::recursive|std::filesystem::copy_options::overwrite_existing);
    }
    /** @brief 释放本测试副本；无输入、无返回值，不抛清理异常。 */
    ~FixtureCopy() {std::error_code error; /* error为清理诊断。 */ std::filesystem::remove_all(path,error);}
    /** @brief 在副本中替换一处精确文本。 @param file 文件名。 @param before 原文。 @param after 新文。 无返回值。 */
    void replace(const std::string& file,const std::string& before,const std::string& after) {
        std::string text=ReadText((path/file).string()); // 小型输入文本副本。
        const auto index=text.find(before); // 要替换的精确位置。
        ASSERT_NE(index,std::string::npos);text.replace(index,before.size(),after);
        std::ofstream stream(path/file); // 本夹具输出流。
        stream<<text;
    }
};
/** @brief 独立验证SHA-256已知向量；无输入，无返回值。 */
TEST(InputAudit,KnownSha256) {EXPECT_EQ(Sha256("abc"),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");}
/** @brief TC-IN-01/02/05：AUX关联和空权重默认值；无输入，无返回值。 */
TEST(BookshelfInput,CountsOffsetsRowsAndDefaultWeights) {
    ControlToken control; // 独立加载取消令牌。
    InputSpec input; // AUX输入请求。
    input.auxPath=fixture("tiny/tiny.aux");
    const auto database=LoadDesign(input,Config{},control); // 成功后只读拓扑。
    ASSERT_EQ(database->nodes.size(),3U);
    ASSERT_EQ(database->pins.size(),4U);
    ASSERT_EQ(database->nets.size(),2U);
    ASSERT_EQ(database->rows.size(),2U);
    EXPECT_EQ(database->inputFiles.size(),6U);
    EXPECT_EQ(database->datasetId.size(),64U);
    EXPECT_EQ(database->nets[0].weight,1);
    EXPECT_TRUE(database->nodes[2].fixed);
    EXPECT_EQ(database->nodes[0].kind,NodeKind::Standard);
    EXPECT_EQ(database->originalPositions[2].x,-1);
}
/** @brief TC-IN-03：缺输入错误带路径，不发布空数据库；无输入，无返回值。 */
TEST(BookshelfInput,MissingInputHasExplicitError) {
    ControlToken control; // 本次失败加载令牌。
    InputSpec input; // 不存在的AUX。
    input.auxPath=fixture("does-not-exist.aux");
    try {LoadDesign(input,Config{},control);FAIL();} catch(const Error& error) {EXPECT_EQ(error.code,"REFERENCE_MISSING");
    EXPECT_FALSE(error.context.empty());} // 业务错误证据。
}
/** @brief TC-IN-06：F-H单位与真实pin点手算；无输入，无返回值。 */
TEST(LefDefInput,MicronUnitsMasterPinsAndTopPort) {
    ControlToken control; // 本次加载令牌。
    InputSpec input; // 配套人工LEF/DEF请求。
    input.lefPaths={fixture("lefdef/tiny.lef")};input.defPath=fixture("lefdef/tiny.def");
    const auto database=LoadDesign(input,Config{},control); // 已归一化数据库。
    ASSERT_EQ(database->nodes.size(),3U);
    ASSERT_EQ(database->rows.size(),2U);
    ASSERT_EQ(database->pins.size(),4U);
    EXPECT_TRUE(database->physicalScaleKnown);
    EXPECT_EQ(database->unit,"um");
    EXPECT_DOUBLE_EQ(database->originalPositions[0].x,1);
    EXPECT_DOUBLE_EQ(database->originalPositions[0].y,2);
    const Vec2 pin=pinPosition(*database,database->originalPositions,database->pins[0]); // 手算期待(1.5,3)μm。
    EXPECT_NEAR(pin.x,1.5,1e-12);
    EXPECT_NEAR(pin.y,3,1e-12);
    EXPECT_DOUBLE_EQ(database->originalPositions[2].x,-1);
}
/** @brief TC-IN-07：不配套master必须拒绝；无输入，无返回值。 */
TEST(LefDefInput,MissingMasterIsRejected) {
    const std::filesystem::path temporary=std::filesystem::temp_directory_path()/"placement-missing-master.def"; // 独立负例路径。
    std::ifstream source(fixture("lefdef/tiny.def")); // 原始人工输入。
    std::string text((std::istreambuf_iterator<char>(source)),{}); // 仅小夹具的文本副本。
    text.replace(text.find("A CELL"),6,"A UNKNOWN");
    {std::ofstream output(temporary);output<<text;} // 完整写入负例。
    ControlToken control; // 负例控制令牌。
    InputSpec input; // 配套LEF加缺master DEF。
    input.lefPaths={fixture("lefdef/tiny.lef")};input.defPath=temporary.string();
    try {LoadDesign(input,Config{},control);FAIL();} catch(const Error& error) {EXPECT_EQ(error.code,"REFERENCE_MISSING");} // 应产生明确缺失引用。
    std::filesystem::remove(temporary);
}
/** @brief 缺失PL位置仍按尺寸分类，auto补齐后进入标准合法化；无输入，无返回值。 */
TEST(BookshelfInput,MissingPlacementDoesNotChangeNodeClassification) {
    FixtureCopy copy("tiny"); // 本测试独立可变输入。
    copy.replace("tiny.pl","B 2 0 : N\n","");
    InputSpec input; // 缺失移动位置的合法拓扑。
    input.auxPath=(copy.path/"tiny.aux").string();ControlToken control; // 本次加载/准备控制。
    const auto database=LoadDesign(input,Config{},control); // 拓扑分类不依赖NaN位置。
    EXPECT_EQ(database->nodes[1].kind,NodeKind::Standard);
    const Snapshot snapshot=PrepareInitial(database,Config{},"missing-position",control); // auto填充。
    EXPECT_TRUE(std::isfinite(snapshot.positions[1].x));
    EXPECT_FALSE(snapshot.adjustments.empty());
}
/** @brief TC-IN-04/05：声明计数、重名、悬空引脚和负权重分别拒绝；无输入，无返回值。 */
TEST(BookshelfInput,RejectsCountsNamesReferencesAndWeights) {
    const std::vector<std::array<std::string,3>> mutations={{{"tiny.nodes","NumNodes : 3","NumNodes : 4"}},{{"tiny.nodes","B 3 2","A 3 2"}},{{"tiny.nets","B I : 0 0","MISSING I : 0 0"}},{{"tiny.wts","UCLA wts 1.0","UCLA wts 1.0\nAB -1"}}}; // 文件/原文/新文三项负例规格。
    for(const auto& mutation:mutations) { // 当前独立失败场景。
        FixtureCopy copy("tiny"); // 不共享变更的夹具。
        copy.replace(mutation[0],mutation[1],mutation[2]);
        InputSpec input; // 当前负例请求。
        input.auxPath=(copy.path/"tiny.aux").string();ControlToken control; // 每例独立控制令牌。
        EXPECT_THROW(LoadDesign(input,Config{},control),Error);
    }
}
/** @brief TC-IN-08：非矩形DIE/REGIONS/软阻挡明确不支持；无输入，无返回值。 */
TEST(LefDefInput,RejectsUnsupportedPlacementConstraints) {
    const std::vector<std::array<std::string,2>> mutations={{{"DIEAREA ( 0 0 ) ( 40000 24000 ) ;","DIEAREA ( 0 0 ) ( 40000 0 ) ( 30000 24000 ) ;"}},{{"END DESIGN","REGIONS 1 ;\n- R ( 0 0 ) ( 100 100 ) ;\nEND REGIONS\nEND DESIGN"}},{{"END DESIGN","BLOCKAGES 1 ;\n- PLACEMENT + SOFT RECT ( 0 0 ) ( 100 100 ) ;\nEND BLOCKAGES\nEND DESIGN"}}}; // 原文/替换约束。
    for(const auto& mutation:mutations) { // 当前不支持构造。
        FixtureCopy copy("lefdef"); // 独立LEF/DEF副本。
        copy.replace("tiny.def",mutation[0],mutation[1]);InputSpec input; // 修改后的请求。
        input.lefPaths={(copy.path/"tiny.lef").string()};input.defPath=(copy.path/"tiny.def").string();ControlToken control; // 本例令牌。
        try {LoadDesign(input,Config{},control);FAIL();}catch(const Error& error) {EXPECT_EQ(error.code,"UNSUPPORTED")<<error.what();} // 明确约束诊断。
    }
}
/** @brief 非零ORIGIN加八方向的独立手算点与bbox；无输入，无返回值。
 * 来源：LEF/DEF5.8的ORIGIN先平移几何，SIZE框旋转后左下角对齐DEF placement点。 */
TEST(LefDefInput,NonzeroOriginAndEightDirections) {
    const std::array<std::string,8> directions={"N","S","W","E","FN","FS","FW","FE"}; // 独立方向顺序。
    const std::array<Vec2,8> expected={Vec2{.75,1.5},Vec2{1.25,2.5},Vec2{2.5,.75},Vec2{1.5,1.25},Vec2{1.25,1.5},Vec2{.75,2.5},Vec2{1.5,.75},Vec2{2.5,1.25}}; // 手算归一化PIN坐标，μm，非复用实现变换。
    for(std::size_t direction=0;direction<directions.size();++direction) { // 当前朝向索引。
        FixtureCopy copy("lefdef"); // 本朝向输入副本。
        copy.replace("tiny.lef","ORIGIN 0 0","ORIGIN 0.25 0.5");copy.replace("tiny.def","( 2000 4000 ) N","( 2000 4000 ) "+directions[direction]);
        InputSpec input; // 配套文件请求。
        input.lefPaths={(copy.path/"tiny.lef").string()};input.defPath=(copy.path/"tiny.def").string();ControlToken control; // 朝向例令牌。
        const auto database=LoadDesign(input,Config{},control); // 归一化模型。
        const Vec2 pin=pinPosition(*database,database->originalPositions,database->pins[0]); // 实际全局点，μm。
        EXPECT_NEAR(pin.x,1+expected[direction].x,1e-12);
    EXPECT_NEAR(pin.y,2+expected[direction].y,1e-12);
        EXPECT_DOUBLE_EQ(database->originalPositions[0].x,1);
    EXPECT_DOUBLE_EQ(database->originalPositions[0].y,2);
    }
}
} // namespace
} // namespace placement
