#include "shared/dialog_chrome.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QInputDialog>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace {
class DialogTitle : public QWidget
{
public:
    explicit DialogTitle(QDialog *dialog) : QWidget(dialog), m_dialog(dialog) {
        setFixedHeight(28);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0,0,0,0);
        auto *label = new QLabel(dialog->windowTitle(), this);
        label->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:14px;font-weight:600;background:transparent;"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label); row->addStretch();
        auto *close = new QToolButton(this);
        close->setObjectName(QStringLiteral("dialogClose"));
        close->setIcon(cardIcon(Icon::CardClose)); close->setFixedSize(28,28);
        close->setAccessibleName(QCoreApplication::translate("OcrDialog","关闭 (Esc)"));
        close->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;}QToolButton:hover{background:#222326;border-radius:6px;}"));
        row->addWidget(close);
        connect(close,&QToolButton::clicked,dialog,&QDialog::reject);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button()==Qt::LeftButton) m_drag=event->globalPosition().toPoint()-m_dialog->pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons()&Qt::LeftButton) m_dialog->move(event->globalPosition().toPoint()-m_drag);
    }
private:
    QDialog *m_dialog;
    QPoint m_drag;
};
class DialogDrag : public QObject
{
public:
    explicit DialogDrag(QDialog *dialog):QObject(dialog),m_dialog(dialog){}
    bool eventFilter(QObject *,QEvent *event) override {
        if(event->type()==QEvent::MouseButtonPress){auto *mouse=static_cast<QMouseEvent *>(event);
            m_pressed=mouse->button()==Qt::LeftButton&&mouse->position().y()<56;
            if(m_pressed)m_offset=mouse->globalPosition().toPoint()-m_dialog->pos();
        }else if(event->type()==QEvent::MouseMove&&m_pressed){auto *mouse=static_cast<QMouseEvent *>(event);
            if(mouse->buttons()&Qt::LeftButton)m_dialog->move(mouse->globalPosition().toPoint()-m_offset);
        }else if(event->type()==QEvent::MouseButtonRelease)m_pressed=false;
        return false;
    }
private:
    QDialog *m_dialog;QPoint m_offset;bool m_pressed=false;
};
void styleMessageBox(QMessageBox *message)
{
    if(message->property("namiDialogChrome").toBool())return;
    message->setProperty("namiDialogChrome",true);
    auto *surface=new QWidget(message);
    surface->setObjectName(QStringLiteral("messageSurface"));
    surface->setAttribute(Qt::WA_TransparentForMouseEvents);
    surface->setStyleSheet(QStringLiteral("background:#18191d;border:1px solid #2b2d33;border-radius:12px;"));
    surface->setGeometry(message->rect());
    surface->show();
    surface->lower();
    class SurfaceSync : public QObject {
    public:
        SurfaceSync(QMessageBox *message,QWidget *surface):QObject(message),m_message(message),m_surface(surface) {}
        bool eventFilter(QObject *,QEvent *event) override {
            if(event->type()==QEvent::Resize||event->type()==QEvent::Show){
                m_surface->setGeometry(m_message->rect());m_surface->lower();
            }
            return false;
        }
    private:
        QMessageBox *m_message;QWidget *m_surface;
    };
    message->installEventFilter(new SurfaceSync(message,surface));
    message->setStyleSheet(QStringLiteral(
        "QLabel{color:#f4f5f8;background:transparent;border:none;}"
        "QLabel#qt_msgbox_informativelabel{color:#9c9da1;font-size:12px;}"
        "QPushButton{background:#18191d;border:1px solid #2b2d33;color:#f4f5f8;"
        "border-radius:6px;padding:0 12px;min-height:32px;font-size:14px;text-align:center;}"
        "QPushButton:hover{background:#222326;}"));
    message->setMinimumSize(480,200);
    if(message->icon()==QMessageBox::Question||message->icon()==QMessageBox::Information)
        message->setIcon(QMessageBox::NoIcon);
    else if(message->icon()==QMessageBox::Warning||message->icon()==QMessageBox::Critical) {
        QPixmap warning=makeIcon(Icon::Warning,24,true).pixmap(24,24);QPainter p(&warning);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);p.fillRect(warning.rect(),QColor("#e9be74"));
        p.end();message->setIconPixmap(warning);
    }
    if(auto *label=message->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
        label->setWordWrap(true);
        const int textWidth=label->fontMetrics().horizontalAdvance(message->text());
        label->setFixedWidth(qMin(432,qMax(220,textWidth+2)));
    }
    if(auto *detail=message->findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel"))) {
        detail->setWordWrap(true);detail->setFixedWidth(432);
    }
    if(message->standardButtons().testFlag(QMessageBox::Yes)&&message->standardButtons().testFlag(QMessageBox::No)) {
        const QString title=message->windowTitle();
        const QString action=title.contains(QCoreApplication::translate("HistoryDialog","删除"))?QCoreApplication::translate("HistoryDialog","删除")
            :title.contains(QCoreApplication::translate("HistoryDialog","清空"))?QCoreApplication::translate("HistoryDialog","清空")
            :QCoreApplication::translate("OcrDialog","重新识别");
        message->button(QMessageBox::Yes)->setText(action);
        message->button(QMessageBox::Yes)->setStyleSheet(QStringLiteral("background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;padding:0 12px;min-height:32px;"));
        message->button(QMessageBox::No)->setText(QCoreApplication::translate("QPlatformTheme","Cancel"));
    }
    if(auto *ok=message->button(QMessageBox::Ok))
        ok->setStyleSheet(QStringLiteral("background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;padding:0 12px;min-height:32px;"));
}
class DialogPolish : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *object,QEvent *event) override {
        if (event->type()==QEvent::Polish)
            if (auto *dialog=qobject_cast<QDialog *>(object)) {
                if (auto *message=qobject_cast<QMessageBox *>(dialog)) {
                    message->setWindowFlag(Qt::FramelessWindowHint);
                    message->setAttribute(Qt::WA_TranslucentBackground);
                    QTimer::singleShot(0,message,[message] {
                        styleMessageBox(message);
                        message->setFixedSize(480,200);
                        if(auto *layout=message->layout()){layout->invalidate();layout->activate();}
                        message->update();
                    });
                } else if (qobject_cast<QInputDialog *>(dialog))
                    styleDialog(dialog);
            }
        return false;
    }
};
class ColorPlane : public QWidget
{
public:
    explicit ColorPlane(QWidget *parent):QWidget(parent) {setMinimumSize(280,200);setCursor(Qt::CrossCursor);}
    QColor color;
    std::function<void(qreal,qreal)> picked;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);p.setBrush(QColor::fromHsvF(qMax(0.0,color.hsvHueF()),1,1));p.drawRoundedRect(rect(),8,8);
        QLinearGradient white(0,0,width(),0);white.setColorAt(0,Qt::white);white.setColorAt(1,QColor(255,255,255,0));p.fillRect(rect(),white);
        QLinearGradient black(0,0,0,height());black.setColorAt(0,QColor(0,0,0,0));black.setColorAt(1,Qt::black);p.fillRect(rect(),black);
        p.setBrush(Qt::NoBrush);p.setPen(QPen(Qt::white,2));
        p.drawEllipse(QPointF(color.hsvSaturationF()*(width()-1),(1-color.valueF())*(height()-1)),6,6);
    }
    void mousePressEvent(QMouseEvent *e) override {if(e->button()==Qt::LeftButton) pick(e->position());}
    void mouseMoveEvent(QMouseEvent *e) override {if(e->buttons()&Qt::LeftButton) pick(e->position());}
private:
    void pick(QPointF p){if(picked)picked(qBound(0.0,p.x()/qMax(1,width()-1),1.0),1-qBound(0.0,p.y()/qMax(1,height()-1),1.0));}
};
}

void styleDialog(QDialog *dialog,bool showTitle)
{
    if(dialog->property("namiDialogChrome").toBool())return;
    dialog->setProperty("namiDialogChrome",true);
    dialog->setWindowFlag(Qt::FramelessWindowHint);
    dialog->setAttribute(Qt::WA_TranslucentBackground);
    auto *body=new QWidget(dialog);
    body->setObjectName(QStringLiteral("dialogContent"));
    if (dialog->layout()) body->setLayout(dialog->layout());
    auto *outer=new QVBoxLayout(dialog);outer->setContentsMargins(0,0,0,0);
    auto *surface=new QWidget(dialog);surface->setObjectName(QStringLiteral("dialogSurface"));
    surface->setStyleSheet(QStringLiteral("QWidget#dialogSurface{background:#18191d;border:1px solid #2b2d33;border-radius:12px;}QWidget#dialogContent{background:transparent;border:none;}"));
    auto *column=new QVBoxLayout(surface);column->setContentsMargins(showTitle?24:0,showTitle?12:0,showTitle?24:0,showTitle?24:0);column->setSpacing(12);
    if(showTitle)column->addWidget(new DialogTitle(dialog));
    column->addWidget(body,1);outer->addWidget(surface);
    if(!showTitle)dialog->installEventFilter(new DialogDrag(dialog));
    if(qobject_cast<QInputDialog *>(dialog))dialog->setMinimumWidth(480);
}

void installDialogChrome(QApplication *application)
{
    application->installEventFilter(new DialogPolish(application));
}

namespace {
void showMessageCard(QWidget *parent,const QString &heading,const QString &detail,
                     bool alert)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("messageCard"));
    dialog.setWindowTitle(alert?QStringLiteral("Nami"):heading);
    auto *layout=new QVBoxLayout(&dialog);
    layout->setContentsMargins(0,0,0,0);
    auto *content=new QWidget(&dialog);
    auto *body=new QVBoxLayout(content);
    body->setContentsMargins(0,0,0,0);body->setSpacing(12);
    if(alert) {
        auto *headingRow=new QHBoxLayout;headingRow->setSpacing(12);
        auto *icon=new QLabel(content);
        QPixmap warning=makeIcon(Icon::Warning,24,true).pixmap(24,24);QPainter painter(&warning);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);painter.fillRect(warning.rect(),QColor("#e9be74"));
        painter.end();icon->setPixmap(warning);icon->setFixedSize(24,24);headingRow->addWidget(icon);
        auto *title=new QLabel(heading,content);
        title->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:16px;font-weight:600;background:transparent;"));
        headingRow->addWidget(title);headingRow->addStretch();body->addLayout(headingRow);
    } else {
        auto *title=new QLabel(heading,content);
        title->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:16px;font-weight:600;background:transparent;"));
        body->addWidget(title);
    }
    auto *text=new QLabel(detail,content);
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("color:#9c9da1;font-size:14px;background:transparent;"));
    body->addWidget(text);body->addStretch();
    auto *buttons=new QHBoxLayout;buttons->addStretch();
    auto *ok=new QPushButton(QCoreApplication::translate("OcrDialog","确定"),content);
    ok->setFixedSize(80,32);
    ok->setStyleSheet(QStringLiteral("QPushButton{background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;font-size:14px;text-align:center;}QPushButton:hover{background:#6f7ae1;}"));
    buttons->addWidget(ok);body->addLayout(buttons);
    layout->addWidget(content);
    QObject::connect(ok,&QPushButton::clicked,&dialog,&QDialog::accept);
    styleDialog(&dialog,alert);
    if(auto *surface=dialog.findChild<QWidget *>(QStringLiteral("dialogSurface")))
        if(auto *column=surface->layout()) {
            column->setContentsMargins(alert?24:22,alert?12:20,alert?24:22,alert?20:18);
            column->setSpacing(alert?12:0);
        }
    dialog.setFixedSize(alert?524:300,alert?260:160);
    dialog.exec();
}
}

void showNotice(QWidget *parent,const QString &heading,const QString &detail)
{
    showMessageCard(parent,heading,detail,false);
}

void showAlert(QWidget *parent,const QString &heading,const QString &detail)
{
    showMessageCard(parent,heading,detail,true);
}

ColorDialog::ColorDialog(const QColor &initial,QWidget *parent):QColorDialog(initial,parent)
{
    setOption(DontUseNativeDialog);setOption(ShowAlphaChannel);setOption(NoButtons);
    setWindowTitle(QCoreApplication::translate("CaptureOverlay","选择颜色"));
    auto *native=new QWidget(this);native->setObjectName(QStringLiteral("nativeColorControls"));
    if(layout())native->setLayout(layout());
    native->hide();
    auto *root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(18);
    auto *fields=new QHBoxLayout;fields->setSpacing(28);
    auto *left=new QVBoxLayout;left->setSpacing(16);
    auto *plane=new ColorPlane(this);plane->setObjectName(QStringLiteral("colorPlane"));plane->setFixedHeight(380);left->addWidget(plane);
    auto *hue=new QSlider(Qt::Horizontal,this);hue->setObjectName(QStringLiteral("colorHue"));hue->setRange(0,359);hue->setFixedHeight(20);
    hue->setStyleSheet(QStringLiteral("QSlider::groove:horizontal{height:12px;border-radius:6px;background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #ff0000,stop:.17 #ffff00,stop:.33 #00ff00,stop:.5 #00ffff,stop:.67 #0000ff,stop:.83 #ff00ff,stop:1 #ff0000);}QSlider::handle:horizontal{width:10px;margin:-2px 0;background:white;border:1px solid #9c9da1;border-radius:4px;}"));left->addWidget(hue);
    fields->addLayout(left,3);
    auto *form=new QVBoxLayout;form->setSpacing(8);
    auto *hex=new QLineEdit(this);hex->setObjectName(QStringLiteral("colorHex"));
    auto *hexLabel=new QLabel(QStringLiteral("HEX"),this);form->addWidget(hexLabel);form->addWidget(hex);
    QList<QSpinBox *> channels;
    for(const QString &name:{QStringLiteral("R"),QStringLiteral("G"),QStringLiteral("B"),QStringLiteral("Alpha")}){
        form->addWidget(new QLabel(name,this));auto *spin=new QSpinBox(this);spin->setObjectName(QStringLiteral("color")+name);
        spin->setRange(0,255);spin->setButtonSymbols(QAbstractSpinBox::NoButtons);spin->setFixedHeight(36);channels.append(spin);form->addWidget(spin);
    }
    hex->setFixedHeight(36);auto *fieldWidget=new QWidget(this);fieldWidget->setLayout(form);fieldWidget->setFixedWidth(200);fields->addWidget(fieldWidget);root->addLayout(fields);root->addStretch();
    auto *buttons=new QHBoxLayout;buttons->setSpacing(12);buttons->addStretch();
    auto *cancel=new QPushButton(QCoreApplication::translate("QPlatformTheme","Cancel"),this);
    auto *ok=new QPushButton(QCoreApplication::translate("QPlatformTheme","OK"),this);
    cancel->setFixedSize(88,32);ok->setFixedSize(88,32);buttons->addWidget(cancel);buttons->addWidget(ok);root->addLayout(buttons);
    connect(ok,&QPushButton::clicked,this,&QDialog::accept);connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
    ok->setDefault(true);
    const auto refresh=[this,plane,hue,hex,channels](const QColor &color){
        plane->color=color;plane->update();
        const QSignalBlocker h(hue),x(hex);hue->setValue(qMax(0,color.hsvHue()));hex->setText(color.name(QColor::HexRgb).toUpper());
        const int values[]={color.red(),color.green(),color.blue(),color.alpha()};
        for(int i=0;i<channels.size();++i){const QSignalBlocker block(channels[i]);channels[i]->setValue(values[i]);}
    };
    connect(this,&QColorDialog::currentColorChanged,this,refresh);
    connect(hex,&QLineEdit::editingFinished,this,[this,hex]{QColor next(hex->text());if(next.isValid()){next.setAlpha(currentColor().alpha());setCurrentColor(next);}});
    for(auto *spin:channels)connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,channels]{setCurrentColor(QColor(channels[0]->value(),channels[1]->value(),channels[2]->value(),channels[3]->value()));});
    connect(hue,&QSlider::valueChanged,this,[this](int value){const QColor c=currentColor();setCurrentColor(QColor::fromHsv(value,c.hsvSaturation(),c.value(),c.alpha()));});
    plane->picked=[this,hue](qreal saturation,qreal value){setCurrentColor(QColor::fromHsvF(hue->value()/360.0,saturation,value,currentColor().alphaF()));};
    setStyleSheet(QStringLiteral("QLabel{color:#9c9da1;font-size:14px;background:transparent;border:none;}QLineEdit,QSpinBox{background:#111216;color:#f4f5f8;border:1px solid #2b2d33;border-radius:6px;padding:0 12px;font-size:14px;}"));
    refresh(initial);styleDialog(this);setMinimumSize(820,600);resize(820,600);
}
