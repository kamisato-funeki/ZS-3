#include "window.hpp"
#include <QApplication>
/** @brief Qt桌面入口。 @param argc 参数数。 @param argv 参数数组。 @return Qt事件循环退出码。 */
int main(int argc,char** argv){QApplication application(argc,argv); /* application 为主线程唯一Qt应用。 */application.setApplicationName("placement_gui");placement::gui::MainWindow window; /* window 拥有全部主线程控件和线程控制器。 */window.show();return application.exec();}
