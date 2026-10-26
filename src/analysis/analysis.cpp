#include <placement/analysis/analysis.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <tuple>

namespace placement {
namespace {

/** @brief 确认快照拓扑身份和位置数量，不把几何非法与身份错误混淆。
 * @param snapshot 待检查快照。 @return 只读数据库引用；身份错误抛 INPUT_INVALID。 */
const DesignDatabase& requireDatabase(const Snapshot& snapshot) {
    if (!snapshot.database || snapshot.datasetId != snapshot.database->datasetId ||
        snapshot.positions.size() != snapshot.database->nodes.size()) {
        Throw("INPUT_INVALID", "快照数据库、身份或位置数量无效", snapshot.stage, true);
    }
    return *snapshot.database;
}

/** @brief 判断方向值有效。 @param orientation 待检查枚举。 @return 是否为八方向之一。 */
bool validDirection(Orientation orientation) {
    return static_cast<int>(orientation) >= static_cast<int>(Orientation::N) &&
           static_cast<int>(orientation) <= static_cast<int>(Orientation::FE);
}

/** @brief 判断矩形有限且非倒置。 @param rectangle 待检查矩形，DU。 @return 是否有效。 */
bool finiteRect(const Rect& rectangle) {
    return std::isfinite(rectangle.lx) && std::isfinite(rectangle.ly) && std::isfinite(rectangle.ux) &&
           std::isfinite(rectangle.uy) && rectangle.lx <= rectangle.ux && rectangle.ly <= rectangle.uy;
}

/** @brief 计算两个半开矩形的非负交集。 @param left 第一个范围。 @param right 第二个范围。 @return 交集，DU。 */
Rect intersection(const Rect& left, const Rect& right) {
    const double lx = std::max(left.lx, right.lx); // 交集横向下界，DU。
    const double ly = std::max(left.ly, right.ly); // 交集纵向下界，DU。
    return {lx, ly, std::max(lx, std::min(left.ux, right.ux)), std::max(ly, std::min(left.uy, right.uy))};
}

/** @brief 判断正面积接触，不使用面积乘积避免薄长交集被错误吞掉。
 * @param left 第一个矩形。 @param right 第二个矩形。 @param epsilon 每轴容差，DU。 @return 两轴均超容差时 true。 */
bool collides(const Rect& left, const Rect& right, double epsilon = 0.0) {
    return std::min(left.ux, right.ux) - std::max(left.lx, right.lx) > epsilon &&
           std::min(left.uy, right.uy) - std::max(left.ly, right.ly) > epsilon;
}

/** @brief 不变几何的包围盒树，按中位数划分；只读查询可跨线程，构建由调用线程独占。 */
class RectIndex {
    /** @brief 包围盒树节点，保存几何或两个子节点。 */
    struct Branch {
        Rect bounds; ///< 子树矩形包围盒，DU。
        std::size_t begin = 0; ///< 叶节点对象范围的起始索引。
        std::size_t end = 0; ///< 叶节点对象范围的排他末端。
        int left = -1; ///< 左子节点，-1 表示叶子。
        int right = -1; ///< 右子节点，-1 表示叶子。
    };
    const std::vector<Rect>& rectangles_; ///< 外部矩形只读引用，生命周期覆盖索引。
    std::vector<std::size_t> order_; ///< 中位数划分后的原始矩形索引。
    std::vector<Branch> branches_; ///< 节点连续存储，不共享可变状态。
    /** @brief 递归构建平衡空间树。 @param begin 起始索引。 @param end 排他末端。 @return 新分支编号。 */
    int build(std::size_t begin, std::size_t end) {
        Branch branch; // 当前构建分支的局部值，避免 vector 扩容后引用失效。
        branch.begin = begin;
        branch.end = end;
        branch.bounds = rectangles_[order_[begin]];
        for (std::size_t index = begin + 1; index < end; ++index) { // 当前子树对象索引。
            const Rect& rectangle = rectangles_[order_[index]]; // 待合并范围，DU。
            branch.bounds.lx = std::min(branch.bounds.lx, rectangle.lx);
            branch.bounds.ly = std::min(branch.bounds.ly, rectangle.ly);
            branch.bounds.ux = std::max(branch.bounds.ux, rectangle.ux);
            branch.bounds.uy = std::max(branch.bounds.uy, rectangle.uy);
        }
        const int id = static_cast<int>(branches_.size()); // 新分支稳定索引。
        branches_.push_back(branch);
        if (end - begin > 8) {
            const std::size_t middle = begin + (end - begin) / 2; // 以对象数量二等分。
            const bool horizontal = branch.bounds.width() >= branch.bounds.height(); // 当前最大跨度轴。
            /** @brief 按选定轴的中心排序。 @param left 左对象索引。 @param right 右对象索引。 @return 中心排序关系。 */
            std::nth_element(order_.begin() + static_cast<std::ptrdiff_t>(begin), order_.begin() + static_cast<std::ptrdiff_t>(middle),
                order_.begin() + static_cast<std::ptrdiff_t>(end), [this, horizontal](std::size_t left, std::size_t right) {
                    const double a = horizontal ? rectangles_[left].lx * 0.5 + rectangles_[left].ux * 0.5 : rectangles_[left].ly * 0.5 + rectangles_[left].uy * 0.5; // 左中心，DU。
                    const double b = horizontal ? rectangles_[right].lx * 0.5 + rectangles_[right].ux * 0.5 : rectangles_[right].ly * 0.5 + rectangles_[right].uy * 0.5; // 右中心，DU。
                    return a < b || (a == b && left < right);
                });
            branch.left = build(begin, middle);
            branch.right = build(middle, end);
            branches_[id] = branch;
        }
        return id;
    }
    /** @brief 递归收集与查询区域正面积相交的对象。
     * @param id 子树编号。 @param query 查询矩形。 @param result 输出原始索引数组。 无返回值。 */
    void queryBranch(int id, const Rect& query, std::vector<std::size_t>& result) const {
        const Branch& branch = branches_[id]; // 当前只读分支。
        if (!collides(branch.bounds, query)) return;
        if (branch.left < 0) {
            for (std::size_t index = branch.begin; index < branch.end; ++index) { // 叶对象索引。
                if (collides(rectangles_[order_[index]], query)) result.push_back(order_[index]);
            }
        } else {
            queryBranch(branch.left, query, result);
            queryBranch(branch.right, query, result);
        }
    }
public:
    /** @brief 建立只读索引。 @param rectangles 生命周期须覆盖索引的矩形数组。 */
    explicit RectIndex(const std::vector<Rect>& rectangles) : rectangles_(rectangles), order_(rectangles.size()) {
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        if (!order_.empty()) build(0, order_.size());
    }
    /** @brief 查询正面积相交矩形，不持有输出引用。 @param rectangle 查询范围。 @return 原始索引列表。 */
    std::vector<std::size_t> query(const Rect& rectangle) const {
        std::vector<std::size_t> result; // 当前查询命中集合。
        if (!branches_.empty()) queryBranch(0, rectangle, result);
        return result;
    }
};

/** @brief 确定性违规顺序，详情截断始终保留全局排序最前面的条目。 */
struct ViolationLess {
    /** @brief 比较违规类型、对象和定位信息。 @param left 左记录。 @param right 右记录。 @return 是否先于右记录。 */
    bool operator()(const Violation& left, const Violation& right) const {
        return std::tie(left.kind, left.objectIds, left.region.lx, left.region.ly, left.region.ux, left.region.uy, left.message) <
               std::tie(right.kind, right.objectIds, right.region.lx, right.region.ly, right.region.ux, right.region.uy, right.message);
    }
};

/** @brief 完整计数与有界详情收集器，线程局部。 */
class Reporter {
    std::size_t limit_; ///< 最大明细数量，不限制计数。
    std::multiset<Violation, ViolationLess> details_; ///< 有界且确定性排序的明细。
public:
    ViolationReport report; ///< 最终报告计数与范围。
    /** @brief 初始化收集范围。 @param scope all/macro。 @param limit 最大明细数。 */
    Reporter(const std::string& scope, std::size_t limit) : limit_(limit) { report.scope = scope; }
    /** @brief 累加精确计数而不创建详情；用于已证明整棵子树相交的批量计数。
     * @param kind 违规类型。 @param amount 本次唯一相交对数量。 无返回值。 */
    void count(const std::string& kind,std::uint64_t amount) {report.totalCount+=amount;report.countByType[kind]+=amount;}
    /** @brief 判断子树最小ID对应的OVERLAP是否可能进入排序前limit条。
     * @param current 当前扫描ID。 @param minimum 子树最小ID。 @return 可能改善详情集合。 */
    bool mayImprove(NodeId current,NodeId minimum) const {
        if(limit_==0) return false;
        if(details_.size()<limit_) return true;
        const Violation& last=*std::prev(details_.end()); // 当前有界集合的最大排序键。
        if(last.kind!="OVERLAP") return std::string("OVERLAP")<last.kind;
        return std::vector<NodeId>{std::min(current,minimum),std::max(current,minimum)}<=last.objectIds;
    }
    /** @brief 仅维护详情，不改变已批量统计的计数。 @param violation 确切相交对记录。 无返回值。 */
    void detail(Violation violation) {
        if(limit_==0) return;
        details_.insert(std::move(violation));
        if(details_.size()>limit_) details_.erase(std::prev(details_.end()));
    }
    /** @brief 增加一条独立违规并维护有界详情。 @param violation 完整记录值。 无返回值。 */
    void add(Violation violation) {
        count(violation.kind,1);detail(std::move(violation));
    }
    /** @brief 完成报告。 @return 全部计数及至多 limit 条排序详情。 */
    ViolationReport finish() {
        report.records.assign(details_.begin(), details_.end());
        report.truncated = report.totalCount > report.records.size();
        return report;
    }
};

/** @brief 活动 y 区间的增广 treap，x 扫描过程中支持插入/删除与相交报告。 */
class ActiveIntervals {
    /** @brief 一个活动区间树节点，以 (ly,id) 为键，maxUpper 用于整棵子树剪枝。 */
    struct Entry {
        Rect rectangle; ///< 当前实体矩形，DU。
        NodeId id = 0; ///< 原始稳定节点编号。
        std::uint64_t priority = 0; ///< 由 ID 确定性混合生成的堆优先级。
        int left = -1; ///< 左子树编号。
        int right = -1; ///< 右子树编号。
        double maxUpper = 0.0; ///< 子树最大 uy，DU。
        Rect bounds; ///< 子树矩形并集bbox，用于不相交剪枝。
        Rect common; ///< 子树全部矩形的公共交集，可倒置；正面积命中证明整棵子树均相交。
        NodeId minimumId=0; ///< 子树最小稳定ID，用于有界详情排序剪枝。
        std::uint64_t count=1; ///< 子树活动节点数，用于密集聚集输入的精确批量计数。
    };
    std::vector<Entry> entries_; ///< 每次插入一个新条目，不复用地址。
    int root_ = -1; ///< 当前活动根节点。
    /** @brief 获取子树最大上界。 @param id 子树编号，可为-1。 @return 最大上界，空子树为负无穷。 */
    double maximum(int id) const { return id < 0 ? -std::numeric_limits<double>::infinity() : entries_[id].maxUpper; }
    /** @brief 更新子树增广值。 @param id 非空子树根。 无返回值。 */
    void update(int id) {
        Entry& entry=entries_[id]; // 当前待更新节点，不跨vector扩容保留引用。
        entry.maxUpper=std::max({entry.rectangle.uy,maximum(entry.left),maximum(entry.right)});
        entry.bounds=entry.rectangle;entry.common=entry.rectangle;entry.minimumId=entry.id;entry.count=1;
        for(const int child:{entry.left,entry.right}) { // 子树句柄，-1为空。
            if(child<0) continue;
            const Entry& subtree=entries_[child]; // 已更新的孩子增广值。
            entry.bounds={std::min(entry.bounds.lx,subtree.bounds.lx),std::min(entry.bounds.ly,subtree.bounds.ly),std::max(entry.bounds.ux,subtree.bounds.ux),std::max(entry.bounds.uy,subtree.bounds.uy)};
            entry.common={std::max(entry.common.lx,subtree.common.lx),std::max(entry.common.ly,subtree.common.ly),std::min(entry.common.ux,subtree.common.ux),std::min(entry.common.uy,subtree.common.uy)};
            entry.minimumId=std::min(entry.minimumId,subtree.minimumId);entry.count+=subtree.count;
        }
    }
    /** @brief 从已计数子树收集可能进入前limit的确切相交详情。
     * @param root 子树。 @param query 查询框。 @param current 查询ID。 @param reporter 详情收集器。
     * @param epsilon 距离容差。 @param control 可空取消。 @param work 访问计数。 无返回值。 */
    void detailsAt(int root,const Rect& query,NodeId current,Reporter& reporter,double epsilon,ControlToken* control,std::size_t& work) const {
        if(root<0) return;
        if(control&&++work%4096==0) control->checkCancelled();
        const Entry& entry=entries_[root]; // 子树只读增广几何。
        if(!collides(entry.bounds,query,epsilon)||!reporter.mayImprove(current,entry.minimumId)) return;
        detailsAt(entry.left,query,current,reporter,epsilon,control,work);
        if(collides(entry.rectangle,query,epsilon)&&reporter.mayImprove(current,entry.id)) {
            const Rect overlap=intersection(entry.rectangle,query); // 相交区域及精确面积，DU。
            reporter.detail({"OVERLAP",{std::min(current,entry.id),std::max(current,entry.id)},overlap,overlap.area(),"实体正面积重叠"});
        }
        detailsAt(entry.right,query,current,reporter,epsilon,control,work);
    }
    /** @brief 精确统计子树相交；公共交集命中时可批量计数，避免聚集输入枚举数十亿对。
     * @param root 子树。 @param query 查询框。 @param current 查询ID。 @param reporter 输出。
     * @param epsilon 距离容差。 @param control 可空取消。 @param work 访问计数。 无返回值。 */
    void countAt(int root,const Rect& query,NodeId current,Reporter& reporter,double epsilon,ControlToken* control,std::size_t& work) const {
        if(root<0) return;
        if(control&&++work%4096==0) control->checkCancelled();
        const Entry& entry=entries_[root]; // 当前子树的边界和公共区域。
        if(!collides(entry.bounds,query,epsilon)) return;
        if(collides(entry.common,query,epsilon)) {
            reporter.count("OVERLAP",entry.count);detailsAt(root,query,current,reporter,epsilon,control,work);return;
        }
        countAt(entry.left,query,current,reporter,epsilon,control,work);
        if(collides(entry.rectangle,query,epsilon)) {
            reporter.count("OVERLAP",1);
            if(reporter.mayImprove(current,entry.id)) {
                const Rect overlap=intersection(entry.rectangle,query); // 当前精确交集。
                reporter.detail({"OVERLAP",{std::min(current,entry.id),std::max(current,entry.id)},overlap,overlap.area(),"实体正面积重叠"});
            }
        }
        countAt(entry.right,query,current,reporter,epsilon,control,work);
    }
    /** @brief 比较区间树稳定键。 @param left 左条目编号。 @param right 右条目编号。 @return 左键是否更小。 */
    bool less(int left, int right) const {
        return std::tie(entries_[left].rectangle.ly, entries_[left].id) < std::tie(entries_[right].rectangle.ly, entries_[right].id);
    }
    /** @brief 合并键区间有序的两个 treap。 @param left 左树。 @param right 右树。 @return 合并根。 */
    int merge(int left, int right) {
        if (left < 0) return right;
        if (right < 0) return left;
        if (entries_[left].priority < entries_[right].priority) {
            entries_[left].right = merge(entries_[left].right, right);
            update(left);
            return left;
        }
        entries_[right].left = merge(left, entries_[right].left);
        update(right);
        return right;
    }
    /** @brief 插入一个尚未激活的新条目。 @param root 子树根。 @param added 新条目。 @return 新根。 */
    int insertAt(int root, int added) {
        if (root < 0) return added;
        if (less(added, root)) {
            entries_[root].left = insertAt(entries_[root].left, added);
            const int child = entries_[root].left; // 可能需向上旋转的左孩子。
            if (entries_[child].priority < entries_[root].priority) {
                entries_[root].left = entries_[child].right;
                entries_[child].right = root;
                update(root);
                update(child);
                return child;
            }
        } else {
            entries_[root].right = insertAt(entries_[root].right, added);
            const int child = entries_[root].right; // 可能需向上旋转的右孩子。
            if (entries_[child].priority < entries_[root].priority) {
                entries_[root].right = entries_[child].left;
                entries_[child].left = root;
                update(root);
                update(child);
                return child;
            }
        }
        update(root);
        return root;
    }
    /** @brief 删除指定稳定条目。 @param root 当前子树。 @param removed 被删除条目。 @return 新根。 */
    int eraseAt(int root, int removed) {
        if (root < 0) return root;
        if (root == removed) return merge(entries_[root].left, entries_[root].right);
        if (less(removed, root)) entries_[root].left = eraseAt(entries_[root].left, removed);
        else entries_[root].right = eraseAt(entries_[root].right, removed);
        update(root);
        return root;
    }
    /** @brief 查询子树内的正面积 y 相交条目。 @param root 子树根。 @param query 查询矩形。 @param ids 命中的NodeId输出。 无返回值。 */
    void queryAt(int root, const Rect& query, std::vector<NodeId>& ids) const {
        if (root < 0 || maximum(root) <= query.ly) return;
        const Entry& entry = entries_[root]; // 当前树节点。
        queryAt(entry.left, query, ids);
        if (entry.rectangle.ly < query.uy && entry.rectangle.uy > query.ly) ids.push_back(entry.id);
        if (entry.rectangle.ly < query.uy) queryAt(entry.right, query, ids);
    }
public:
    /** @brief 插入活动区间。 @param rectangle 实体矩形。 @param id 稳定节点编号。 @return 用于删除的内部句柄。 */
    int insert(const Rect& rectangle, NodeId id) {
        std::uint64_t mixed = id + 0x9e3779b97f4a7c15ULL; // SplitMix64 的确定性散列种子，不消耗算法随机数。
        mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
        mixed ^= mixed >> 31U;
        const int added = static_cast<int>(entries_.size()); // 新增条目稳定句柄。
        Entry entry; // 完整初始化的新活动条目，后续update生成子树统计。
        entry.rectangle=rectangle;entry.id=id;entry.priority=mixed;entry.maxUpper=rectangle.uy;
        entries_.push_back(entry);
        update(added);
        root_ = insertAt(root_, added);
        return added;
    }
    /** @brief 删除已过 x 右边界的区间。 @param handle insert 返回的有效句柄。 无返回值。 */
    void erase(int handle) { root_ = eraseAt(root_, handle); }
    /** @brief 将活动几何相交计数与有界详情加入报告。
     * @param rectangle 当前扫描框。 @param id 稳定ID。 @param reporter 输出收集器。
     * @param epsilon 距离容差。 @param control 可空取消。 无返回值。 */
    void report(const Rect& rectangle,NodeId id,Reporter& reporter,double epsilon,ControlToken* control) const {
        std::size_t work=0; // 当前查询访问节点数，控制取消批次。
        countAt(root_,rectangle,id,reporter,epsilon,control,work);
    }
    /** @brief 获取当前活动 y 相交对象。 @param rectangle 查询矩形。 @return 候选稳定NodeId。 */
    std::vector<NodeId> query(const Rect& rectangle) const {
        std::vector<NodeId> ids; // 查询命中集合。
        queryAt(root_, rectangle, ids);
        return ids;
    }
};

/** @brief 判断实例方向是否允许。 @param direction 实例方向。 @param allowed 允许集合。 @param fallback 空集合时允许的行方向。 @return 是否匹配。 */
bool allowedDirection(Orientation direction, const std::vector<Orientation>& allowed, Orientation fallback) {
    return allowed.empty() ? direction == fallback : std::find(allowed.begin(), allowed.end(), direction) != allowed.end();
}

/** @brief 保证数值指标有限。 @param value 当前累计量。 @param name 指标名称。 无返回值；溢出抛 NUMERIC_FAILURE。 */
void requireFinite(double value, const std::string& name) {
    if (!std::isfinite(value)) Throw("NUMERIC_FAILURE", "指标累加产生非有限值", name);
}

} // namespace

/** @brief 用实际朝向引脚重新计算线长和参考位移，完整参数契约见公共声明。 */
Metrics ComputeMetrics(const Snapshot& snapshot, const Snapshot* baseline) {
    const DesignDatabase& database = requireDatabase(snapshot); // 当前只读拓扑。
    if (baseline && (baseline->datasetId != snapshot.datasetId || requireDatabase(*baseline).nodes.size() != database.nodes.size())) Throw("INPUT_INVALID", "跨数据集或不同节点数量不能比较", "metrics", true);
    for (std::size_t index = 0; index < database.nodes.size(); ++index) { // 包括无网络节点在内的完整位置有效性检查。
        if (!finiteRect(nodeRect(database.nodes[index], snapshot.positions[index]))) Throw("NUMERIC_FAILURE", "指标快照包含无效物理几何", database.nodes[index].name);
        if (baseline && !finiteRect(nodeRect(baseline->database->nodes[index], baseline->positions[index]))) Throw("NUMERIC_FAILURE", "位移基准包含无效物理几何", database.nodes[index].name);
    }
    Metrics metrics = snapshot.metrics; // 保留同轮密度、溢出和耗时，覆盖重新计算的量。
    double hpwl = 0.0; // 无权真实线长累计，DU。
    double weighted = 0.0; // 加权真实线长累计，DU。
    for (const Net& net : database.nets) { // 当前拓扑网络，空/单引脚贡献零。
        if (net.pinIds.empty()) continue;
        double minX = std::numeric_limits<double>::infinity(); // 网络引脚最小横坐标，DU。
        double minY = std::numeric_limits<double>::infinity(); // 网络引脚最小纵坐标，DU。
        double maxX = -std::numeric_limits<double>::infinity(); // 网络引脚最大横坐标，DU。
        double maxY = -std::numeric_limits<double>::infinity(); // 网络引脚最大纵坐标，DU。
        for (const PinId id : net.pinIds) { // 网络引脚稳定编号。
            if (id >= database.pins.size()) Throw("REFERENCE_MISSING", "线长网络引用不存在的引脚", net.name);
            const Vec2 point = pinPosition(database, snapshot.positions, database.pins[id]); // 实际朝向引脚坐标，DU。
            requireFinite(point.x, net.name);
            requireFinite(point.y, net.name);
            minX = std::min(minX, point.x); minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x); maxY = std::max(maxY, point.y);
        }
        const double length = (maxX - minX) + (maxY - minY); // 当前网络真实HPWL，DU。
        hpwl += length;
        weighted += net.weight * length;
    }
    requireFinite(hpwl, "hpwl"); requireFinite(weighted, "weightedHpwl");
    metrics.hpwl = hpwl;
    metrics.weightedHpwl = weighted;
    metrics.meanDisplacement.reset(); metrics.maxDisplacement.reset(); metrics.quadraticDisplacement.reset();metrics.l1Displacement.reset();
    metrics.macroDisplacement.reset();metrics.standardDisplacement.reset();metrics.fixedDisplacement.reset();
    if (baseline) {
        double sum = 0.0; // 可移动真实节点欧氏位移和，DU。
        double maximum = 0.0; // 最大位移，DU。
        double quadratic = 0.0; // 位移平方和，DU²。
        double l1=0; // 可移动对象L1位移总和，DU。
        DisplacementMetrics macros; // 宏分类位移，mean在循环中暂存欧氏总和。
        DisplacementMetrics standards; // 标准分类位移，mean在循环中暂存总和。
        DisplacementMetrics fixed; // 固定对象分类位移。
        std::size_t count = 0; // 可比可移动实体数。
        for (std::size_t index = 0; index < database.nodes.size(); ++index) { // 对应NodeId的节点索引。
            const Node& node = database.nodes[index]; // 当前真实节点。
            const Rect current = nodeRect(node, snapshot.positions[index]); // 当前方向包围盒，DU。
            const Rect previous = nodeRect(baseline->database->nodes[index], baseline->positions[index]); // 参考方向包围盒，DU。
            const double dx = (current.lx - previous.lx) + (current.width() - previous.width()) * 0.5; // 中心横向变化，DU。
            const double dy = (current.ly - previous.ly) + (current.height() - previous.height()) * 0.5; // 中心纵向变化，DU。
            const double distance = std::hypot(dx, dy); // 当前节点欧氏位移，DU。
            requireFinite(distance, node.name);
            DisplacementMetrics& category=node.fixed?fixed:node.kind==NodeKind::Macro?macros:standards; // 互斥对象类别。
            ++category.count;category.mean+=distance;category.maximum=std::max(category.maximum,distance);category.l1+=std::abs(dx)+std::abs(dy);category.quadratic+=dx*dx+dy*dy;
            if(node.fixed||node.kind==NodeKind::Terminal) continue;
            sum += distance; maximum = std::max(maximum, distance); quadratic += dx * dx + dy * dy;
            l1+=std::abs(dx)+std::abs(dy);
            ++count;
        }
        requireFinite(sum, "displacement"); requireFinite(quadratic, "quadraticDisplacement");
        if (count != 0) { metrics.meanDisplacement = sum / static_cast<double>(count); metrics.maxDisplacement = maximum; metrics.quadraticDisplacement = quadratic;metrics.l1Displacement=l1; }
        for(DisplacementMetrics* category:{&macros,&standards,&fixed}) if(category->count) category->mean/=category->count; // 各分类独立归一化。
        if(macros.count) metrics.macroDisplacement=macros;
        if(standards.count) metrics.standardDisplacement=standards;
        if(fixed.count) metrics.fixedDisplacement=fixed;
    }
    return metrics;
}

/** @brief 比较同数据集阶段的真实指标和逐对象中心位移，完整参数契约见公共声明。 */
StageComparison CompareStages(const Snapshot& before, const Snapshot& after) {
    const DesignDatabase& database = requireDatabase(before); // 参考只读数据库。
    requireDatabase(after);
    if (before.datasetId != after.datasetId || before.positions.size() != after.positions.size()) Throw("INPUT_INVALID", "不能比较不同数据集", "CompareStages", true);
    StageComparison result; // 当前比较结果，不修改输入快照。
    result.before = ComputeMetrics(before);
    result.after = ComputeMetrics(after, &before);
    if (result.before.hpwl && *result.before.hpwl != 0.0) result.hpwlImprovement = (*result.before.hpwl - *result.after.hpwl) / *result.before.hpwl;
    result.displacement.reserve(database.nodes.size());
    for (std::size_t index = 0; index < database.nodes.size(); ++index) { // 对应稳定NodeId的索引。
        const Rect first = nodeRect(database.nodes[index], before.positions[index]); // 参考矩形，DU。
        const Rect second = nodeRect(after.database->nodes[index], after.positions[index]); // 当前矩形，DU。
        const double distance = std::hypot((second.lx - first.lx) + (second.width() - first.width()) * 0.5,
                                          (second.ly - first.ly) + (second.height() - first.height()) * 0.5); // 中心欧氏位移，DU。
        requireFinite(distance, database.nodes[index].name);
        result.displacement.push_back(distance);
    }
    return result;
}

/** @brief 独立几何检查：扫描线碰撞、行并集覆盖、方向/站点及固定基线，完整契约见公共声明。 */
ViolationReport CheckLegality(const Snapshot& snapshot, const std::string& scope, std::size_t limit, ControlToken* control) {
    const DesignDatabase& database = requireDatabase(snapshot); // 当前快照只读拓扑。
    if (scope != "all" && scope != "macro") Throw("INPUT_INVALID", "检查范围必须为 all 或 macro", scope, true);
    Reporter reporter(scope, limit); // 全计数且有界明细收集器。
    const double epsilon = collisionTolerance(database); // 两轴碰撞容差 εc，DU。
    const double geometryEpsilon = geometryTolerance(database); // core尺度容差 εg，DU。
    std::vector<Rect> rows; // 原始行矩形集合，用作允许区域并集，DU。
    for (const RowSegment& row : database.rows) rows.push_back(rowRect(row)); // 当前行拓扑仅生成只读矩形。
    const RectIndex rowIndex(rows); // 行区域空间查询树，避免每个实例扫描所有行。
    std::vector<Rect> toleratedRows=rows; // 每轴扩张εc后的允许区域，仅用于边界距离比较。
    for(Rect& row:toleratedRows) {row.lx-=epsilon;row.ly-=epsilon;row.ux+=epsilon;row.uy+=epsilon;} // 当前允许行，DU。
    const RectIndex toleratedRowIndex(toleratedRows); // 统一物理距离容差下的行并集查询树。
    std::vector<Rect> obstacles; // 硬阻挡范围，索引与数据库障碍顺序一致。
    for (const Obstacle& obstacle : database.obstacles) obstacles.push_back(obstacle.rect); // 当前输入障碍。
    const RectIndex obstacleIndex(obstacles); // 硬阻挡空间查询树。
    std::vector<Rect> rectangles(database.nodes.size()); // 按NodeId存储的有效物理包围盒，DU。
    std::vector<NodeId> entities; // 当前scope中有面积且数值有限的实体编号。
    std::vector<long double> occupiedWidths(database.rows.size(), 0.0L); // 每条原始行内完整标准单元占用跨度总和，DU。
    std::vector<std::vector<NodeId>> rowMembers(database.rows.size()); // 用于容量违规定位的标准单元稳定编号。
    for (std::size_t index = 0; index < database.nodes.size(); ++index) { // 稳定NodeId对应索引。
        if (control && index % 4096 == 0) control->checkCancelled();
        const Node& node = database.nodes[index]; // 当前拓扑节点。
        const Position& position = snapshot.positions[index]; // 当前几何位置。
        const bool selected = scope == "all" || node.kind == NodeKind::Macro || node.fixed; // 当前范围是否检查该对象几何。
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(node.width) || !std::isfinite(node.height) || node.width < 0 || node.height < 0 || !validDirection(position.orientation)) {
            reporter.add({"NON_FINITE", {node.id}, {}, 1.0, "节点坐标、尺寸或方向无效: " + node.name});
            continue;
        }
        const Rect rectangle = nodeRect(node, position); // 方向后的当前实体范围，DU。
        if (!finiteRect(rectangle)) { reporter.add({"NON_FINITE", {node.id}, {}, 1.0, "物理包围盒超出有限范围: " + node.name}); continue; }
        rectangles[index] = rectangle;
        if (node.fixed) {
            if (index >= database.originalPositions.size()) Throw("INPUT_INVALID", "固定基线缺失", node.name);
            const Position& original = database.originalPositions[index]; // 固定输入位置基线，DU。
            if (position.x != original.x || position.y != original.y || position.orientation != original.orientation) reporter.add({"FIXED_CHANGED", {node.id}, rectangle, 1.0, "固定对象位置或方向改变: " + node.name});
        }
        if (!selected) continue;
        if (position.orientation != node.orientation || (!node.allowedOrientations.empty() && !allowedDirection(position.orientation, node.allowedOrientations, node.orientation))) reporter.add({"ORIENTATION", {node.id}, rectangle, 1.0, "实例方向不满足输入/master约束: " + node.name});
        if (rectangle.width() > 0 && rectangle.height() > 0) entities.push_back(node.id);
        if (!node.fixed && node.kind != NodeKind::Terminal) {
            std::vector<Rect> covered; // 行并集与实体相交的有效区域，DU。
            for (const std::size_t rowId : rowIndex.query(rectangle)) covered.push_back(intersection(rectangle, rows[rowId])); // 当前相交行的原始索引。
            const double missing = std::max(0.0, rectangle.area() - unionArea(covered)); // 位于允许区域外的面积，DU²。
            std::vector<Rect> toleratedCoverage; // 应用距离容差后仍需被行并集覆盖的范围。
            for(const std::size_t rowId:toleratedRowIndex.query(rectangle)) toleratedCoverage.push_back(intersection(rectangle,toleratedRows[rowId])); // 容差行索引。
            const double remaining=std::max(0.0,rectangle.area()-unionArea(toleratedCoverage)); // 扩张允许区域后真正未覆盖面积，DU²。
            const double rounding=32*std::numeric_limits<double>::epsilon()*std::max(1.0,rectangle.area()); // 仅抑制面积求和的机器舍入，不替代物理εc。
            if (remaining > rounding) reporter.add({"OUT_OF_BOUNDS", {node.id}, rectangle, missing, "实体未完全落入可放置行并集: " + node.name});
        }
        if (rectangle.width() > 0 && rectangle.height() > 0) {
            for (const std::size_t obstacleId : obstacleIndex.query(rectangle)) { // 当前空间命中的硬阻挡索引。
                if (collides(rectangle, obstacles[obstacleId], epsilon)) reporter.add({"HARD_BLOCKAGE", {node.id}, intersection(rectangle, obstacles[obstacleId]), intersection(rectangle, obstacles[obstacleId]).area(), "硬阻挡: " + database.obstacles[obstacleId].source});
            }
        }
        if (scope == "all" && !node.fixed && node.kind == NodeKind::Standard) {
            bool onRow = false; // 是否有容纳完整物理框且高度/底边匹配的行。
            bool onSite = false; // 是否存在几何匹配且站点残差合格的行。
            bool orientationMatches = false; // 是否存在几何匹配且方向允许的行。
            bool supported = false; // 是否存在单高度、等宽站点、整数宽度的支持行。
            double closestSite = std::numeric_limits<double>::infinity(); // 最小站点DU残差。
            Rect expanded = rectangle; // 略扩张后查询接近行边界的候选，避免查询漏掉浮点偏差。
            expanded.lx -= geometryEpsilon; expanded.ly -= geometryEpsilon;
            expanded.ux += geometryEpsilon; expanded.uy += geometryEpsilon;
            for (const std::size_t rowId : rowIndex.query(expanded)) { // 当前候选行原始索引。
                const RowSegment& row = database.rows[rowId]; // 候选行约束。
                const double siteEpsilon = siteTolerance(database, row.sitePitch); // 当前行站点容差，DU。
                const double widthSites = rectangle.width() / row.sitePitch; // 单元物理宽度折合站点数。
                const bool compatible = std::abs(rectangle.height() - row.height) <= siteEpsilon && std::abs(row.siteWidth - row.sitePitch) <= siteEpsilon && std::abs(rectangle.width() - std::round(widthSites) * row.sitePitch) <= siteEpsilon; // 支持子集判定。
                supported = supported || compatible;
                if (std::abs(rectangle.ly - row.y) > siteEpsilon || std::abs(rectangle.height() - row.height) > siteEpsilon || rectangle.lx < rows[rowId].lx - siteEpsilon || rectangle.ux > rows[rowId].ux + siteEpsilon) continue;
                if (!onRow) { occupiedWidths[rowId] += rectangle.width(); rowMembers[rowId].push_back(node.id); }
                onRow = true;
                const double residual = std::abs((rectangle.lx - row.xOrigin) - std::round((rectangle.lx - row.xOrigin) / row.sitePitch) * row.sitePitch); // 使用DU而非无量纲站点误差。
                closestSite = std::min(closestSite, residual);
                onSite = onSite || residual <= siteEpsilon;
                orientationMatches = orientationMatches || allowedDirection(position.orientation, row.allowedOrientations, row.orientation);
            }
            if (!onRow) reporter.add({"OFF_ROW", {node.id}, rectangle, 1.0, "标准单元不在单个匹配行内: " + node.name});
            if (onRow && !onSite) reporter.add({"OFF_SITE", {node.id}, rectangle, closestSite, "横坐标未对齐原始站点网格: " + node.name});
            if (onRow && !orientationMatches) reporter.add({"ORIENTATION", {node.id}, rectangle, 1.0, "标准单元方向与候选行不兼容: " + node.name});
            if (!supported) reporter.add({"UNSUPPORTED_OBJECT", {node.id}, rectangle, 1.0, "不存在单高度整数站点宽度支持行: " + node.name});
        }
    }
    if (scope == "all") {
        for (std::size_t index = 0; index < database.rows.size(); ++index) { // 逐条独立核对原始行容量。
            const double epsilonSite = siteTolerance(database, database.rows[index].sitePitch); // 该行跨度比较容差，DU。
            const long double excess = occupiedWidths[index] - rows[index].width(); // 物理跨度超额，DU。
            if (excess > epsilonSite) reporter.add({"ROW_CAPACITY", rowMembers[index], rows[index], static_cast<double>(excess), "原始行段内标准单元总占用跨度超过物理容量"});
        }
    }
    /** @brief 按左边界和稳定ID排序扫描对象。 @param left 左ID。 @param right 右ID。 @return 确定性x顺序。 */
    std::sort(entities.begin(), entities.end(), [&rectangles](NodeId left, NodeId right) {
        return rectangles[left].lx < rectangles[right].lx || (rectangles[left].lx == rectangles[right].lx && left < right);
    });
    /** @brief x轴过期事件；优先队列中最小右边界先处理。 */
    struct Expiry {
        double right = 0.0; ///< 实体右边界，DU。
        int handle = -1; ///< 活动y区间树删除句柄。
        /** @brief 形成最小堆。 @param other 另一事件。 @return 是否更晚过期。 */
        bool operator>(const Expiry& other) const { return right > other.right || (right == other.right && handle > other.handle); }
    };
    ActiveIntervals active; // x扫描时的增广活动y区间树。
    std::priority_queue<Expiry, std::vector<Expiry>, std::greater<Expiry>> expiry; // 活动实体最早右边界。
    std::size_t processed = 0; // 已处理实体数，用于取消批次。
    for (const NodeId id : entities) { // 当前x扫描实体编号。
        if (control && processed++ % 4096 == 0) control->checkCancelled();
        const Rect& rectangle = rectangles[id]; // 当前实体范围，DU。
        while (!expiry.empty() && expiry.top().right <= rectangle.lx) { active.erase(expiry.top().handle); expiry.pop(); }
        active.report(rectangle,id,reporter,epsilon,control);
        expiry.push({rectangle.ux, active.insert(rectangle, id)});
    }
    if (control) control->checkCancelled();
    return reporter.finish();
}

} // namespace placement
