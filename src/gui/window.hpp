#pragma once
#include "canvas.hpp"
#include "workers.hpp"
#include <QMainWindow>
#include <QTimer>
#include <map>

class QAction; class QTreeWidget; class QPlainTextEdit; class QComboBox; class QLineEdit; class QLabel; class QDockWidget; class QPushButton;
namespace placement::gui {
/** @brief 内存有界收敛曲线；HPWL与overflow分图绘制，主线程拥有。 */
class ConvergencePlot:public QWidget {
    Q_OBJECT
public:
    /** @brief 构造空曲线。 @param parent 主线程父控件。 */
    explicit ConvergencePlot(QWidget* parent=nullptr);
    /** @brief 附加小型指标并压缩旧桶。 @param points 新指标。 无返回值。 */
    void append(std::deque<MetricPoint> points);
    /** @brief 清空新任务曲线；无参数、无返回值。 */
    void clear();
    /** @brief 获取有界点数。 @return 不超过5000。 */
    std::size_t pointCount() const;
protected:
    /** @brief 按独立单位分图绘制。 @param event 重绘区域。 无返回值。 */
    void paintEvent(QPaintEvent* event) override;
    /** @brief 悬停显示最近完整指标。 @param event 鼠标位置。 无返回值。 */
    void mouseMoveEvent(QMouseEvent* event) override;
private:
    std::deque<MetricPoint> points_; ///< 至多5000个曲线点。
};
/** @brief 完整只读Qt主窗口；所有控件与场景仅主线程触达。 */
class MainWindow:public QMainWindow {
    Q_OBJECT
public:
    /** @brief 构造空项目窗口。 @param parent 可空主线程父控件。 */
    explicit MainWindow(QWidget* parent=nullptr);
    /** @brief 读取可测试状态。 @return 主任务状态，独立于导出。 */
    RunState state() const;
    /** @brief 获取主画布。 @return 主线程拥有的非空画布。 */
    Canvas* primaryCanvas() const;
    /** @brief 获取对比画布。 @return 主线程拥有的非空画布。 */
    Canvas* secondaryCanvas() const;
    /** @brief 从输入请求开始事务，供菜单或测试调用。 @param input AUX或LEF/DEF。 无返回值。 */
    void load(InputSpec input);
    /** @brief 安装共享有效配置文本。 @param text JSON。 @return 校验成功；失败不改变配置。 */
    bool applyConfiguration(const QString& text);
    /** @brief 提交后台已完成包，供应用内部和假worker测试调用。
     * @param packet 完整包。 @param completed true加入成功阶段。 无返回值。 */
    void install(std::shared_ptr<const RenderPacket> packet,bool completed);
    /** @brief 测试/自动化入口，启动提供的后台函数并进入running。 @param job 后台纯任务。 @param dataset 身份。 @param run 身份。 无返回值。 */
    void startBackground(BackgroundJob job,std::string dataset,std::string run);
    /** @brief 从当前画布捕获源并独立后台导出，供对话框与程序化操作共用。
     * @param right true选右画布。 @param format placement/bmp/gds/report。 @param path 输出路径。
     * @param overwrite 显式覆盖许可。 @return 已启动为true；无源/关闭中/已有导出时false，主算法状态不变。 */
    bool exportSnapshot(bool right,std::string format,std::string path,bool overwrite=false);
    /** @brief 请求异步关闭；主线程事件循环持续工作。 @param confirmed 已经用户确认取消。 无返回值。 */
    void requestShutdown(bool confirmed);
public slots:
    /** @brief 200ms消费槽与暂停确认；无参数、无返回值。 */
    void consume();
    /** @brief 请求安全点暂停；无参数、无返回值。 */
    void pauseRun();
    /** @brief 恢复核心；无参数、无返回值。 */
    void resumeRun();
    /** @brief 取消主任务；无参数、无返回值。 */
    void cancelRun();
signals:
    /** @brief 主任务状态变更。 @param state 新状态。 无返回值。 */
    void stateChanged(int state);
    /** @brief 全部后台线程安全停止，可以销毁；无参数、无返回值。 */
    void shutdownReady();
protected:
    /** @brief 活动线程时采用异步协作关闭。 @param event 关闭事件。 无返回值。 */
    void closeEvent(QCloseEvent* event) override;
private:
    /** @brief 构建主线程控件和连接；无参数、无返回值。 */
    void buildUi();
    /** @brief 更新按钮、参数锁定和状态栏。 @param state 新主状态。 无返回值。 */
    void setState(RunState state);
    /** @brief 选择AUX文件事务；无参数、无返回值。 */
    void openAux();
    /** @brief 多LEF与单DEF事务；无参数、无返回值。 */
    void openLefDef();
    /** @brief 重载结果并核对当前数据库指纹；无参数、无返回值。 */
    void reload();
    /** @brief 以阶段前置条件构造应用请求。 @param stage global/macro/standard/all。 无返回值。 */
    void run(std::string stage);
    /** @brief 选择格式、源和单位后独立后台导出；无参数、无返回值。 */
    void exportResult();
    /** @brief 保存当前视口截图，名称明确区别标准布局BMP；无参数、无返回值。 */
    void screenshot();
    /** @brief 切换双视图及默认阶段；无参数、无返回值。 */
    void toggleComparison();
    /** @brief 更新项目树、阶段选择和摘要。 无参数、无返回值。 */
    void refreshStages();
    /** @brief 将阶段选择应用到画布。 @param right 是否右视图。 无返回值。 */
    void chooseStage(bool right);
    /** @brief 展示只读对象属性和所属网络。 @param ids 当前稳定ID选择。 无返回值。 */
    void properties(QSet<qulonglong> ids);
    /** @brief 执行精确/前缀名称查找的下一页。 @param reset 是否从头开始。 无返回值。 */
    void findName(bool reset=true);
    /** @brief 更新有界违规列表。 @param packet 当前完整几何。 无返回值。 */
    void showViolations(const RenderPacket& packet);
    /** @brief 旧网络请求结束后调度最近一次选择，隐藏时不调度；无输入、无返回值。 */
    void scheduleNetwork();
    /** @brief 使用两份已计算快照指标刷新阶段比较表，无算法或I/O；无输入、无返回值。 */
    void updateComparisonMetrics();
    /** @brief 统一刷新图层并按需后台计算热图；无参数、无返回值。 */
    void updateLayers();
    /** @brief 过滤并刷新5000条环形日志；无参数、无返回值。 */
    void refreshLogs();
    /** @brief 向环形日志写入一条消息。 @param text 可读消息。 无返回值。 */
    void log(QString text);
    /** @brief 获取与源快照绑定的运行配置。 @param snapshot 源。 @return 配置副本。 */
    Config sourceConfig(const Snapshot& snapshot) const;
    RunState state_=RunState::Empty; ///< 主运行状态。
    Config config_; ///< 当前未运行配置，默认来自core共享类型。
    Config activeConfig_; ///< 本次任务冻结配置。
    std::map<std::string,Config> stageConfig_; ///< 四个完成阶段各自冻结配置。
    std::map<std::string,std::shared_ptr<const RenderPacket>> stages_; ///< 完成阶段最多四份。
    std::shared_ptr<const RenderPacket> preview_; ///< 至多一份已显示预览。
    std::shared_ptr<const DesignDatabase> database_; ///< 当前事务成功的只读数据集。
    InputSpec input_; ///< 当前成功输入来源。
    InputSpec pendingInput_; ///< 加载成功才生效的输入事务。
    std::string runId_; ///< 当前唯一主任务身份。
    std::string heatRequestIdentity_; ///< 最近请求的双视图热图身份，避免失败无限重试。
    std::uint64_t networkRequest_=0; ///< 网络选择/隐藏递增序号。
    std::optional<NetId> pendingNetwork_; ///< 等待旧任务退出后的最新网络选择，null表示隐藏。
    std::shared_ptr<const Snapshot> violationSnapshot_; ///< 当前违规列表绑定源，拒绝旧列表ID定位。
    std::vector<Violation> listedViolations_; ///< 当前源的有界详情副本，双击不读取另一画布报告。
    QString outputDirectory_; ///< 完整流式指标和日志保存位置。
    std::string exportId_; ///< 当前独立导出身份。
    std::string loadingDataset_; ///< 加载前保留的项目身份，可空。
    bool loading_=false; ///< 主任务是否导入/重载事务。
    bool reloading_=false; ///< placement重载不清空同数据集其它阶段。
    bool closing_=false; ///< 已确认协作关闭。
    bool closeAllowed_=false; ///< 所有任务已停，可完成closeEvent。
    bool comparison_=false; ///< 是否显示第二画布。
    bool processing_=false; ///< 防止consume重入。
    std::size_t searchCursor_=0; ///< 名称候选下一页起始NodeId。
    TaskController mainTask_; ///< 唯一主任务线程及令牌。
    TaskController exportTask_; ///< 独立导出任务，最多一个。
    TaskController heatTask_; ///< 可取消后台精确密度。
    TaskController networkTask_; ///< 可取消网络抽样后台任务。
    QTimer timer_; ///< 主线程200ms消费与安全点确认。
    Canvas* left_=nullptr; ///< 中央视图，主线程拥有。
    Canvas* right_=nullptr; ///< 对比视图，主线程拥有。
    QComboBox* leftStage_=nullptr; ///< 左视图阶段选择。
    QComboBox* rightStage_=nullptr; ///< 右视图阶段选择。
    QTreeWidget* stageTree_=nullptr; ///< 四阶段树。
    QLabel* summary_=nullptr; ///< 数据集审计摘要。
    QTreeWidget* attributes_=nullptr; ///< 只读对象属性。
    QTreeWidget* comparisonTable_=nullptr; ///< 同数据集双视图指标绝对值、差值与改善率。
    QTreeWidget* violations_=nullptr; ///< 有界违规列表。
    QComboBox* violationFilter_=nullptr; ///< 类型过滤。
    QComboBox* networks_=nullptr; ///< 所选对象所属网络。
    QLineEdit* search_=nullptr; ///< 区分大小写名称查询。
    QComboBox* searchMode_=nullptr; ///< 精确/前缀模式。
    QTreeWidget* searchResults_=nullptr; ///< 最多100候选，支持翻页。
    QPlainTextEdit* configEditor_=nullptr; ///< 统一JSON高级参数表单。
    QPushButton* applyConfig_=nullptr; ///< 校验并提交有效配置。
    QDockWidget* configDock_=nullptr; ///< 参数停靠面板。
    QPlainTextEdit* logView_=nullptr; ///< 有界只读日志视图。
    QLineEdit* logFilter_=nullptr; ///< 日志文本过滤。
    QComboBox* logLevel_=nullptr; ///< 日志严重度过滤。
    std::deque<QString> logs_; ///< 最新5000条日志。
    ConvergencePlot* plot_=nullptr; ///< 5000点收敛曲线。
    QLabel* metrics_=nullptr; ///< 同轮状态指标。
    QLabel* exportStatus_=nullptr; ///< 独立导出状态及路径。
    QAction* openAux_=nullptr; ///< BookShelf打开。
    QAction* openLef_=nullptr; ///< LEF/DEF打开。
    QAction* reload_=nullptr; ///< 结果重载。
    QAction* global_=nullptr; ///< 全局启动。
    QAction* macro_=nullptr; ///< 宏启动。
    QAction* standard_=nullptr; ///< 标准启动。
    QAction* all_=nullptr; ///< 全流程启动。
    QAction* pause_=nullptr; ///< 安全点暂停。
    QAction* resume_=nullptr; ///< 恢复。
    QAction* cancel_=nullptr; ///< 取消主任务。
    QAction* export_=nullptr; ///< 独立结果导出。
    QAction* cancelExport_=nullptr; ///< 只取消导出。
    std::array<QAction*,7> layerActions_{}; ///< 标准/宏/固定/行/bin/密度/违规开关。
};
} // namespace placement::gui
