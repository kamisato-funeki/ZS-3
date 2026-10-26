#pragma once
#include <placement/io/result.hpp>

namespace placement {
/** @brief 离线数学到像素的单次等比映射，无界面对象。 */
struct ImageTransform {
    Rect bounds; ///< 数学chip范围，DU。
    double scale=1; ///< 像素/DU，两轴相同。
    int width=0; ///< 输出总宽，像素。
    int height=0; ///< 输出总高，像素，含图例。
    int margin=0; ///< 内容边距，像素。
    /** @brief 将数学点映射到屏幕。 @param point 数学DU点。 @return 浮点像素坐标，仅一次Y翻转。 */
    Vec2 map(Vec2 point) const;
};
/** @brief 生成等比图像映射。 @param bounds 正尺寸chip。 @param width 期望有效宽，像素。 @param margin 边距，像素。
 * @return 最大总高8192的映射；非法几何抛INPUT_INVALID。 */
ImageTransform MakeImageTransform(Rect bounds,int width,int margin);
/** @brief CImg无窗口BMP写出。 @param snapshot 完整源。 @param path 目标。
 * @param config 显示配置。 @param control 取消。 @param overwrite 显式覆盖。 无返回值，失败保持已有文件。 */
void ExportBmp(const Snapshot& snapshot,const std::string& path,const Config& config,ControlToken& control,bool overwrite=false);
/** @brief Limbo真实GDSII写出，量化后独立重检并保存伴随元数据。
 * @param snapshot 源。 @param path 目标。 @param config DBU/单位配置。 @param control 取消。
 * @param overwrite 显式覆盖。 无返回值；范围/精度/写出错误抛Error。 */
void ExportGds(const Snapshot& snapshot,const std::string& path,const Config& config,ControlToken& control,bool overwrite=false);
} // namespace placement
