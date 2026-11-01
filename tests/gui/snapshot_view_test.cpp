#include "window.hpp"
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QGraphicsScene>
#include <QLabel>
#include <QSplitter>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>
#include <QSplitterHandle>

using namespace placement;
using namespace placement::gui;
namespace {
/** @brief 构建相互分离、可用填色判定真实绘制的阶段。 @param stage 阶段名。 @param revision 修订。 @return 只读几何包；无文件写入。 */
std::shared_ptr<const RenderPacket> visualPacket(const std::string& stage,std::uint64_t revision) {
    auto database=std::make_shared<DesignDatabase>(); // 各阶段共享身份的微型拓扑，单位DU。
    database->datasetId="visual-regression";
    Node macro; // 独立绿色宏，避免标准单元遮挡。
    macro.id=0;macro.name="macro";macro.kind=NodeKind::Macro;macro.width=20;macro.height=20;
    database->nodes.push_back(macro);database->originalPositions.push_back({20,20,Orientation::N});
    Node cell; // 独立红色标准单元。
    cell.id=1;cell.name="cell";cell.width=10;cell.height=10;
    database->nodes.push_back(cell);database->originalPositions.push_back({65,65,Orientation::N});
    RowSegment row; // 100×100 DU可显示域。
    row.height=100;row.siteWidth=1;row.sitePitch=1;row.numSites=100;database->rows.push_back(row);
    auto snapshot=std::make_shared<Snapshot>(); // 指标与几何保持同一修订。
    snapshot->database=database;snapshot->datasetId=database->datasetId;snapshot->runId="visual-run";
    snapshot->stage=stage;snapshot->revision=revision;snapshot->iteration=revision;
    snapshot->positions=database->originalPositions;snapshot->metrics.hpwl=revision*100;
    snapshot->metrics.elapsedMs=revision*10;snapshot->termination="converged";
    if(stage=="global")snapshot->metrics.overflow=.0856393;
    snapshot->adjustments.resize(revision);snapshot->positions[1].x+=revision;
    ControlToken token; // 仅生成GUI空间包，不运行算法。
    return BuildRenderPacket(snapshot,token,false);
}
/** @brief 统计截图中实体填色，排除文字、背景和边框。 @param canvas 待检查画布。 @param color 实体标准填色。 @return 匹配的物理像素数。 */
int filledPixels(Canvas& canvas,QColor color) {
    const QImage image=canvas.viewport()->grab().toImage(); // 实际viewport绘制结果，非领域几何模拟。
    int count=0; // 目标填色像素计数。
    for(int y=0;y<image.height();++y) { // 截图像素行。
        for(int x=0;x<image.width();++x) { // 截图像素列。
            if(image.pixelColor(x,y)==color)++count;
        }
    }
    return count;
}
/** @brief 读取数学位置对应的真实viewport像素，用于区分左右不同几何。 @param canvas 当前实际画布。 @param point 数学DU坐标。 @return 对应填色，视口外返回无效色。 */
QColor layoutColor(Canvas& canvas,Vec2 point){
    const QPoint pixel=canvas.mapFromScene(QPointF(point.x,-point.y)); // 独立查询已安装相机的屏幕映射。
    if(!canvas.viewport()->rect().contains(pixel))return {};
    const QImage image=canvas.viewport()->grab().toImage(); // 含真实布局的viewport截图。
    return image.pixelColor(qRound(pixel.x()*image.devicePixelRatio()),qRound(pixel.y()*image.devicePixelRatio()));
}
}
/** @brief TC-UI-14/15：快照显示一致性与双视图真实像素回归；全部Qt对象由主线程拥有。 */
class SnapshotViewTest:public QObject {
    Q_OBJECT
private slots:
    /** @brief TC-UI-14：终态重装与消费交错产生乱序批次时仍显示最新正式阶段；无输入、无返回值。 */
    void terminalBatchUsesStageOrder(){
        MainWindow window; // 接收确定性乱序阶段的GUI窗口。
        window.install(visualPacket("initial",0),true);
        const auto initial=visualPacket("initial",0); // 模拟终态重装时较晚加入的initial。
        const auto global=visualPacket("global",1); // 较早进入待消费槽的中间阶段。
        const auto standard=visualPacket("standard_legal",24); // 本次最新正式完成阶段。
        /** @brief 捕获三个不可变包模拟合法乱序批次；不派生线程之外的任务。 @param token 未使用控制令牌。 @param mailbox 受控单批次事件。 无返回值。 */
        window.startBackground([initial,global,standard](ControlToken& token,Mailbox& mailbox){
            Q_UNUSED(token)
            mailbox.packet(global,true);mailbox.packet(standard,true);mailbox.packet(initial,true);
            RunResult result; // 唯一成功终态。
            result.runId="visual-run";result.termination="completed";mailbox.finish(result);
        },"visual-regression","visual-run");
        QTRY_COMPARE(window.state(),RunState::Completed);
        QCOMPARE(window.primaryCanvas()->packet(),standard);
        QCOMPARE(window.findChild<QLabel*>("snapshotMetrics")->text(),QString("standard_legal · i=24 · HPWL 2400 · overflow — · 240 ms"));
    }
    /** @brief TC-UI-07/14：检查侧、类型过滤及共享快照的定位目标始终一致；无输入、无返回值。 */
    void comparisonReportsFollowInspection(){
        MainWindow window; // 左右报告及属性共同绑定检查侧。
        auto left=std::make_shared<RenderPacket>(*visualPacket("initial",0)); // 左侧特有违规报告。
        left->violations.records={{"LEFT_ONLY",{1},{65,65,75,75},1,"left"}};left->violations.totalCount=1;left->violations.countByType["LEFT_ONLY"]=1;
        auto right=std::make_shared<RenderPacket>(*visualPacket("global",1)); // 右侧不同类型及不同区域。
        right->violations.records={{"RIGHT_ONLY",{1},{66,65,76,75},2,"right"}};right->violations.totalCount=1;right->violations.countByType["RIGHT_ONLY"]=1;
        window.show();QCoreApplication::processEvents();window.install(left,true);window.install(right,true);
        window.findChild<QAction*>("compareStages")->trigger();
        auto* list=window.findChild<QTreeWidget*>("violations"); // 实际报告列表。
        auto* filter=window.findChild<QComboBox*>("violationFilter"); // 随快照重建的类型选项。
        QCOMPARE(list->topLevelItem(0)->text(0),QString("RIGHT_ONLY"));QVERIFY(list->headerItem()->text(0).contains("右视图 global"));
        filter->setCurrentIndex(filter->findText("RIGHT_ONLY"));
        auto* selector=window.findChild<QComboBox*>("leftStage"); // 改变左源时切换检查侧。
        selector->setCurrentIndex(selector->findData("global"));selector->setCurrentIndex(selector->findData("initial"));
        QCOMPARE(list->topLevelItemCount(),1);QCOMPARE(list->topLevelItem(0)->text(0),QString("LEFT_ONLY"));
        QCOMPARE(filter->currentIndex(),0);QVERIFY(list->headerItem()->text(0).contains("左视图 initial"));
        selector->setCurrentIndex(selector->findData("global")); // 两边共享同一个只读包，场景所有权仍独立。
        window.secondaryCanvas()->setSelection({1});
        QSignalSpy leftCamera(window.primaryCanvas(),&Canvas::cameraChanged); // 被动同步不能冒充左侧主动定位。
        QSignalSpy rightCamera(window.secondaryCanvas(),&Canvas::cameraChanged); // 报告源为右侧，应只发右侧主动定位信号。
        list->itemDoubleClicked(list->topLevelItem(0),0);
        QCOMPARE(rightCamera.count(),1);QCOMPARE(leftCamera.count(),0);
        QVERIFY(window.findChild<QTreeWidget*>("attributes")->findItems("右视图 global · i=1 · r=1",Qt::MatchExactly,1).size()>0);
    }
    /** @brief TC-UI-14：安装/阶段选择/单双切换共用真实快照指标，空值与零严格区分；无输入、无返回值。 */
    void stageMetricsAndSummaryFollowDisplayedPackets() {
        MainWindow window; // 保存四阶段及预览的窗口。
        window.install(visualPacket("initial",0),true);window.install(visualPacket("global",1),true);
        window.install(visualPacket("macro_legal",2),true);window.install(visualPacket("standard_legal",24),true);
        auto* selector=window.findChild<QComboBox*>("leftStage"); // 左侧独立阶段选择器。
        auto* metrics=window.findChild<QLabel*>("snapshotMetrics"); // 实际底部指标，断言内容而非控件存在。
        auto* summary=window.findChild<QLabel*>("projectSummary"); // 含快照准备审计的项目摘要。
        QVERIFY(selector&&metrics&&summary);
        const QStringList stages{"initial","global","macro_legal","standard_legal"}; // 完成阶段顺序。
        const QStringList expected{
            "initial · i=0 · HPWL 0 · overflow — · 0 ms",
            "global · i=1 · HPWL 100 · overflow 0.0856393 · 10 ms",
            "macro_legal · i=2 · HPWL 200 · overflow — · 20 ms",
            "standard_legal · i=24 · HPWL 2400 · overflow — · 240 ms"}; // 各快照独立已知数值，不用界面格式函数做判定器。
        for(int i=0;i<stages.size();++i){ // i为预期阶段及指标行号。
            selector->setCurrentIndex(selector->findData(stages[i]));QCOMPARE(metrics->text(),expected[i]);
            QVERIFY(summary->text().contains("当前视图 "+stages[i]));
            QVERIFY(summary->text().contains(QString("位置调整 %1 项").arg(i==3?24:i)));
        }
        window.findChild<QAction*>("compareStages")->trigger();
        QCOMPARE(metrics->text(),"左："+expected[0]+"\n右："+expected[3]);
        QVERIFY(summary->text().contains("左视图 initial"));QVERIFY(summary->text().contains("右视图 standard_legal"));
        auto* rightSelector=window.findChild<QComboBox*>("rightStage"); // 右阶段与左阶段互相独立。
        QVERIFY(rightSelector);rightSelector->setCurrentIndex(rightSelector->findData("global"));
        QCOMPARE(metrics->text(),"左："+expected[0]+"\n右："+expected[1]);
        window.secondaryCanvas()->setSelection({1});
        auto* attributes=window.findChild<QTreeWidget*>("attributes"); // 必须显示右侧坐标和明确源身份。
        QVERIFY(!attributes->findItems("66, 65",Qt::MatchExactly,1).isEmpty());
        QVERIFY(!attributes->findItems("右视图 global · i=1 · r=1",Qt::MatchExactly,1).isEmpty());
        selector->setCurrentIndex(selector->findData("macro_legal"));
        QCOMPARE(metrics->text(),"左："+expected[2]+"\n右："+expected[1]);
        QVERIFY(!attributes->findItems("67, 65",Qt::MatchExactly,1).isEmpty());
        auto* comparison=window.findChild<QTreeWidget*>("comparisonMetrics"); // 可空比较值的来源也必须正确。
        QCOMPARE(comparison->topLevelItem(0)->text(1),QString("200"));QCOMPARE(comparison->topLevelItem(0)->text(2),QString("100"));
        QCOMPARE(comparison->topLevelItem(2)->text(1),QString("—"));QCOMPARE(comparison->topLevelItem(2)->text(2),QString("0.0856393"));
        window.findChild<QAction*>("compareStages")->trigger();QCOMPARE(metrics->text(),expected[2]);
        window.install(visualPacket("preview",7),false);
        QCOMPARE(metrics->text(),QString("preview · i=7 · HPWL 700 · overflow — · 70 ms"));
        selector->setCurrentIndex(selector->findData("initial"));QCOMPARE(metrics->text(),expected[0]);
        auto zero=std::make_shared<Snapshot>(*visualPacket("global",5)->snapshot); // HPWL/overflow真实零、耗时缺失组合。
        zero->metrics.hpwl=0;zero->metrics.overflow=0;zero->metrics.elapsedMs.reset();
        ControlToken token; // GUI包生成令牌。
        window.install(BuildRenderPacket(zero,token,false),true);
        QCOMPARE(metrics->text(),QString("global · i=5 · HPWL 0 · overflow 0 · — ms"));
        auto missing=std::make_shared<Snapshot>(*visualPacket("standard_legal",8)->snapshot); // 全部未计算指标应保留空值。
        missing->metrics=Metrics{};window.install(BuildRenderPacket(missing,token,false),true);
        QCOMPARE(metrics->text(),QString("standard_legal · i=8 · HPWL — · overflow — · — ms"));
    }
    /** @brief TC-UI-17：逐侧导出实际选择包，不因检查面板或另一侧阶段改变来源；无输入、无返回值。 */
    void comparisonExportsExplicitSource() {
        MainWindow window; // 双源导出窗口。
        QTemporaryDir directory; // 本用例独立输出目录。
        QVERIFY(directory.isValid());window.install(visualPacket("initial",0),true);window.install(visualPacket("standard_legal",24),true);
        window.findChild<QAction*>("compareStages")->trigger();
        for(bool right:{false,true}){ // right显式选择导出源，而非焦点或最后安装包。
            const std::string path=(directory.path()+(right?"/right.json":"/left.json")).toStdString(); // 每侧原子输出文件。
            QVERIFY(window.exportSnapshot(right,"placement",path));QTRY_VERIFY(!window.findChild<QAction*>("cancelExport")->isEnabled());
            const QJsonObject saved=QJsonDocument::fromJson(QByteArray::fromStdString(ReadText(path))).object(); // 独立读取落盘身份。
            QCOMPARE(saved.value("stage").toString(),right?QString("standard_legal"):QString("initial"));
            QCOMPARE(saved.value("iteration").toInt(),right?24:0);QCOMPARE(saved.value("revision").toInt(),right?24:0);
        }
    }
    /** @brief 首次开启比较须同时画出背景与两类实体，无需用户缩放；无输入、无返回值。 */
    void firstComparisonPaintsBothLayouts() {
        MainWindow window; // 首次显示后再安装阶段，贴近真实加载顺序。
        window.show();QCoreApplication::processEvents();
        window.install(visualPacket("initial",0),true);window.install(visualPacket("standard_legal",24),true);
        window.findChild<QAction*>("compareStages")->trigger();QCoreApplication::processEvents();
        QVERIFY(filledPixels(*window.primaryCanvas(),QColor(35,155,86))>50);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(35,155,86))>50);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(220,76,76))>10);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(20,28,38))>50);
        QCOMPARE(layoutColor(*window.primaryCanvas(),{70,70}),QColor(220,76,76)); // 左initial标准单元位于65..75 DU。
        QCOMPARE(layoutColor(*window.secondaryCanvas(),{94,70}),QColor(220,76,76)); // 右standard_legal标准单元已移到89..99 DU。
        QCOMPARE(layoutColor(*window.secondaryCanvas(),{70,70}),QColor(32,43,55)); // 右侧不能复用左侧旧几何或复制左图。
        QVERIFY(window.primaryCanvas()->transform().isInvertible());QVERIFY(window.secondaryCanvas()->transform().isInvertible());
        QVERIFY(window.primaryCanvas()->scene()!=window.secondaryCanvas()->scene());
        QSignalSpy echo(window.primaryCanvas(),&Canvas::cameraChanged); // 被动同步与resize不得产生反馈循环。
        window.secondaryCanvas()->setCamera(window.primaryCanvas()->camera());QCOMPARE(echo.count(),0);
        auto* rightSelector=window.findChild<QComboBox*>("rightStage"); // 可选择与左侧相同的只读包而不转移图元所有权。
        rightSelector->setCurrentIndex(rightSelector->findData("initial"));QCoreApplication::processEvents();
        QCOMPARE(window.primaryCanvas()->packet(),window.secondaryCanvas()->packet());
        QVERIFY(filledPixels(*window.primaryCanvas(),QColor(35,155,86))>50);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(35,155,86))>50);
        rightSelector->setCurrentIndex(rightSelector->findData("standard_legal"));
        for(int i=0;i<3;++i){ // i为窗口/分隔条/单双模式重复变化轮次。
            window.resize(1440+i*100,900+i*60);window.findChild<QSplitter*>()->setSizes({400+i*20,600-i*20});
            auto* handle=window.findChild<QSplitter*>()->handle(1); // 两画布之间的真实分隔条控件。
            const QPoint grip=handle->rect().center(); // 鼠标拖动起点，逻辑px。
            QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,grip);
            QTest::mouseMove(handle,grip+QPoint(30,0));QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,grip+QPoint(30,0));
            QCoreApplication::processEvents();
            QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(35,155,86))>50);
            window.findChild<QAction*>("compareStages")->trigger();QCoreApplication::processEvents();
            window.findChild<QAction*>("compareStages")->trigger();QCoreApplication::processEvents();
            QVERIFY(filledPixels(*window.primaryCanvas(),QColor(220,76,76))>10);
            QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(220,76,76))>10);
        }
        QCOMPARE(echo.count(),0);
    }
    /** @brief 教学数据真实导入、配置后重导入、全流程及独立合法性验收；无输入、无返回值，产物由临时目录接管。 */
    void teachingWorkflow(){
        const QString root=QDir(qEnvironmentVariable("PLACEMENT_TEACHING_ROOT",QString(PLACEMENT_SOURCE_DIR)+"/../examples")).absolutePath(); // 外部只读教学目录；独立克隆可显式提供。
        if(!QFileInfo::exists(root+"/data/teaching/teaching.aux")||!QFileInfo::exists(root+"/configs/teaching.json"))QSKIP("未提供外部教学数据；设置PLACEMENT_TEACHING_ROOT后执行");
        QTemporaryDir directory; // 隔离完整流水线的runs产物，退出时自动清理。
        QVERIFY(directory.isValid());
        const QString originalDirectory=QDir::currentPath(); // 供所有成功/失败路径恢复的测试进程目录。
        /** @brief 捕获originalDirectory值恢复进程目录；无输入、无返回值，覆盖断言提前返回路径。 */
        const auto restore=qScopeGuard([originalDirectory]{QDir::setCurrent(originalDirectory);}); // 比临时目录先析构，避免删除活动目录。
        QVERIFY(QDir::setCurrent(directory.path()));
        MainWindow window; // 与用户复现相同的真实后台GUI流程。
        window.show();QCoreApplication::processEvents();
        InputSpec input; // 保持只读的原始BookShelf输入。
        input.auxPath=(root+"/data/teaching/teaching.aux").toStdString();window.load(input);QTRY_COMPARE(window.state(),RunState::Ready);
        QVERIFY(window.applyConfiguration(QString::fromStdString(ReadText((root+"/configs/teaching.json").toStdString()))));
        window.load(input);QTRY_COMPARE(window.state(),RunState::Ready);
        window.findChild<QAction*>("runAll")->trigger();QTRY_COMPARE_WITH_TIMEOUT(window.state(),RunState::Completed,60000);
        const auto final=window.primaryCanvas()->packet()->snapshot; // 最终原始快照，独立检查不得依赖GUI成功状态。
        ControlToken token; // 独立几何合法性复检令牌。
        QCOMPARE(final->stage,std::string("standard_legal"));QCOMPARE(CheckLegality(*final,"all",10000,&token).totalCount,std::size_t(0));
        auto* selector=window.findChild<QComboBox*>("leftStage"); // 逐阶段切换实际完成结果。
        auto* metrics=window.findChild<QLabel*>("snapshotMetrics"); // 检查真实教学数值来源。
        for(const QString& stage:QStringList{"initial","global","macro_legal","standard_legal"}){ // stage为教学四阶段。
            selector->setCurrentIndex(selector->findData(stage));
            const auto snapshot=window.primaryCanvas()->packet()->snapshot; // 当前显示身份。
            QVERIFY(metrics->text().startsWith(stage+QString(" · i=%1 · HPWL ").arg(snapshot->iteration)));
            QVERIFY(metrics->text().contains(QString::number(*snapshot->metrics.hpwl,'g',12)));
            if(stage=="global"){QVERIFY(snapshot->metrics.overflow);QVERIFY(std::abs(*snapshot->metrics.overflow-.0856393)<1e-6);}
            if(stage=="macro_legal"||stage=="standard_legal"){QVERIFY(!snapshot->metrics.overflow);QVERIFY(metrics->text().contains("overflow —"));}
        }
        window.findChild<QAction*>("compareStages")->trigger();QCoreApplication::processEvents();
        QCOMPARE(window.primaryCanvas()->packet()->snapshot->stage,std::string("initial"));
        QCOMPARE(window.secondaryCanvas()->packet()->snapshot->stage,std::string("standard_legal"));
        QVERIFY(filledPixels(*window.primaryCanvas(),QColor(220,76,76))>10);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(220,76,76))>10);
        QVERIFY(filledPixels(*window.secondaryCanvas(),QColor(35,155,86))>10);
        const QString screenshot=qEnvironmentVariable("PLACEMENT_GUI_SCREENSHOT"); // 可选离屏或桌面证据路径，由调用者指定。
        if(!screenshot.isEmpty())QVERIFY(window.grab().save(screenshot));
    }
};
QTEST_MAIN(SnapshotViewTest)
#include "snapshot_view_test.moc"
