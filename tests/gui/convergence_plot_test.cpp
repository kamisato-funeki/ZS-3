#include "window.hpp"
#include <QtTest>
#include <QMouseEvent>
#include <QToolTip>
#include <cmath>

using namespace placement;
using namespace placement::gui;
namespace {
/** @brief 统计图中指定区域的曲线填色，容许抗锯齿边缘。 @param image 实际控件截图。 @param area 逻辑像素矩形。 @param color 曲线标准色。 @return 匹配物理像素数，不计算坐标轴或阶段线。 */
int curvePixels(const QImage& image,QRect area,QColor color){
    const double ratio=image.devicePixelRatio(); // 高DPI下从逻辑坐标换算到截图像素。
    const QRect pixels=QRect(QPoint(std::floor(area.left()*ratio),std::floor(area.top()*ratio)),QPoint(std::ceil(area.right()*ratio),std::ceil(area.bottom()*ratio))).intersected(image.rect()); // 有效截图区域。
    int count=0; // 曲线颜色的真实像素数。
    for(int y=pixels.top();y<=pixels.bottom();++y){ // y为截图像素行。
        for(int x=pixels.left();x<=pixels.right();++x){ // x为截图像素列。
            const QColor actual=image.pixelColor(x,y); // 当前抗锯齿混合色。
            if(std::abs(actual.red()-color.red())+std::abs(actual.green()-color.green())+std::abs(actual.blue()-color.blue())<40)++count;
        }
    }
    return count;
}
/** @brief 生成两轴独立但同值的测试序列。 @param values 每个时刻的可空指标。 @return GUI小指标队列，不修改持久化指标。 */
std::deque<MetricPoint> samples(const std::vector<std::optional<double>>& values){
    std::deque<MetricPoint> result; // 按时间排列的小指标。
    for(std::size_t i=0;i<values.size();++i){ // i为测试时间序号。
        MetricPoint point; // 此时刻的两个单位指标。
        point.iteration=i;point.stage=i==0?"global":"standard_legal";point.metrics.hpwl=values[i];point.metrics.overflow=values[i];result.push_back(point);
    }
    return result;
}
}
/** @brief TC-UI-16：使用实际绘图像素与悬停内容验证可空曲线，Qt控件仅在主线程访问。 */
class ConvergencePlotTest:public QObject {
    Q_OBJECT
private slots:
    /** @brief 注册缺失断段、真实零、全缺失和单点边界；无输入、无返回值。 */
    void nullableCurves_data(){
        QTest::addColumn<int>("scenario");
        QTest::newRow("valid-missing-valid")<<0;
        QTest::newRow("real-zero")<<1;
        QTest::newRow("all-missing")<<2;
        QTest::newRow("one-valid-among-missing")<<3;
        QTest::newRow("single-sample")<<4;
        QTest::newRow("all-zero")<<5;
        QTest::newRow("segment-after-missing")<<6;
    }
    /** @brief 两种单位逐一检查实际点/断线/提示，不以队列非空替代绘制；无输入、无返回值。 */
    void nullableCurves(){
        QFETCH(int,scenario); // 数据行指定的可空边界类型。
        std::vector<std::optional<double>> values; // 测试时刻的原始可空值。
        if(scenario==0)values={.8,std::nullopt,.4};
        else if(scenario==1)values={.8,0,.4};
        else if(scenario==2)values={std::nullopt,std::nullopt,std::nullopt};
        else if(scenario==3)values={std::nullopt,.4,std::nullopt};
        else if(scenario==4)values={.4};
        else if(scenario==5)values={0,0,0};
        else values={.8,std::nullopt,.4,.6};
        ConvergencePlot plot; // 800×300逻辑像素，轴范围与样本数独立。
        plot.resize(800,300);plot.append(samples(values));plot.show();QCoreApplication::processEvents();
        const QImage image=plot.grab().toImage(); // 实际paintEvent输出。
        for(int panel=0;panel<2;++panel){ // panel为HPWL/overflow两个独立单位分图。
            const QColor color=panel==0?QColor(76,188,214):QColor(236,172,86); // 对应曲线色，排除轴/文字/阶段线。
            const int top=20+panel*150; // 图区域顶端，逻辑px。
            if(scenario==0){
                QCOMPARE(curvePixels(image,{350,top+4,130,113},color),0); // 整个缺失时刻附近没有跨段连线。
                QVERIFY(curvePixels(image,{51,top-4,9,9},color)>0); // 有效最大值被画出。
                QVERIFY(curvePixels(image,{776,top+56,9,9},color)>0); // 缺失不污染范围：.4在.8的一半高度。
            }else if(scenario==1||scenario==5){
                QVERIFY(curvePixels(image,{413,top+116,10,9},color)>0); // 中间真实零位于纵轴基线。
            }else if(scenario==2){
                QCOMPARE(curvePixels(image,{51,top-4,734,129},color),0); // 全缺失不画任何虚假点或水平零线。
            }else if(scenario==3){
                QVERIFY(curvePixels(image,{413,top-4,10,9},color)>0); // 仅一个有效点仍有标记。
                QCOMPARE(curvePixels(image,{100,top+5,250,110},color),0);
                QCOMPARE(curvePixels(image,{480,top+5,250,110},color),0);
            }else if(scenario==4){
                QVERIFY(curvePixels(image,{51,top-4,9,9},color)>0); // 单样本不发生除零且可见。
                QCOMPARE(curvePixels(image,{100,top+5,630,110},color),0);
            }else{
                QCOMPARE(curvePixels(image,{250,top+4,90,113},color),0); // 不跨越中间缺失时刻。
                QVERIFY(curvePixels(image,{640,top+35,40,30},color)>0); // 后续两个有效点构成新线段。
            }
        }
        const QPoint hover(scenario==4?55:scenario==6?297:418,80); // 最近样本位于中间；单点取第一时刻。
        QMouseEvent event(QEvent::MouseMove,QPointF(hover),QPointF(plot.mapToGlobal(hover)),Qt::NoButton,Qt::NoButton,Qt::NoModifier); // 实际鼠标移动提示事件。
        QCoreApplication::sendEvent(&plot,&event);
        const QString expected=(scenario==0||scenario==2||scenario==6)?QString("—"):(scenario==1||scenario==5)?QString("0"):QString("0.4"); // 独立已知的可空文本。
        QVERIFY(QToolTip::text().contains("HPWL "+expected+" DU"));QVERIFY(QToolTip::text().contains("overflow "+expected));
        QToolTip::hideText();
    }
    /** @brief 一轴缺失不影响另一轴真实零，且压缩断段标志同样阻止跨越；无输入、无返回值。 */
    void independentUnitsAndCompactedGap(){
        auto points=samples({.8,0,.4}); // overflow连续有效，HPWL中间缺失。
        points[1].metrics.hpwl.reset();
        ConvergencePlot plot; // 两种单位分图控件。
        plot.resize(800,300);plot.append(points);
        const QImage image=plot.grab().toImage(); // 独立单位下的实际绘制。
        QCOMPARE(curvePixels(image,{350,24,130,113},QColor(76,188,214)),0);
        QVERIFY(curvePixels(image,{413,286,10,9},QColor(236,172,86))>0);
        plot.clear();points=samples({.8,.4});points.back().breakBefore={true,true};plot.append(points);
        const QImage disconnected=plot.grab().toImage(); // 原缺失样本被压掉后仍须断段。
        QCOMPARE(curvePixels(disconnected,{350,24,130,113},QColor(76,188,214)),0);
        QCOMPARE(curvePixels(disconnected,{350,174,130,113},QColor(236,172,86)),0);
    }
    /** @brief 超过5000点时保留有效极值、末点及被丢弃缺失区间的断段信息；无输入、无返回值。 */
    void compactionPreservesMissingIntervals(){
        std::deque<MetricPoint> points; // 超上限输入，缺失点刻意放在旧桶内部。
        for(std::uint64_t i=0;i<10000;++i){ // i为原始历史时刻。
            MetricPoint point; // 双单位同轮指标，不含坐标。
            point.iteration=i;point.stage=i<42?"global":"macro_legal";
            point.metrics.hpwl=i==45?100:1;point.metrics.overflow=i==46?2:.1;
            if(i==43){point.metrics.hpwl.reset();point.metrics.overflow.reset();}
            points.push_back(point);
        }
        CompactMetrics(points);QVERIFY(points.size()<=5000);QCOMPARE(points.back().iteration,std::uint64_t(9999));
        bool crossed=false; // 是否找到跨越原始43号缺失样本的保留邻点。
        bool hpwlPeak=false; // 100 DU手工极值是否保留。
        bool overflowPeak=false; // 无量纲2极值是否保留。
        bool stageBoundary=false; // 正式阶段首次出现点42是否保留。
        for(std::size_t i=0;i<points.size();++i){ // i为压缩后时间序号。
            hpwlPeak=hpwlPeak||points[i].metrics.hpwl==std::optional<double>(100);
            overflowPeak=overflowPeak||points[i].metrics.overflow==std::optional<double>(2);
            stageBoundary=stageBoundary||points[i].iteration==42;
            if(i&&points[i-1].iteration<43&&points[i].iteration>43){crossed=true;QVERIFY(points[i].breakBefore[0]);QVERIFY(points[i].breakBefore[1]);}
        }
        QVERIFY(crossed);QVERIFY(hpwlPeak);QVERIFY(overflowPeak);QVERIFY(stageBoundary);
    }
};
QTEST_MAIN(ConvergencePlotTest)
#include "convergence_plot_test.moc"
