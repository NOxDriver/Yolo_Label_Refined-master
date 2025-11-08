#include "mainwindow.h"
#include <QProcess>
#include <QFileInfo>
#include <QTextStream>
#include "ui_mainwindow.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QFile>
#include <QJsonObject>

#include <QFileDialog>
#include <QColorDialog>
#include <QKeyEvent>
#include <QDebug>
#include <QShortcut>
#include <QCollator>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <QProcessEnvironment>
#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QAbstractItemView>
static QString locateAutolabelScript();

using std::cout;
using std::endl;
using std::ofstream;
using std::ifstream;
using std::string;

QString locateAutolabelScript() {
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/models/autolabel.py",                // dev/run-in-place
        QCoreApplication::applicationDirPath() + "/../Resources/models/autolabel.py"    // .app bundle
    };
    for (const auto &p : candidates)
        if (QFileInfo::exists(p)) return QFileInfo(p).absoluteFilePath();
    return {};
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    // --- Auto-label config ---
    m_namesPath = ""; // set when user opens names/txt file
    m_pythonPath = resolvePythonPath();
    m_autolabelScript = locateAutolabelScript();

    // --- add a simple menu action programmatically (or add via .ui Designer) ---
    auto *menu = menuBar()->addMenu(tr("Model"));
    auto *actChoose = menu->addAction(tr("Choose model (.pt or .onnx)…"));
    connect(actChoose, &QAction::triggered, this, &MainWindow::on_actionChooseModel_triggered);

    loadModelFromSettings();           // << load persisted ONNX path

    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_S), this), SIGNAL(activated()), this, SLOT(save_label_data()));
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_C), this), SIGNAL(activated()), this, SLOT(clear_label_data()));

    connect(new QShortcut(QKeySequence(Qt::Key_S), this), SIGNAL(activated()), this, SLOT(next_label()));
    connect(new QShortcut(QKeySequence(Qt::Key_W), this), SIGNAL(activated()), this, SLOT(prev_label()));
    connect(new QShortcut(QKeySequence(Qt::Key_A), this), SIGNAL(activated()), this, SLOT(prev_img()));
    connect(new QShortcut(QKeySequence(Qt::Key_D), this), SIGNAL(activated()), this, SLOT(next_img()));
    connect(new QShortcut(QKeySequence(Qt::Key_Space), this), SIGNAL(activated()), this, SLOT(next_img()));
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this), SIGNAL(activated()), this, SLOT(remove_img()));
    connect(new QShortcut(QKeySequence(Qt::Key_Delete), this), SIGNAL(activated()), this, SLOT(remove_img()));
    connect(new QShortcut(QKeySequence(Qt::Key_C), this), SIGNAL(activated()), this, SLOT(on_pushButton_crop_clicked()));

    init_table_widget();

    connect(ui->label_image, &label_img::cropApplied, this, [this]() {
        statusBar()->showMessage(tr("Crop applied"), 4000);
        updateStatusCounts();
    });
    connect(ui->label_image, &label_img::cropCanceled, this, [this]() {
        statusBar()->showMessage(tr("Crop canceled"), 2500);
    });
}



MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_pushButton_open_files_clicked()
{
    bool bRetImgDir     = false;
    open_img_dir(bRetImgDir);

    if (!bRetImgDir) return ;

    if (m_objList.empty())
    {
        bool bRetObjFile    = false;
        open_obj_file(bRetObjFile);
        if (!bRetObjFile) return ;
    }

    init();
}

//void MainWindow::on_pushButton_change_dir_clicked()
//{
//    bool bRetImgDir     = false;

//    open_img_dir(bRetImgDir);

//    if (!bRetImgDir) return ;

//    init();
//}

void MainWindow::updateStatusCounts()
{
    const auto &boxes = ui->label_image->m_objBoundingBoxes;
    const int total   = boxes.size();

    // Size count array to cover both your name list and any out-of-range labels
    int maxLabel = -1;
    for (const auto &b : boxes) if (b.label > maxLabel) maxLabel = b.label;
    const int nClasses = std::max<int>(m_objList.size(), maxLabel + 1);

    QVector<int> counts(nClasses, 0);
    for (const auto &b : boxes) {
        if (b.label >= 0 && b.label < nClasses)
            ++counts[b.label];
    }

    QStringList parts;
    for (int i = 0; i < nClasses; ++i) {
        if (counts[i] == 0) continue; // show only classes present in this image
        const QString name = (i >= 0 && i < m_objList.size())
                             ? m_objList.at(i)
                             : QString("Class %1").arg(i);
        parts << QString("%1 (%2): %3").arg(name).arg(i).arg(counts[i]);
    }

    QString msg = QString("Boxes: %1").arg(total);
    if (!parts.isEmpty())
        msg += " | " + parts.join(" | ");

    statusBar()->showMessage(msg);
}




void MainWindow::init()
{
    m_lastLabeledImgIndex = -1;

    ui->label_image->init();
    // Update counts when boxes change
    connect(ui->label_image, &label_img::boxesChanged,
        this, &MainWindow::updateStatusCounts,
        Qt::UniqueConnection);

    init_button_event();
    init_horizontal_slider();

    int firstVisible = findNextVisibleRow(-1, +1);
    if (firstVisible == -1)
        set_label(0);
    else
        set_label(firstVisible);
    goto_img(0);
}

void MainWindow::set_label_progress(const int fileIndex)
{
    QString strCurFileIndex = QString::number(fileIndex);
    QString strEndFileIndex = QString::number(m_imgList.size() - 1);

    ui->label_progress->setText(strCurFileIndex + " / " + strEndFileIndex);
}

void MainWindow::set_focused_file(const int fileIndex)
{
    QString str = "";

    if(m_lastLabeledImgIndex >= 0)
    {
        str += "Last Labeled Image: " + m_imgList.at(m_lastLabeledImgIndex);
        str += '\n';
    }

    str += "Current Image: " + m_imgList.at(fileIndex);

    ui->textEdit_log->setText(str);
}

void MainWindow::goto_img(const int fileIndex)
{
    refreshImageList();

    if (m_imgList.isEmpty())
        return;

    int clampedIndex = fileIndex;
    if (clampedIndex < 0)
        clampedIndex = 0;
    if (clampedIndex >= m_imgList.size())
        clampedIndex = m_imgList.size() - 1;

    m_imgIndex = clampedIndex;

    bool bImgOpened;
    ui->label_image->openImage(m_imgList.at(m_imgIndex), bImgOpened);

    QString lblPath = get_labeling_data(m_imgList.at(m_imgIndex));
    qDebug() << "[autolabel] image =" << m_imgList.at(m_imgIndex);
    qDebug() << "[autolabel] label =" << lblPath;

    ui->label_image->loadLabelData(lblPath);

    // --- Auto run model if label missing or empty ---
    bool needAuto = true;
    QFileInfo li(lblPath);
    if (li.exists() && li.isFile()) {
        QFile f(lblPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            needAuto = (f.size() == 0);
            f.close();
        }
    }
    if (needAuto && !m_namesPath.isEmpty()) {
        QString program = m_pythonPath;
        QStringList args;
        args << m_autolabelScript
             << m_imgList.at(m_imgIndex)   // image path
             << lblPath                    // label path to create
             << m_namesPath;               // classes file
        // If we have a persisted model, pass it as the 4th arg
        if (!m_modelOverrideOnnx.isEmpty())
            args << m_modelOverrideOnnx;

        QProcess p;

        // Environment: make Spotlight launches work (PATH) + model env var (belt & braces)
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString path = env.value("PATH");
        if (path.isEmpty()) path = "/usr/bin:/bin:/usr/sbin:/sbin";
        if (!path.contains("/opt/homebrew/bin"))
            path += ":/opt/homebrew/bin:/usr/local/bin";
        env.insert("PATH", path);
        if (!m_modelOverrideOnnx.isEmpty())
            env.insert("YOLO_MODEL_PATH", m_modelOverrideOnnx);
        p.setProcessEnvironment(env);

        // Run from script dir so relative paths inside autolabel.py work
        p.setWorkingDirectory(QFileInfo(m_autolabelScript).absolutePath());

        p.start(program, args);
        if (!p.waitForStarted(5000)) {
            qDebug() << "[autolabel] failed to start" << program << ":" << p.errorString();
        } else {
            p.waitForFinished(60000);
        }

        qDebug() << "[autolabel] exitCode=" << p.exitCode();
        if (auto out = p.readAllStandardOutput(); !out.isEmpty())
            qDebug() << "[autolabel][stdout]" << out;
        if (auto err = p.readAllStandardError(); !err.isEmpty())
            qDebug() << "[autolabel][stderr]" << err;

        // Reload labels if the script produced them
        ui->label_image->loadLabelData(lblPath);
    }



    // --- Read one-time confidences written by autolabel.py, then delete the file ---
    // --- Read one-time confidences written by autolabel.py, then delete the file ---
    ui->label_image->m_confForThisImage.clear();
    {
        QFile jf(lblPath + ".json");
        if (jf.exists() && jf.open(QIODevice::ReadOnly)) {
            QJsonParseError pe;
            const QByteArray raw = jf.readAll();
            jf.close();

            QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
            if (pe.error == QJsonParseError::NoError) {
                QVector<double> confs;

                auto readArray = [&](const QJsonArray &arr) {
                    for (const QJsonValue &v : arr) {
                        if (v.isObject()) {
                            QJsonObject o = v.toObject();
                            if (o.contains("conf"))        confs.push_back(o.value("conf").toDouble());
                            else if (o.contains("confidence")) confs.push_back(o.value("confidence").toDouble());
                        } else if (v.isDouble()) {
                            confs.push_back(v.toDouble());
                        }
                    }
                };

                if (doc.isArray()) {
                    readArray(doc.array());
                } else if (doc.isObject()) {
                    QJsonObject o = doc.object();
                    if (o.value("confs").isArray())       readArray(o.value("confs").toArray());
                    else if (o.value("detections").isArray()) readArray(o.value("detections").toArray());
                }

                ui->label_image->m_confForThisImage = confs;
            }

            QFile::remove(lblPath + ".json"); // delete it immediately
        }
    }


    emit ui->label_image->boxesChanged();
    ui->label_image->showImage();
    updateStatusCounts();

    set_label_progress(m_imgIndex);
    set_focused_file(m_imgIndex);

    ui->horizontalSlider_images->blockSignals(true);
    ui->horizontalSlider_images->setRange(0, std::max(0, m_imgList.size() - 1));
    ui->horizontalSlider_images->setValue(m_imgIndex);
    ui->horizontalSlider_images->blockSignals(false);
}


void MainWindow::next_img(bool bSavePrev)
{
    if(bSavePrev && ui->label_image->isOpened()) save_label_data();
    goto_img(m_imgIndex + 1);
}

void MainWindow::prev_img(bool bSavePrev)
{
    if(bSavePrev) save_label_data();
    goto_img(m_imgIndex - 1);
}

void MainWindow::save_label_data()
{
    if(m_imgList.size() == 0) return;

    QString qstrOutputLabelData = get_labeling_data(m_imgList.at(m_imgIndex));
    ofstream fileOutputLabelData(qPrintable(qstrOutputLabelData));

    if(fileOutputLabelData.is_open())
    {
        for(int i = 0; i < ui->label_image->m_objBoundingBoxes.size(); i++)
        {
            ObjectLabelingBox objBox = ui->label_image->m_objBoundingBoxes[i];

            double midX     = objBox.box.x() + objBox.box.width() / 2.;
            double midY     = objBox.box.y() + objBox.box.height() / 2.;
            double width    = objBox.box.width();
            double height   = objBox.box.height();

            fileOutputLabelData << objBox.label;
            fileOutputLabelData << " ";
            fileOutputLabelData << std::fixed << std::setprecision(6) << midX;
            fileOutputLabelData << " ";
            fileOutputLabelData << std::fixed << std::setprecision(6) << midY;
            fileOutputLabelData << " ";
            fileOutputLabelData << std::fixed << std::setprecision(6) << width;
            fileOutputLabelData << " ";
            fileOutputLabelData << std::fixed << std::setprecision(6) << height << std::endl;
        }
        m_lastLabeledImgIndex = m_imgIndex;
        fileOutputLabelData.close();
    }

    if (ui->label_image->hasPendingImageChanges()) {
        if (!ui->label_image->saveCurrentImage(m_imgList.at(m_imgIndex))) {
            qWarning() << "Failed to save cropped image" << m_imgList.at(m_imgIndex);
        }
    }
}

void MainWindow::clear_label_data()
{
    ui->label_image->m_objBoundingBoxes.clear();
    ui->label_image->showImage();
}

void MainWindow::remove_img()
{
    if(m_imgList.size() > 0) {
        //remove a image
        QFile::remove(m_imgList.at(m_imgIndex));

        //remove a txt file
        QString qstrOutputLabelData = get_labeling_data(m_imgList.at(m_imgIndex));
        QFile::remove(qstrOutputLabelData);

        m_imgList.removeAt(m_imgIndex);

        if(m_imgList.size() == 0)
        {
            pjreddie_style_msgBox(QMessageBox::Information,"End", "In directory, there are not any image. program quit.");
            QCoreApplication::quit();
        }
        else if( m_imgIndex == m_imgList.size())
        {
            m_imgIndex--;
        }

        goto_img(m_imgIndex);
    }
}

void MainWindow::next_label()
{
    int current = m_objIndex;
    if (current < 0)
        current = -1;

    int next = findNextVisibleRow(current, +1);
    if (next != -1)
        set_label(next);
}

void MainWindow::prev_label()
{
    int current = m_objIndex;
    if (current < 0)
        current = ui->tableWidget_label->rowCount();

    int prev = findNextVisibleRow(current, -1);
    if (prev != -1)
        set_label(prev);
}

void MainWindow::load_label_list_data(QString qstrLabelListFile)
{
    ifstream inputLabelListFile(qPrintable(qstrLabelListFile));

    if(inputLabelListFile.is_open()) {
        // Remember the class names file for auto-label
        m_namesPath = qstrLabelListFile;

        for(int i = 0 ; i <= ui->tableWidget_label->rowCount(); i++)
            ui->tableWidget_label->removeRow(ui->tableWidget_label->currentRow());

        m_objList.clear();

        ui->tableWidget_label->setRowCount(0);
        ui->label_image->m_drawObjectBoxColor.clear();

        string strLabel;
        int fileIndex = 0;
        while(getline(inputLabelListFile, strLabel))
        {
            int nRow = ui->tableWidget_label->rowCount();
  
            QString qstrLabel   = QString().fromStdString(strLabel);
            QColor  labelColor  = label_img::BOX_COLORS[(fileIndex++)%10];
            m_objList << qstrLabel;

            ui->tableWidget_label->insertRow(nRow);

            ui->tableWidget_label->setItem(nRow, 0, new QTableWidgetItem(qstrLabel));
            ui->tableWidget_label->item(nRow, 0)->setFlags(ui->tableWidget_label->item(nRow, 0)->flags() ^  Qt::ItemIsEditable);

            ui->tableWidget_label->setItem(nRow, 1, new QTableWidgetItem(QString().fromStdString("")));
            ui->tableWidget_label->item(nRow, 1)->setBackground(labelColor);
            ui->tableWidget_label->item(nRow, 1)->setFlags(ui->tableWidget_label->item(nRow, 1)->flags() ^  Qt::ItemIsEditable ^  Qt::ItemIsSelectable);

            ui->label_image->m_drawObjectBoxColor.push_back(labelColor);
        }
        ui->label_image->m_objList = m_objList;
        applyClassFilter(ui->lineEdit_class_filter->text());
    }
}

QString MainWindow::get_labeling_data(QString qstrImgFile) const
{
    QFileInfo fi(qstrImgFile);
    QDir imgDir = fi.dir();        // e.g. .../images_abc
    QString imgDirName = imgDir.dirName();  // "images_abc"

    QDir classDir = imgDir;
    classDir.cdUp();               // go up one level

    // derive corresponding labels folder name
    QString labelsDirName = imgDirName;
    labelsDirName.replace("images", "labels", Qt::CaseInsensitive);

    QDir labelsDir(classDir.absoluteFilePath(labelsDirName));

    QString stem = fi.completeBaseName();
    return labelsDir.absoluteFilePath(stem + ".txt");
}



void MainWindow::set_label(const int labelIndex)
{
    bool bIndexIsOutOfRange = (labelIndex < 0 || labelIndex > m_objList.size() - 1);

    if(!bIndexIsOutOfRange)
    {
        if (ui->tableWidget_label->isRowHidden(labelIndex)) {
            QSignalBlocker blocker(ui->lineEdit_class_filter);
            ui->lineEdit_class_filter->clear();
            applyClassFilter(QString());
        }

        m_objIndex = labelIndex;
        ui->label_image->setFocusObjectLabel(m_objIndex);
        ui->label_image->setFocusObjectName(m_objList.at(m_objIndex));
        ui->tableWidget_label->setCurrentCell(m_objIndex, 0);
    }
    updateStatusCounts();

}

void MainWindow::set_label_color(const int index, const QColor color)
{
    ui->label_image->m_drawObjectBoxColor.replace(index, color);
}

void MainWindow::pjreddie_style_msgBox(QMessageBox::Icon icon, QString title, QString content)
{
    QMessageBox msgBox(icon, title, content, QMessageBox::Ok);

    QFont font;
    font.setBold(true);
    msgBox.setFont(font);
    msgBox.button(QMessageBox::Ok)->setFont(font);
    msgBox.button(QMessageBox::Ok)->setStyleSheet("border-style: outset; border-width: 2px; border-color: rgb(0, 255, 0); color : rgb(0, 255, 0);");
    msgBox.button(QMessageBox::Ok)->setFocusPolicy(Qt::ClickFocus);
    msgBox.setStyleSheet("background-color : rgb(34, 0, 85); color : rgb(0, 255, 0);");

    msgBox.exec();
}

QString MainWindow::resolvePythonPath() const
{
    auto tryProg = [](const QString &prog) {
        QProcess p;
        p.start(prog, {"-V"});
        return p.waitForFinished(1500)
            && p.exitStatus() == QProcess::NormalExit
            && p.error() == QProcess::UnknownError;
    };

    // Good macOS candidates
    const QStringList candidates = {
	"/opt/anaconda3/bin/python",
        "/usr/bin/python3",
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "python3",
        "python"
    };

    for (const auto &c : candidates)
        if (tryProg(c)) return c;

    return "python3"; // last resort
}


void MainWindow::refreshImageList()
{
    if (m_imgDir.isEmpty())
        return;

    QDir dir(m_imgDir);
    if (!dir.exists())
        return;

    QCollator collator;
    collator.setNumericMode(true);

    QStringList fileList = dir.entryList(
        QStringList() << "*.jpg" << "*.JPG" << "*.png" << "*.bmp",
        QDir::Files);

    std::sort(fileList.begin(), fileList.end(), collator);

    QStringList absolute;
    absolute.reserve(fileList.size());
    for (const QString &file : fileList)
        absolute.append(dir.absoluteFilePath(file));

    if (absolute == m_imgList)
        return;

    QString currentPath;
    if (m_imgIndex >= 0 && m_imgIndex < m_imgList.size())
        currentPath = m_imgList.at(m_imgIndex);

    m_imgList = absolute;

    if (m_imgList.isEmpty()) {
        m_imgIndex = -1;
        ui->horizontalSlider_images->setRange(0, 0);
        return;
    }

    if (!currentPath.isEmpty()) {
        int idx = m_imgList.indexOf(currentPath);
        if (idx != -1)
            m_imgIndex = idx;
        else if (m_imgIndex >= m_imgList.size())
            m_imgIndex = m_imgList.size() - 1;
    } else {
        if (m_imgIndex < 0)
            m_imgIndex = 0;
        else if (m_imgIndex >= m_imgList.size())
            m_imgIndex = m_imgList.size() - 1;
    }

    ui->horizontalSlider_images->setRange(0, std::max(0, m_imgList.size() - 1));
}

void MainWindow::open_img_dir(bool& ret)
{
    // pjreddie_style_msgBox(QMessageBox::Information,"Help", "Step 1. Open Your Data Set Directory");

    QString opened_dir;

    if(m_imgDir.size() > 0) opened_dir = m_imgDir;
    else opened_dir = QDir::currentPath();

    QString imgDir = QFileDialog::getExistingDirectory(
                nullptr,
                tr("Open Dataset Directory"),
                opened_dir,
                QFileDialog::ShowDirsOnly);

    QDir dir(imgDir);
    QCollator collator;
    collator.setNumericMode(true);

    QStringList fileList = dir.entryList(
                QStringList() << "*.jpg" << "*.JPG" << "*.png" << "*.bmp",
                QDir::Files);

    std::sort(fileList.begin(), fileList.end(), collator);

    if(fileList.empty())
    {
        ret = false;
        return;  // silently ignore instead of popup
    }

    else
    {
        ret = true;
        m_imgDir    = imgDir;
        m_imgList  = fileList;

        for(QString& str: m_imgList)
            str = m_imgDir + "/" + str;
    }
}

void MainWindow::open_obj_file(bool& ret)
{
   // pjreddie_style_msgBox(QMessageBox::Information,"Help", "Step 2. Open Your Label List File(*.txt or *.names)");

    QString opened_dir;
    if(m_imgDir.size() > 0) opened_dir = m_imgDir;
    else opened_dir = QDir::currentPath();

    QString fileLabelList = QFileDialog::getOpenFileName(
                nullptr,
                tr("Open LabelList file"),
                opened_dir,
                tr("LabelList Files (*.txt *.names)"));

    if(fileLabelList.isEmpty())
    {
        ret = false;
        return; // cancel silently
    }
    else
    {
        ret = true;
        load_label_list_data(fileLabelList);
    }
}

void MainWindow::wheelEvent(QWheelEvent *ev)
{
    Q_UNUSED(ev);
    // Disable scroll navigation entirely
}

void MainWindow::on_pushButton_prev_clicked()
{
    prev_img();
}

void MainWindow::on_pushButton_next_clicked()
{
    next_img();
}

void MainWindow::keyPressEvent(QKeyEvent * event)
{
    int     nKey = event->key();

    bool    graveAccentKeyIsPressed    = (nKey == Qt::Key_QuoteLeft);
    bool    numKeyIsPressed            = (nKey >= Qt::Key_0 && nKey <= Qt::Key_9 );

    if(graveAccentKeyIsPressed)
    {
        set_label(0);
    }
    else if(numKeyIsPressed)
    {
        int asciiToNumber = nKey - Qt::Key_0;

        if(asciiToNumber < m_objList.size() )
        {
            set_label(asciiToNumber);
        }
    }
}

void MainWindow::on_pushButton_crop_clicked()
{
    if (!ui->label_image->isOpened()) {
        statusBar()->showMessage(tr("Open an image before cropping."), 3000);
        return;
    }

    ui->label_image->beginCropSelection();
    statusBar()->showMessage(
        tr("Crop mode: drag to select an area. Click again to apply, right-click to cancel."),
        6000);
}

//void MainWindow::on_pushButton_save_clicked()
//{
//    save_label_data();
//}

//void MainWindow::on_pushButton_remove_clicked()
//{
//    remove_img();
//}

void MainWindow::on_tableWidget_label_cellDoubleClicked(int row, int column)
{
    bool bColorAttributeClicked = (column == 1);

    if(bColorAttributeClicked)
    {
        QColor color = QColorDialog::getColor(Qt::white,nullptr,"Set Label Color");
        if(color.isValid())
        {
            set_label_color(row, color);
            ui->tableWidget_label->item(row, 1)->setBackground(color);
        }
        set_label(row);
        ui->label_image->showImage();
    }
}

void MainWindow::on_tableWidget_label_cellClicked(int row, int column)
{
    set_label(row);
}

void MainWindow::on_lineEdit_class_filter_textChanged(const QString &text)
{
    applyClassFilter(text);
}

void MainWindow::on_horizontalSlider_images_sliderMoved(int position)
{
    goto_img(position);
}

void MainWindow::init_button_event()
{
//    ui->pushButton_change_dir->setEnabled(true);
}

void MainWindow::init_horizontal_slider()
{
    ui->horizontalSlider_images->setEnabled(true);
    ui->horizontalSlider_images->setRange(0, m_imgList.size() - 1);
    ui->horizontalSlider_images->blockSignals(true);
    ui->horizontalSlider_images->setValue(0);
    ui->horizontalSlider_images->blockSignals(false);

    ui->horizontalSlider_contrast->setEnabled(true);
    ui->horizontalSlider_contrast->setRange(0, 1000);
    ui->horizontalSlider_contrast->setValue(ui->horizontalSlider_contrast->maximum()/2);
    ui->label_image->setContrastGamma(1.0);
    ui->label_contrast->setText(QString("Contrast(%) ") + QString::number(50));
}

void MainWindow::init_table_widget()
{
    ui->tableWidget_label->horizontalHeader()->setVisible(true);
    ui->tableWidget_label->horizontalHeader()->setStyleSheet("");
    ui->tableWidget_label->horizontalHeader()->setStretchLastSection(true);

    disconnect(ui->tableWidget_label->horizontalHeader(), SIGNAL(sectionPressed(int)),ui->tableWidget_label, SLOT(selectColumn(int)));
}

void MainWindow::on_horizontalSlider_contrast_sliderMoved(int value)
{
    float valueToPercentage = float(value)/ui->horizontalSlider_contrast->maximum(); //[0, 1.0]
    float percentageToGamma = std::pow(1/(valueToPercentage + 0.5), 7.);

    ui->label_image->setContrastGamma(percentageToGamma);
    ui->label_contrast->setText(QString("Contrast(%) ") + QString::number(int(valueToPercentage * 100.)));
}

void MainWindow::on_checkBox_visualize_class_name_clicked(bool checked)
{
    ui->label_image->m_bVisualizeClassName = checked;
    ui->label_image->showImage();
}

void MainWindow::applyClassFilter(const QString &text)
{
    if (!ui->tableWidget_label)
        return;

    const QString term = text.trimmed();
    const int rowCount = ui->tableWidget_label->rowCount();
    int firstMatch = -1;

    for (int row = 0; row < rowCount; ++row) {
        bool match = term.isEmpty();
        if (!match) {
            QTableWidgetItem *item = ui->tableWidget_label->item(row, 0);
            match = item && item->text().contains(term, Qt::CaseInsensitive);
        }

        ui->tableWidget_label->setRowHidden(row, !match);
        if (match && firstMatch == -1)
            firstMatch = row;
    }

    if (term.isEmpty()) {
        statusBar()->clearMessage();
        return;
    }

    if (firstMatch == -1) {
        statusBar()->showMessage(tr("No classes match \"%1\"").arg(term), 4000);
        return;
    }

    if (ui->tableWidget_label->isRowHidden(m_objIndex)) {
        QSignalBlocker blocker(ui->tableWidget_label);
        set_label(firstMatch);
    } else {
        if (auto *item = ui->tableWidget_label->item(firstMatch, 0))
            ui->tableWidget_label->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

int MainWindow::findNextVisibleRow(int start, int step) const
{
    if (!ui->tableWidget_label)
        return -1;

    const int rowCount = ui->tableWidget_label->rowCount();
    if (rowCount == 0)
        return -1;

    int row = start;
    while (true) {
        row += step;
        if (row < 0 || row >= rowCount)
            return -1;
        if (!ui->tableWidget_label->isRowHidden(row))
            return row;
    }
}

QString MainWindow::appModelsDir() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir d(base + "/models");
    if (!d.exists()) d.mkpath(".");
    return d.absolutePath();
}

void MainWindow::loadModelFromSettings() {
    QSettings s;
    m_modelOverrideOnnx = s.value("modelOverrideOnnx").toString(); // may be empty
    if (!m_modelOverrideOnnx.isEmpty())
        statusBar()->showMessage(tr("Using model: %1").arg(m_modelOverrideOnnx), 5000);
}

void MainWindow::saveModelToSettings(const QString &onnxPath) {
    QSettings s;
    s.setValue("modelOverrideOnnx", onnxPath);
    m_modelOverrideOnnx = onnxPath;
}

void MainWindow::on_actionChooseModel_triggered() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose model"),
        QDir::homePath(),
        tr("Models (*.onnx *.pt)")
    );
    if (file.isEmpty()) return;

    QString onnx;
    QString err;
    if (file.endsWith(".onnx", Qt::CaseInsensitive)) {
        // Copy to our app data models dir (so it doesn't break if user moves it)
        const QString dstDir = appModelsDir();
        const QString dst = QDir(dstDir).filePath(QFileInfo(file).fileName());
        if (!QFile::exists(dst) || QFileInfo(file).lastModified() > QFileInfo(dst).lastModified())
            QFile::remove(dst), QFile::copy(file, dst);
        onnx = dst;
    } else {
        if (!convertPtToOnnx(file, onnx, &err)) {
            pjreddie_style_msgBox(QMessageBox::Critical, "Model convert failed", err.isEmpty()? "Conversion failed." : err);
            return;
        }
    }

    saveModelToSettings(onnx);
    pjreddie_style_msgBox(QMessageBox::Information, "Model set",
                          tr("Default model set to:\n%1\n\nIt will be used next runs too.").arg(onnx));
}

bool MainWindow::convertPtToOnnx(const QString &ptPath, QString &onnxOut, QString *errOut) {
    const QString outDir = appModelsDir();
    const QString baseName = QFileInfo(ptPath).completeBaseName();
    const QString targetOnnx = QDir(outDir).filePath(baseName + ".onnx");

    QProcess p;
    p.setProgram(m_pythonPath);

    // Python snippet to run
    const char *py =
        "import sys,os,shutil,subprocess\n"
        "from pathlib import Path\n"
        "pt=sys.argv[1]; out=sys.argv[2]\n"
        "# Ensure ultralytics is installed\n"
        "try:\n"
        "  from ultralytics import YOLO\n"
        "except ImportError:\n"
        "  subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'ultralytics'])\n"
        "  from ultralytics import YOLO\n"
        "try:\n"
        "  m=YOLO(pt)\n"
        "  m.export(format='onnx', imgsz=640, opset=12, dynamic=False, simplify=True, device='cpu')\n"
        "except Exception:\n"
        "  import traceback; traceback.print_exc(); sys.exit(3)\n"
        "# pick newest .onnx near the .pt\n"
        "ptdir=str(Path(pt).resolve().parent)\n"
        "cands=[str(p) for p in Path(ptdir).rglob('*.onnx')]\n"
        "if not cands: sys.exit(4)\n"
        "cand=max(cands, key=os.path.getmtime)\n"
        "shutil.copy2(cand, out)\n"
        "print(out)\n";

    p.setArguments({ "-c", QString::fromUtf8(py), ptPath, targetOnnx });

    // PATH for Spotlight; Homebrew locations
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString path = env.value("PATH");
    if (path.isEmpty()) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    if (!path.contains("/opt/homebrew/bin"))
        path += ":/opt/homebrew/bin:/usr/local/bin";
    env.insert("PATH", path);
    p.setProcessEnvironment(env);

    p.start();
    if (!p.waitForFinished(180000)) {
        if (errOut) *errOut = "Python conversion timed out.";
        return false;
    }
    if (p.exitCode() != 0) {
        if (errOut) *errOut = QString("Converter exit=%1\nstdout:\n%2\nstderr:\n%3")
                                .arg(p.exitCode())
                                .arg(QString::fromUtf8(p.readAllStandardOutput()))
                                .arg(QString::fromUtf8(p.readAllStandardError()));
        return false;
    }
    if (!QFileInfo::exists(targetOnnx)) {
        if (errOut) *errOut = "Converted .onnx not found.";
        return false;
    }
    onnxOut = targetOnnx;
    return true;
}

