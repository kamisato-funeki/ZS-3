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
/** @brief 可省略ORIGIN和四角矩形DIEAREA仍使用相同几何；无输入，无返回值。 */
TEST(LefDefInput,OptionalOriginAndFourCornerRectangle) {
    FixtureCopy copy("lefdef"); // 独立可变格式夹具。
    copy.replace("tiny.lef"," ORIGIN 0 0 ;\n","");copy.replace("tiny.def","DIEAREA ( 0 0 ) ( 40000 24000 ) ;","DIEAREA ( 0 0 ) ( 40000 0 ) ( 40000 24000 ) ( 0 24000 ) ;");
    InputSpec input; // 可选原点与四角合法矩形。
    input.lefPaths={(copy.path/"tiny.lef").string()};input.defPath=(copy.path/"tiny.def").string();ControlToken control; // 本例取消令牌。
    const auto database=LoadDesign(input,Config{},control); // 明确使用缺省ORIGIN=0。
    const Vec2 point=pinPosition(*database,database->originalPositions,database->pins[0]); // F-H同一手算引脚。
    EXPECT_DOUBLE_EQ(point.x,1.5);
    EXPECT_DOUBLE_EQ(point.y,3);
}
/** @brief 为输入审计构造不执行位置准备的只读几何快照。
 * @param database 已成功加载的只读数据库，由返回快照共享所有权。
 * @return 保留全部输入坐标及固定性的initial快照，无文件写出或位置修改。 */
Snapshot inputSnapshot(const std::shared_ptr<const DesignDatabase>& database) {
    Snapshot snapshot; // 当前输入的独立位置缓冲，指标计算不隐式准备布局。
    snapshot.database=database;
    snapshot.datasetId=database->datasetId;
    snapshot.runId="input-integration";
    snapshot.stage="initial";
    snapshot.positions=database->originalPositions;
    return snapshot;
}
/** @brief TC-IN-02：同一F-A端到端夹具组合空行、整行/行尾注释和tab，实际引脚线长为手算10 DU。
 * 无输入、无返回值；只读取仓库新增小夹具，不修改已有输入。 */
TEST(BookshelfInput,WhitespaceCommentsTabsAndFaEndToEnd) {
    InputSpec input; // 含空白及注释组合的独立BookShelf请求。
    input.auxPath=fixture("input-equivalence/bookshelf/equivalent.aux");
    ControlToken control; // 本次读取的独立取消令牌。
    const auto database=LoadDesign(input,Config{},control); // 输入位置与引脚由解析器直接归一化，未进行auto准备。
    ASSERT_EQ(database->nodes.size(),3U);
    ASSERT_EQ(database->nets.size(),2U);
    ASSERT_EQ(database->pins.size(),4U);
    ASSERT_EQ(database->rows.size(),5U);
    const Vec2 first=pinPosition(*database,database->originalPositions,database->pins[0]); // F-A引脚A真值(2.5,1.5)，DU。
    const Vec2 second=pinPosition(*database,database->originalPositions,database->pins[1]); // F-A引脚B真值(7.5,6.5)，DU。
    EXPECT_DOUBLE_EQ(first.x,2.5); EXPECT_DOUBLE_EQ(first.y,1.5);
    EXPECT_DOUBLE_EQ(second.x,7.5); EXPECT_DOUBLE_EQ(second.y,6.5);
    EXPECT_DOUBLE_EQ(std::abs(second.x-first.x)+std::abs(second.y-first.y),10.0);
    EXPECT_TRUE(database->nodes[1].fixed);
    EXPECT_TRUE(database->nodes[2].fixed);
    EXPECT_EQ(database->nodes[1].kind,NodeKind::Standard);
}
/** @brief TC-IN-05：显式2/0网络权重必须分别保留，零权网络仍贡献无权HPWL。
 * 无输入、无返回值；F-A网络10 DU加端口网络5 DU，期望无权15、加权20 DU。 */
TEST(BookshelfInput,ExplicitTwoAndZeroWeightsProduceIndependentHpwl) {
    InputSpec input; // 两条显式权重网络的完整BookShelf请求。
    input.auxPath=fixture("input-equivalence/bookshelf/equivalent.aux");
    ControlToken control; // 本例取消令牌。
    const auto database=LoadDesign(input,Config{},control); // 直接加载权重2/0，未沿用缺省1。
    ASSERT_EQ(database->nets.size(),2U);
    EXPECT_EQ(database->nets[0].name,"AB"); EXPECT_DOUBLE_EQ(database->nets[0].weight,2.0);
    EXPECT_EQ(database->nets[1].name,"AP"); EXPECT_DOUBLE_EQ(database->nets[1].weight,0.0);
    const Metrics metrics=ComputeMetrics(inputSnapshot(database)); // 由真实引脚重新计算，而非读取输入文件中的期望指标。
    ASSERT_TRUE(metrics.hpwl.has_value()); ASSERT_TRUE(metrics.weightedHpwl.has_value());
    EXPECT_DOUBLE_EQ(*metrics.hpwl,15.0);
    EXPECT_DOUBLE_EQ(*metrics.weightedHpwl,20.0);
}
/** @brief TC-IN-04/05：失败必须给出稳定错误代码，并能定位数量文件、非法数值行或悬空/重名对象。
 * 无输入、无返回值；每个负例使用独占临时副本，保留已存在的小夹具及其他测试改动。 */
TEST(BookshelfInput,InvalidInputsIdentifySourcePathLineOrObject) {
    {
        FixtureCopy copy("tiny"); // 仅改变节点声明数量的独立负例。
        copy.replace("tiny.nodes","NumNodes : 3","NumNodes : 4");
        InputSpec input; // 数量不一致输入。
        input.auxPath=(copy.path/"tiny.aux").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"节点声明计数不一致应拒绝"; }
        catch(const Error& error) { // 数量错误须直接定位来源文件，不能只给无上下文异常。
            EXPECT_EQ(error.code,"INPUT_INVALID");
            EXPECT_EQ(error.context,(copy.path/"tiny.nodes").string());
            EXPECT_NE(std::string(error.what()).find("声明数量"),std::string::npos);
        }
    }
    {
        FixtureCopy copy("tiny"); // 第5行节点宽度不是数字的独立负例。
        copy.replace("tiny.nodes","B 3 2","B invalid-width 2");
        InputSpec input; // 非法数值字段请求。
        input.auxPath=(copy.path/"tiny.aux").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"非法节点宽度应拒绝"; }
        catch(const Error& error) { // 数值错误保留文件与一基源行号。
            EXPECT_EQ(error.code,"INPUT_INVALID");
            EXPECT_EQ(error.context,(copy.path/"tiny.nodes").string()+":5");
            EXPECT_NE(std::string(error.what()).find("invalid-width"),std::string::npos);
        }
    }
    {
        FixtureCopy copy("tiny"); // 重复名称A的独立负例。
        copy.replace("tiny.nodes","B 3 2","A 3 2");
        InputSpec input; // 节点名称冲突输入。
        input.auxPath=(copy.path/"tiny.aux").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"重复节点名称应拒绝"; }
        catch(const Error& error) { // 重名错误必须定位冲突对象A。
            EXPECT_EQ(error.code,"INPUT_INVALID"); EXPECT_EQ(error.context,"A");
            EXPECT_NE(std::string(error.what()).find("重复节点"),std::string::npos);
        }
    }
    {
        FixtureCopy copy("tiny"); // 第6行网络引用不存在节点的独立负例。
        copy.replace("tiny.nets","B I : 0 0","MISSING_NODE I : 0 0");
        InputSpec input; // 引脚归属悬空请求。
        input.auxPath=(copy.path/"tiny.aux").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"悬空引脚归属应拒绝"; }
        catch(const Error& error) { // 同时保留对象名、输入文件与源行号。
            EXPECT_EQ(error.code,"REFERENCE_MISSING");
            EXPECT_EQ(error.context,"MISSING_NODE "+(copy.path/"tiny.nets").string()+":6");
        }
    }
    {
        FixtureCopy copy("tiny"); // 显式负权重的独立负例。
        copy.replace("tiny.wts","UCLA wts 1.0","UCLA wts 1.0\nAB -1");
        InputSpec input; // 权重越界请求。
        input.auxPath=(copy.path/"tiny.aux").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"负网络权重应拒绝"; }
        catch(const Error& error) { // 负权重错误明确定位网络AB。
            EXPECT_EQ(error.code,"INPUT_INVALID"); EXPECT_EQ(error.context,"AB");
            EXPECT_NE(std::string(error.what()).find("权重"),std::string::npos);
        }
    }
}
/** @brief TC-IN-07：缺LEF master引脚、DEF ROW所需SITE和LEF MACRO所需SITE分别拒绝并定位对象。
 * 无输入、无返回值；只修改临时LEF/DEF副本，不修改已有夹具。 */
TEST(LefDefInput,MissingPinsAndSitesHaveExplicitObjectContext) {
    /** @brief 单项配套引用缺失变体，字段以名字说明失败来源。 */
    struct MissingReference {
        std::string file; ///< 副本中需要改变的文件名。
        std::string before; ///< 必须存在且只替换一次的原文。
        std::string after; ///< 造成一个明确缺失引用的新文本。
        std::string context; ///< 应由业务Error精确定位的master/pin或site名称。
    };
    const std::array<MissingReference,3> cases={{
        {"tiny.lef"," PIN P\n  DIRECTION INOUT ;\n  PORT\n   LAYER M1 ;\n   RECT 0.4 0.9 0.6 1.1 ;\n  END\n END P\n","","CELL/P"},
        {"tiny.def","ROW R0 CoreSite","ROW R0 MISSING_ROW_SITE","MISSING_ROW_SITE"},
        {"tiny.lef"," SITE CoreSite ;"," SITE MISSING_MASTER_SITE ;","MISSING_MASTER_SITE"}
    }}; // 三项相互独立的LEF/DEF语义引用错误。
    for(const MissingReference& missing:cases) { // 当前缺失引用规格。
        FixtureCopy copy("lefdef"); // 当前失败场景独占副本。
        copy.replace(missing.file,missing.before,missing.after);
        InputSpec input; // 缺失一个语义引用的配套输入。
        input.lefPaths={(copy.path/"tiny.lef").string()};
        input.defPath=(copy.path/"tiny.def").string();
        ControlToken control; // 当前负例取消令牌。
        try { LoadDesign(input,Config{},control); FAIL()<<"缺失引用应拒绝: "<<missing.context; }
        catch(const Error& error) { // 不允许解析器将缺失内容静默降级或当作空引脚。
            EXPECT_EQ(error.code,"REFERENCE_MISSING")<<error.what();
            EXPECT_EQ(error.context,missing.context);
        }
    }
}
/** @brief TC-INT-05：两格式等价F-A扩展小例逐项核对归一化范围、全部世界引脚、网络HPWL及固定性。
 * 无输入、无返回值；独立纸面真值以μm给出，允许误差1e-12 μm。 */
TEST(InputEquivalence,BookshelfAndLefDefMatchAllNormalizedGeometryAndMetrics) {
    InputSpec bookshelfInput; // 显式1 μm/DU的BookShelf等价输入。
    bookshelfInput.auxPath=fixture("input-equivalence/bookshelf/equivalent.aux");
    InputSpec lefDefInput; // DBU/μm=2000的配套LEF/DEF等价输入。
    lefDefInput.lefPaths={fixture("input-equivalence/lefdef/equivalent.lef")};
    lefDefInput.defPath=fixture("input-equivalence/lefdef/equivalent.def");
    Config bookshelfConfig; // 仅声明真实物理比例，不改变源坐标。
    bookshelfConfig.micronsPerDu=1.0;
    ControlToken control; // 两个串行独立加载共享取消请求，不跨线程。
    const auto bookshelf=LoadDesign(bookshelfInput,bookshelfConfig,control); // BookShelf规范模型。
    const auto lefDef=LoadDesign(lefDefInput,Config{},control); // LEF/DEF已从2000 DBU/μm归一化模型。
    ASSERT_EQ(bookshelf->nodes.size(),3U); ASSERT_EQ(lefDef->nodes.size(),3U);
    ASSERT_EQ(bookshelf->pins.size(),4U); ASSERT_EQ(lefDef->pins.size(),4U);
    ASSERT_EQ(bookshelf->nets.size(),2U); ASSERT_EQ(lefDef->nets.size(),2U);
    ASSERT_EQ(bookshelf->rows.size(),5U); ASSERT_EQ(lefDef->rows.size(),5U);
    EXPECT_TRUE(bookshelf->physicalScaleKnown); EXPECT_TRUE(lefDef->physicalScaleKnown);
    EXPECT_EQ(bookshelf->unit,"DU"); EXPECT_EQ(lefDef->unit,"um");
    const std::array<Rect,3> expectedBounds={Rect{1,1,3,3},Rect{7,5,9,7},Rect{-1,3,-1,3}}; // 各节点纸面物理框，μm。
    const std::array<Vec2,4> expectedPins={Vec2{2.5,1.5},Vec2{7.5,6.5},Vec2{2.5,1.5},Vec2{-1,3}}; // 原始网络顺序下全部世界引脚真值，μm。
    const std::array<bool,3> expectedFixed={false,true,true}; // A可移动、B显式FIXED、PORT固定。
    const std::array<double,2> expectedNetHpwl={10.0,5.0}; // AB和AP分别手算的无权HPWL，μm。
    const std::array<std::shared_ptr<const DesignDatabase>,2> databases={bookshelf,lefDef}; // 分别对纸面真值核对，避免两个解析器以相同错误互相证明。
    for(const auto& database:databases) { // 当前需要独立验证的格式模型。
        const double scale=database->micronsPerDu; // 当前DU到μm的物理换算比例。
        const Rect core=coreBounds(*database); // 原始行并集包围盒，DU。
        const Rect chip=chipBounds(*database); // core及外部端口的显示范围，DU。
        EXPECT_NEAR(core.lx*scale,0,1e-12); EXPECT_NEAR(core.ly*scale,0,1e-12);
        EXPECT_NEAR(core.ux*scale,10,1e-12); EXPECT_NEAR(core.uy*scale,10,1e-12);
        EXPECT_NEAR(chip.lx*scale,-1,1e-12); EXPECT_NEAR(chip.ly*scale,0,1e-12);
        EXPECT_NEAR(chip.ux*scale,10,1e-12); EXPECT_NEAR(chip.uy*scale,10,1e-12);
        for(std::size_t id=0;id<expectedBounds.size();++id) { // 对应NodeId的纸面范围索引。
            const Rect bounds=nodeRect(database->nodes[id],database->originalPositions[id]); // 当前实例的方向后物理框，DU。
            EXPECT_NEAR(bounds.lx*scale,expectedBounds[id].lx,1e-12);
            EXPECT_NEAR(bounds.ly*scale,expectedBounds[id].ly,1e-12);
            EXPECT_NEAR(bounds.ux*scale,expectedBounds[id].ux,1e-12);
            EXPECT_NEAR(bounds.uy*scale,expectedBounds[id].uy,1e-12);
            EXPECT_EQ(database->nodes[id].fixed,expectedFixed[id]);
        }
        for(std::size_t id=0;id<expectedPins.size();++id) { // 对应PinId的全部世界引脚真值索引。
            const Vec2 point=pinPosition(*database,database->originalPositions,database->pins[id]); // 当前实际引脚位置，DU。
            EXPECT_NEAR(point.x*scale,expectedPins[id].x,1e-12);
            EXPECT_NEAR(point.y*scale,expectedPins[id].y,1e-12);
        }
        for(std::size_t id=0;id<expectedNetHpwl.size();++id) { // 对应NetId的两引脚网络索引。
            const Net& net=database->nets[id]; // 当前真实网络引用。
            ASSERT_EQ(net.pinIds.size(),2U);
            const Vec2 first=pinPosition(*database,database->originalPositions,database->pins[net.pinIds[0]]); // 当前网络首引脚，DU。
            const Vec2 second=pinPosition(*database,database->originalPositions,database->pins[net.pinIds[1]]); // 当前网络次引脚，DU。
            const double length=(std::abs(second.x-first.x)+std::abs(second.y-first.y))*scale; // 两引脚网络独立曼哈顿长度，μm。
            EXPECT_NEAR(length,expectedNetHpwl[id],1e-12);
        }
        EXPECT_DOUBLE_EQ(database->nets[0].weight,2.0); EXPECT_DOUBLE_EQ(database->nets[1].weight,0.0);
        const Metrics metrics=ComputeMetrics(inputSnapshot(database)); // 对本格式独立计算完整真实指标，不跨dataset调用CompareStages。
        ASSERT_TRUE(metrics.hpwl.has_value()); ASSERT_TRUE(metrics.weightedHpwl.has_value());
        EXPECT_NEAR(*metrics.hpwl*scale,15.0,1e-12);
        EXPECT_NEAR(*metrics.weightedHpwl*scale,20.0,1e-12);
    }
    for(std::size_t id=0;id<bookshelf->nodes.size();++id) { // 跨格式显式核对稳定节点名称、类别和输入方向。
        EXPECT_EQ(bookshelf->nodes[id].name,lefDef->nodes[id].name);
        EXPECT_EQ(bookshelf->nodes[id].kind,lefDef->nodes[id].kind);
        EXPECT_EQ(bookshelf->originalPositions[id].orientation,lefDef->originalPositions[id].orientation);
    }
    for(std::size_t id=0;id<bookshelf->pins.size();++id) { // 跨格式全部引脚归属必须一致。
        EXPECT_EQ(bookshelf->pins[id].nodeId,lefDef->pins[id].nodeId);
        EXPECT_EQ(bookshelf->pins[id].netId,lefDef->pins[id].netId);
    }
}
} // namespace
} // namespace placement
