#include "widget.h"
#include "ui_widget.h"
#include <rfidWidget/IEEE14443ControlWidget.h>

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget)
{
    ui->setupUi(this);
}

Widget::~Widget()
{
    delete ui;
}

void Widget::on_pushButton_clicked()
{
    IEEE14443ControlWidget::showOut();
}
