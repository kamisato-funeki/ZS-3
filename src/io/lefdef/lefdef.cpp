#include <placement/io/design.hpp>
#include <limbo/thirdparty/lefdef/5.8/lef/lef/lefrReader.hpp>
#include <limbo/thirdparty/lefdef/5.8/def/def/defrReader.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace placement {
namespace {
/** @brief 隔离 Limbo 所附非重入 LEF/DEF 解析器；每次加载独占会话。 */
std::mutex parserMutex;
/** @brief LEF master 临时模型；所有几何仍为 μm，以 LEF 放置原点为参考。 */
struct Master {
    double width=0; ///< N朝向宽度，μm。
    double height=0; ///< N朝向高度，μm。
    Vec2 origin; ///< LEF ORIGIN：几何归一化到SIZE框前施加的平移，μm。
    std::string category; ///< LEF CLASS。
    std::vector<Orientation> orientations; ///< 由SYMMETRY生成的允许方向群。
    std::map<std::string,Vec2> pins; ///< 引脚几何bbox中心，相对LEF原点，μm。
};
/** @brief SITE临时信息，所有权归解析事务。 */
struct Site {
    double width=0; ///< 站点宽，μm。
    double height=0; ///< 站点高，μm。
    std::vector<Orientation> orientations; ///< SYMMETRY群。
};
/** @brief 延迟网络连接，避免依赖DEF各节的合法排列顺序。 */
struct Connection {
    std::string instance; ///< 实例名或顶层端口标志PIN。
    std::string pin; ///< 引脚/端口名称。
};
/** @brief 解析回调事务；异常保存后返回失败码，禁止穿过解析器回调边界。 */
struct ParseContext {
    DesignDatabase database; ///< 尚未发布的数据库。
    std::map<std::string,Master> masters; ///< 多LEF合并的master定义。
    std::map<std::string,Site> sites; ///< SITE定义。
    std::string currentMaster; ///< 当前LEF宏名。
    std::map<std::string,NodeId> instances; ///< 实例到稳定ID。
    std::map<std::string,NodeId> ports; ///< 顶层端口到Terminal ID，独立命名空间。
    std::vector<std::vector<Connection>> connections; ///< 按NetId待解析的连接。
    double dbuPerMicron=0; ///< DEF每μm的整数DBU数，必须在坐标消费前定义。
    std::uint64_t expectedComponents=0; ///< DEF声明组件数。
    std::uint64_t actualComponents=0; ///< 实际组件回调数。
    std::uint64_t expectedNets=0; ///< DEF声明网络数。
    std::uint64_t expectedPorts=0; ///< DEF声明端口数。
    ControlToken* control=nullptr; ///< 外部令牌，不拥有，加载期间有效。
    std::exception_ptr failure; ///< 首个业务错误，回调返回后重新抛出。
    std::string diagnostic; ///< 上游解析诊断摘要。
};
/** @brief 当前线程读取回调上下文；受parserMutex保护生命周期，不跨线程共享。 */
thread_local ParseContext* activeContext=nullptr;
/** @brief FILE的RAII关闭器，避免将带编译器属性的函数指针作为模板参数。 */
struct FileCloser {
    /** @brief 关闭借用进来的已拥有文件。 @param file 可空FILE。 无返回值。 */
    void operator()(FILE* file) const noexcept {if(file) std::fclose(file);}
};

/** @brief 在C风格回调边界捕获错误。 @tparam Action 无参数操作类型。
 * @param data 解析事务。 @param action 不保留捕获的同步操作。 @return 0成功，1错误；失败保存首异常。 */
template<class Action> int guarded(void* data,Action action) {
    auto& context=*static_cast<ParseContext*>(data); // 当前独占事务。
    try {context.control->checkCancelled(); action(context); return 0;}
    catch(...) {if(!context.failure) context.failure=std::current_exception(); return 1;}
}
/** @brief 可取消的底层读取，不向解析器抛异常。 @param file 输入流。 @param buffer 解析器缓冲。 @param size 请求字节。 @return 已读字节，取消时EOF由外层转为CANCELLED。 */
std::size_t readCancelable(FILE* file,char* buffer,std::size_t size) {
    if(activeContext && activeContext->control->isCancelled()) return 0;
    return std::fread(buffer,1,size,file);
}
/** @brief 捕获解析器诊断而不创建工作目录日志。 @param text 上游消息。 无返回值，更新当前事务摘要。 */
void parserLog(const char* text) {
    if(activeContext && activeContext->diagnostic.size()<8192) activeContext->diagnostic+=text;
}
/** @brief 获取LEF对称性生成的方向群。 @param x X镜像。 @param y Y镜像。 @param rotate R90。 @return 允许朝向集合。 */
std::vector<Orientation> symmetry(bool x,bool y,bool rotate) {
    if(rotate && (x||y)) return {Orientation::N,Orientation::S,Orientation::W,Orientation::E,Orientation::FN,Orientation::FS,Orientation::FW,Orientation::FE};
    if(rotate) return {Orientation::N,Orientation::S,Orientation::W,Orientation::E};
    if(x&&y) return {Orientation::N,Orientation::S,Orientation::FN,Orientation::FS};
    if(x) return {Orientation::N,Orientation::FS};
    if(y) return {Orientation::N,Orientation::FN};
    return {Orientation::N};
}
/** @brief 合成正交方向。 @param outer 外部方向。 @param inner 内部方向。 @return outer(inner(v))对应枚举。 */
Orientation compose(Orientation outer,Orientation inner) {
    const Vec2 target=transformOffset(transformOffset({1,2},inner),outer); // 非对称基准向量唯一识别八方向。
    for(const Orientation value:{Orientation::N,Orientation::S,Orientation::W,Orientation::E,Orientation::FN,Orientation::FS,Orientation::FW,Orientation::FE}) { // 候选方向。
        const Vec2 candidate=transformOffset({1,2},value); // 候选变换。
        if(candidate.x==target.x && candidate.y==target.y) return value;
    }
    Throw("INPUT_INVALID","无效方向合成");
}
/** @brief 将DEF枚举转换为领域方向。 @param code 上游方向编号。 @return 八方向；非法抛INPUT_INVALID。 */
Orientation defOrientation(int code) {
    const std::array<Orientation,8> values={Orientation::N,Orientation::W,Orientation::S,Orientation::E,Orientation::FN,Orientation::FW,Orientation::FS,Orientation::FE}; // DEF官方方向编号顺序。
    if(code<0 || code>=8) Throw("INPUT_INVALID","未知DEF方向");
    return values[static_cast<std::size_t>(code)];
}
/** @brief 扩展bbox；首次点单独初始化。 @param box 当前框。 @param initialized 是否已有点。 @param point 新点，μm。 无返回值，更新输出框。 */
void extend(Rect& box,bool& initialized,Vec2 point) {
    if(!initialized) {box={point.x,point.y,point.x,point.y};initialized=true;return;}
    box.lx=std::min(box.lx,point.x);box.ly=std::min(box.ly,point.y);
    box.ux=std::max(box.ux,point.x);box.uy=std::max(box.uy,point.y);
}
/** @brief 变换SIZE四角后，将其bbox左下角对齐DEF placement点。
 * @param master LEF物理几何。 @param anchor DEF放置原点，μm。 @param orientation 方向。 @return 归一化左下角位置。 */
Position anchoredPosition(const Master& master,Vec2 anchor,Orientation orientation) {
    Rect box; // 变换后的相对原点bbox。
    bool initialized=false; // 是否已消费首角点。
    // LEF/DEF 5.8 SIZE规定：SIZE框归一化为[0,w]×[0,h]，DEF placement点对齐旋转后bbox左下角。
    // ORIGIN只在PIN几何归一化时加一次，不移动归一化SIZE框；见文档04格式勘误。
    for(const double x:{0.0,master.width}) { // 归一化SIZE左/右边界，μm。
        for(const double y:{0.0,master.height}) // 归一化SIZE下/上边界，μm。
            extend(box,initialized,transformOffset({x,y},orientation));
    }
    const Vec2 translation={anchor.x-box.lx,anchor.y-box.ly}; // 将方向后框对齐DEF给定左下角的平移。
    return {translation.x+box.lx,translation.y+box.ly,orientation};
}
/** @brief SITE回调。 @param type 未消费的上游类型。 @param site 上游SITE临时对象。 @param data 事务。 @return 回调状态，复制后不保留上游指针。 */
int siteCallback(LefDefParser::lefrCallbackType_e,LefDefParser::lefiSite* site,void* data) {
    // 复制尺寸与对称性，输入context是本次事务；无返回值，可能抛输入错误给guard。
    return guarded(data,[site](ParseContext& context) {
        if(!context.sites.emplace(site->name(),Site{site->sizeX(),site->sizeY(),symmetry(site->hasXSymmetry(),site->hasYSymmetry(),site->has90Symmetry())}).second)
            Throw("INPUT_INVALID","重复LEF SITE",site->name());
    });
}
/** @brief 宏开始回调。 @param type 上游类型。 @param name 宏名。 @param data 事务。 @return 状态。 */
int macroBeginCallback(LefDefParser::lefrCallbackType_e,const char* name,void* data) {
    // 设置当前master；捕获name仅在同步回调期间有效，context为独占事务。
    return guarded(data,[name](ParseContext& context) {
        context.currentMaster=name;
        if(!context.masters.emplace(name,Master{}).second) Throw("INPUT_INVALID","重复LEF MACRO",name);
    });
}
/** @brief LEF引脚几何bbox回调。 @param type 上游类型。 @param pin 临时引脚。 @param data 事务。 @return 状态。 */
int lefPinCallback(LefDefParser::lefrCallbackType_e,LefDefParser::lefiPin* pin,void* data) {
    // 复制所有有效PORT的RECT/POLYGON几何，context输出当前master引脚中心。
    return guarded(data,[pin](ParseContext& context) {
        Rect box; // 引脚全部有效几何的bbox，μm。
        bool initialized=false; // 是否已有有效点。
        for(int port=0;port<pin->numPorts();++port) { // 当前PORT编号。
            const auto* geometry=pin->port(port); // 上游借用几何，当前回调后失效。
            for(int item=0;item<geometry->numItems();++item) { // PORT几何项编号。
                if(geometry->itemType(item)==LefDefParser::lefiGeomRectE) {
                    const auto* rect=geometry->getRect(item); // 输入矩形。
                    extend(box,initialized,{rect->xl,rect->yl});extend(box,initialized,{rect->xh,rect->yh});
                } else if(geometry->itemType(item)==LefDefParser::lefiGeomPolygonE) {
                    const auto* polygon=geometry->getPolygon(item); // 输入多边形，线长仅使用其bbox。
                    for(int vertex=0;vertex<polygon->numPoints;++vertex) // 当前多边形顶点。
                        extend(box,initialized,{polygon->x[vertex],polygon->y[vertex]});
                }
            }
        }
        if(!initialized) Throw("UNSUPPORTED","引脚缺少可用RECT/POLYGON几何",context.currentMaster+"/"+pin->name());
        if(!context.masters.at(context.currentMaster).pins.emplace(pin->name(),Vec2{(box.lx+box.ux)/2,(box.ly+box.uy)/2}).second)
            Throw("INPUT_INVALID","重复LEF引脚",pin->name());
    });
}
/** @brief 完成LEF宏属性复制。 @param type 上游类型。 @param macro 临时宏。 @param data 事务。 @return 状态。 */
int macroCallback(LefDefParser::lefrCallbackType_e,LefDefParser::lefiMacro* macro,void* data) {
    // 仅更新当前事务master元数据，context持有独立副本。
    return guarded(data,[macro](ParseContext& context) {
        Master& master=context.masters.at(context.currentMaster); // 当前master的独占可变副本。
        master.width=macro->sizeX();master.height=macro->sizeY();
        master.origin={macro->originX(),macro->originY()};master.category=macro->macroClass();
        master.orientations=symmetry(macro->hasXSymmetry(),macro->hasYSymmetry(),macro->has90Symmetry());
    });
}
/** @brief DEF单位回调。 @param type 上游类型。 @param units DBU/μm。 @param data 事务。 @return 状态。 */
int unitsCallback(LefDefParser::defrCallbackType_e,double units,void* data) {
    // 校验并设置context中的长度转换比例。
    return guarded(data,[units](ParseContext& context) {if(!std::isfinite(units)||units<=0) Throw("INPUT_INVALID","DEF单位无效");context.dbuPerMicron=units;});
}
/** @brief DEF水平ROW映射。 @param type 上游类型。 @param source 临时ROW。 @param data 事务。 @return 状态。 */
int rowCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiRow* source,void* data) {
    // 将source DBU转换μm，context新增每个重复横向行。
    return guarded(data,[source](ParseContext& context) {
        const auto found=context.sites.find(source->macro()); // 配套站点定义。
        if(found==context.sites.end()) Throw("REFERENCE_MISSING","DEF ROW缺少SITE",source->macro());
        if(context.dbuPerMicron<=0) Throw("INPUT_INVALID","DEF必须先声明UNITS");
        const Site& site=found->second; // 不持上游指针的站点元数据。
        const Orientation orientation=defOrientation(source->orient()); // 行朝向。
        if(orientation==Orientation::W||orientation==Orientation::E||orientation==Orientation::FW||orientation==Orientation::FE) Throw("UNSUPPORTED","仅支持水平站点行",source->name());
        const int copies=source->hasDo()?static_cast<int>(source->yNum()):1; // 沿y重复的横行数量。
        for(int copy=0;copy<copies;++copy) { // 当前重复行。
            RowSegment row; // 归一化水平行段。
            row.id=context.database.rows.size();row.y=(source->y()+copy*source->yStep())/context.dbuPerMicron;
            row.height=site.height;row.xOrigin=source->x()/context.dbuPerMicron;row.siteWidth=site.width;
            row.sitePitch=source->hasDoStep()?source->xStep()/context.dbuPerMicron:site.width;
            row.numSites=source->hasDo()?static_cast<std::uint64_t>(source->xNum()):1;row.orientation=orientation;
            for(const Orientation value:site.orientations) row.allowedOrientations.push_back(compose(orientation,value)); // 当前站点对称方向。
            context.database.rows.push_back(row);
        }
    });
}
/** @brief DEF实例映射。 @param type 上游类型。 @param source 临时实例。 @param data 事务。 @return 状态。 */
int componentCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiComponent* source,void* data) {
    // 从source复制实例到context，锚点经四角变换而不是直接当左下角。
    return guarded(data,[source](ParseContext& context) {
        const auto found=context.masters.find(source->name()); // defiComponent::name是master，id是实例名。
        if(found==context.masters.end()) Throw("REFERENCE_MISSING","DEF实例缺少LEF master",source->name());
        if(source->hasRegionName()||source->hasRegionBounds()) Throw("UNSUPPORTED","不支持实例REGION约束",source->id());
        const Master& master=found->second; // 配套master元数据。
        Node node; // 统一实例记录。
        node.id=context.database.nodes.size();node.name=source->id();node.masterName=source->name();
        node.width=master.width;node.height=master.height;node.fixed=source->isFixed()||source->isCover();
        node.orientation=source->isUnplaced()?Orientation::N:defOrientation(source->placementOrient());node.allowedOrientations=master.orientations;
        node.kind=master.category.find("BLOCK")==0?NodeKind::Macro:NodeKind::Standard;
        if(master.category.find("CORE")!=0 && node.kind!=NodeKind::Macro) Throw("UNSUPPORTED","不支持的LEF MACRO CLASS",master.category);
        if(!context.instances.emplace(node.name,node.id).second) Throw("INPUT_INVALID","重复DEF实例名",node.name);
        Position position; // 输入归一化位置，μm。
        if(source->isUnplaced()) position={std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),node.orientation};
        else position=anchoredPosition(master,{source->placementX()/context.dbuPerMicron,source->placementY()/context.dbuPerMicron},node.orientation);
        context.database.nodes.push_back(node);context.database.originalPositions.push_back(position);++context.actualComponents;
    });
}
/** @brief DEF顶层端口映射为零面积固定锚点。 @param type 上游类型。 @param source 临时端口。 @param data 事务。 @return 状态。 */
int portCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiPin* source,void* data) {
    // source几何转换全局bbox中心作为context中零面积Terminal，审计说明几何近似。
    return guarded(data,[source](ParseContext& context) {
        Rect box; // 端口所有PORT的全局bbox，μm。
        bool initialized=false; // 是否已有几何；无几何时以合法放置锚点代替。
        // 输入局部几何与放置锚点都采用DBU；同步累加全局物理角点。
        const auto addGeometry=[&box,&initialized,&context](const auto& geometry,int x,int y,int orient) {
            const Orientation direction=defOrientation(orient); // PORT朝向。
            for(int layer=0;layer<geometry.numLayer();++layer) { // 矩形所在图层编号。
                int lx=0; // 矩形左边，DBU。
                int ly=0; // 矩形下边，DBU。
                int ux=0; // 矩形右边，DBU。
                int uy=0; // 矩形上边，DBU。
                geometry.bounds(layer,&lx,&ly,&ux,&uy);
                for(const int px:{lx,ux}) for(const int py:{ly,uy}) { // 当前矩形角点坐标，DBU。
                    const Vec2 point=transformOffset({static_cast<double>(px),static_cast<double>(py)},direction); // 方向后局部坐标。
                    extend(box,initialized,{(x+point.x)/context.dbuPerMicron,(y+point.y)/context.dbuPerMicron});
                }
            }
            for(int polygon=0;polygon<geometry.numPolygons();++polygon) { // 端口多边形编号。
                const auto points=geometry.getPolygon(polygon); // 借用多边形顶点。
                for(int vertex=0;vertex<points.numPoints;++vertex) { // 当前顶点。
                    const Vec2 point=transformOffset({static_cast<double>(points.x[vertex]),static_cast<double>(points.y[vertex])},direction); // 朝向后坐标。
                    extend(box,initialized,{(x+point.x)/context.dbuPerMicron,(y+point.y)/context.dbuPerMicron});
                }
            }
            if(!initialized) extend(box,initialized,{x/context.dbuPerMicron,y/context.dbuPerMicron});
        };
        if(source->hasPort()) {
            for(int port=0;port<source->numPorts();++port) { // 当前端口PORT。
                const auto* geometry=source->pinPort(port); // PORT临时对象。
                if(!geometry->hasPlacement()) Throw("INPUT_INVALID","顶层PORT没有放置锚点",source->pinName());
                addGeometry(*geometry,geometry->placementX(),geometry->placementY(),geometry->orient());
            }
        } else {
            if(!source->hasPlacement()) Throw("INPUT_INVALID","顶层端口没有放置锚点",source->pinName());
            addGeometry(*source,source->placementX(),source->placementY(),source->orient());
        }
        Node node; // 顶层端口没有可放置面积，以点参与线长。
        node.id=context.database.nodes.size();node.name=source->pinName();node.kind=NodeKind::Terminal;node.fixed=true;
        if(!context.ports.emplace(node.name,node.id).second || context.instances.count(node.name)) Throw("INPUT_INVALID","重复端口/实例名称",node.name);
        context.database.nodes.push_back(node);context.database.originalPositions.push_back({(box.lx+box.ux)/2,(box.ly+box.uy)/2,Orientation::N});
    });
}
/** @brief DEF网络复制，连接稍后解析。 @param type 上游类型。 @param source 网络。 @param data 事务。 @return 状态。 */
int netCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiNet* source,void* data) {
    // 按source原始网络顺序生成稳定ID，context保存连接名称副本。
    return guarded(data,[source](ParseContext& context) {
        Net net; // 当前统一网络。
        net.id=context.database.nets.size();net.name=source->name();
        if(source->hasWeight()) net.weight=source->weight();
        std::vector<Connection> links; // 当前网络延迟连接列表。
        for(int pin=0;pin<source->numConnections();++pin) { // 当前连接索引。
            if(pin%4096==0) context.control->checkCancelled();
            links.push_back({source->instance(pin),source->pin(pin)});
        }
        context.database.nets.push_back(net);context.connections.push_back(std::move(links));
    });
}
/** @brief 矩形硬placement阻挡回调。 @param type 上游类型。 @param source 阻挡。 @param data 事务。 @return 状态。 */
int blockageCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiBlockage* source,void* data) {
    // context只消费硬placement矩形，其余布线阻挡记为忽略。
    return guarded(data,[source](ParseContext& context) {
        if(!source->hasPlacement()) {context.database.auditWarnings.push_back("忽略非placement布线阻挡");return;}
        if(source->hasSoft()||source->hasPartial()||source->numPolygons()) Throw("UNSUPPORTED","仅支持矩形硬placement阻挡");
        for(int rect=0;rect<source->numRectangles();++rect) { // 阻挡矩形索引。
            context.database.obstacles.push_back({{source->xl(rect)/context.dbuPerMicron,source->yl(rect)/context.dbuPerMicron,source->xh(rect)/context.dbuPerMicron,source->yh(rect)/context.dbuPerMicron},"DEF placement blockage"});
        }
    });
}
/** @brief DIEAREA仅接受矩形。 @param type 上游类型。 @param source 区域。 @param data 事务。 @return 状态。 */
int dieCallback(LefDefParser::defrCallbackType_e,LefDefParser::defiBox* source,void* data) {
    // 非矩形die不能静默降级为bbox；context只记录校验成功摘要。
    return guarded(data,[source](ParseContext& context) {
        const auto points=source->getPoint(); // 上游DIEAREA顶点。
        if(points.numPoints!=2) Throw("UNSUPPORTED","仅支持两角点矩形DIEAREA");
        if(points.x[0]>=points.x[1]||points.y[0]>=points.y[1]) Throw("INPUT_INVALID","DIEAREA无正面积");
        context.database.auditWarnings.push_back("矩形DIEAREA已验证；core按可放置行并集bbox定义");
    });
}
/** @brief 声明数量回调。 @param type 上游节类型。 @param amount 数量。 @param data 事务。 @return 状态，非空REGIONS/GROUPS拒绝。 */
int countCallback(LefDefParser::defrCallbackType_e type,int amount,void* data) {
    // 根据type更新context计数或拒绝未支持约束。
    return guarded(data,[type,amount](ParseContext& context) {
        if(amount<0) Throw("INPUT_INVALID","负DEF节数量");
        if(type==LefDefParser::defrComponentStartCbkType) context.expectedComponents=amount;
        else if(type==LefDefParser::defrNetStartCbkType) context.expectedNets=amount;
        else if(type==LefDefParser::defrStartPinsCbkType) context.expectedPorts=amount;
        else if(amount) Throw("UNSUPPORTED","不支持REGIONS/GROUPS布局约束");
    });
}
/** @brief 结束一个第三方解析会话，异常路径也清理全局数据。 */
struct ParserSession {
    bool lef; ///< true为LEF会话，否则DEF。
    /** @brief 析构清理解析全局资源；无输入、无返回值，不抛异常。 */
    ~ParserSession() {if(lef) LefDefParser::lefrClear();else LefDefParser::defrClear();activeContext=nullptr;}
};
} // namespace

/** @brief LEF/DEF适配实现，公共契约见design.hpp。 @param input 配套文件。 @param control 取消。 @return μm数据库。 */
DesignDatabase LoadLefDef(const InputSpec& input,ControlToken& control) {
    if(input.lefPaths.empty()||input.defPath.empty()) Throw("INPUT_INVALID","需要至少一个LEF和一个DEF");
    std::unique_lock<std::mutex> lock(parserMutex,std::defer_lock); // 非重入解析器互斥，等待期间也可取消。
    while(!lock.try_lock()) {control.checkCancelled();std::this_thread::yield();}
    ParseContext context; // 整个加载事务的独占状态。
    context.control=&control;context.database.unit="um";context.database.physicalScaleKnown=true;
    context.database.auditWarnings.push_back("引脚使用有效几何包围盒中心；布线/via/轨道/工艺层数据不参与布局");
    for(const std::string& path:input.lefPaths) { // 当前配套LEF文件。
        context.database.inputFiles.push_back(AuditFile(path,control));
        LefDefParser::lefrInit();ParserSession session{true};activeContext=&context; // 当前LEF解析会话RAII。
        LefDefParser::lefrSetSiteCbk(siteCallback);LefDefParser::lefrSetMacroBeginCbk(macroBeginCallback);
        LefDefParser::lefrSetMacroCbk(macroCallback);LefDefParser::lefrSetPinCbk(lefPinCallback);
        LefDefParser::lefrSetReadFunction(readCancelable);LefDefParser::lefrSetLogFunction(parserLog);LefDefParser::lefrSetWarningLogFunction(parserLog);
        std::unique_ptr<FILE,FileCloser> file(std::fopen(path.c_str(),"rb")); // 输入FILE仅本会话拥有。
        if(!file) Throw("REFERENCE_MISSING","无法打开LEF",path);
        const int status=LefDefParser::lefrRead(file.get(),path.c_str(),&context); // 上游语法解析返回码。
        control.checkCancelled();if(context.failure) std::rethrow_exception(context.failure);
        if(status) Throw("INPUT_INVALID","LEF解析失败："+context.diagnostic,path+":"+std::to_string(LefDefParser::lefrLineNumber()));
    }
    context.database.inputFiles.push_back(AuditFile(input.defPath,control));
    {
        LefDefParser::defrInit();ParserSession session{false};activeContext=&context; // 当前DEF解析会话RAII。
        LefDefParser::defrSetUnitsCbk(unitsCallback);LefDefParser::defrSetRowCbk(rowCallback);LefDefParser::defrSetComponentCbk(componentCallback);
        LefDefParser::defrSetPinCbk(portCallback);LefDefParser::defrSetNetCbk(netCallback);LefDefParser::defrSetBlockageCbk(blockageCallback);
        LefDefParser::defrSetDieAreaCbk(dieCallback);LefDefParser::defrSetComponentStartCbk(countCallback);LefDefParser::defrSetNetStartCbk(countCallback);
        LefDefParser::defrSetStartPinsCbk(countCallback);LefDefParser::defrSetRegionStartCbk(countCallback);LefDefParser::defrSetGroupsStartCbk(countCallback);
        LefDefParser::defrSetReadFunction(readCancelable);LefDefParser::defrSetLogFunction(parserLog);LefDefParser::defrSetWarningLogFunction(parserLog);
        std::unique_ptr<FILE,FileCloser> file(std::fopen(input.defPath.c_str(),"rb")); // 本会话拥有DEF流。
        if(!file) Throw("REFERENCE_MISSING","无法打开DEF",input.defPath);
        const int status=LefDefParser::defrRead(file.get(),input.defPath.c_str(),&context,1); // 上游语法返回码。
        control.checkCancelled();if(context.failure) std::rethrow_exception(context.failure);
        if(status) Throw("INPUT_INVALID","DEF解析失败："+context.diagnostic,input.defPath);
    }
    if(context.expectedComponents!=context.actualComponents||context.expectedNets!=context.database.nets.size()||context.expectedPorts!=context.ports.size())
        Throw("INPUT_INVALID","DEF节声明数量与实际不一致",input.defPath);
    for(std::size_t netId=0;netId<context.connections.size();++netId) { // 规范输入顺序的网络ID。
        for(const Connection& link:context.connections[netId]) { // 当前延迟连接。
            control.checkCancelled();Pin pin; // 映射后的统一引脚。
            pin.id=context.database.pins.size();pin.netId=netId;
            if(link.instance=="PIN") {
                const auto found=context.ports.find(link.pin); // 顶层端口引用。
                if(found==context.ports.end()) Throw("REFERENCE_MISSING","DEF网络引用未知端口",link.pin);
                pin.nodeId=found->second;
            } else {
                const auto found=context.instances.find(link.instance); // 实例连接引用。
                if(found==context.instances.end()) Throw("REFERENCE_MISSING","DEF网络引用未知实例",link.instance);
                pin.nodeId=found->second;
                const Node& node=context.database.nodes[pin.nodeId]; // 所属实例。
                const Master& master=context.masters.at(node.masterName); // 对应LEF master。
                const auto point=master.pins.find(link.pin); // master内部引脚几何中心。
                if(point==master.pins.end()) Throw("REFERENCE_MISSING","DEF网络引用未知master引脚",node.masterName+"/"+link.pin);
                pin.offset={point->second.x+master.origin.x-master.width/2,point->second.y+master.origin.y-master.height/2};
            }
            context.database.pins.push_back(pin);context.database.nets[netId].pinIds.push_back(pin.id);
        }
    }
    return std::move(context.database);
}
} // namespace placement
