#include "placement/application/global.hpp"
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>

namespace placement {
namespace {
/** @brief FFTW 计划缓存；规划和销毁由同一互斥锁串行，执行使用独立数组。 */
struct PlanCache {
    std::mutex mutex; ///< 保护 planner 全局状态及 map。
    std::map<std::tuple<int,int,bool>, fftw_plan> plans; ///< 键依次为 nx、ny、逆向标志；值拥有计划。
    /** @brief 在静态退出时释放全部计划；无参数、无返回值。 */
    ~PlanCache() { for (const auto& entry : plans) { // entry 为缓存键与拥有的计划。
        fftw_destroy_plan(entry.second);
    } }
};
/** @brief 获取进程计划缓存。 @return 生命周期覆盖所有调用的缓存。 */
PlanCache& cache() { static PlanCache instance; /* instance 为进程唯一 planner 缓存。 */ return instance; }
/** @brief 检查网格契约。 @param input 网格。 @param nx 宽。 @param ny 高。 无返回值；错误抛 Error。 */
void checkGrid(const std::vector<double>& input, int nx, int ny) {
    if (nx <= 0 || ny <= 0 || input.size() != static_cast<std::size_t>(nx)*ny)
        Throw("INPUT_INVALID", "DCT 网格尺寸无效", "global");
    for (double value : input) { // value 是待检查的无量纲/频域数值。
        if (!std::isfinite(value)) Throw("NUMERIC_FAILURE", "DCT 输入非有限", "global");
    }
}
}
/** @brief 实现公共正交变换契约；参数及结果见 global.hpp。 */
std::vector<double> OrthonormalDct(const std::vector<double>& input, int nx, int ny,
                                  bool inverse, ControlToken* control) {
    checkGrid(input,nx,ny);
    if (control) control->checkCancelled();
    std::vector<double> work(input); ///< 当前调用独占 FFTW 输入。
    std::vector<double> output(input.size()); ///< 当前调用独占 FFTW 输出。
    fftw_plan plan; ///< 缓存拥有、本次仅借用的计划。
    {
        auto& storage = cache(); ///< 全局 planner 缓存。
        std::lock_guard<std::mutex> lock(storage.mutex); ///< 串行创建 FFTW 计划。
        const auto key = std::make_tuple(nx,ny,inverse); ///< 网格与方向缓存键。
        auto found = storage.plans.find(key); ///< 现有计划迭代器。
        if (found == storage.plans.end()) {
            const auto kind = inverse ? FFTW_REDFT01 : FFTW_REDFT10; ///< 两轴 DCT 类型。
            plan = fftw_plan_r2r_2d(ny,nx,work.data(),output.data(),kind,kind,FFTW_ESTIMATE|FFTW_UNALIGNED);
            if (!plan) Throw("NUMERIC_FAILURE", "FFTW 计划创建失败", "global");
            storage.plans.emplace(key,plan);
        } else plan = found->second;
    }
    for (int y=0;y<ny;++y) { // y 为纵向频率索引。
        const double cy = y==0 ? 1/std::sqrt(static_cast<double>(ny)) : std::sqrt(2.0/ny); ///< 纵向正交系数。
        for (int x=0;x<nx;++x) { // x 为横向频率索引。
            const double cx = x==0 ? 1/std::sqrt(static_cast<double>(nx)) : std::sqrt(2.0/nx); ///< 横向正交系数。
            if (inverse) work[static_cast<std::size_t>(y)*nx+x] /= cx*cy;
        }
    }
    fftw_execute_r2r(plan,work.data(),output.data());
    if (control) control->checkCancelled();
    for (int y=0;y<ny;++y) { // y 为纵向频率/空间索引。
        const double cy = y==0 ? 1/std::sqrt(static_cast<double>(ny)) : std::sqrt(2.0/ny); ///< 纵向正交系数。
        for (int x=0;x<nx;++x) { // x 为横向频率/空间索引。
            const double cx = x==0 ? 1/std::sqrt(static_cast<double>(nx)) : std::sqrt(2.0/nx); ///< 横向正交系数。
            output[static_cast<std::size_t>(y)*nx+x] *= inverse ? 1.0/(static_cast<double>(nx)*ny) : cx*cy/4;
        }
    }
    checkGrid(output,nx,ny);
    return output;
}
/** @brief 实现公共离散 Neumann 逆算子契约；参数及结果见 global.hpp。 */
std::vector<double> SolvePoisson(const std::vector<double>& density,int nx,int ny,double dx,double dy,ControlToken* control) {
    checkGrid(density,nx,ny);
    if (!(dx>0 && dy>0 && std::isfinite(dx) && std::isfinite(dy))) Throw("INPUT_INVALID","泊松网格间距无效","global");
    std::vector<double> residual(density); ///< 去直流后的密度。
    const double mean = std::accumulate(residual.begin(),residual.end(),0.0)/residual.size(); ///< 无量纲平均密度。
    for (double& value:residual) value-=mean; // value 为单 bin 残差。
    auto spectrum = OrthonormalDct(residual,nx,ny,false,control); ///< 正交残差谱。
    const double pi = std::acos(-1.0); ///< 圆周率，无量纲。
    for (int y=0;y<ny;++y) { // y 为纵向离散模式编号。
        for (int x=0;x<nx;++x) { // x 为横向离散模式编号。
            const std::size_t index=static_cast<std::size_t>(y)*nx+x; ///< 频谱扁平索引。
            if (x==0 && y==0) { spectrum[index]=0; continue; }
            const double sx=std::sin(pi*x/(2*nx)); ///< 横向半角模式。
            const double sy=std::sin(pi*y/(2*ny)); ///< 纵向半角模式。
            const double eigenvalue=4*sx*sx/(dx*dx)+4*sy*sy/(dy*dy); ///< 离散负拉普拉斯特征值，DU⁻²。
            spectrum[index]/=eigenvalue;
        }
    }
    return OrthonormalDct(spectrum,nx,ny,true,control);
}
} // namespace placement
