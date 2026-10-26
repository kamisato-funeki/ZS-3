#include "window.hpp"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace placement::gui {
namespace {
/** @brief 可选数值的人类显示。 @param value 可空数值。 @return 保留12位有效数字，空为破折号。 */
QString number(std::optional<double> value){return value?QString::number(*value,'g',12):QStringLiteral("—");}
/** @brief 给主窗口添加可停靠组件。 @param window 主线程窗口。 @param title 面板标题。 @param widget 被接管组件。 @param area 停靠区域。 @return 主窗口拥有的Dock。 */
QDockWidget* dock(QMainWindow& window,const QString& title,QWidget* widget,Qt::DockWidgetArea area){auto* result=new QDockWidget(title,&window); /* result 为主线程父所有的停靠面板。 */result->setWidget(widget);window.addDockWidget(area,result);return result;}
/** @brief 创建只读两列表。 @param headers 列名。 @param parent 父控件。 @return 父对象拥有的树表。 */
QTreeWidget* tree(QStringList headers,QWidget* parent){auto* result=new QTreeWidget(parent); /* result 为只读树表。 */result->setHeaderLabels(headers);result->setEditTriggers(QAbstractItemView::NoEditTriggers);result->setRootIsDecorated(false);result->setAlternatingRowColors(true);result->header()->setSectionResizeMode(QHeaderView::ResizeToContents);return result;}
/** @brief 给属性表加入单行。 @param widget 表。 @param name 字段名。 @param value 只读值。 无返回值。 */
void addProperty(QTreeWidget& widget,QString name,QString value){new QTreeWidgetItem(&widget,{std::move(name),std::move(value)});}
/** @brief 标准阶段序列；用于严格四阶段存储。 @return 静态阶段名数组。 */
const std::array<std::string,4>& stageNames(){static const std::array<std::string,4> names{"initial","global","macro_legal","standard_legal"}; /* names 为公开阶段顺序。 */return names;}
/** @brief 是否算法/导入处于活跃状态。 @param state 主任务状态。 @return 活跃与否。 */
bool active(RunState state){return state==RunState::Loading||state==RunState::Running||state==RunState::Pausing||state==RunState::Paused||state==RunState::Cancelling;}
}
/** @brief 创建分图曲线控件。 */
ConvergencePlot::ConvergencePlot(QWidget* parent):QWidget(parent){setMinimumHeight(145);setMouseTracking(true);}
/** @brief 合并小指标并保持内存上界。 */
void ConvergencePlot::append(std::deque<MetricPoint> points){points_.insert(points_.end(),std::make_move_iterator(points.begin()),std::make_move_iterator(points.end()));CompactMetrics(points_);update();}
/** @brief 清空曲线。 */
void ConvergencePlot::clear(){points_.clear();update();}
/** @brief 读取曲线点数。 */
std::size_t ConvergencePlot::pointCount()const{return points_.size();}
/** @brief 两图分别使用DU与无量纲坐标。 */
void ConvergencePlot::paintEvent(QPaintEvent* event){
    Q_UNUSED(event)
    QPainter painter(this); ///< 主线程曲线画笔。
    painter.fillRect(rect(),QColor(23,31,42));painter.setRenderHint(QPainter::Antialiasing);
    for(int panel=0;panel<2;++panel){ // panel 为HPWL/overflow分图编号。
        const QRectF area(55,panel*height()/2.0+20,std::max(1,width()-75),height()/2.0-30); ///< 独立坐标图区域px。
        painter.setPen(QColor(160,177,192));painter.drawRect(area);painter.drawText(QPointF(8,area.top()+12),panel==0?QStringLiteral("HPWL"):QStringLiteral("overflow"));
        double maximum=panel==0?1:0.1; ///< 当前分图纵轴最大值，单位独立。
        for(const MetricPoint& point:points_)maximum=std::max(maximum,panel==0?point.metrics.hpwl.value_or(0):point.metrics.overflow.value_or(0)); // point 为历史小指标。
        painter.drawText(QPointF(area.left(),area.top()-3),QString::number(maximum,'g',5)+(panel==0?QStringLiteral(" DU"):QString()));
        if(points_.size()<2)continue;
        QPainterPath path; ///< 当前分图折线路径。
        std::string previousStage; ///< 用于画阶段标记。
        for(std::size_t i=0;i<points_.size();++i){ // i 为有界曲线点序号。
            const MetricPoint& point=points_[i]; ///< 当前历史点。
            const double value=panel==0?point.metrics.hpwl.value_or(0):point.metrics.overflow.value_or(0); ///< 对应单位的指标。
            const QPointF pixel(area.left()+i*area.width()/(points_.size()-1),area.bottom()-value/maximum*area.height()); ///< 分图像素点。
            if(i==0)path.moveTo(pixel);else path.lineTo(pixel);
            if(point.stage!=previousStage){painter.setPen(QPen(QColor(96,114,135),1,Qt::DashLine));painter.drawLine(QPointF(pixel.x(),area.top()),QPointF(pixel.x(),area.bottom()));previousStage=point.stage;}
        }
        painter.setPen(QPen(panel==0?QColor(76,188,214):QColor(236,172,86),1.5));painter.drawPath(path);
    }
}
/** @brief 悬停最近点使用完整指标，不混轴。 */
void ConvergencePlot::mouseMoveEvent(QMouseEvent* event){if(points_.empty())return;const std::size_t index=static_cast<std::size_t>(std::clamp((event->position().x()-55)/std::max(1,width()-75),0.0,1.0)*(points_.size()-1)); /* index 为最近时间序号。 */const auto& point=points_[index]; /* point 为只读指标。 */QToolTip::showText(event->globalPosition().toPoint(),QString::fromStdString(point.stage)+tr(" · iteration %1\nHPWL %2 DU\noverflow %3").arg(point.iteration).arg(number(point.metrics.hpwl)).arg(number(point.metrics.overflow)),this);}
/** @brief 构造主线程窗口和可协作停止的控制器。 */
MainWindow::MainWindow(QWidget* parent):QMainWindow(parent),mainTask_(this),exportTask_(this),heatTask_(this),networkTask_(this){
    buildUi();resize(1440,900);setMinimumSize(1024,720);setWindowTitle(tr("布局实验室 · 只读分析与布局流水线"));
    timer_.setInterval(200);connect(&timer_,&QTimer::timeout,this,&MainWindow::consume);timer_.start();
    connect(&mainTask_,&TaskController::idle,this,&MainWindow::consume);connect(&exportTask_,&TaskController::idle,this,&MainWindow::consume);connect(&heatTask_,&TaskController::idle,this,&MainWindow::consume);connect(&networkTask_,&TaskController::idle,this,&MainWindow::consume);setState(RunState::Empty);
}
/** @brief 构建控件；所有连接的this捕获仅主线程访问控件，不逃逸窗口生命周期。 */
void MainWindow::buildUi(){
    auto* toolbar=addToolBar(tr("文件与任务")); ///< 主线程任务工具栏。
    toolbar->setObjectName("taskToolbar");
    auto* fileMenu=menuBar()->addMenu(tr("文件")); ///< 导入与导出菜单。
    auto* runMenu=menuBar()->addMenu(tr("运行")); ///< 阶段运行菜单。
    auto* viewMenu=menuBar()->addMenu(tr("视图")); ///< 相机与图层菜单。
    auto* exportMenu=menuBar()->addMenu(tr("导出")); ///< 独立导出菜单。
    openAux_=fileMenu->addAction(tr("打开 BookShelf AUX…"));openAux_->setShortcut(QKeySequence::Open);openAux_->setObjectName("openAux");toolbar->addAction(openAux_);
    openLef_=fileMenu->addAction(tr("打开 LEF / DEF…"));reload_=fileMenu->addAction(tr("加载 placement 结果…"));
    connect(openAux_,&QAction::triggered,this,&MainWindow::openAux);connect(openLef_,&QAction::triggered,this,&MainWindow::openLefDef);connect(reload_,&QAction::triggered,this,&MainWindow::reload);
    all_=runMenu->addAction(tr("全流程"));global_=runMenu->addAction(tr("全局布局"));macro_=runMenu->addAction(tr("宏合法化"));standard_=runMenu->addAction(tr("标准单元合法化"));
    all_->setObjectName("runAll");global_->setObjectName("runGlobal");macro_->setObjectName("runMacro");standard_->setObjectName("runStandard");toolbar->addAction(all_);
    /** @brief 启动对应应用阶段；捕获this仅主线程访问。无参数、无返回值。 */
    connect(all_,&QAction::triggered,this,[this]{run("all");});
    /** @brief 启动global；捕获this主窗口。无参数、无返回值。 */
    connect(global_,&QAction::triggered,this,[this]{run("global");});
    /** @brief 启动macro；捕获this主窗口。无参数、无返回值。 */
    connect(macro_,&QAction::triggered,this,[this]{run("macro");});
    /** @brief 启动standard；捕获this主窗口。无参数、无返回值。 */
    connect(standard_,&QAction::triggered,this,[this]{run("standard");});
    pause_=runMenu->addAction(tr("暂停：等待安全点"));resume_=runMenu->addAction(tr("继续"));cancel_=runMenu->addAction(tr("取消主任务"));pause_->setObjectName("pause");resume_->setObjectName("resume");cancel_->setObjectName("cancel");toolbar->addAction(pause_);toolbar->addAction(resume_);toolbar->addAction(cancel_);
    connect(pause_,&QAction::triggered,this,&MainWindow::pauseRun);connect(resume_,&QAction::triggered,this,&MainWindow::resumeRun);connect(cancel_,&QAction::triggered,this,&MainWindow::cancelRun);
    auto* fitAction=viewMenu->addAction(tr("适应芯片（F）")); ///< 画布fit快捷操作。
    fitAction->setShortcut(QKeySequence(Qt::Key_F));
    /** @brief 适应主视图，捕获this。无参数、无返回值。 */
    connect(fitAction,&QAction::triggered,this,[this]{left_->fit();});toolbar->addAction(fitAction);
    auto* coreFit=viewMenu->addAction(tr("适应 core（Shift+F）")); ///< 核心域fit快捷操作。
    coreFit->setShortcut(QKeySequence(Qt::SHIFT|Qt::Key_F));
    /** @brief 适应核心域，捕获this。无参数、无返回值。 */
    connect(coreFit,&QAction::triggered,this,[this]{left_->fit(true);});
    auto* compare=viewMenu->addAction(tr("单 / 双视图对比")); ///< 双视图模式开关。
    compare->setObjectName("compareStages");
    connect(compare,&QAction::triggered,this,&MainWindow::toggleComparison);toolbar->addAction(compare);
    auto* restore=viewMenu->addAction(tr("恢复精细 LOD")); ///< 慢帧降级的用户恢复入口。
    /** @brief 恢复两视图细节，捕获this。无参数、无返回值。 */
    connect(restore,&QAction::triggered,this,[this]{left_->restoreDetail();right_->restoreDetail();});
    export_=exportMenu->addAction(tr("导出完整布局 / JSON / 报告…"));export_->setShortcut(QKeySequence(Qt::CTRL|Qt::Key_E));export_->setObjectName("exportResult");toolbar->addAction(export_);connect(export_,&QAction::triggered,this,&MainWindow::exportResult);
    cancelExport_=exportMenu->addAction(tr("只取消导出"));
    cancelExport_->setObjectName("cancelExport");
    /** @brief 导出独立取消，不触达算法token；捕获this。无参数、无返回值。 */
    connect(cancelExport_,&QAction::triggered,this,[this]{exportTask_.cancel();exportStatus_->setText(tr("导出正在取消"));});
    auto* screenshotAction=exportMenu->addAction(tr("保存当前视口截图…")); ///< 明确命名的视口截图。
    connect(screenshotAction,&QAction::triggered,this,&MainWindow::screenshot);
    const QStringList layerNames{tr("标准单元"),tr("宏单元"),tr("固定 / 端口"),tr("行边界"),tr("bin 网格"),tr("密度热图"),tr("违规框")}; ///< 七类只读图层。
    for(std::size_t i=0;i<layerActions_.size();++i){ // i 为图层索引。
        layerActions_[i]=viewMenu->addAction(layerNames[static_cast<int>(i)]);layerActions_[i]->setCheckable(true);layerActions_[i]->setChecked(i<3||i==6);connect(layerActions_[i],&QAction::toggled,this,&MainWindow::updateLayers);
    }
    auto* splitter=new QSplitter(this); ///< 两画布水平分割器。
    auto* leftPanel=new QWidget(splitter); ///< 左阶段选择及画布容器。
    auto* leftLayout=new QVBoxLayout(leftPanel); ///< 左容器主线程布局。
    leftLayout->setContentsMargins(0,0,0,0);leftStage_=new QComboBox(leftPanel);leftStage_->setObjectName("leftStage");left_=new Canvas(leftPanel);left_->setObjectName("primaryCanvas");leftLayout->addWidget(leftStage_);leftLayout->addWidget(left_);
    auto* rightPanel=new QWidget(splitter); ///< 右对比容器。
    auto* rightLayout=new QVBoxLayout(rightPanel); ///< 右容器布局。
    rightLayout->setContentsMargins(0,0,0,0);rightStage_=new QComboBox(rightPanel);right_=new Canvas(rightPanel);rightLayout->addWidget(rightStage_);rightLayout->addWidget(right_);rightPanel->hide();setCentralWidget(splitter);
    /** @brief 左相机同步数学中心和比例；捕获this主窗口。 @param x DU。 @param y DU。 @param du DU/px。 无返回值。 */
    connect(left_,&Canvas::cameraChanged,this,[this](double x,double y,double du){if(comparison_)right_->setCamera({{x,y},du});});
    /** @brief 右相机同步且setCamera不再次发信号；捕获this。 @param x DU。 @param y DU。 @param du DU/px。 无返回值。 */
    connect(right_,&Canvas::cameraChanged,this,[this](double x,double y,double du){if(comparison_)left_->setCamera({{x,y},du});});
    /** @brief 左选择按NodeId同步，捕获this。 @param ids 稳定ID集。 无返回值。 */
    connect(left_,&Canvas::selectionChanged,this,[this](QSet<qulonglong> ids){if(comparison_)right_->setSelection(ids,false);properties(std::move(ids));});
    /** @brief 右选择按NodeId同步，捕获this。 @param ids 稳定ID集。 无返回值。 */
    connect(right_,&Canvas::selectionChanged,this,[this](QSet<qulonglong> ids){left_->setSelection(ids,false);properties(std::move(ids));});
    /** @brief 状态栏数学坐标无回写，捕获this。 @param x DU。 @param y DU。 无返回值。 */
    connect(left_,&Canvas::pointerChanged,this,[this](double x,double y){statusBar()->showMessage(tr("x=%1  y=%2  · %3 DU/px · 选中 %4").arg(x,0,'g',12).arg(y,0,'g',12).arg(left_->camera().duPerPixel,0,'g',5).arg(left_->selection().size()));});
    connect(left_,&Canvas::renderWarning,this,&MainWindow::log);connect(right_,&Canvas::renderWarning,this,&MainWindow::log);
    /** @brief 左阶段选择，捕获this。 @param index 当前选择序号。 无返回值。 */
    connect(leftStage_,&QComboBox::currentIndexChanged,this,[this](int index){Q_UNUSED(index)chooseStage(false);});
    /** @brief 右阶段选择，捕获this。 @param index 当前选择序号。 无返回值。 */
    connect(rightStage_,&QComboBox::currentIndexChanged,this,[this](int index){Q_UNUSED(index)chooseStage(true);});
    auto* project=new QWidget(this); ///< 项目/查找左面板。
    auto* projectLayout=new QVBoxLayout(project); ///< 项目面板布局。
    summary_=new QLabel(tr("尚未打开数据集"),project);summary_->setWordWrap(true);summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);projectLayout->addWidget(summary_);
    stageTree_=tree({tr("阶段"),tr("状态")},project);stageTree_->setObjectName("stageTree");projectLayout->addWidget(stageTree_);
    /** @brief 完成阶段双击定位到主画布；捕获this。 @param item 已完成阶段项。 @param column 点击列。 无返回值。 */
    connect(stageTree_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item,int column){Q_UNUSED(column)if(!(item->flags()&Qt::ItemIsEnabled))return;const int index=leftStage_->findData(item->data(0,Qt::UserRole)); /* index 为阶段下拉对应项。 */if(index>=0)leftStage_->setCurrentIndex(index);});
    search_=new QLineEdit(project);search_->setObjectName("nameSearch");search_->setPlaceholderText(tr("区分大小写的对象名称"));searchMode_=new QComboBox(project);searchMode_->addItems({tr("精确"),tr("前缀")});projectLayout->addWidget(search_);projectLayout->addWidget(searchMode_);
    auto* searchButton=new QPushButton(tr("查找 / 首100项"),project); ///< 名称查找提交。
    auto* nextSearch=new QPushButton(tr("下一页"),project); ///< 大候选集翻页。
    projectLayout->addWidget(searchButton);projectLayout->addWidget(nextSearch);searchResults_=tree({tr("ID"),tr("名称")},project);projectLayout->addWidget(searchResults_);
    searchResults_->setObjectName("searchResults");
    /** @brief 重置名称检索分页，捕获this。无参数、无返回值。 */
    connect(searchButton,&QPushButton::clicked,this,[this]{findName();});
    /** @brief Enter定位第一候选，捕获this。无参数、无返回值。 */
    connect(search_,&QLineEdit::returnPressed,this,[this]{findName();if(searchResults_->topLevelItemCount()){const qulonglong id=searchResults_->topLevelItem(0)->data(0,Qt::UserRole).toULongLong(); /* id 为第一候选。 */left_->setSelection({id});left_->locate(left_->packet()->boxes.at(id));}});
    /** @brief 名称候选翻页，捕获this。无参数、无返回值。 */
    connect(nextSearch,&QPushButton::clicked,this,[this]{findName(false);});
    /** @brief 名称候选双击只读定位，捕获this。 @param item 候选。 @param column 点击列。 无返回值。 */
    connect(searchResults_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item,int column){Q_UNUSED(column)if(!left_->packet())return;const qulonglong id=item->data(0,Qt::UserRole).toULongLong(); /* id 为候选稳定ID。 */if(id>=left_->packet()->boxes.size()||item->data(0,Qt::UserRole+1).toString()!=QString::fromStdString(left_->packet()->snapshot->datasetId))return;left_->setSelection({id});left_->locate(left_->packet()->boxes.at(id));});dock(*this,tr("项目与名称查找"),project,Qt::LeftDockWidgetArea);
    auto* inspectTabs=new QTabWidget(this); ///< 右侧只读属性/违规分页。
    comparisonTable_=tree({tr("指标"),tr("左阶段"),tr("右阶段"),tr("右−左"),tr("改善率")},inspectTabs);
    comparisonTable_->setObjectName("comparisonMetrics");inspectTabs->addTab(comparisonTable_,tr("阶段指标对比"));
    auto* attributePanel=new QWidget(inspectTabs); ///< 属性及网络高亮面板。
    auto* attributeLayout=new QVBoxLayout(attributePanel); ///< 属性布局。
    attributes_=tree({tr("属性"),tr("值")},attributePanel);attributes_->setObjectName("attributes");
    auto* copyProperties=new QAction(tr("复制属性原始精度"),attributes_); ///< 使用17位有效数字复制只读属性。
    copyProperties->setShortcut(QKeySequence::Copy);copyProperties->setShortcutContext(Qt::WidgetWithChildrenShortcut);attributes_->addAction(copyProperties);attributes_->setContextMenuPolicy(Qt::ActionsContextMenu);
    /** @brief 复制选中属性行或全部行，捕获this；无参数、无返回值。 */
    connect(copyProperties,&QAction::triggered,this,[this]{QStringList lines; /* lines 为原始精度字段文本。 */for(int i=0;i<attributes_->topLevelItemCount();++i){const auto* item=attributes_->topLevelItem(i); /* item 为只读属性行。 */lines.push_back(item->text(0)+"\t"+item->text(1));}QApplication::clipboard()->setText(lines.join('\n'));});
    networks_=new QComboBox(attributePanel);networks_->addItem(tr("隐藏网络高亮"),-1);attributeLayout->addWidget(attributes_);attributeLayout->addWidget(networks_);inspectTabs->addTab(attributePanel,tr("属性 / 网络"));
    /** @brief 将所属网络计算交给后台；捕获this主窗口。 @param index 当前网络选项。 无返回值。 */
    connect(networks_,&QComboBox::currentIndexChanged,this,[this](int index){
        ++networkRequest_;networkTask_.cancel();left_->setNetwork({});right_->setNetwork({});
        pendingNetwork_=index>0?std::optional<NetId>(networks_->itemData(index).toULongLong()):std::nullopt;
        scheduleNetwork();
    });
    auto* violationPanel=new QWidget(inspectTabs); ///< 违规过滤与详情。
    auto* violationLayout=new QVBoxLayout(violationPanel); ///< 违规布局。
    violationFilter_=new QComboBox(violationPanel);violationFilter_->addItem(tr("全部类型"));violations_=tree({tr("类型"),tr("严重度"),tr("数值"),tr("说明")},violationPanel);violationLayout->addWidget(violationFilter_);violationLayout->addWidget(violations_);inspectTabs->addTab(violationPanel,tr("违规 / 定位"));dock(*this,tr("只读检查"),inspectTabs,Qt::RightDockWidgetArea);
    violations_->setObjectName("violations");
    /** @brief 违规类型过滤，仅更新有界列表，捕获this。 @param index 类型选项。 无返回值。 */
    connect(violationFilter_,&QComboBox::currentIndexChanged,this,[this](int index){Q_UNUSED(index)if(right_->packet()&&right_->packet()->snapshot==violationSnapshot_)showViolations(*right_->packet());else if(left_->packet())showViolations(*left_->packet());});
    /** @brief 违规双击带10%边距定位并临时轮廓选中隐藏对象；捕获this。
     * @param item 有界违规项。 @param column 点击列。 无返回值。 */
    connect(violations_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item,int column){Q_UNUSED(column)if(!left_->packet())return;const std::size_t index=item->data(0,Qt::UserRole).toULongLong(); /* index 为当前报告记录索引。 */if(index>=listedViolations_.size()||!violationSnapshot_)return;Canvas* destination=left_->packet()&&left_->packet()->snapshot==violationSnapshot_?left_:right_->packet()&&right_->packet()->snapshot==violationSnapshot_?right_:nullptr; /* destination为列表绑定源画布。 */if(!destination)return;const Violation& violation=listedViolations_[index]; /* violation 为完整定位信息。 */QSet<qulonglong> ids; /* ids 为受影响真实对象。 */for(NodeId id:violation.objectIds)ids.insert(id); /* id 为违规关联对象。 */destination->setSelection(ids);destination->locate(violation.region);statusBar()->showMessage(QString::fromStdString(violation.kind)+": "+QString::number(violation.magnitude,'g',15));});
    auto* configPanel=new QWidget(this); ///< 单一高级参数编辑器，无第二套默认值。
    auto* configLayout=new QVBoxLayout(configPanel); ///< 参数布局。
    configLayout->addWidget(new QLabel(tr("共享配置 JSON：初始 / 全局 / 宏 / 标准 / 显示与导出\n修改后需重新运行，历史快照保留原配置。"),configPanel));configEditor_=new QPlainTextEdit(QString::fromStdString(SerializeConfig(config_)),configPanel);configEditor_->setObjectName("configEditor");applyConfig_=new QPushButton(tr("校验并应用"),configPanel);configLayout->addWidget(configEditor_);configLayout->addWidget(applyConfig_);configDock_=dock(*this,tr("统一配置（高级）"),configPanel,Qt::RightDockWidgetArea);configDock_->hide();viewMenu->addAction(configDock_->toggleViewAction());toolbar->addAction(configDock_->toggleViewAction());
    /** @brief ParseConfig统一校验，捕获this。无参数、无返回值。 */
    connect(applyConfig_,&QPushButton::clicked,this,[this]{applyConfiguration(configEditor_->toPlainText());});
    auto* bottom=new QTabWidget(this); ///< 有界曲线及日志面板。
    plot_=new ConvergencePlot(bottom);bottom->addTab(plot_,tr("收敛曲线（分单位）"));
    auto* logPanel=new QWidget(bottom); ///< 日志过滤与保存位置提示。
    auto* logLayout=new QVBoxLayout(logPanel); ///< 日志布局。
    logFilter_=new QLineEdit(logPanel);logFilter_->setPlaceholderText(tr("过滤日志文本；完整记录写入运行目录 events.jsonl"));logLevel_=new QComboBox(logPanel);logLevel_->addItems({tr("Info 及以上"),tr("仅错误")});logView_=new QPlainTextEdit(logPanel);logView_->setReadOnly(true);logView_->setMaximumBlockCount(5000);logLayout->addWidget(logFilter_);logLayout->addWidget(logLevel_);logLayout->addWidget(logView_);bottom->addTab(logPanel,tr("运行 / 加载诊断日志"));dock(*this,tr("曲线与日志"),bottom,Qt::BottomDockWidgetArea);
    auto* openOutput=exportMenu->addAction(tr("打开完整指标 / 日志保存目录")); ///< 外部文件管理器定位完整流式文件。
    /** @brief 打开应用输出目录，捕获this；无参数、无返回值。 */
    connect(openOutput,&QAction::triggered,this,[this]{if(outputDirectory_.isEmpty()){log(tr("尚无运行输出目录"));return;}QDesktopServices::openUrl(QUrl::fromLocalFile(outputDirectory_));});
    connect(logFilter_,&QLineEdit::textChanged,this,&MainWindow::refreshLogs);connect(logLevel_,&QComboBox::currentIndexChanged,this,&MainWindow::refreshLogs);
    metrics_=new QLabel(tr("无快照"),this);exportStatus_=new QLabel(this);statusBar()->addPermanentWidget(metrics_,1);statusBar()->addPermanentWidget(exportStatus_);
    auto* help=menuBar()->addMenu(tr("帮助")); ///< 只读交互说明入口。
    auto* controls=help->addAction(tr("画布与 GDS 查看说明")); ///< 外部KLayout非强制依赖。
    /** @brief 显示可发现的只读操作说明，捕获this。无参数、无返回值。 */
    connect(controls,&QAction::triggered,this,[this]{QMessageBox::information(this,tr("操作说明"),tr("滚轮锚定缩放；中键或空格+左键平移；Ctrl+单击增减；Alt+单击轮换重叠对象；拖框选择；Esc清除。\nF适应芯片，Shift+F适应core。物体与属性均只读。\n网络高亮为至多5000引脚的示意星形，非布线。\n完整布局BMP使用导出；截图仅保存当前视口。GDS可使用外部KLayout打开；未安装不影响导出。"));});
    refreshStages();
}
/** @brief 读取主状态。 */
RunState MainWindow::state()const{return state_;}
/** @brief 读取主画布。 */
Canvas* MainWindow::primaryCanvas()const{return left_;}
/** @brief 读取对比画布。 */
Canvas* MainWindow::secondaryCanvas()const{return right_;}
/** @brief 同步状态与按钮；主/导出完全独立。 */
void MainWindow::setState(RunState state){
    state_=state;
    const bool busy=active(state_)||mainTask_.running()||closing_; ///< 算法参数与输入锁定条件。
    const bool available=database_!=nullptr; ///< 是否存在可浏览的成功输入。
    openAux_->setEnabled(!busy);openLef_->setEnabled(!busy);reload_->setEnabled(!busy&&available);
    configEditor_->setReadOnly(busy);applyConfig_->setEnabled(!busy);
    all_->setEnabled(!busy&&available);global_->setEnabled(!busy&&available);macro_->setEnabled(!busy&&stages_.count("global"));standard_->setEnabled(!busy&&stages_.count("macro_legal"));macro_->setToolTip(tr("需要已完成 global 阶段"));standard_->setToolTip(tr("需要已完成 macro_legal 阶段"));
    pause_->setEnabled(state_==RunState::Running&&!loading_);resume_->setEnabled(state_==RunState::Paused);cancel_->setEnabled(active(state_)&&state_!=RunState::Cancelling);export_->setEnabled(left_->packet()&&!exportTask_.running()&&!closing_);cancelExport_->setEnabled(exportTask_.running()&&!closing_);
    const QStringList names{tr("空项目"),tr("正在加载（旧项目可浏览）"),tr("就绪"),tr("运行中"),tr("正在等待安全点"),tr("已在安全点暂停"),tr("正在取消"),tr("已完成"),tr("已取消"),tr("失败，已保留完整阶段")}; ///< 与枚举顺序一致的状态文本。
    statusBar()->showMessage(names[static_cast<int>(state_)]);emit stateChanged(static_cast<int>(state_));
}
/** @brief 菜单选择AUX，事务成功才替换旧项目。 */
void MainWindow::openAux(){const QString path=QFileDialog::getOpenFileName(this,tr("打开 BookShelf AUX"),{},tr("BookShelf (*.aux);;所有文件 (*)")); /* path 为用户选择的输入。 */if(path.isEmpty())return;InputSpec input; /* input 为格式互斥请求。 */input.auxPath=path.toStdString();load(std::move(input));}
/** @brief 多LEF与单DEF导入，两次选择均完成后启动。 */
void MainWindow::openLefDef(){const QStringList lefs=QFileDialog::getOpenFileNames(this,tr("选择一个或多个 LEF"),{},tr("LEF (*.lef);;所有文件 (*)")); /* lefs 为保持选择顺序的库列表。 */if(lefs.isEmpty())return;const QString def=QFileDialog::getOpenFileName(this,tr("选择配套 DEF"),{},tr("DEF (*.def);;所有文件 (*)")); /* def 为单设计文件。 */if(def.isEmpty())return;InputSpec input; /* input 为LEF/DEF请求。 */for(const QString& path:lefs)input.lefPaths.push_back(path.toStdString()); /* path 为库路径。 */input.defPath=def.toStdString();load(std::move(input));}
/** @brief 事务式后台导入入口。 */
void MainWindow::load(InputSpec input){if(mainTask_.running()||closing_)return;pendingInput_=std::move(input);activeConfig_=config_;runId_=NewRunId();loading_=true;reloading_=false;loadingDataset_=database_?database_->datasetId:"";mainTask_.start(LoadJob(pendingInput_,activeConfig_,runId_),{},runId_);setState(RunState::Loading);log(tr("开始输入事务：%1").arg(QString::fromStdString(pendingInput_.auxPath.empty()?pendingInput_.defPath:pendingInput_.auxPath)));}
/** @brief 核对当前输入指纹后后台载入placement。 */
void MainWindow::reload(){if(!database_||mainTask_.running())return;const QString path=QFileDialog::getOpenFileName(this,tr("载入 placement JSON"),{},tr("JSON (*.json)")); /* path 为结果文件。 */if(path.isEmpty())return;runId_=NewRunId();activeConfig_=config_;loading_=true;reloading_=true;loadingDataset_=database_->datasetId;mainTask_.start(ReloadJob(database_,path.toStdString(),runId_),database_->datasetId,runId_);setState(RunState::Loading);}
/** @brief 共享ParseConfig验证后才更改当前未运行配置。 */
bool MainWindow::applyConfiguration(const QString& text){if(active(state_)||mainTask_.running())return false;try{config_=ParseConfig(text.toStdString(),config_);configEditor_->setPlainText(QString::fromStdString(SerializeConfig(config_)));log(tr("配置已校验；现有快照保持原配置，当前参数尚未运行"));return true;}catch(const Error& error){log(tr("ERROR 配置拒绝：%1").arg(QString::fromUtf8(error.what())));statusBar()->showMessage(tr("参数未应用：%1").arg(QString::fromUtf8(error.what())));return false;}}
/** @brief 以四阶段前置条件生成唯一应用请求。 */
void MainWindow::run(std::string stage){
    if(!database_||mainTask_.running()||closing_)return;
    const std::string prerequisite=stage=="macro"?"global":stage=="standard"?"macro_legal":"initial"; ///< 请求必须依赖的完成阶段。
    if(!stages_.count(prerequisite)){log(tr("缺少前置阶段：%1").arg(QString::fromStdString(prerequisite)));return;}
    RunRequest request; ///< 唯一应用流水线请求，不复制算法。
    request.database=database_;request.config=config_;request.stage=std::move(stage);request.runId=NewRunId();
    if(request.stage=="macro"||request.stage=="standard") request.startingSnapshot=stages_.at(prerequisite)->snapshot;
    if(request.startingSnapshot&&request.startingSnapshot->stage=="global"&&request.startingSnapshot->termination!="converged"){
        request.continueUnconverged=QMessageBox::question(this,tr("全局未收敛"),tr("global未收敛，是否显式允许继续合法化？"))==QMessageBox::Yes;if(!request.continueUnconverged)return;
    }
    activeConfig_=config_;runId_=request.runId;loading_=false;plot_->clear();preview_.reset();mainTask_.start(PipelineJob(request),database_->datasetId,runId_);setState(RunState::Running);log(tr("开始运行 %1，输出 runs/%2").arg(QString::fromStdString(request.stage),QString::fromStdString(runId_)));
}
/** @brief 测试入口复用相同后台生命周期。 */
void MainWindow::startBackground(BackgroundJob job,std::string dataset,std::string run){if(mainTask_.running())return;runId_=run;loading_=false;mainTask_.start(std::move(job),std::move(dataset),std::move(run));setState(RunState::Running);}
/** @brief 设置暂停请求，状态明确尚未停止。 */
void MainWindow::pauseRun(){if(state_!=RunState::Running||loading_)return;mainTask_.pause();setState(RunState::Pausing);}
/** @brief 继续直接唤醒核心令牌。 */
void MainWindow::resumeRun(){if(state_!=RunState::Paused)return;mainTask_.resume();setState(RunState::Running);}
/** @brief 取消优先，唤醒已暂停核心。 */
void MainWindow::cancelRun(){if(!mainTask_.running())return;mainTask_.cancel();setState(RunState::Cancelling);}
/** @brief 主线程有界轮询；接收终态前先安装完整成功阶段。 */
void MainWindow::consume(){
    if(processing_)return;
    processing_=true;
    MailBatch batch=mainTask_.take(); ///< 自上次消费累积的小事件和至多一个预览。
    for(const QString& line:batch.logs){logs_.push_back(line);if(logs_.size()>5000)logs_.pop_front();} // line 为有界应用日志。
    if(!batch.logs.empty())refreshLogs();
    if(batch.droppedLogs)log(tr("Info 日志环形缓冲已覆盖 %1 条；完整日志见 events.jsonl").arg(batch.droppedLogs));
    plot_->append(std::move(batch.metrics));
    if(loading_&&!batch.stages.empty()){
        if(!reloading_){stages_.clear();stageConfig_.clear();preview_.reset();heatTask_.cancel();networkTask_.cancel();++networkRequest_;pendingNetwork_.reset();comparison_=false;right_->parentWidget()->hide();left_->setPacket({});right_->setPacket({});searchResults_->clear();attributes_->clear();violations_->clear();listedViolations_.clear();violationSnapshot_.reset();heatRequestIdentity_.clear();left_->setSelection({});right_->setSelection({});input_=pendingInput_;}
        database_=batch.stages.front()->snapshot->database;
    }
    if(batch.preview&&!loading_&&batch.preview->snapshot->runId==runId_&&database_&&batch.preview->snapshot->datasetId==database_->datasetId)install(batch.preview,false);
    for(const auto& packet:batch.stages){ // packet 为最多四个完成阶段。
        if(packet->snapshot->runId==runId_&&(!database_||packet->snapshot->datasetId==database_->datasetId))install(packet,true);
    }
    if(batch.result&&batch.result->runId==runId_){
        preview_.reset();outputDirectory_=QString::fromStdString(batch.result->outputDirectory);log(QString::fromStdString(batch.result->message+" · "+batch.result->outputDirectory));
        setState(batch.result->termination=="completed"?RunState::Completed:(batch.result->termination=="CANCELLED"||batch.result->termination=="cancelled")?RunState::Cancelled:RunState::Failed);
    }
    if(!batch.error.isEmpty()){log("ERROR "+batch.error);setState(batch.error.contains("CANCELLED")?RunState::Cancelled:RunState::Failed);}
    if(batch.success&&loading_){setState(RunState::Ready);log(tr("输入事务成功；初始化策略 %1，审计警告 %2 条；位置调整 %3 项").arg(QString::fromStdString(activeConfig_.initialMode)).arg(database_?database_->auditWarnings.size():0).arg(left_->packet()?left_->packet()->snapshot->adjustments.size():0));}
    if(state_==RunState::Pausing&&mainTask_.paused())setState(RunState::Paused);
    if(!mainTask_.running()){
        if(state_==RunState::Loading)setState(database_?RunState::Ready:RunState::Empty);
        else if(state_==RunState::Running)setState(RunState::Completed);
        else if(state_==RunState::Cancelling)setState(RunState::Cancelled);
        loading_=false;setState(state_);
    }
    MailBatch exported=exportTask_.take(); ///< 独立导出结果，与主状态分离。
    if(exported.success){exportStatus_->setText(tr("导出完成"));log(tr("导出 %1 完成；源快照身份未修改").arg(QString::fromStdString(exportId_)));}
    if(!exported.error.isEmpty()){exportStatus_->setText(tr("导出失败 / 取消"));log("ERROR 导出 "+exported.error);}
    MailBatch heated=heatTask_.take(); ///< 后台精确密度结果。
    for(const auto& heat:heated.heatmaps){left_->setHeatmap(heat);right_->setHeatmap(heat);} // heat 为各阶段同身份热图。
    if(!heated.error.isEmpty())log(tr("密度热图不可用：%1").arg(heated.error));
    MailBatch network=networkTask_.take(); ///< 后台星形高亮，至多5000引脚。
    for(const auto& overlay:network.networks){
        if(!overlay||overlay->requestId!=networkRequest_||networks_->currentIndex()<=0||overlay->netId!=networks_->currentData().toULongLong())continue; // overlay 为双视图各自的有界网络示意。
        for(Canvas* canvas:{left_,right_}){ // canvas 为同数据集不同阶段视图。
            const auto packet=canvas->packet(); ///< 当前显示源。
            if(packet&&packet->snapshot->datasetId==overlay->dataset&&packet->snapshot->runId==overlay->run&&packet->snapshot->stage==overlay->stage&&packet->snapshot->revision==overlay->revision)canvas->setNetwork(*overlay);
        }
    }
    if(!network.error.isEmpty()&&!network.error.contains("CANCELLED"))log("网络高亮："+network.error);
    scheduleNetwork();
    export_->setEnabled(left_->packet()&&!exportTask_.running()&&!closing_);cancelExport_->setEnabled(exportTask_.running()&&!closing_);
    processing_=false;
    if(!closing_&&layerActions_[5]->isChecked())updateLayers();
    if(closing_&&!closeAllowed_&&!mainTask_.running()&&!exportTask_.running()&&!heatTask_.running()&&!networkTask_.running()){closeAllowed_=true;emit shutdownReady();close();}
}
/** @brief 整体安装主线程包，预览与完成阶段分开。 */
void MainWindow::install(std::shared_ptr<const RenderPacket> packet,bool completed){
    if(!packet||!packet->snapshot->database)return;
    if(database_&&packet->snapshot->datasetId!=database_->datasetId&&!loading_)return;
    database_=packet->snapshot->database;
    if(completed){
        if(std::find(stageNames().begin(),stageNames().end(),packet->snapshot->stage)==stageNames().end())return;
        stages_[packet->snapshot->stage]=packet;stageConfig_[packet->snapshot->stage]=activeConfig_;
        if(preview_&&preview_->snapshot->runId==packet->snapshot->runId)preview_.reset();
        refreshStages();
    }else preview_=packet;
    if(comparison_)right_->setPacket(packet);else left_->setPacket(packet);
    if(completed){QComboBox* selector=comparison_?rightStage_:leftStage_; /* selector为实际被替换画布的阶段选择。 */const QSignalBlocker blocked(selector); /* blocked防止安装同轮包时重复切换。 */selector->setCurrentIndex(selector->findData(QString::fromStdString(packet->snapshot->stage)));}
    else {QComboBox* selector=comparison_?rightStage_:leftStage_; /* selector为预览画布的选择器，完成阶段仍可重新选取。 */const QSignalBlocker blocked(selector); /* blocked防止预览占位触发阶段切换。 */selector->setPlaceholderText(tr("preview · i=%1").arg(packet->snapshot->iteration));selector->setCurrentIndex(-1);}
    showViolations(*packet);properties(left_->selection());updateComparisonMetrics();
    const Metrics& metric=packet->snapshot->metrics; ///< 同轮完整指标。
    metrics_->setText(tr("%1 · i=%2 · HPWL %3 · overflow %4 · %5 ms").arg(QString::fromStdString(packet->snapshot->stage)).arg(packet->snapshot->iteration).arg(number(metric.hpwl),number(metric.overflow),number(metric.elapsedMs)));
    const auto& db=*database_; ///< 当前只读输入。
    summary_->setText(tr("dataset %1\n%2 nodes · %3 nets · %4 pins\n单位 %5 · %6\n输入审计 %7 项").arg(QString::fromStdString(db.datasetId).left(20)).arg(db.nodes.size()).arg(db.nets.size()).arg(db.pins.size()).arg(QString::fromStdString(db.unit)).arg(db.physicalScaleKnown?tr("真实 μm/DU=%1").arg(db.micronsPerDu):tr("物理比例未知，DU显示映射")).arg(db.inputFiles.size())+tr("\n准备策略 %1 · 位置调整 %2 项").arg(QString::fromStdString(sourceConfig(*packet->snapshot).initialMode)).arg(packet->snapshot->adjustments.size()));
    if(!active(state_)&&!mainTask_.running())setState(RunState::Ready);
}
/** @brief 四阶段树只允许完整完成项。 */
void MainWindow::refreshStages(){
    const QSignalBlocker leftBlock(leftStage_); ///< 防止重建时误切换场景。
    const QSignalBlocker rightBlock(rightStage_); ///< 防止重建时同步回调。
    const QString leftCurrent=leftStage_->currentData().toString(); ///< 当前左阶段身份。
    const QString rightCurrent=rightStage_->currentData().toString(); ///< 当前右阶段身份。
    leftStage_->clear();rightStage_->clear();stageTree_->clear();
    for(const std::string& name:stageNames()){ // name 为严格四阶段之一。
        const auto found=stages_.find(name); ///< 当前已完成包。
        const QString text=QString::fromStdString(name); ///< 界面阶段名。
        auto* item=new QTreeWidgetItem(stageTree_,{text,found==stages_.end()?tr("待完成"):QString::fromStdString(found->second->snapshot->termination)}); ///< 阶段只读项。
        item->setData(0,Qt::UserRole,text);
        if(found==stages_.end())item->setFlags(item->flags()&~Qt::ItemIsEnabled);else{leftStage_->addItem(text,text);rightStage_->addItem(text,text);}
    }
    const int leftIndex=leftStage_->findData(leftCurrent); ///< 原阶段是否仍存在。
    const int rightIndex=rightStage_->findData(rightCurrent); ///< 原右阶段是否仍存在。
    leftStage_->setCurrentIndex(leftIndex>=0?leftIndex:0);rightStage_->setCurrentIndex(rightIndex>=0?rightIndex:rightStage_->count()-1);
}
/** @brief 显示选择的完整阶段，不改变原配置。 */
void MainWindow::chooseStage(bool right){const std::string name=(right?rightStage_:leftStage_)->currentData().toString().toStdString(); /* name 为下拉绑定阶段。 */const auto found=stages_.find(name); /* found 为完成包。 */if(found==stages_.end())return;(right?right_:left_)->setPacket(found->second);showViolations(*found->second);if(!right)properties(left_->selection());updateComparisonMetrics();}
/** @brief 单双视图切换默认initial与最新成功阶段，传递数学相机避免反馈循环。 */
void MainWindow::toggleComparison(){if(!comparison_&&stages_.size()<2){log(tr("对比至少需要两个完整阶段"));return;}comparison_=!comparison_;right_->parentWidget()->setVisible(comparison_);if(comparison_){leftStage_->setCurrentIndex(leftStage_->findData(QStringLiteral("initial")));rightStage_->setCurrentIndex(rightStage_->count()-1);chooseStage(false);chooseStage(true);right_->setCamera(left_->camera());}else {if(active(state_)&&preview_)left_->setPacket(preview_);else chooseStage(false);}updateComparisonMetrics();}
/** @brief 比较已计算指标，无输入无返回值；不在主线程重新计算全网线长。 */
void MainWindow::updateComparisonMetrics(){
    comparisonTable_->clear();if(!comparison_||!left_->packet()||!right_->packet())return;
    const Snapshot& left=*left_->packet()->snapshot; // 左侧完整身份与指标。
    const Snapshot& right=*right_->packet()->snapshot; // 右侧独立阶段。
    if(left.datasetId!=right.datasetId)return;
    /** @brief 添加可空指标的差值与相对改善率。 @param name 含单位名称。 @param a 左值。 @param b 右值。 无返回值。 */
    const auto add=[this](const QString& name,std::optional<double> a,std::optional<double> b){
        const std::optional<double> difference=a&&b?std::optional<double>(*b-*a):std::nullopt; // 同单位绝对变化。
        const QString improvement=a&&b&&*a!=0?QString::number(100*(*a-*b)/ *a,'g',6)+"%":QStringLiteral("—"); // 零基准不除零。
        new QTreeWidgetItem(comparisonTable_,{name,number(a),number(b),number(difference),improvement});
    };
    add(tr("HPWL (DU)"),left.metrics.hpwl,right.metrics.hpwl);add(tr("加权HPWL (DU)"),left.metrics.weightedHpwl,right.metrics.weightedHpwl);
    add(tr("overflow"),left.metrics.overflow,right.metrics.overflow);add(tr("相对各自前置阶段平均位移 (DU)"),left.metrics.meanDisplacement,right.metrics.meanDisplacement);
    add(tr("相对各自前置阶段最大位移 (DU)"),left.metrics.maxDisplacement,right.metrics.maxDisplacement);
}
/** @brief 只读属性完整精度展示，所属网络最多500项。 */
void MainWindow::properties(QSet<qulonglong> ids){
    ++networkRequest_;pendingNetwork_.reset();networkTask_.cancel();left_->setNetwork({});right_->setNetwork({});attributes_->clear();const QSignalBlocker blocked(networks_); ///< 防止重建所属网络时启动任务。
    networks_->clear();networks_->addItem(tr("隐藏网络高亮"),-1);
    const auto packet=left_->packet(); ///< 当前主画布快照。
    if(!packet||ids.isEmpty())return;
    const NodeId id=*std::min_element(ids.begin(),ids.end()); ///< 多选时展示最小稳定ID对象。
    if(id>=packet->boxes.size())return;
    const Node& node=packet->snapshot->database->nodes[id]; ///< 当前只读节点。
    const Rect rect=packet->boxes[id]; ///< 同轮物理包围盒。
    addProperty(*attributes_,tr("选中数量"),QString::number(ids.size()));addProperty(*attributes_,tr("ID"),QString::number(id));addProperty(*attributes_,tr("名称"),QString::fromStdString(node.name));addProperty(*attributes_,tr("类别"),node.kind==NodeKind::Macro?"Macro":node.kind==NodeKind::Terminal?"Terminal":"Standard");addProperty(*attributes_,tr("固定"),node.fixed?"true":"false");
    addProperty(*attributes_,tr("ll (DU)"),QString::number(rect.lx,'g',17)+", "+QString::number(rect.ly,'g',17));addProperty(*attributes_,tr("center (DU)"),QString::number((rect.lx+rect.ux)/2,'g',17)+", "+QString::number((rect.ly+rect.uy)/2,'g',17));addProperty(*attributes_,tr("width × height (DU)"),QString::number(rect.width(),'g',17)+" × "+QString::number(rect.height(),'g',17));addProperty(*attributes_,tr("方向"),QString::fromStdString(orientationName(packet->snapshot->positions[id].orientation)));
    const auto& nets=packet->topology->nodeNets[id]; ///< 后台预建的所属网络，主线程不扫全部引脚。
    for(NetId net:nets){if(networks_->count()>500)break;networks_->addItem(QString::fromStdString(packet->snapshot->database->nets[net].name),QVariant::fromValue<qulonglong>(net));} // net 为所属网络稳定ID。
    if(nets.size()>500)addProperty(*attributes_,tr("网络列表"),tr("显示前500 / %1项").arg(nets.size()));
}
/** @brief 分页只保留100个名称候选，匹配严格区分大小写。 */
void MainWindow::findName(bool reset){
    const auto packet=left_->packet(); ///< 查询绑定当前画布数据集。
    if(!database_||!packet)return;
    const std::string query=search_->text().toStdString(); ///< UTF-8字典序查询或前缀。
    searchResults_->clear();if(query.empty())return;
    const auto& names=packet->topology->names; ///< 后台排序的名称ID数组。
    if(reset){
        /** @brief 比较索引节点名与查询；捕获this只读数据库。 @param id 索引ID。 @param key UTF-8查询。 @return 字典序。 */
        const auto found=std::lower_bound(names.begin(),names.end(),query,[packet](NodeId id,const std::string& key){return packet->snapshot->database->nodes.at(id).name<key;}); ///< O(log N)页起点。
        searchCursor_=static_cast<std::size_t>(found-names.begin());
    }
    int count=0; ///< 本页最多100候选。
    for(;searchCursor_<names.size()&&count<100;++searchCursor_){
        const Node& node=packet->snapshot->database->nodes.at(names[searchCursor_]); ///< 当前字典序候选。
        const bool matches=searchMode_->currentIndex()==0?node.name==query:node.name.compare(0,query.size(),query)==0; ///< 区分大小写的匹配。
        if(!matches)break;
        auto* item=new QTreeWidgetItem(searchResults_,{QString::number(node.id),QString::fromStdString(node.name)}); ///< 有界候选行。
        item->setData(0,Qt::UserRole,QVariant::fromValue<qulonglong>(node.id));item->setData(0,Qt::UserRole+1,QString::fromStdString(packet->snapshot->datasetId));++count;
    }
    statusBar()->showMessage(tr("本页 %1 项；下一页从字典序 %2 开始").arg(count).arg(searchCursor_));
}
/** @brief 重建最多1000条独立违规，并显示完整计数。 */
void MainWindow::showViolations(const RenderPacket& packet){
    violationSnapshot_=packet.snapshot;listedViolations_=packet.violations.records;violations_->clear();
    if(violationFilter_->count()<=1){const QSignalBlocker blocked(violationFilter_); /* blocked 防止过滤重入。 */for(const auto& count:packet.violations.countByType)violationFilter_->addItem(QString::fromStdString(count.first)); /* count 为类型和完整计数。 */}
    const QString filter=violationFilter_->currentIndex()>0?violationFilter_->currentText():QString(); ///< 当前可选类型。
    for(std::size_t i=0;i<packet.violations.records.size();++i){ // i 为有界详情记录索引。
        const auto& record=packet.violations.records[i]; ///< 同轮独立检查违规。
        if(!filter.isEmpty()&&filter!=QString::fromStdString(record.kind))continue;
        auto* item=new QTreeWidgetItem(violations_,{QString::fromStdString(record.kind),tr("Error"),QString::number(record.magnitude,'g',12),QString::fromStdString(record.message)}); ///< 违规只读行。
        item->setData(0,Qt::UserRole,QVariant::fromValue<qulonglong>(i));item->setToolTip(0,tr("总计 %1，已列出 %2%3").arg(packet.violations.totalCount).arg(packet.violations.records.size()).arg(packet.violations.truncated?tr("（详情已截断）"):QString()));
    }
}
/** @brief 刷新图层，热图只启动后台数值计算。 */
void MainWindow::updateLayers(){
    Layers layers; ///< 左右图层统一开关。
    layers.standard=layerActions_[0]->isChecked();layers.macro=layerActions_[1]->isChecked();layers.fixed=layerActions_[2]->isChecked();layers.rows=layerActions_[3]->isChecked();layers.bins=layerActions_[4]->isChecked();layers.heat=layerActions_[5]->isChecked();layers.violations=layerActions_[6]->isChecked();left_->setLayers(layers);right_->setLayers(layers);
    if(!layers.heat){heatTask_.cancel();heatRequestIdentity_.clear();return;}
    if(!left_->packet()||heatTask_.running())return;
    const auto source=left_->packet()->snapshot; ///< 左侧源引用，无坐标副本。
    const auto other=comparison_&&right_->packet()?right_->packet()->snapshot:source; ///< 右侧源引用。
    const std::string identity=source->datasetId+source->runId+source->stage+std::to_string(source->revision)+"/"+other->runId+other->stage+std::to_string(other->revision); ///< 双源身份组合。
    if(identity==heatRequestIdentity_)return;
    heatRequestIdentity_=identity;
    const Config firstConfig=sourceConfig(*source); ///< 左源冻结配置。
    const Config secondConfig=sourceConfig(*other); ///< 右源冻结配置。
    /** @brief 两源依次后台计算，捕获source/other与两份配置值。 @param token 热图取消。 @param mailbox 至多两份结果。 无返回值。 */
    const auto job=[source,other,firstConfig,secondConfig](ControlToken& token,Mailbox& mailbox){HeatJob(source,firstConfig)(token,mailbox);if(other!=source)HeatJob(other,secondConfig)(token,mailbox);};
    heatTask_.start(job,source->datasetId,source->runId);
}
/** @brief 读取快照保存的冻结参数，防止新配置污染旧结果导出。 */
/** @brief 启动最新选择的网络高亮；旧任务退出后才复用其邮箱，无输入无返回值。 */
void MainWindow::scheduleNetwork(){
    if(closing_||networkTask_.running()||!pendingNetwork_||!left_->packet())return;
    const NetId net=*pendingNetwork_; // 最新选择的网络ID。
    const std::uint64_t request=networkRequest_; // 本次请求序号，后续选择使其过期。
    const auto source=left_->packet()->snapshot; // 左侧完整源。
    const auto other=comparison_&&right_->packet()?right_->packet()->snapshot:source; // 右侧独立源。
    pendingNetwork_.reset();
    /** @brief 在后台生成双源网络，捕获不可变源/网络/请求身份。 @param token 取消。 @param mailbox 有界结果。 无返回值。 */
    const auto job=[source,other,net,request](ControlToken& token,Mailbox& mailbox){NetworkJob(source,net,request)(token,mailbox);if(other!=source)NetworkJob(other,net,request)(token,mailbox);};
    networkTask_.start(job,source->datasetId,source->runId);
}
/** @brief 读取快照保存的冻结参数，防止新配置污染旧结果导出。 */
Config MainWindow::sourceConfig(const Snapshot& snapshot)const{const auto found=stages_.find(snapshot.stage); /* found 为同名完整阶段。 */if(found!=stages_.end()&&found->second->snapshot->runId==snapshot.runId&&stageConfig_.count(snapshot.stage))return stageConfig_.at(snapshot.stage);return activeConfig_;}
/** @brief 独立导出对话框明确源、单位、preview和覆盖。 */
void MainWindow::exportResult(){
    if(exportTask_.running()||!left_->packet())return;
    QDialog dialog(this); ///< 主线程独立导出参数对话框。
    dialog.setWindowTitle(tr("独立导出完整布局"));QFormLayout layout(&dialog); ///< 栈拥有的对话框布局。
    QComboBox source(&dialog); ///< 明确选择左/右画布源。
    source.addItem(tr("左视图"));if(comparison_&&right_->packet())source.addItem(tr("右视图"));
    QComboBox format(&dialog); ///< 共用服务支持格式。
    format.addItems({"placement","bmp","gds","report"});
    QLabel description(&dialog); ///< 源身份和非最终标记。
    description.setWordWrap(true);
    /** @brief 更新所选源摘要，捕获this/source/description栈引用，仅对话框存活期。
     * @param index 源下拉索引。 无返回值。 */
    const auto describe=[this,&source,&description](int index){Q_UNUSED(index)const auto snapshot=(source.currentIndex()==1?right_:left_)->packet()->snapshot; /* snapshot 为本次说明源。 */description.setText(QString::fromStdString(snapshot->stage)+tr(" · iteration %1 · revision %2\n单位 %3 · %4\n%5").arg(snapshot->iteration).arg(snapshot->revision).arg(QString::fromStdString(snapshot->database->unit)).arg(snapshot->database->physicalScaleKnown?tr("物理比例已知"):tr("物理比例未知，DU 显示映射")).arg(snapshot->stage=="preview"||snapshot->termination.empty()?tr("preview：非最终完整安全点"):tr("已完成阶段")));};
    connect(&source,&QComboBox::currentIndexChanged,&dialog,describe);describe(0);layout.addRow(tr("源快照"),&source);layout.addRow(&description);layout.addRow(tr("格式"),&format);
    QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); ///< 明确提交导出请求。
    layout.addRow(&buttons);connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);if(dialog.exec()!=QDialog::Accepted)return;
    const auto snapshot=(source.currentIndex()==1?right_:left_)->packet()->snapshot; ///< 本次导出固定持有一份源。
    const QString extension=format.currentText()=="placement"?"json":format.currentText()=="report"?"json":format.currentText(); ///< 合理的格式扩展名。
    const QString name=QString::fromStdString(snapshot->termination.empty()?"preview":snapshot->stage)+QString("_i%1_r%2.").arg(snapshot->iteration).arg(snapshot->revision)+extension; ///< 含preview/stage及迭代身份的建议文件名。
    const QString path=QFileDialog::getSaveFileName(this,tr("保存 %1（源 %2）").arg(format.currentText(),QString::fromStdString(snapshot->stage)),name); ///< 用户确定的导出路径。
    if(path.isEmpty())return;
    const bool overwrite=QFileInfo::exists(path); ///< 保存对话框已获得用户覆盖选择。
    exportSnapshot(source.currentIndex()==1,format.currentText().toStdString(),path.toStdString(),overwrite);
}
/** @brief 程序化与对话框共用相同独立任务；显式捕获源，失败不改变主状态。 */
bool MainWindow::exportSnapshot(bool right,std::string format,std::string path,bool overwrite){
    const auto packet=(right?right_:left_)->packet(); // 当前选定完整不可变源，可在主任务继续时持有。
    if(!packet||exportTask_.running()||closing_)return false;
    const auto snapshot=packet->snapshot; // 独立导出线程拥有的源引用。
    exportId_=NewRunId();
    if(!exportTask_.start(ExportJob(snapshot,std::move(format),path,sourceConfig(*snapshot),overwrite),snapshot->datasetId,exportId_))return false;
    exportStatus_->setObjectName("exportStatus");exportStatus_->setText(tr("导出中：%1").arg(QString::fromStdString(path)));
    log(tr("独立导出 %1，源 %2 / iteration %3 / revision %4").arg(QString::fromStdString(path),QString::fromStdString(snapshot->stage)).arg(snapshot->iteration).arg(snapshot->revision));setState(state_);return true;
}
/** @brief 当前viewport截图命名明确，不冒充完整布局BMP。 */
void MainWindow::screenshot(){if(!left_->packet())return;const QString path=QFileDialog::getSaveFileName(this,tr("保存当前视口截图（非完整布局）"),"viewport.png",tr("PNG (*.png)")); /* path 为截图目标。 */if(path.isEmpty())return;if(!left_->viewport()->grab().save(path))log(tr("ERROR 无法保存视口截图：%1").arg(path));else log(tr("已保存当前视口截图：%1").arg(path));}
/** @brief 内存日志环形保持5000条。 */
void MainWindow::log(QString text){logs_.push_back(std::move(text));if(logs_.size()>5000)logs_.pop_front();refreshLogs();}
/** @brief 按文本和严重度过滤内存日志；完整日志由应用层流式保存。 */
void MainWindow::refreshLogs(){QStringList visible; /* visible 为本次最多5000条过滤结果。 */for(const QString& line:logs_){if(!logFilter_->text().isEmpty()&&!line.contains(logFilter_->text(),Qt::CaseInsensitive))continue;if(logLevel_->currentIndex()==1&&!line.contains("ERROR")&&!line.contains("FAIL"))continue;visible.push_back(line);} /* line 为当前环形日志项。 */logView_->setPlainText(visible.join('\n'));}
/** @brief 已确认后只设置取消并保持事件循环，不wait/terminate。 */
void MainWindow::requestShutdown(bool confirmed){if(!confirmed)return;closing_=true;setEnabled(false);mainTask_.cancel();exportTask_.cancel();heatTask_.cancel();networkTask_.cancel();consume();}
/** @brief 活动线程关闭明确确认；线程结束后再次close自然析构。 */
void MainWindow::closeEvent(QCloseEvent* event){if(closeAllowed_||(!mainTask_.running()&&!exportTask_.running()&&!heatTask_.running()&&!networkTask_.running())){event->accept();return;}event->ignore();if(closing_)return;QMessageBox question(QMessageBox::Question,tr("后台任务仍在运行"),tr("取消任务并安全关闭？窗口将保持响应直到后台退出。"),QMessageBox::NoButton,this); /* question 为明确关闭选择。 */auto* cancelAndClose=question.addButton(tr("取消任务并关闭"),QMessageBox::AcceptRole); /* cancelAndClose 为显式协作取消确认。 */question.addButton(tr("返回"),QMessageBox::RejectRole);question.exec();if(question.clickedButton()==cancelAndClose)requestShutdown(true);}
} // namespace placement::gui
