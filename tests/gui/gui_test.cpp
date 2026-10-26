#include "window.hpp"
#include <QtTest>
#include <QAction>
#include <QGraphicsScene>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <future>
#include <algorithm>

using namespace placement;
using namespace placement::gui;
namespace {
/** @brief 构建只读GUI测试快照，三对象重叠可检验层次命中。
 * @param stage 测试阶段。 @param revision 几何修订。 @return 完整小型快照，无I/O。 */
std::shared_ptr<const Snapshot> fixture(const std::string& stage="initial",std::uint64_t revision=0){
    auto db=std::make_shared<DesignDatabase>(); ///< 测试共享只读拓扑源。
    db->datasetId="gui-fixture";
    for(int i=0;i<3;++i){ // i 为实体稳定ID。
        Node node; ///< 测试真实实体。
        node.id=i;node.name="node"+std::to_string(i);node.width=i==0?8:2;node.height=i==0?8:2;node.kind=i==0?NodeKind::Macro:NodeKind::Standard;node.fixed=i==2;db->nodes.push_back(node);db->originalPositions.push_back({2,2,Orientation::N});
    }
    for(int i=0;i<16;++i){ // i 为可放置行ID。
        RowSegment row; ///< 16站点可放置行。
        row.id=i;row.y=i;row.height=1;row.siteWidth=1;row.sitePitch=1;row.numSites=16;db->rows.push_back(row);
    }
    auto snapshot=std::make_shared<Snapshot>(); ///< 完整几何测试源。
    snapshot->database=db;snapshot->datasetId=db->datasetId;snapshot->runId="run";snapshot->stage=stage;snapshot->revision=revision;snapshot->positions=db->originalPositions;snapshot->termination="converged";snapshot->metrics.hpwl=0;snapshot->metrics.overflow=0.4;
    return snapshot;
}
/** @brief 创建后台渲染包但关闭不相关合法性检查。 @param stage 阶段。 @param revision 修订。 @return 不可变空间包。 */
std::shared_ptr<const RenderPacket> packet(const std::string& stage="initial",std::uint64_t revision=0){ControlToken token; /* token 为同步小夹具控制。 */return BuildRenderPacket(fixture(stage,revision),token,false);}
}
/** @brief offscreen Qt GUI契约测试；所有控件在主线程，无任意sleep。 */
class GuiTest:public QObject {
    Q_OBJECT
private slots:
    /** @brief TC-UI-03：加载失败保留数据集、选择与坐标，不替换为半成品；无输入，无返回值。 */
    void failedLoadRetainsCurrentDataset(){
        MainWindow window; // 保留成功数据的主窗口。
        const auto original=packet(); // 事务失败前的只读几何。
        window.install(original,true);window.primaryCanvas()->setSelection({1});
        InputSpec bad; // 明确缺失的新输入。
        bad.auxPath="/does-not-exist/placement.aux";window.load(bad);
        QTRY_COMPARE(window.state(),RunState::Failed);
        QCOMPARE(window.primaryCanvas()->packet(),original);QCOMPARE(window.primaryCanvas()->selection(),QSet<qulonglong>({1}));
    }
    /** @brief TC-UI-14：热图电荷只含源轮真实/静态值，12 DU²不被filler补成64；无输入，无返回值。 */
    void heatmapExcludesTaskPrivateFiller(){
        ControlToken token; // 后台数值测试令牌。
        InputSpec input; // 带两个3×2真实单元的80 DU²规则行域。
        input.auxPath=std::string(PLACEMENT_SOURCE_DIR)+"/tests/fixtures/tiny/tiny.aux";
        Config config; // 目标d=.8时存在52 DU²的filler，不应出现在只读热图中。
        config.gridNx=32;config.gridNy=32;
        const auto initial=std::make_shared<const Snapshot>(PrepareInitial(LoadDesign(input,config,token),config,"heat",token)); // 同轮几何。
        Mailbox mailbox; // 完整网格输出。
        HeatJob(initial,config)(token,mailbox);
        const auto batch=mailbox.take(); // 单个源身份网格。
        QCOMPARE(batch.heatmaps.size(),std::size_t(1));
        double charge=0; // 独立面积积分，DU²。
        for(double density:batch.heatmaps.front()->density)charge+=density*80/1024; // density为每bin无量纲值，32×32等面积bin。
        QVERIFY(std::abs(charge-12)<1e-10);QCOMPARE(batch.heatmaps.front()->revision,initial->revision);
    }
    /** @brief TC-UI-14：同ID阶段切换同步刷新属性，不遗留旧坐标；无输入，无返回值。 */
    void stageSwitchRefreshesSelectedProperties(){
        MainWindow window; // 主线程阶段切换窗口。
        window.install(packet(),true);
        auto moved=std::make_shared<Snapshot>(*fixture("global",1)); // 同ID新阶段具有不同精确坐标。
        moved->positions[1].x=9;
        ControlToken token; // 渲染包测试令牌。
        window.install(BuildRenderPacket(moved,token,false),true);window.primaryCanvas()->setSelection({1});
        auto* attributes=window.findChild<QTreeWidget*>("attributes"); // 同轮属性表。
        auto* stages=window.findChild<QComboBox*>("leftStage"); // 左侧完整阶段选择。
        QVERIFY(stages);
        stages->setCurrentIndex(stages->findData("initial"));
        QVERIFY(attributes->findItems("2, 2",Qt::MatchExactly,1).size()>0);
        stages->setCurrentIndex(stages->findData("global"));
        QVERIFY(attributes->findItems("9, 2",Qt::MatchExactly,1).size()>0);
    }
    /** @brief TC-UI-09/17/18：暂停时独立导出预览/完成阶段，导出失败保留主任务并允许取消再启动；无输入，无返回值。 */
    void independentExportWhilePaused(){
        MainWindow window; // 主任务和导出任务均归此窗口所有。
        QTemporaryDir directory; // 自动清理测试导出。
        QVERIFY(directory.isValid());window.install(packet(),true);window.install(packet("global",1),true);
        std::promise<void> entered; // 工作线程已进入安全点前的确定性通知。
        auto ready=entered.get_future(); // 主线程非阻塞观察屏障。
        /** @brief 请求暂停后等待取消，捕获entered仅在测试存活期间借用。 @param token 主任务令牌。 @param mailbox 不产生伪终态。 无返回值。 */
        window.startBackground([&entered](ControlToken& token,Mailbox& mailbox){Q_UNUSED(mailbox)token.requestPause();entered.set_value();token.safePoint();},"gui-fixture","export-main");
        QTRY_VERIFY(ready.wait_for(std::chrono::seconds(0))==std::future_status::ready);window.pauseRun();QTRY_COMPARE(window.state(),RunState::Paused);
        auto preview=std::make_shared<Snapshot>(*fixture("preview",7)); // 指定iteration/revision的非最终源。
        preview->termination.clear();preview->iteration=12;
        ControlToken token; // 渲染测试令牌。
        window.install(BuildRenderPacket(preview,token,false),false);
        const std::string previewPath=(directory.path()+"/preview.json").toStdString(); // 预览原子输出。
        QVERIFY(window.exportSnapshot(false,"placement",previewPath));
        QTRY_VERIFY(!window.findChild<QAction*>("cancelExport")->isEnabled());
        const QJsonObject saved=QJsonDocument::fromJson(QByteArray::fromStdString(ReadText(previewPath))).object(); // 独立JSON读取。
        QCOMPARE(saved.value("stage").toString(),QString("preview"));QCOMPARE(saved.value("iteration").toInt(),12);QCOMPARE(window.state(),RunState::Paused);
        QVERIFY(window.exportSnapshot(false,"placement",previewPath)); // 默认拒绝覆盖，真实导出错误不能终止主任务。
        QTRY_VERIFY(!window.findChild<QAction*>("cancelExport")->isEnabled());QCOMPARE(window.state(),RunState::Paused);
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(ReadText(previewPath))).object(),saved);
        auto* stages=window.findChild<QComboBox*>("leftStage"); // 切换到已完成global作为第二种源。
        QVERIFY(stages);stages->setCurrentIndex(stages->findData("global"));
        const std::string completePath=(directory.path()+"/global.json").toStdString(); // 完整阶段输出。
        QVERIFY(window.exportSnapshot(false,"placement",completePath));QTRY_VERIFY(!window.findChild<QAction*>("cancelExport")->isEnabled());
        QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(ReadText(completePath))).object().value("stage").toString(),QString("global"));
        window.cancelRun();QTRY_COMPARE(window.state(),RunState::Cancelled);
        /** @brief 重启独立主任务，验证取消后的线程与邮箱已释放。 @param token 新令牌。 @param mailbox 新任务终态。 无返回值。 */
        window.startBackground([](ControlToken& token,Mailbox& mailbox){Q_UNUSED(token)mailbox.finishTask({},true);},"gui-fixture","after-cancel");QTRY_COMPARE(window.state(),RunState::Completed);
    }
    /** @brief 真实CANCELLED终态不能显示Failed；无输入，无返回值。 */
    void cancelledTerminalIsNotFailure(){
        MainWindow window; // 后台结果消费测试窗口。
        /** @brief 构造唯一取消终态。 @param token 未使用令牌。 @param mailbox 当前任务邮箱。 无返回值。 */
        window.startBackground([](ControlToken& token,Mailbox& mailbox){Q_UNUSED(token)RunResult result; /* result为应用原样返回的取消。 */result.runId="cancel-case";result.termination="CANCELLED";result.exitCode=130;mailbox.finish(result);},"gui-fixture","cancel-case");
        QTRY_COMPARE(window.state(),RunState::Cancelled);
    }
    /** @brief 左右违规数量不同仍按绑定源定位，终态后不退回preview；无输入，无返回值。 */
    void comparisonViolationSourceAndFinalPreview(){
        MainWindow window; // 同数据集双视图窗口。
        window.resize(1280,900);window.show();window.install(packet(),true);window.install(packet("global",1),true);
        window.findChild<QAction*>("compareStages")->trigger();
        QCOMPARE(window.findChild<QTreeWidget*>("comparisonMetrics")->topLevelItemCount(),5);
        auto right=std::make_shared<RenderPacket>(*packet("global",2)); // 右侧独有的一条违规。
        right->violations.records={{"OVERLAP",{1},{10,10,12,12},1,"right-only"}};right->violations.totalCount=1;
        window.install(right,true);
        auto* list=window.findChild<QTreeWidget*>("violations"); // 明确绑定右侧源的列表。
        QCOMPARE(list->topLevelItemCount(),1);
        list->itemDoubleClicked(list->topLevelItem(0),0); // 左报告为空也不能越界读取。
        QCOMPARE(window.secondaryCanvas()->selection(),QSet<qulonglong>({1}));
        window.install(packet("preview",3),false);window.install(packet("global",4),true);
        window.findChild<QAction*>("compareStages")->trigger();
        QVERIFY(window.primaryCanvas()->packet()->snapshot->stage!="preview");
    }
    /** @brief 新数据集事务提交清空双画布和旧查找ID；无输入，无返回值。 */
    void loadingNewDatasetClearsComparisonAndSearch(){
        MainWindow window; // 先持有旧数据的窗口。
        window.install(packet(),true);window.install(packet("global",1),true);window.findChild<QAction*>("compareStages")->trigger();
        auto* results=window.findChild<QTreeWidget*>("searchResults"); // 模拟上一个项目的搜索结果。
        auto* old=new QTreeWidgetItem(results,{"999","stale"}); // 当前数据集中无效ID。
        old->setData(0,Qt::UserRole,QVariant::fromValue<qulonglong>(999));old->setData(0,Qt::UserRole+1,"old");
        results->itemDoubleClicked(old,0); // 未更新列表时也必须安全忽略越界ID。
        InputSpec input; // 新项目加载事务。
        input.auxPath=std::string(PLACEMENT_SOURCE_DIR)+"/tests/fixtures/tiny/tiny.aux";window.load(input);
        QTRY_COMPARE(window.state(),RunState::Ready);
        QCOMPARE(results->topLevelItemCount(),0);
        QVERIFY(window.primaryCanvas()->packet()->snapshot->datasetId!="gui-fixture");QVERIFY(!window.secondaryCanvas()->packet());
    }
    /** @brief TC-UI：数学/场景负y、分块命中、图层和重叠优先级。无参数、无返回值。 */
    void coordinatesAndHitIndex(){
        const auto render=packet(); ///< 只读小型空间包。
        QCOMPARE(SceneRect({2,3,7,11}),QRectF(2,-11,5,8));
        const auto hits=HitTest(*render,{3,3},0,Layers{}); ///< 同点三个重叠候选。
        QCOMPARE(hits.size(),std::size_t(3));QCOMPARE(hits[0],NodeId(2));QCOMPARE(hits[1],NodeId(1));QCOMPARE(hits[2],NodeId(0));
        Layers layers; ///< 测试隐藏固定图层。
        layers.fixed=false;const auto hidden=HitTest(*render,{3,3},0,layers); ///< 隐藏后的命中。
        QCOMPARE(hidden.size(),std::size_t(2));QCOMPARE(hidden.front(),NodeId(1));
        QVERIFY(HitTest(*render,{50,50},0,Layers{}).empty());
        Canvas canvas; ///< 主线程批量画布。
        canvas.resize(640,480);canvas.setPacket(render);QVERIFY(canvas.scene()->items().size()<=4097);
    }
    /** @brief TC-UI：同尺度数学相机同步不依赖两个viewport大小且无信号循环。无参数、无返回值。 */
    void cameraAndSelectionSynchronization(){
        Canvas first; ///< 第一视图。
        Canvas second; ///< 不同尺寸的第二视图。
        first.resize(640,480);second.resize(400,300);first.setPacket(packet());second.setPacket(packet("global",1));first.show();second.show();QCoreApplication::processEvents();
        /** @brief 只转发数学相机；捕获second主线程引用。 @param x DU。 @param y DU。 @param du DU/px。 无返回值。 */
        connect(&first,&Canvas::cameraChanged,&second,[&second](double x,double y,double du){second.setCamera({{x,y},du});});
        /** @brief 稳定ID选择同步；捕获second主线程引用。 @param ids 选择集。 无返回值。 */
        connect(&first,&Canvas::selectionChanged,&second,[&second](QSet<qulonglong> ids){second.setSelection(std::move(ids),false);});
        QSignalSpy loop(&second,&Canvas::cameraChanged); ///< 确保被动相机安装不发回环。
        first.locate({2,2,4,4});
        const Camera a=first.camera(); ///< 数学相机A。
        const Camera b=second.camera(); ///< 数学相机B。
        QVERIFY(std::abs(a.center.x-b.center.x)<=b.duPerPixel*1.5);QVERIFY(std::abs(a.center.y-b.center.y)<=b.duPerPixel*1.5);QCOMPARE(a.duPerPixel,b.duPerPixel);QCOMPARE(loop.count(),0);
        first.setSelection({1,2});QCOMPARE(second.selection(),QSet<qulonglong>({1,2}));
        const QPoint selectedPixel=first.mapFromScene(QPointF(3,-3)); ///< 数学点映射至viewport像素。
        QTest::mouseClick(first.viewport(),Qt::LeftButton,Qt::NoModifier,selectedPixel);QCOMPARE(first.selection(),QSet<qulonglong>({2}));
    }
    /** @brief TC-UI：旧身份/旧序列被拒绝，容量1预览覆盖且终态不丢。无参数、无返回值。 */
    void mailboxIdentityCapacityAndTerminal(){
        Mailbox mailbox; ///< 双线程安全有界传递器。
        mailbox.reset("gui-fixture","run");
        Event event; ///< 小型指标事件。
        event.datasetId="gui-fixture";event.runId="run";event.sequence=10;event.kind="metrics";event.metrics.hpwl=123;
        QVERIFY(mailbox.accept(event));QVERIFY(!mailbox.accept(event));event.runId="old";event.sequence=11;QVERIFY(!mailbox.accept(event));event.runId="run";event.datasetId="other";QVERIFY(!mailbox.accept(event));
        for(std::uint64_t revision=0;revision<50;++revision)mailbox.packet(packet("global",revision),false); // revision 为被覆盖预览修订。
        QCOMPARE(mailbox.previewCount(),std::size_t(1));
        RunResult result; ///< 必须可靠交付的终态。
        result.runId="run";result.termination="completed";mailbox.finish(result);result.termination="cancelled";mailbox.finish(result);
        const MailBatch batch=mailbox.take(); ///< 原子消费批次。
        QVERIFY(batch.preview);QCOMPARE(batch.preview->snapshot->revision,std::uint64_t(49));QVERIFY(batch.result.has_value());QCOMPARE(batch.result->termination,std::string("completed"));QCOMPARE(batch.metrics.size(),std::size_t(1));
        QVERIFY(!mailbox.take().result.has_value());QCOMPARE(mailbox.previewCount(),std::size_t(0));
        for(const auto& stage:std::array<std::string,5>{"initial","global","macro_legal","standard_legal","diagnostic"})mailbox.packet(packet(stage),true); // stage 为测试完成槽上限。
        QCOMPARE(mailbox.take().stages.size(),std::size_t(4));
    }
    /** @brief TC-UI：曲线与日志容量固定，压缩保留最终点及旧极值。无参数、无返回值。 */
    void boundedCurvesAndLog(){
        std::deque<MetricPoint> metrics; ///< 有界压缩输入。
        for(std::uint64_t i=0;i<10000;++i){MetricPoint point; /* point 为一条小型指标。 */point.iteration=i;point.metrics.hpwl=i==41?1000000:double(i);point.metrics.overflow=i==45?99:0.1;metrics.push_back(point);} // i 为模拟迭代序号。
        CompactMetrics(metrics);QVERIFY(metrics.size()<=5000);QCOMPARE(metrics.back().iteration,std::uint64_t(9999));
        QVERIFY(std::any_of(metrics.begin(),metrics.end(),[](const MetricPoint& point){return point.metrics.hpwl.value_or(0)==1000000;}));
        QVERIFY(std::any_of(metrics.begin(),metrics.end(),[](const MetricPoint& point){return point.metrics.overflow.value_or(0)==99;}));
        ConvergencePlot plot; ///< 主线程曲线控件。
        plot.append(std::move(metrics));QVERIFY(plot.pointCount()<=5000);
        Mailbox mailbox; ///< 尚未被主线程消费的日志缓冲。
        mailbox.reset("d","r");for(std::uint64_t i=0;i<6000;++i){Event event; /* event 为高频小型日志。 */event.datasetId="d";event.runId="r";event.kind="log";event.sequence=i;event.message="info";QVERIFY(mailbox.accept(event));} // i 为日志序列。
        const auto batch=mailbox.take(); ///< 有界待消费日志。
        QCOMPARE(batch.logs.size(),std::size_t(5000));QCOMPARE(batch.droppedLogs,std::uint64_t(1000));
    }
    /** @brief TC-UI：空项目按钮、共享JSON校验、完成阶段树与只读参数。无参数、无返回值。 */
    void windowStatesAndConfiguration(){
        MainWindow window; ///< 主线程完整空项目窗口。
        QCOMPARE(window.state(),RunState::Empty);QVERIFY(!window.findChild<QAction*>("runGlobal")->isEnabled());QVERIFY(!window.findChild<QAction*>("exportResult")->isEnabled());
        QVERIFY(!window.applyConfiguration(QStringLiteral("{\"target_density\":0}")));
        QVERIFY(window.applyConfiguration(QString::fromStdString(SerializeConfig(Config{}))));
        window.install(packet(),true);QCOMPARE(window.state(),RunState::Ready);QVERIFY(window.findChild<QAction*>("runGlobal")->isEnabled());QVERIFY(!window.findChild<QAction*>("runMacro")->isEnabled());
        window.install(packet("global",1),true);QVERIFY(window.findChild<QAction*>("runMacro")->isEnabled());QCOMPARE(window.findChild<QTreeWidget*>("stageTree")->topLevelItemCount(),4);
        window.primaryCanvas()->setSelection({1});QVERIFY(window.findChild<QTreeWidget*>("attributes")->topLevelItemCount()>5);
        QVERIFY(window.findChild<QTreeWidget*>("attributes")->editTriggers()==QAbstractItemView::NoEditTriggers);
        if(!qEnvironmentVariableIsEmpty("PLACEMENT_SCREENSHOT_PATH")) {
            window.show();QApplication::processEvents();window.primaryCanvas()->fit();QApplication::processEvents();QVERIFY(window.grab().save(qEnvironmentVariable("PLACEMENT_SCREENSHOT_PATH"))); // 使用真实展开后的viewport尺寸适应几何；截图路径由调用者明确提供。
        }
    }
    /** @brief TC-UI：假worker用受控屏障确认暂停与参数锁定，无任意sleep。无参数、无返回值。 */
    void controlledPauseAndResume(){
        MainWindow window; ///< 主线程控制状态机。
        std::promise<void> entered; ///< 后台进入工作函数的确定性通知。
        std::promise<void> release; ///< 主线程允许后台进入安全点的屏障。
        auto enteredFuture=entered.get_future(); ///< 主线程非阻塞观察进入状态。
        const auto gate=release.get_future().share(); ///< 工作线程等待的共享屏障。
        /** @brief 屏障后抵达完整安全点；捕获entered引用及gate值。
         * @param token 工作令牌。 @param mailbox 有界结果。 无返回值。 */
        window.startBackground([&entered,gate](ControlToken& token,Mailbox& mailbox){entered.set_value();gate.wait();token.safePoint();mailbox.finishTask({},true);},"gui-fixture","pause-run");
        QTRY_VERIFY(enteredFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready);
        window.pauseRun();QCOMPARE(window.state(),RunState::Pausing);QVERIFY(window.findChild<QPlainTextEdit*>("configEditor")->isReadOnly());release.set_value();
        QTRY_COMPARE(window.state(),RunState::Paused);window.resumeRun();QCOMPARE(window.state(),RunState::Running);QTRY_COMPARE(window.state(),RunState::Completed);
    }
    /** @brief TC-UI：关闭协作取消能唤醒已暂停假worker，主事件循环仍然运行。无参数、无返回值。 */
    void cooperativeCloseWithFakeWorker(){
        MainWindow window; ///< 活跃任务窗口。
        std::promise<void> entered; ///< 后台安全点前的屏障通知。
        auto ready=entered.get_future(); ///< 非阻塞进入状态观察器。
        /** @brief 自行进入可取消的安全点等待；捕获entered借用屏障。
         * @param token 线程令牌。 @param mailbox 任务结果。 无返回值。 */
        window.startBackground([&entered](ControlToken& token,Mailbox& mailbox){Q_UNUSED(mailbox)token.requestPause();entered.set_value();token.safePoint();},"gui-fixture","close-run");
        QTRY_VERIFY(ready.wait_for(std::chrono::seconds(0))==std::future_status::ready);
        QSignalSpy closed(&window,&MainWindow::shutdownReady); ///< 全部线程结束确认信号。
        window.requestShutdown(true);QTRY_COMPARE(closed.count(),1);
    }
    /** @brief TC-UI：大网络后台遍历全部引脚求中心，确定性抽样上限5000。无参数、无返回值。 */
    void sampledNetworkAndStaleHeat(){
        auto base=std::make_shared<Snapshot>(*fixture()); ///< 可构造大网络的测试快照。
        auto db=std::make_shared<DesignDatabase>(*base->database); ///< 测试拓扑独占副本。
        Net net; ///< 10001个引脚的大网络。
        net.name="huge";for(std::uint64_t i=0;i<10001;++i){Pin pin; /* pin 为偏移变化的同节点引脚。 */pin.id=i;pin.nodeId=1;pin.netId=0;pin.offset={double(i),0};db->pins.push_back(pin);net.pinIds.push_back(i);} // i 为大网络引脚序号。
        db->nets.push_back(net);base->database=db;
        ControlToken token; ///< 网络后台测试取消令牌。
        Mailbox mailbox; ///< 小型高亮输出。
        NetworkJob(base,0)(token,mailbox);const auto batch=mailbox.take(); ///< 后台抽样结果。
        QCOMPARE(batch.networks.size(),std::size_t(1));QCOMPARE(batch.networks.front()->pins.size(),std::size_t(5000));QCOMPARE(batch.networks.front()->originalCount,std::size_t(10001));QVERIFY(std::abs(batch.networks.front()->mean.x-5003)<1e-9);
        Canvas canvas; ///< 身份隔离测试画布。
        canvas.setPacket(packet());auto heat=std::make_shared<Heatmap>(); ///< 旧修订热图应被忽略而不污染场景。
        heat->dataset="old";canvas.setHeatmap(heat);QCOMPARE(canvas.packet()->snapshot->datasetId,std::string("gui-fixture"));
    }
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
