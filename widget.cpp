#include "widget.h"
#include "ui_widget.h"
#include <rfidWidget/IEEE14443ControlWidget.h>
#include <QVBoxLayout>

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget)
{
    ui->setupUi(this);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new IEEE14443ControlWidget(this));
    setLayout(layout);
}

Widget::~Widget()
{
    delete ui;
}
