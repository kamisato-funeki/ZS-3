#pragma once
#include "gui_model.hpp"
#include <QObject>
#include <QThread>

namespace placement::gui {
/** @brief 一个任务的纯后台函数；输入分别为线程安全控制令牌及有界邮箱。 */
using BackgroundJob=std::function<void(ControlToken&,Mailbox&)>;
/** @brief moveToThread 的唯一执行对象；不持有窗口、场景或QPixmap。 */
class TaskWorker:public QObject {
    Q_OBJECT
public:
    /** @brief 构造任务私有资源。 @param job 后台函数。 @param token 共享令牌。 @param mailbox 共享有界传递器。 */
    TaskWorker(BackgroundJob job,std::shared_ptr<ControlToken> token,std::shared_ptr<Mailbox> mailbox);
public slots:
    /** @brief 执行任务并捕获异常；无参数、无返回值，必发finished。 */
    void execute();
signals:
    /** @brief 后台函数已退出；无参数、无返回值，不携大快照。 */
    void finished();
private:
    BackgroundJob job_; ///< 工作线程独占执行函数。
    std::shared_ptr<ControlToken> token_; ///< 主线程可直接原子控制。
    std::shared_ptr<Mailbox> mailbox_; ///< 有界共享邮箱。
};
/** @brief 主线程任务生命周期控制器；关闭必须协作等待finished，不阻塞主线程。 */
class TaskController:public QObject {
    Q_OBJECT
public:
    /** @brief 构造空闲控制器。 @param parent 主线程所有者。 */
    explicit TaskController(QObject* parent=nullptr);
    /** @brief 启动独占工作线程。 @param job 后台函数。 @param dataset 事件身份。 @param run 任务身份。
     * @return 空闲时true；活动时false且不修改原任务。 */
    bool start(BackgroundJob job,std::string dataset,std::string run);
    /** @brief 直接请求线程安全取消；无参数、无返回值。 */
    void cancel();
    /** @brief 直接请求安全点暂停；无参数、无返回值。 */
    void pause();
    /** @brief 直接恢复安全点等待；无参数、无返回值。 */
    void resume();
    /** @brief 是否仍有活动线程。 @return 活动状态。 */
    bool running() const;
    /** @brief 工作线程是否已确认暂停。 @return token安全点确认状态。 */
    bool paused() const;
    /** @brief 消费有界邮箱。 @return 独占批次。 */
    MailBatch take();
signals:
    /** @brief QThread已停止且可安全销毁窗口；无参数、无返回值。 */
    void idle();
private:
    QThread* thread_=nullptr; ///< 主线程拥有；finished后延迟删除。
    std::shared_ptr<ControlToken> token_; ///< 任务私有控制令牌。
    std::shared_ptr<Mailbox> mailbox_; ///< 任务私有邮箱，200ms消费。
};
/** @brief 构造事务导入任务。 @param input AUX或LEF/DEF。 @param config 有效配置。 @param run 新身份。
 * @return 后台加载、准备、指标及渲染包函数。 */
BackgroundJob LoadJob(InputSpec input,Config config,std::string run);
/** @brief 构造placement重载任务。 @param database 当前数据库。 @param path placement路径。
 * @param run 本次GUI事务身份。 @return 不改变旧项目的后台函数。 */
BackgroundJob ReloadJob(std::shared_ptr<const DesignDatabase> database,std::string path,std::string run);
/** @brief 构造共用流水线任务。 @param request 应用请求。 @return 只通过有界邮箱交付预览及终态的函数。 */
BackgroundJob PipelineJob(RunRequest request);
/** @brief 构造独立原子导出。 @param snapshot 源快照。 @param format placement/bmp/gds/report。
 * @param path 目标路径。 @param config 共享有效显示配置。 @param overwrite 显式覆盖许可。 @return 独立后台函数。 */
BackgroundJob ExportJob(std::shared_ptr<const Snapshot> snapshot,std::string format,std::string path,Config config,bool overwrite);
/** @brief 构造精确密度热图任务。 @param snapshot 源快照。 @param config 对应配置。 @return 后台函数，失败不改主算法状态。 */
BackgroundJob HeatJob(std::shared_ptr<const Snapshot> snapshot,Config config);
/** @brief 构造至多5000条示意星形线。 @param snapshot 源快照。 @param net 稳定网络ID。
 * @param requestId 选择请求序号，默认0供同步调用。 @return 后台函数，全部引脚参与平均中心。 */
BackgroundJob NetworkJob(std::shared_ptr<const Snapshot> snapshot,NetId net,std::uint64_t requestId=0);
} // namespace placement::gui
