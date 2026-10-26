#include "placement/application/global.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace placement {
/** @brief 实现 WA 正反指数矩与导数；参数、固定性及错误契约见 global.hpp。 */
WirelengthEvaluation EvaluateWirelength(const DesignDatabase& database,const PositionState& positions,double gamma,ControlToken* control) {
    if (!(gamma>0 && std::isfinite(gamma))) Throw("INPUT_INVALID","WA gamma 必须正有限","global");
    validatePositions(database,positions);
    WirelengthEvaluation result; ///< 累积的 WA 与按节点梯度。
    result.gradient.resize(database.nodes.size());
    std::size_t processed=0; ///< 引脚批次计数，用于有界取消响应。
    for (const Net& net:database.nets) { // net 为当前超边。
        if (control) control->checkCancelled();
        if (net.pinIds.size()<2 || net.weight==0) continue;
        std::vector<Vec2> points; ///< 当前网络真实引脚点，DU。
        points.reserve(net.pinIds.size());
        for (PinId id:net.pinIds) { // id 为当前网络中的引脚稳定编号。
            if (control && (++processed%4096==0)) control->checkCancelled();
            points.push_back(pinPosition(database,positions,database.pins.at(id)));
        }
        for (int axis=0;axis<2;++axis) { // axis 为 x=0/y=1。
            double low=std::numeric_limits<double>::infinity(); ///< 最小引脚坐标，DU。
            double high=-low; ///< 最大引脚坐标，DU。
            for (const Vec2& point:points) { // point 为引脚真实位置。
                if (control && (++processed%4096==0)) control->checkCancelled();
                const double coordinate=axis==0?point.x:point.y; ///< 当前轴坐标，DU。
                low=std::min(low,coordinate); high=std::max(high,coordinate);
            }
            double sumPlus=0; ///< 正指数总权，无量纲。
            double sumMinus=0; ///< 负指数总权，无量纲。
            double momentPlus=0; ///< 正指数相对 low 的坐标矩，DU。
            double momentMinus=0; ///< 负指数相对 low 的坐标矩，DU。
            for (const Vec2& point:points) { // point 为引脚真实位置。
                if (control && (++processed%4096==0)) control->checkCancelled();
                const double coordinate=axis==0?point.x:point.y; ///< 当前轴坐标，DU。
                const double plus=std::exp((coordinate-high)/gamma); ///< 数值稳定的正指数权。
                const double minus=std::exp((low-coordinate)/gamma); ///< 数值稳定的负指数权。
                sumPlus+=plus; sumMinus+=minus;
                momentPlus+=(coordinate-low)*plus; momentMinus+=(coordinate-low)*minus;
            }
            const double meanPlus=momentPlus/sumPlus; ///< 相对 low 的正指数均值，DU。
            const double meanMinus=momentMinus/sumMinus; ///< 相对 low 的负指数均值，DU。
            result.value+=net.weight*(meanPlus-meanMinus);
            for (std::size_t index=0;index<points.size();++index) { // index 将引脚点映射到网络 pinIds。
                if (control && (++processed%4096==0)) control->checkCancelled();
                const Pin& pin=database.pins.at(net.pinIds[index]); ///< 当前引脚拓扑。
                if (database.nodes.at(pin.nodeId).fixed) continue;
                const double coordinate=(axis==0?points[index].x:points[index].y)-low; ///< 平移后的轴坐标，DU。
                const double plus=std::exp((coordinate-(high-low))/gamma); ///< 正指数权。
                const double minus=std::exp(-coordinate/gamma); ///< 负指数权。
                const double derivative=net.weight*(plus/sumPlus*(1+(coordinate-meanPlus)/gamma)-minus/sumMinus*(1-(coordinate-meanMinus)/gamma)); ///< 精确 WA 轴导数。
                if (axis==0) result.gradient[pin.nodeId].x+=derivative;
                else result.gradient[pin.nodeId].y+=derivative;
            }
        }
    }
    if (!std::isfinite(result.value)) Throw("NUMERIC_FAILURE","WA 目标非有限","global");
    for (const Vec2& gradient:result.gradient) { // gradient 为按节点累积的导数。
        if (!std::isfinite(gradient.x)||!std::isfinite(gradient.y)) Throw("NUMERIC_FAILURE","WA 梯度非有限","global");
    }
    return result;
}
} // namespace placement
