#include <placement/io/visualization.hpp>
#include <CImg.h>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace placement {
/** @brief 单次屏幕映射。 @param point DU点。 @return 像素点。 */
Vec2 ImageTransform::map(Vec2 point) const {return {margin+scale*(point.x-bounds.lx),margin+scale*(bounds.uy-point.y)};}
/** @brief 等比尺寸计算，详见visualization.hpp。 @param bounds chip。 @param width 有效宽。 @param margin 边距。 @return 映射。 */
ImageTransform MakeImageTransform(Rect bounds,int width,int margin) {
    if(bounds.width()<=0||bounds.height()<=0||width<=0||margin<0||margin>1024) Throw("INPUT_INVALID","无有效图像区域或边距");
    const double scale=std::min(width/bounds.width(),(8192.0-2*margin-64)/bounds.height()); // 预留64px图例，保持相同两轴比例。
    return {bounds,scale,static_cast<int>(std::ceil(scale*bounds.width()))+2*margin,static_cast<int>(std::ceil(scale*bounds.height()))+2*margin+64,margin};
}
/** @brief 无窗口标准BMP输出。 @param snapshot 源。 @param path 目标。 @param config 显示。 @param control 取消。 @param overwrite 覆盖。 无返回值。 */
void ExportBmp(const Snapshot& snapshot,const std::string& path,const Config& config,ControlToken& control,bool overwrite) {
    validatePositions(*snapshot.database,snapshot.positions);
    Rect bounds=chipBounds(*snapshot.database,snapshot.positions); // 显示完整chip而非仅core。
    if(bounds.width()<=0||bounds.height()<=0) bounds={0,0,1,1}; // 空设计使用明确占位视图，不伪造实体。
    const ImageTransform transform=MakeImageTransform(bounds,config.imageWidth,config.margin); // 本次统一数学映射。
    /** @brief 在临时文件绘制完整布局并关闭输出。 @param temporary 同目录临时路径。 无返回值，失败由AtomicWrite处理。 */
    AtomicWrite(path,[&snapshot,&config,&control,&transform](const std::string& temporary) {
        cimg_library::CImg<unsigned char> image(transform.width,transform.height,1,3,255); // 私有RGB画布，不访问显示服务器。
        const unsigned char standard[3]={220,76,76}; // 标准单元色，设计06。
        const unsigned char macro[3]={35,155,86}; // 可移动宏色。
        const unsigned char fixed[3]={71,85,105}; // 固定实体/端口色。
        const unsigned char core[3]={51,65,85}; // core边界色。
        const unsigned char rowColor[3]={203,213,225}; // 可放置行辅助色。
        const unsigned char violationColor[3]={192,38,211}; // 非法几何轮廓色。
        /** @brief 绘制clip后的矩形。 @param rect DU范围。 @param color RGB。 @param fill 是否填充。 无返回值，修改私有画布。 */
        const auto rectangle=[&image,&transform](Rect rect,const unsigned char* color,bool fill) {
            const Vec2 first=transform.map({rect.lx,rect.uy}); // 像素左上角。
            const Vec2 last=transform.map({rect.ux,rect.ly}); // 像素右下角。
            const int x0=std::clamp(static_cast<int>(std::lround(first.x)),0,image.width()-1); // clip后左像素。
            const int y0=std::clamp(static_cast<int>(std::lround(first.y)),0,image.height()-1); // clip后上像素。
            const int x1=std::clamp(static_cast<int>(std::lround(last.x)),0,image.width()-1); // clip后右像素。
            const int y1=std::clamp(static_cast<int>(std::lround(last.y)),0,image.height()-1); // clip后下像素。
            if(fill) image.draw_rectangle(x0,y0,x1,y1,color);else image.draw_rectangle(x0,y0,x1,y1,color,1.0f,~0U);
        };
        for(const RowSegment& row:snapshot.database->rows) rectangle(rowRect(row),rowColor,false); // 行辅助轮廓。
        for(const Obstacle& obstacle:snapshot.database->obstacles) rectangle(obstacle.rect,fixed,true); // 硬阻挡按固定几何绘制。
        for(const Node& node:snapshot.database->nodes) { // 当前真实节点，无filler。
            if(node.id%4096==0) control.checkCancelled();
            const Rect box=nodeRect(node,snapshot.positions[node.id]); // 数学实体框。
            const unsigned char* color=node.fixed?fixed:(node.kind==NodeKind::Macro?macro:standard); // 图层互斥色。
            if(box.area()>0) rectangle(box,color,true);
            else {
                const Vec2 point=transform.map({box.lx,box.ly}); // 零面积端口锚点。
                const int x=static_cast<int>(std::lround(point.x)); // 端口像素x。
                const int y=static_cast<int>(std::lround(point.y)); // 端口像素y。
                image.draw_line(x-3,y,x+3,y,color);image.draw_line(x,y-3,x,y+3,color);
            }
        }
        rectangle(coreBounds(*snapshot.database),core,false);rectangle(transform.bounds,core,false);
        const ViolationReport report=CheckLegality(snapshot,"all",10000,&control); // 独立检查，不根据颜色判断合法。
        for(const Violation& violation:report.records) rectangle(violation.region,violationColor,false); // 可定位违规区域。
        const int legendY=transform.height-56; // 固定图例条上边。
        const std::string title=snapshot.stage+" rev="+std::to_string(snapshot.revision)+" ["+snapshot.database->unit+"] "+snapshot.termination+" violations="+std::to_string(report.totalCount); // ASCII元数据标题。
        image.draw_text(8,legendY,title.c_str(),core,0,1,13);
        image.draw_text(8,legendY+20,"standard",standard,0,1,13);image.draw_text(100,legendY+20,"macro",macro,0,1,13);
        image.draw_text(170,legendY+20,"fixed/port",fixed,0,1,13);image.draw_text(290,legendY+20,"violation",violationColor,0,1,13);
        image.draw_text(8,legendY+38,"Subpixel objects use visibility marks; see JSON geometry.",core,0,1,11);
        image.save_bmp(temporary.c_str());
    },control,overwrite);
}
} // namespace placement
