#include <placement/io/design.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <set>
#include <unordered_map>

namespace placement {
namespace {
/** @brief 返回无注释的词法行；每行保留源行号，所有权归调用者。 */
struct TextLine {
    std::size_t number; ///< 文件中的一基行号。
    std::vector<std::string> words; ///< 冒号已作为独立 token 的字段。
};
/** @brief 按行读取小型元数据文件，核查取消并去除注释。
 * @param path 输入路径。 @param control 取消令牌。 @return 词法行；I/O 错误抛出。 */
std::vector<TextLine> readLines(const std::filesystem::path& path, ControlToken& control) {
    std::ifstream stream(path); // 当前输入文件，只在本函数持有。
    if (!stream) Throw("REFERENCE_MISSING", "无法读取关联文件", path.string(), true);
    std::vector<TextLine> lines; // 非空词法行，顺序与原文件一致。
    std::string text; // 当前原始行文本。
    std::size_t number = 0; // 用于错误定位和批次取消的源行计数。
    while (std::getline(stream, text)) {
        ++number;
        if (number % 4096 == 0) control.checkCancelled();
        text = text.substr(0, text.find('#'));
        std::string spaced; // 分隔冒号后的词法文本，避免依赖空格格式。
        for (const char character : text) { // 当前源字符。
            if (character == ':') spaced += " : "; else spaced += character;
        }
        std::istringstream parser(spaced); // 当前行字段读取器。
        TextLine line{number, {}}; // 本行编号及字段集合。
        std::string word; // 当前字段。
        while (parser >> word) line.words.push_back(word);
        if (!line.words.empty()) lines.push_back(std::move(line));
    }
    if (!stream.eof()) Throw("IO_FAILURE", "读取中断", path.string());
    control.checkCancelled();
    return lines;
}
/** @brief 严格转换有限浮点数。 @param value 数字字段。 @param context 源位置。 @return DU 数值；格式错误抛 INPUT_INVALID。 */
double number(const std::string& value, const std::string& context) {
    try {
        std::size_t consumed = 0; // 已解析字符数，拒绝尾部垃圾。
        const double result = std::stod(value, &consumed); // 解析结果，单位由调用字段决定。
        if (consumed != value.size() || !std::isfinite(result)) Throw("INPUT_INVALID", "数值必须有限", context, true);
        return result;
    } catch (const Error&) { throw; }
    catch (const std::exception&) { Throw("INPUT_INVALID", "无法解析数值 " + value, context, true); }
}
/** @brief 严格读取非负整数。 @param value 字段。 @param context 位置。 @return 计数；非法抛 INPUT_INVALID。 */
std::uint64_t count(const std::string& value, const std::string& context) {
    const double parsed = number(value, context); // 计数值，无量纲。
    if (parsed < 0 || std::floor(parsed) != parsed || parsed > 1e12) Throw("INPUT_INVALID", "非法数量", context, true);
    return static_cast<std::uint64_t>(parsed);
}
/** @brief 为词法错误生成源定位。 @param path 文件。 @param line 词法行。 @return path:line。 */
std::string location(const std::filesystem::path& path, const TextLine& line) {
    return path.string() + ":" + std::to_string(line.number);
}
/** @brief 判断计数头并取值。 @param line 当前词法行。 @param key 头字段。 @param output 输出数量。 @return 是否匹配；错误格式抛 INPUT_INVALID。 */
bool header(const TextLine& line, const std::string& key, std::uint64_t& output) {
    if (line.words[0] != key) return false;
    if (line.words.size() != 3 || line.words[1] != ":") Throw("INPUT_INVALID", "无效计数头", key);
    output = count(line.words[2], key);
    return true;
}
/** @brief 从节点文件建立稳定节点顺序。 @param path nodes。 @param db 独占数据库。 @param index 名称映射输出。 @param control 取消令牌。 无返回值，修改拓扑。 */
void readNodes(const std::filesystem::path& path, DesignDatabase& db, std::unordered_map<std::string, NodeId>& index, ControlToken& control) {
    std::uint64_t expected = std::numeric_limits<std::uint64_t>::max(); // 声明节点数，最大值表示尚未声明。
    std::uint64_t terminals = std::numeric_limits<std::uint64_t>::max(); // 声明终端数。
    std::uint64_t actualTerminals = 0; // 实际终端数。
    for (const TextLine& line : readLines(path, control)) { // 当前有意义的源行。
        const auto& words = line.words; // 本行只读字段视图。
        if (words[0] == "UCLA" || header(line,"NumNodes",expected) || header(line,"NumTerminals",terminals)) continue;
        if (words.size() < 3 || words.size() > 4) Throw("INPUT_INVALID","nodes 字段数量错误",location(path,line));
        Node node; // 当前输入节点，ID 是规范文件顺序。
        node.id = db.nodes.size(); node.name = words[0];
        node.width = number(words[1],location(path,line)); node.height = number(words[2],location(path,line));
        if (words.size() == 4) {
            if (words[3] != "terminal" && words[3] != "terminal_NI") Throw("UNSUPPORTED","未知节点属性",location(path,line));
            node.kind = NodeKind::Terminal; node.fixed = true; ++actualTerminals;
        }
        if (!index.emplace(node.name,node.id).second) Throw("INPUT_INVALID","重复节点名",node.name);
        db.nodes.push_back(node);
        db.originalPositions.push_back({std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),Orientation::N});
    }
    if (expected != db.nodes.size() || terminals != actualTerminals) Throw("INPUT_INVALID","nodes 声明数量与实际不一致",path.string());
}
/** @brief 读取输入左下角，保留固定性和方向。 @param path pl。 @param db 独占数据库。 @param index 节点名称索引。 @param control 取消令牌。 无返回值。 */
void readPositions(const std::filesystem::path& path, DesignDatabase& db, const std::unordered_map<std::string,NodeId>& index, ControlToken& control) {
    std::vector<bool> seen(db.nodes.size(),false); // 节点重复放置检测。
    for (const TextLine& line : readLines(path,control)) { // 当前放置记录。
        const auto& words = line.words; // 本行字段视图。
        if (words[0] == "UCLA") continue;
        if (words.size() < 5 || words[3] != ":") Throw("INPUT_INVALID","pl 字段格式错误",location(path,line));
        const auto found = index.find(words[0]); // 名称对应稳定ID。
        if (found == index.end()) Throw("REFERENCE_MISSING","pl 引用未知节点",words[0]);
        const NodeId id = found->second; // 当前节点ID。
        if (seen[id]) Throw("INPUT_INVALID","重复放置记录",words[0]);
        seen[id] = true;
        const Orientation orientation = parseOrientation(words[4]); // 输入方向，不允许优化器旋转。
        db.nodes[id].orientation = orientation;
        db.originalPositions[id] = {number(words[1],location(path,line)),number(words[2],location(path,line)),orientation};
        if (words.size() > 6) Throw("INPUT_INVALID","pl 存在多余字段",location(path,line));
        if (words.size() == 6) {
            if (words[5] != "/FIXED" && words[5] != "/FIXED_NI") Throw("UNSUPPORTED","未知放置约束",location(path,line));
            db.nodes[id].fixed = true;
        }
    }
}
/** @brief 加载水平行及多个 subrow。 @param path scl。 @param db 输出数据库。 @param control 取消令牌。 无返回值。 */
void readRows(const std::filesystem::path& path, DesignDatabase& db, ControlToken& control) {
    std::uint64_t expected = std::numeric_limits<std::uint64_t>::max(); // 声明CoreRow段数。
    std::uint64_t actual = 0; // 已完成CoreRow数。
    RowSegment row; // 当前CoreRow的共享字段。
    bool active = false; // 是否处于CoreRow内部。
    for (const TextLine& line : readLines(path,control)) { // 当前SCL记录。
        const auto& words = line.words; // 字段视图。
        if (words[0] == "UCLA" || header(line,"NumRows",expected)) continue;
        if (words[0] == "CoreRow") {
            if (active || words.size()!=2 || words[1]!="Horizontal") Throw("UNSUPPORTED","仅支持水平CoreRow",location(path,line));
            row = {}; active = true; continue;
        }
        if (words[0] == "End") {
            if (!active) Throw("INPUT_INVALID","未开始的CoreRow结束",location(path,line));
            active = false; ++actual; continue;
        }
        if (!active || words.size()<3 || words[1]!=":") Throw("INPUT_INVALID","SCL字段错误",location(path,line));
        if (words[0]=="Coordinate") row.y = number(words[2],location(path,line));
        else if (words[0]=="Height") row.height = number(words[2],location(path,line));
        else if (words[0]=="Sitewidth") row.siteWidth = number(words[2],location(path,line));
        else if (words[0]=="Sitespacing") row.sitePitch = number(words[2],location(path,line));
        else if (words[0]=="Siteorient") {
            if (words[2]=="1") { row.orientation=Orientation::N; db.auditWarnings.push_back("SCL Siteorient=1 按课程兼容规则映射 N："+location(path,line)); }
            else row.orientation=parseOrientation(words[2]);
        } else if (words[0]=="Sitesymmetry") {
            row.allowedOrientations = {row.orientation};
            // BookShelf 对称性定义可接受的行朝向，算法仍保留实例输入朝向。
            for (std::size_t field=2;field<words.size();++field) { // 当前对称性字段索引。
                if (words[field]=="Y") row.allowedOrientations.push_back(Orientation::FN);
                else if (words[field]=="X") row.allowedOrientations.push_back(Orientation::FS);
                else if (words[field]!="1") Throw("UNSUPPORTED","不支持的站点对称性",location(path,line));
            }
        } else if (words[0]=="SubrowOrigin") {
            if(words.size()!=6 || words[3]!="NumSites" || words[4]!=":") Throw("INPUT_INVALID","Subrow格式错误",location(path,line));
            RowSegment segment = row; // 继承本CoreRow字段的独立子行。
            segment.id=db.rows.size(); segment.xOrigin=number(words[2],location(path,line)); segment.numSites=count(words[5],location(path,line));
            db.rows.push_back(segment);
        } else Throw("UNSUPPORTED","未知SCL字段",location(path,line));
    }
    if (active || actual != expected) Throw("INPUT_INVALID","SCL行数不符或未结束",path.string());
}
/** @brief 流式加载大网络文件，不保留原始行副本。 @param path nets。 @param db 输出数据库。 @param index 节点索引。 @param control 取消令牌。 无返回值。 */
void readNets(const std::filesystem::path& path, DesignDatabase& db, const std::unordered_map<std::string,NodeId>& index, ControlToken& control) {
    std::ifstream stream(path); // 大型网络输入流。
    if(!stream) Throw("REFERENCE_MISSING","无法读取nets",path.string());
    std::uint64_t expectedNets=std::numeric_limits<std::uint64_t>::max(); // 声明网络数。
    std::uint64_t expectedPins=std::numeric_limits<std::uint64_t>::max(); // 声明引脚数。
    std::uint64_t remaining=0; // 当前网络待消费引脚数。
    std::string text; // 当前源行。
    std::size_t lineNumber=0; // 源行号。
    while(std::getline(stream,text)) {
        ++lineNumber; if(lineNumber%4096==0) control.checkCancelled();
        text=text.substr(0,text.find('#'));
        std::istringstream parser(text); // 当前网络行解析器。
        std::string first; // 行首名称或头字段。
        if(!(parser>>first) || first=="UCLA") continue;
        const std::string context=path.string()+":"+std::to_string(lineNumber); // 错误定位。
        if(first=="NumNets" || first=="NumPins" || first=="NetDegree") {
            if(remaining) Throw("INPUT_INVALID","网络引脚数量不足",context);
            std::string colon; // 冒号分隔符。
            std::string quantity; // 声明数量字段。
            if(!(parser>>colon>>quantity) || colon!=":") Throw("INPUT_INVALID","网络计数格式错误",context);
            const std::uint64_t amount=count(quantity,context); // 非负声明数量。
            if(first=="NumNets") expectedNets=amount;
            else if(first=="NumPins") expectedPins=amount;
            else {
                Net net; // 当前超边。
                net.id=db.nets.size(); if(!(parser>>net.name)) net.name="net_"+std::to_string(net.id);
                db.nets.push_back(std::move(net)); remaining=amount;
            }
            continue;
        }
        if(!remaining || db.nets.empty()) Throw("INPUT_INVALID","引脚不属于任何网络",context);
        const auto found=index.find(first); // 所属实例稳定ID。
        if(found==index.end()) Throw("REFERENCE_MISSING","网络引用未知节点",first+" "+context);
        std::string direction; // 输入引脚方向，仅审计、不影响线长。
        std::string colon; // 偏移分隔符。
        std::string x; // 中心相对x偏移，DU。
        std::string y; // 中心相对y偏移，DU。
        if(!(parser>>direction>>colon>>x>>y) || colon!=":") Throw("INPUT_INVALID","引脚字段格式错误",context);
        Pin pin; // 规范化引脚记录。
        pin.id=db.pins.size(); pin.nodeId=found->second; pin.netId=db.nets.back().id; pin.offset={number(x,context),number(y,context)};
        db.nets.back().pinIds.push_back(pin.id); db.pins.push_back(pin); --remaining;
    }
    if(!stream.eof()) Throw("IO_FAILURE","nets 读取失败",path.string());
    if(remaining || db.nets.size()!=expectedNets || db.pins.size()!=expectedPins) Throw("INPUT_INVALID","nets/pins 声明数量不一致",path.string());
}
/** @brief 加载可为空的网络权重。 @param path wts。 @param db 数据库。 @param control 取消令牌。 无返回值。 */
void readWeights(const std::filesystem::path& path,DesignDatabase& db,ControlToken& control) {
    std::unordered_map<std::string,NetId> index; // 网络名称到稳定ID。
    for(const Net& net:db.nets) index.emplace(net.name,net.id); // 当前网络。
    std::vector<bool> seen(db.nets.size(),false); // 重复权重检测。
    for(const TextLine& line:readLines(path,control)) { // 当前非空权重行。
        const auto& words=line.words; // 本行字段。
        if(words[0]=="UCLA") continue;
        if(words.size()!=2) Throw("INPUT_INVALID","权重字段数错误",location(path,line));
        const auto found=index.find(words[0]); // 被赋权网络。
        if(found==index.end()) Throw("REFERENCE_MISSING","权重引用未知网络",words[0]);
        if(seen[found->second]) Throw("INPUT_INVALID","重复网络权重",words[0]);
        seen[found->second]=true;
        const double weight=number(words[1],location(path,line)); // 非负线长权重。
        if(weight<0) Throw("INPUT_INVALID","网络权重不能为负",words[0]);
        db.nets[found->second].weight=weight;
    }
}
} // namespace

/** @brief BookShelf 完整加载，输入输出和失败契约见 design.hpp。 @param path AUX。 @param config 单位。 @param control 取消。 @return 独占数据库。 */
DesignDatabase LoadBookshelf(const std::string& path,const Config& config,ControlToken& control) {
    const std::filesystem::path aux=std::filesystem::absolute(path); // AUX的绝对路径。
    const auto lines=readLines(aux,control); // AUX仅应有一个主关联记录。
    std::unordered_map<std::string,std::filesystem::path> files; // 后缀到关联路径映射。
    for(const TextLine& line:lines) { // 当前AUX行。
        for(const std::string& word:line.words) { // 当前关联文件字段。
            const std::filesystem::path candidate(word); // 未规范化关联路径。
            const std::string extension=candidate.extension().string(); // BookShelf文件角色。
            if(extension==".nodes" || extension==".nets" || extension==".pl" || extension==".scl" || extension==".wts") {
                if(!files.emplace(extension,aux.parent_path()/candidate).second) Throw("INPUT_INVALID","AUX重复文件角色",extension);
            }
        }
    }
    for(const std::string extension:{".nodes",".nets",".pl",".scl",".wts"}) // 必须出现的关联角色。
        if(!files.count(extension)) Throw("REFERENCE_MISSING","AUX缺少关联文件",extension);
    DesignDatabase db; // 独占构建的输入数据。
    if(config.micronsPerDu) {db.micronsPerDu=*config.micronsPerDu; db.physicalScaleKnown=true;}
    db.inputFiles.push_back(AuditFile(aux.string(),control));
    for(const std::string extension:{".nodes",".nets",".pl",".scl",".wts"}) // 固定审计顺序，避免unordered_map破坏可重现性。
        db.inputFiles.push_back(AuditFile(files.at(extension).string(),control));
    std::unordered_map<std::string,NodeId> index; // 解析时名称到节点ID的唯一映射。
    readNodes(files.at(".nodes"),db,index,control);
    readPositions(files.at(".pl"),db,index,control);
    readRows(files.at(".scl"),db,control);
    readNets(files.at(".nets"),db,index,control);
    readWeights(files.at(".wts"),db,control);
    const double tolerance=geometryTolerance(db); // 只计算一次core尺度容差，避免按对象重复遍历行。
    std::set<double> rowHeights; // 去重行高，DU。
    for(const RowSegment& row:db.rows) rowHeights.insert(row.height); // 当前输入行高。
    for(Node& node:db.nodes) { // 以方向后高度分类的真实非端口实例。
        if(node.kind==NodeKind::Terminal) continue;
        const Rect box=nodeRect(node,{0,0,node.orientation}); // 尺寸分类独立于可能缺失的原始坐标。
        bool matches=false; // 是否匹配某个可放置行高。
        for(const double height:rowHeights) { // 去重候选行高。
            if(std::abs(box.height()-height)<=tolerance) {matches=true;break;}
        }
        node.kind=matches?NodeKind::Standard:NodeKind::Macro;
        db.auditWarnings.push_back("BookShelf几何分类："+node.name+" -> "+(matches?"standard":"macro"));
    }
    return db;
}
} // namespace placement
