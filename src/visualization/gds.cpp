#include <placement/io/visualization.hpp>
#include <limbo/parsers/gdsii/stream/GdsWriter.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <atomic>
#include <unistd.h>

namespace placement {
namespace {
/** @brief 预量化GDS元素；所有整数坐标已进行int32范围验证。 */
struct Element {
    int layer; ///< GDS互斥实体或辅助层编号。
    bool path; ///< true输出PATH边框，false输出BOUNDARY实体。
    std::vector<int> x; ///< 闭合五点或端口十字的DBU横坐标。
    std::vector<int> y; ///< 与x对应的DBU纵坐标。
};
/** @brief 量化一个物理坐标，正负中点均远离零。 @param coordinate μm坐标。 @param dbu μm/DBU。
 * @return int32范围DBU；超范围抛INPUT_INVALID。 */
int quantize(double coordinate,double dbu) {
    const double scaled=coordinate/dbu; // 浮点DBU值。
    if(!std::isfinite(scaled)||scaled<std::numeric_limits<std::int32_t>::min()||scaled>std::numeric_limits<std::int32_t>::max()) Throw("INPUT_INVALID","GDS坐标超int32范围，请增大gds_dbu_um");
    const auto value=std::llround(scaled); // 最近整数，标准库中点远离零。
    if(value<std::numeric_limits<std::int32_t>::min()||value>std::numeric_limits<std::int32_t>::max()) Throw("INPUT_INVALID","GDS量化坐标溢出");
    return static_cast<int>(value);
}
/** @brief 写出后验证记录边界和结束标记，检测上游流写失败。
 * @param path 临时GDS。 @param expectedElements 预期完整元素数。 无返回值；截断/损坏抛IO_FAILURE。 */
void verifyStream(const std::string& path,std::size_t expectedElements) {
    std::ifstream stream(path,std::ios::binary); // 独立顺序读回文件记录。
    std::size_t elements=0; // ENDEL记录计数。
    bool ended=false; // 是否已到ENDLIB。
    while(stream.peek()!=EOF) {
        std::array<unsigned char,4> header{}; // GDS记录长度/类型头。
        stream.read(reinterpret_cast<char*>(header.data()),header.size());
        const int length=(header[0]<<8)|header[1]; // 大端记录总字节数。
        if(!stream||length<4||length%2||ended) Throw("IO_FAILURE","GDS记录截断或长度非法",path);
        if(header[2]==0x11) ++elements;
        if(header[2]==0x04) ended=true;
        stream.ignore(length-4);if(!stream) Throw("IO_FAILURE","GDS记录内容截断",path);
    }
    if(!ended||elements!=expectedElements) Throw("IO_FAILURE","GDS结束标记或元素数不符",path);
}
} // namespace
/** @brief GDSII导出与伴随量化报告。 @param snapshot 源。 @param path 输出。 @param config DBU配置。 @param control 取消。 @param overwrite 覆盖。 无返回值。 */
void ExportGds(const Snapshot& snapshot,const std::string& path,const Config& config,ControlToken& control,bool overwrite) {
    validatePositions(*snapshot.database,snapshot.positions);
    if(!std::isfinite(config.gdsDbuUm)||config.gdsDbuUm<=0) Throw("INPUT_INVALID","GDS DBU必须为正");
    const double scale=config.micronsPerDu.value_or(snapshot.database->micronsPerDu); // 本次明确物理/显示比例，μm/DU。
    const std::string metadataPath=path+".metadata.json"; // 与GDS绑定的几何/单位元数据。
    if(!overwrite&&std::filesystem::exists(metadataPath)) Throw("OUTPUT_EXISTS","GDS元数据已存在",metadataPath);
    std::vector<Element> elements; // 预检通过后才能交给第三方写出器。
    std::map<int,std::uint64_t> counts; // 各层元素数量。
    double maximumError=0; // 全部坐标量化最大绝对误差，μm。
    /** @brief 预检并累加一个矩形元素。 @param rect DU范围。 @param layer 图层。 @param outline 是否PATH。 无返回值。 */
    const auto add=[&](Rect rect,int layer,bool outline) {
        if(rect.width()<=0||rect.height()<=0) return;
        const int lx=quantize(rect.lx*scale,config.gdsDbuUm); // 左边DBU。
        const int ly=quantize(rect.ly*scale,config.gdsDbuUm); // 下边DBU。
        const int ux=quantize(rect.ux*scale,config.gdsDbuUm); // 右边DBU。
        const int uy=quantize(rect.uy*scale,config.gdsDbuUm); // 上边DBU。
        if(lx==ux||ly==uy) Throw("INPUT_INVALID","GDS精度不足，非零实体量化后坍缩");
        maximumError=std::max({maximumError,std::abs(lx*config.gdsDbuUm-rect.lx*scale),std::abs(ly*config.gdsDbuUm-rect.ly*scale),std::abs(ux*config.gdsDbuUm-rect.ux*scale),std::abs(uy*config.gdsDbuUm-rect.uy*scale)});
        elements.push_back({layer,outline,{lx,ux,ux,lx,lx},{ly,ly,uy,uy,ly}});++counts[layer];
    };
    for(const Node& node:snapshot.database->nodes) { // 原始实体，不输出filler。
        if(node.id%4096==0) control.checkCancelled();
        const Rect box=nodeRect(node,snapshot.positions[node.id]); // 同一源快照几何。
        const int layer=node.fixed?3:(node.kind==NodeKind::Macro?2:1); // 互斥实体分类。
        if(box.area()>0) add(box,layer,false);
        else {
            const int x=quantize(box.lx*scale,config.gdsDbuUm); // 零面积端口锚点DBU。
            const int y=quantize(box.ly*scale,config.gdsDbuUm); // 零面积端口锚点DBU。
            if(x==std::numeric_limits<int>::min()||x==std::numeric_limits<int>::max()||y==std::numeric_limits<int>::min()||y==std::numeric_limits<int>::max()) Throw("INPUT_INVALID","端口标记超DBU范围");
            elements.push_back({3,true,{x-1,x+1},{y,y}});elements.push_back({3,true,{x,x},{y-1,y+1}});counts[3]+=2;
        }
    }
    add(coreBounds(*snapshot.database),10,true);add(chipBounds(*snapshot.database,snapshot.positions),11,true);
    for(const Obstacle& obstacle:snapshot.database->obstacles) add(obstacle.rect,3,false); // 硬阻挡与固定实体共用图层3。
    for(const RowSegment& row:snapshot.database->rows) add(rowRect(row),10,true); // 保留可用行边界。
    const ViolationReport original=CheckLegality(snapshot,"all",10000,&control); // 量化前独立结论。
    for(const Violation& violation:original.records) add(violation.region,20,true); // 违规辅助层不改变原始合法性。
    // GDS不表达站点栅格。输出几何重检将标准单元视为普通矩形，行仅保留其输出区域。
    auto quantizedDatabase=std::make_shared<DesignDatabase>(*snapshot.database); // 量化重检专属副本，不发布为原结果。
    auto originalGeometryDatabase=std::make_shared<DesignDatabase>(*snapshot.database); // 量化前相同几何检查口径。
    Snapshot quantized=snapshot; // 保留身份，只在内部使用。
    quantized.database=quantizedDatabase;
    Snapshot originalGeometry=snapshot; // 排除站点约束后可比较的原始几何。
    originalGeometry.database=originalGeometryDatabase;
    /** @brief 将DU坐标量化后恢复DU。 @param value 原坐标。 @return 输出几何坐标。 */
    const auto quantizedCoordinate=[scale,&config](double value) {return quantize(value*scale,config.gdsDbuUm)*config.gdsDbuUm/scale;};
    for(Node& node:quantizedDatabase->nodes) { // 当前量化实例。
        const Rect box=nodeRect(node,snapshot.positions[node.id]); // 原始方向后物理框。
        const double lx=quantize(box.lx*scale,config.gdsDbuUm)*config.gdsDbuUm/scale; // 回到DU的量化左边。
        const double ly=quantize(box.ly*scale,config.gdsDbuUm)*config.gdsDbuUm/scale; // 量化下边。
        const double width=(quantize(box.ux*scale,config.gdsDbuUm)*config.gdsDbuUm/scale)-lx; // 量化方向后宽DU。
        const double height=(quantize(box.uy*scale,config.gdsDbuUm)*config.gdsDbuUm/scale)-ly; // 量化方向后高DU。
        const bool rotated=node.orientation==Orientation::E||node.orientation==Orientation::W||node.orientation==Orientation::FE||node.orientation==Orientation::FW; // 是否交换master尺寸。
        node.width=rotated?height:width;node.height=rotated?width:height;
        quantized.positions[node.id].x=lx;quantized.positions[node.id].y=ly;quantizedDatabase->originalPositions[node.id]=quantized.positions[node.id];
        if(node.kind==NodeKind::Standard) {node.kind=NodeKind::Macro;originalGeometryDatabase->nodes[node.id].kind=NodeKind::Macro;}
    }
    for(RowSegment& row:quantizedDatabase->rows) { // 量化后的可用行区域，以单个大站点表示其外框。
        const Rect box=rowRect(row); // 量化前整行bbox。
        row.xOrigin=quantizedCoordinate(box.lx);row.y=quantizedCoordinate(box.ly);
        row.siteWidth=quantizedCoordinate(box.ux)-row.xOrigin;row.height=quantizedCoordinate(box.uy)-row.y;
        row.sitePitch=row.siteWidth;row.numSites=1;
    }
    for(Obstacle& obstacle:quantizedDatabase->obstacles) { // 同样量化硬障碍，禁止混用原坐标。
        obstacle.rect={quantizedCoordinate(obstacle.rect.lx),quantizedCoordinate(obstacle.rect.ly),quantizedCoordinate(obstacle.rect.ux),quantizedCoordinate(obstacle.rect.uy)};
    }
    const ViolationReport originalGeometryReport=CheckLegality(originalGeometry,"all",10000,&control); // 原几何检查口径。
    const ViolationReport quantizedReport=CheckLegality(quantized,"all",10000,&control); // 实际量化几何，不包含不再表达的站点约束。
    /** @brief 为违规生成稳定对象身份；量化后的坐标变化不构成新违规ID。 @param violation 记录。 @return 类型/对象/来源键。 */
    const auto violationKey=[](const Violation& violation) {
        return violation.kind+":"+nlohmann::json(violation.objectIds).dump()+":"+violation.message;
    };
    std::set<std::string> previous; // 量化前可完整比较的违规身份。
    for(const Violation& violation:originalGeometryReport.records) previous.insert(violationKey(violation)); // 原始记录。
    std::uint64_t newlyIntroduced=0; // 在完整记录下可判定的新违规数。
    for(const Violation& violation:quantizedReport.records) if(!previous.count(violationKey(violation))) ++newlyIntroduced; // 量化后新身份。
    nlohmann::json layers=nlohmann::json::object(); // 图层数量JSON。
    for(const auto& item:counts) layers[std::to_string(item.first)]=item.second; // item.first层号，second数量。
    const bool comparable=!originalGeometryReport.truncated&&!quantizedReport.truncated; // 明细截断时不捏造完整集合差。
    const std::string metadata=nlohmann::json{{"schemaVersion",1},{"datasetId",snapshot.datasetId},{"runId",snapshot.runId},{"stage",snapshot.stage},{"revision",snapshot.revision},
        {"physical_scale_known",snapshot.database->physicalScaleKnown||config.micronsPerDu.has_value()},{"microns_per_du",scale},{"dbu_um",config.gdsDbuUm},
        {"maximum_quantization_error_um",maximumError},{"layers",layers},{"original_violation_count",original.totalCount},
        {"original_geometry_violation_count",originalGeometryReport.totalCount},{"quantized_geometry_violation_count",quantizedReport.totalCount},
        {"new_violations_after_quantization",comparable?nlohmann::json(newlyIntroduced>0):nlohmann::json(nullptr)},
        {"quantization_check_scope","output rectangle geometry; site constraints evaluated on original snapshot"}}.dump(2); // 成组发布的伴随元数据。
    static std::atomic<std::uint64_t> transaction{0}; // 同进程回滚备份的唯一序号。
    const std::string backup=metadataPath+".rollback-"+std::to_string(::getpid())+"-"+std::to_string(transaction.fetch_add(1)); // 仅本次拥有的原元数据备份。
    const bool hadMetadata=std::filesystem::exists(metadataPath); // 是否需要保留旧成功元数据。
    bool metadataCommitted=false; // GDS最终原子提交失败时据此恢复元数据。
    if(hadMetadata) {
        std::error_code error; // 原元数据复制错误。
        if(!std::filesystem::copy_file(metadataPath,backup,std::filesystem::copy_options::none,error)) Throw("IO_FAILURE","无法备份原GDS元数据："+error.message(),metadataPath);
    }
    try {
      /** @brief 完成GDS临时文件和元数据后，最后发布GDS；失败可恢复旧元数据。 @param temporary 临时GDS。 无返回值。 */
      AtomicWrite(path,[&](const std::string& temporary) {
        {
            GdsParser::GdsWriter writer(temporary.c_str()); // Limbo持有二进制流，析构刷新关闭。
            writer.create_lib("PLACEMENT",config.gdsDbuUm,config.gdsDbuUm*1e-6);
            writer.gds_write_bgnstr();writer.gds_write_strname(("PLACEMENT_"+snapshot.stage).c_str());
            std::size_t index=0; // 写出元素取消批次计数。
            for(const Element& element:elements) { // 当前已经验证的GDS几何。
                if(index++%4096==0) control.checkCancelled();
                if(element.path) {
                    writer.gds_write_path();writer.gds_write_layer(static_cast<short>(element.layer));writer.gds_write_datatype(0);writer.gds_write_width(1);
                    writer.gds_write_xy(element.x.data(),element.y.data(),static_cast<int>(element.x.size()),true);writer.gds_write_endel();
                }else writer.write_boundary(element.layer,0,element.x,element.y,true);
            }
            writer.gds_write_endstr();writer.gds_write_endlib();
        }
        verifyStream(temporary,elements.size());
        WriteTextAtomic(metadataPath,metadata,control,overwrite);metadataCommitted=true;
      },control,overwrite);
    } catch(...) {
        std::error_code restoration; // 恢复使用rename，不依赖额外磁盘空间。
        if(metadataCommitted) {
            if(hadMetadata) std::filesystem::rename(backup,metadataPath,restoration);
            else std::filesystem::remove(metadataPath,restoration);
        }
        std::error_code cleanup; // 清理仅本次创建的备份。
        std::filesystem::remove(backup,cleanup);
        if(restoration) Throw("IO_FAILURE","GDS事务回滚元数据失败："+restoration.message(),metadataPath);
        throw;
    }
    std::error_code cleanup; // 成功发布两份文件后释放回滚备份。
    std::filesystem::remove(backup,cleanup);
}
} // namespace placement
