// Qt6 front end for Rufux, laid out like Rufus.
//
// The window stays in the user's session. Anything that touches a disk runs
// in a separate root process: `pkexec rufux create ... --real --yes`, the same
// worker the CLI uses. This file only builds the command line, shows progress
// and reports the result.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <unistd.h>
#include <vector>

extern "C" {
#include "device.h"
#include "iso_probe.h"
#include "wininstall.h"
}

extern "C" int rufux_gui_run(int argc, char **argv);

#ifndef RUFUX_VERSION
#define RUFUX_VERSION "dev"
#endif

namespace {

const char *kNoIso = "Disk or ISO image (Please select)";

// Largest label each file system accepts.
int labelLimit(const QString &fs) {
  if (fs == "vfat") return 11;
  if (fs == "exfat") return 15;
  if (fs == "ext4") return 16;
  if (fs == "udf") return 30;
  return 32;
}

QString sanitizeLabel(QString in, const QString &fs) {
  QString out;
  for (QChar c : in) {
    if (c == ' ') c = '_';
    if (c.isLetterOrNumber() && c.unicode() < 128) out += c.toUpper();
    else if (c == '_' || c == '-') out += c;
    if (out.size() >= labelLimit(fs)) break;
  }
  return out.isEmpty() ? QStringLiteral("RUFUX") : out;
}

QString humanSize(quint64 bytes) {
  static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(bytes);
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  return QString::number(v, 'f', (i < 2 || v >= 100) ? 0 : 1) + " " + u[i];
}

// "Drive Properties ————" style header, as in Rufus.
QWidget *sectionHeader(const QString &text) {
  auto *w = new QWidget;
  auto *l = new QHBoxLayout(w);
  l->setContentsMargins(0, 10, 0, 0);
  l->setSpacing(10);
  auto *t = new QLabel(text.toUpper());
  t->setObjectName("section");
  auto *line = new QFrame;
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Plain);
  line->setObjectName("rule");
  l->addWidget(t);
  l->addWidget(line, 1);
  return w;
}

// Collapsible "Show advanced ..." row.
struct Advanced {
  QToolButton *toggle;
  QWidget *body;
  Advanced(const QString &text, QWidget *body_) : body(body_) {
    toggle = new QToolButton;
    toggle->setText(text);
    toggle->setCheckable(true);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setAutoRaise(true);
    body->setVisible(false);
    QObject::connect(toggle, &QToolButton::toggled, [this](bool on) {
      toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
      body->setVisible(on);
      if (QWidget *w = toggle->window()) QTimer::singleShot(0, w, [w] { w->adjustSize(); });
    });
  }
};

struct ProbeResult {
  QString path;
  RufuxIsoInfo info{};
  int windows = 0;  // >0 yes, 0 no, <0 unknown
  bool ok = false;
};

class MainWindow : public QWidget {
 public:
  MainWindow() {
    setWindowTitle(QStringLiteral("Rufux ") + RUFUX_VERSION);
    build();
    log(QStringLiteral("Rufux ") + RUFUX_VERSION + " ready. Select a device and an image, then press START.");
    refreshDevices(true);
    auto *poll = new QTimer(this);
    QObject::connect(poll, &QTimer::timeout, [this] { refreshDevices(false); });
    poll->start(2000);
    syncEnabled();
  }

  void openImage(const QString &path) { loadImage(path); }

 protected:
  void closeEvent(QCloseEvent *e) override {
    if (worker && worker->state() != QProcess::NotRunning) {
      QMessageBox::warning(this, QStringLiteral("Operation in progress"),
                           QStringLiteral("A write is running. Press CANCEL first."));
      e->ignore();
      return;
    }
    e->accept();
  }

 private:
  // --- widgets ---
  QFormLayout *drive = nullptr, *format = nullptr;
  QComboBox *devCombo, *bootCombo, *imageCombo, *schemeCombo, *targetCombo, *fsCombo, *clusterCombo, *passesCombo;
  QPushButton *selectBtn, *hashBtn, *startBtn, *closeBtn, *logBtn, *aboutBtn;
  QLabel *imageLabel, *persistLabel, *persistValue, *devCount;
  QSlider *persist;
  QLineEdit *labelEdit;
  QCheckBox *chkFixed, *chkUefi, *chkQuick, *chkExt, *chkBad, *chkWue;
  QProgressBar *bar;
  QLabel *statusLabel;
  QPlainTextEdit *logView = nullptr;
  QDialog *logDialog = nullptr;

  // --- state ---
  QString isoPath;
  bool isoWindows = false;
  QVector<RufuxDevice> devs;
  QString devSignature;
  QProcess *worker = nullptr;
  QString lastError;
  QStringList logLines;
  QString driversDir;

  void build() {
    auto *root = new QVBoxLayout(this);

    // Drive Properties
    root->addWidget(sectionHeader(QStringLiteral("Drive Properties")));
    drive = new QFormLayout;
    drive->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    drive->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    devCombo = new QComboBox;
    drive->addRow("Device", devCombo);

    bootCombo = new QComboBox;
    bootCombo->addItems({kNoIso, "Non bootable", "FreeDOS"});
    selectBtn = new QPushButton("SELECT");
    auto *bootRow = new QHBoxLayout;
    bootRow->addWidget(bootCombo, 1);
    bootRow->addWidget(selectBtn);
    drive->addRow("Boot selection", bootRow);

    imageCombo = new QComboBox;
    imageCombo->addItems({"Write in DD Image mode", "Write in ISO Image mode", "Windows installation"});
    drive->addRow("Image option", imageCombo);
    imageLabel = qobject_cast<QLabel *>(drive->labelForField(imageCombo));

    persist = new QSlider(Qt::Horizontal);
    persist->setRange(0, 128);  // 512 MiB steps, up to 64 GiB
    persistValue = new QLabel("Disabled");
    persistValue->setMinimumWidth(70);
    auto *persistBox = new QWidget;
    auto *pl = new QHBoxLayout(persistBox);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(persist, 1);
    pl->addWidget(persistValue);
    drive->addRow("Persistent partition size", persistBox);
    persistLabel = qobject_cast<QLabel *>(drive->labelForField(persistBox));

    schemeCombo = new QComboBox;
    schemeCombo->addItems({"GPT", "MBR"});
    drive->addRow("Partition scheme", schemeCombo);
    targetCombo = new QComboBox;
    targetCombo->addItems({"BIOS or UEFI", "BIOS (or UEFI-CSM)", "UEFI (non CSM)"});
    drive->addRow("Target system", targetCombo);
    root->addLayout(drive);

    auto *advDrive = new QWidget;
    auto *adl = new QVBoxLayout(advDrive);
    adl->setContentsMargins(18, 0, 0, 0);
    chkFixed = new QCheckBox("List all drives, including internal disks (dangerous)");
    chkUefi = new QCheckBox("Validate the UEFI bootloader after copying");
    adl->addWidget(chkFixed);
    adl->addWidget(chkUefi);
    auto *adv1 = new Advanced("Show advanced drive properties", advDrive);
    root->addWidget(adv1->toggle);
    root->addWidget(advDrive);

    // Format Options
    root->addWidget(sectionHeader(QStringLiteral("Format Options")));
    format = new QFormLayout;
    format->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    format->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    labelEdit = new QLineEdit("RUFUX");
    format->addRow("Volume label", labelEdit);
    fsCombo = new QComboBox;
    fsCombo->addItems({"FAT32", "NTFS", "exFAT", "UDF", "ext4"});
    format->addRow("File system", fsCombo);
    clusterCombo = new QComboBox;
    clusterCombo->addItems({"Default", "4096 bytes", "8192 bytes", "16384 bytes", "32768 bytes", "65536 bytes"});
    format->addRow("Cluster size", clusterCombo);
    root->addLayout(format);

    auto *advFmt = new QWidget;
    auto *afl = new QVBoxLayout(advFmt);
    afl->setContentsMargins(18, 0, 0, 0);
    chkQuick = new QCheckBox("Quick format");
    chkQuick->setChecked(true);
    chkExt = new QCheckBox("Create extended label and icon files");
    chkExt->setChecked(true);
    chkBad = new QCheckBox("Check device for bad blocks");
    passesCombo = new QComboBox;
    passesCombo->addItems({"1 pass", "2 passes", "3 passes", "4 passes"});
    passesCombo->setEnabled(false);
    auto *badRow = new QHBoxLayout;
    badRow->addWidget(chkBad);
    badRow->addWidget(passesCombo);
    badRow->addStretch(1);
    chkWue = new QCheckBox("Customize the Windows installation");
    chkWue->setChecked(true);
    afl->addWidget(chkQuick);
    afl->addWidget(chkExt);
    afl->addLayout(badRow);
    afl->addWidget(chkWue);
    auto *adv2 = new Advanced("Show advanced format options", advFmt);
    root->addWidget(adv2->toggle);
    root->addWidget(advFmt);

    // Status
    root->addWidget(sectionHeader(QStringLiteral("Status")));
    statusLabel = new QLabel("READY");
    statusLabel->setObjectName("status");
    statusLabel->setProperty("state", "ready");
    root->addWidget(statusLabel);
    bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(100);
    bar->setTextVisible(false);
    bar->setProperty("state", "ready");
    root->addWidget(bar);

    auto *foot = new QHBoxLayout;
    aboutBtn = new QPushButton("i");
    logBtn = new QPushButton("Log");
    hashBtn = new QPushButton("Checksums");
    for (QPushButton *b : {aboutBtn, logBtn, hashBtn}) b->setObjectName("flat");
    aboutBtn->setToolTip("About Rufux");
    logBtn->setToolTip("Show the log");
    hashBtn->setToolTip("Compute MD5, SHA-1, SHA-256 and SHA-512 of the image");
    aboutBtn->setFixedWidth(32);
    startBtn = new QPushButton("START");
    startBtn->setObjectName("start");
    startBtn->setDefault(true);
    closeBtn = new QPushButton("CLOSE");
    startBtn->setMinimumWidth(96);
    closeBtn->setMinimumWidth(96);
    foot->addWidget(aboutBtn);
    foot->addWidget(logBtn);
    foot->addWidget(hashBtn);
    foot->addStretch(1);
    foot->addWidget(startBtn);
    foot->addWidget(closeBtn);
    root->addLayout(foot);

    devCount = new QLabel;
    devCount->setObjectName("muted");
    root->addWidget(devCount);

    setMinimumWidth(600);
    root->setContentsMargins(20, 12, 20, 16);
    root->setSpacing(8);
    drive->setVerticalSpacing(8);
    drive->setHorizontalSpacing(14);
    format->setVerticalSpacing(8);
    format->setHorizontalSpacing(14);

    // One label column width for both forms, so the fields line up.
    for (QFormLayout *f : {drive, format})
      for (int i = 0; i < f->rowCount(); i++)
        if (QLayoutItem *it = f->itemAt(i, QFormLayout::LabelRole))
          if (QWidget *lw = it->widget()) lw->setMinimumWidth(150);

    // --- wiring ---
    QObject::connect(selectBtn, &QPushButton::clicked, [this] { pickImage(); });
    QObject::connect(hashBtn, &QPushButton::clicked, [this] { showChecksums(); });
    QObject::connect(startBtn, &QPushButton::clicked, [this] { onStart(); });
    QObject::connect(closeBtn, &QPushButton::clicked, [this] { close(); });
    QObject::connect(logBtn, &QPushButton::clicked, [this] { showLog(); });
    QObject::connect(aboutBtn, &QPushButton::clicked, [this] { showAbout(); });
    QObject::connect(chkBad, &QCheckBox::toggled, passesCombo, &QComboBox::setEnabled);
    QObject::connect(chkFixed, &QCheckBox::toggled, [this](bool on) {
      if (on)
        log("Internal disks are now listed. Double-check the device before writing.");
      refreshDevices(true);
    });
    QObject::connect(bootCombo, &QComboBox::currentIndexChanged, [this](int) { syncEnabled(); });
    QObject::connect(imageCombo, &QComboBox::currentIndexChanged, [this](int) { syncEnabled(); });
    QObject::connect(fsCombo, &QComboBox::currentIndexChanged, [this](int) { relabel(); });
    QObject::connect(persist, &QSlider::valueChanged, [this](int v) {
      if (v == 0) persistValue->setText("Disabled");
      else if (v % 2) persistValue->setText(QString::number(v * 512) + " MB");
      else persistValue->setText(QString::number(v / 2) + " GB");
    });
    QObject::connect(targetCombo, &QComboBox::currentIndexChanged, [this](int t) {
      QSignalBlocker b(schemeCombo);
      if (t == 1) schemeCombo->setCurrentIndex(1);       // BIOS -> MBR
      else if (t == 2) schemeCombo->setCurrentIndex(0);  // UEFI -> GPT
    });
    QObject::connect(schemeCombo, &QComboBox::currentIndexChanged, [this](int s) {
      QSignalBlocker b(targetCombo);
      targetCombo->setCurrentIndex(s == 0 ? 2 : 0);  // GPT -> UEFI, MBR -> BIOS or UEFI
    });
    targetCombo->setCurrentIndex(2);
  }

  // --- helpers ---
  QString fsKey() const {
    static const char *keys[] = {"vfat", "ntfs", "exfat", "udf", "ext4"};
    int i = fsCombo->currentIndex();
    return QString::fromLatin1(keys[i >= 0 && i < 5 ? i : 0]);
  }

  void log(const QString &line) {
    const QString s = QDateTime::currentDateTime().toString("HH:mm:ss  ") + line;
    logLines << s;
    if (logView) logView->appendPlainText(s);
  }

  void setStatus(const QString &text) {
    statusLabel->setText(text);
    const char *state = text == "READY" ? "ready" : (text == "FAILED" ? "failed" : "busy");
    for (QWidget *w : {static_cast<QWidget *>(statusLabel), static_cast<QWidget *>(bar)}) {
      w->setProperty("state", state);
      w->style()->unpolish(w);
      w->style()->polish(w);
    }
  }

  void relabel() {
    labelEdit->setText(sanitizeLabel(labelEdit->text(), fsKey()));
  }

  bool busy() const { return worker && worker->state() != QProcess::NotRunning; }

  bool isoMode() const { return bootCombo->currentIndex() == 0; }

  void syncEnabled() {
    const bool run = busy();
    const bool iso = isoMode();
    const int img = imageCombo->currentIndex();
    const bool haveIso = !isoPath.isEmpty();
    drive->setRowVisible(imageCombo, iso && haveIso);
    drive->setRowVisible(persist->parentWidget(), iso && haveIso && img == 1);
    chkWue->setVisible(iso && haveIso && img == 2);
    devCombo->setEnabled(!run);
    bootCombo->setEnabled(!run);
    selectBtn->setEnabled(!run);
    imageCombo->setEnabled(!run);
    schemeCombo->setEnabled(!run);
    targetCombo->setEnabled(!run);
    fsCombo->setEnabled(!run);
    clusterCombo->setEnabled(!run);
    labelEdit->setEnabled(!run);
    hashBtn->setEnabled(!run && haveIso);
    closeBtn->setEnabled(!run);
    startBtn->setText(run ? "CANCEL" : "START");
    startBtn->setProperty("busy", run);
    startBtn->style()->unpolish(startBtn);
    startBtn->style()->polish(startBtn);
    (void)persistLabel;
    (void)imageLabel;
  }

  // --- devices ---
  void refreshDevices(bool force) {
    if (busy()) return;
    RufuxDevice raw[64];
    int n = rufux_list_devices(raw, 64, chkFixed->isChecked() ? 1 : 0);
    QString sig;
    for (int i = 0; i < n; i++) sig += QString::fromUtf8(raw[i].devnode) + ":" + QString::number(raw[i].size_bytes) + ";";
    if (!force && sig == devSignature) return;
    devSignature = sig;
    const QString keep = devCombo->currentData().toString();
    devs.clear();
    devCombo->clear();
    for (int i = 0; i < n; i++) {
      devs << raw[i];
      QString name = QString::fromUtf8(raw[i].model).trimmed();
      if (name.isEmpty()) name = QString::fromUtf8(raw[i].vendor).trimmed();
      if (name.isEmpty()) name = "Disk";
      QString text = QString("%1 (%2) [%3]").arg(name, QString::fromUtf8(raw[i].sysname), humanSize(raw[i].size_bytes));
      if (raw[i].mounted) text += " (mounted)";
      devCombo->addItem(text, QString::fromUtf8(raw[i].devnode));
    }
    int idx = devCombo->findData(keep);
    if (idx >= 0) devCombo->setCurrentIndex(idx);
    devCount->setText(n == 1 ? "1 device found" : QString::number(n) + " devices found");
    if (n == 0 && qEnvironmentVariableIsSet("RUFUX_GUI_DEMO")) {  // screenshots only
      devCombo->addItem("SanDisk Ultra (sdb) [14.9 GB]", "/dev/sdb");
      devCount->setText("1 device found");
    } else if (n == 0) {
      devCombo->addItem("No USB drive found");
    }
  }

  // --- image selection ---
  void pickImage() {
    const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString p = QFileDialog::getOpenFileName(
        this, "Select a disk image", start,
        "Disk images (*.iso *.img *.raw *.bin *.vhd *.dd);;All files (*)");
    if (!p.isEmpty()) loadImage(p);
  }

  void loadImage(const QString &path) {
    setStatus("Reading image...");
    bar->setValue(0);
    auto *w = new QFutureWatcher<ProbeResult>(this);
    QObject::connect(w, &QFutureWatcher<ProbeResult>::finished, [this, w] {
      ProbeResult r = w->result();
      w->deleteLater();
      bar->setValue(100);
      setStatus("READY");
      applyProbe(r);
    });
    w->setFuture(QtConcurrent::run([path] {
      ProbeResult r;
      r.path = path;
      const QByteArray b = path.toLocal8Bit();
      r.ok = rufux_probe_iso_detail(b.constData(), &r.info) == 0;
      char err[256] = {0};
      r.windows = rufux_is_windows_iso(b.constData(), err, sizeof err);
      return r;
    }));
  }

  void applyProbe(const ProbeResult &r) {
    isoPath = r.path;
    isoWindows = r.windows > 0;
    bootCombo->setItemText(0, QFileInfo(r.path).fileName());
    bootCombo->setCurrentIndex(0);
    log("Using image: " + r.path + " (" + humanSize(r.ok ? r.info.size_bytes : QFileInfo(r.path).size()) + ")");
    if (isoWindows) {
      imageCombo->setCurrentIndex(2);
      fsCombo->setCurrentIndex(1);  // NTFS
      log("Windows installation media detected.");
    } else {
      imageCombo->setCurrentIndex(r.ok && r.info.bootable ? 0 : (r.ok ? 1 : 0));
      fsCombo->setCurrentIndex(0);
    }
    const QString lab = r.ok && r.info.label[0] ? QString::fromUtf8(r.info.label) : QString("RUFUX");
    labelEdit->setText(sanitizeLabel(lab, fsKey()));
    syncEnabled();
  }

  // --- checksums ---
  void showChecksums() {
    const QString path = isoPath;
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("Checksums");
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto *l = new QVBoxLayout(dlg);
    auto *text = new QPlainTextEdit("Computing...\n");
    text->setReadOnly(true);
    text->setMinimumSize(560, 140);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    l->addWidget(new QLabel(QFileInfo(path).fileName()));
    l->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    l->addWidget(bb);
    dlg->show();
    auto *w = new QFutureWatcher<QString>(dlg);
    QObject::connect(w, &QFutureWatcher<QString>::finished, [text, w] { text->setPlainText(w->result()); });
    w->setFuture(QtConcurrent::run([path] {
      QFile f(path);
      if (!f.open(QIODevice::ReadOnly)) return QString("Cannot open the image.");
      QCryptographicHash md5(QCryptographicHash::Md5), s1(QCryptographicHash::Sha1),
          s256(QCryptographicHash::Sha256), s512(QCryptographicHash::Sha512);
      while (!f.atEnd()) {
        const QByteArray chunk = f.read(4 << 20);
        md5.addData(chunk); s1.addData(chunk); s256.addData(chunk); s512.addData(chunk);
      }
      return QString("MD5      %1\nSHA-1    %2\nSHA-256  %3\nSHA-512  %4\n")
          .arg(QString(md5.result().toHex()), QString(s1.result().toHex()),
               QString(s256.result().toHex()), QString(s512.result().toHex()));
    }));
  }

  // --- log / about ---
  void showLog() {
    if (!logDialog) {
      logDialog = new QDialog(this);
      logDialog->setWindowTitle("Log");
      auto *l = new QVBoxLayout(logDialog);
      logView = new QPlainTextEdit;
      logView->setReadOnly(true);
      logView->setPlainText(logLines.join("\n"));
      logView->setMinimumSize(600, 320);
      l->addWidget(logView);
      auto *bb = new QDialogButtonBox;
      auto *save = bb->addButton("Save...", QDialogButtonBox::ActionRole);
      bb->addButton(QDialogButtonBox::Close);
      QObject::connect(bb, &QDialogButtonBox::rejected, logDialog, &QDialog::hide);
      QObject::connect(save, &QPushButton::clicked, [this] {
        const QString p = QFileDialog::getSaveFileName(this, "Save log", "rufux.log", "Text (*.log *.txt)");
        if (p.isEmpty()) return;
        QFile f(p);
        if (f.open(QIODevice::WriteOnly)) f.write(logLines.join("\n").toUtf8() + "\n");
      });
      l->addWidget(bb);
    }
    logDialog->show();
    logDialog->raise();
  }

  void showAbout() {
    QMessageBox::about(
        this, "About Rufux",
        QString("<h3>Rufux %1</h3><p>Make bootable USB drives on Linux.</p>"
                "<p>Based on <a href=\"https://github.com/pbatard/rufus\">Rufus</a> by Pete Batard. GPLv3.</p>"
                "<p><a href=\"https://github.com/Hultwl/Rufux\">github.com/Hultwl/Rufux</a></p>")
            .arg(RUFUX_VERSION));
  }

  // --- start / cancel ---
  bool askWindowsOptions(QString *out, QString *drivers) {
    QDialog d(this);
    d.setWindowTitle("Windows User Experience");
    auto *l = new QVBoxLayout(&d);
    l->addWidget(new QLabel("Customize the Windows installation?"));
    auto *a = new QCheckBox("Remove requirements for 4 GB+ RAM, Secure Boot and TPM 2.0");
    auto *b = new QCheckBox("Remove the requirement for an online Microsoft account");
    auto *c = new QCheckBox("Disable data collection (skip privacy questions)");
    a->setChecked(true);
    l->addWidget(a);
    l->addWidget(b);
    l->addWidget(c);
    // Setup loads everything in $WinPEDriver$ automatically. This is the fix
    // when the installer shows no drives (Intel RST/VMD, some NVMe/RAID).
    auto *dl = new QLabel("Storage drivers (optional): a folder with .inf drivers, for example Intel RST/VMD."
                          "<br>Use this if Setup says a driver is missing or shows no drives.");
    dl->setWordWrap(true);
    l->addWidget(dl);
    auto *row = new QHBoxLayout;
    auto *dirEdit = new QLineEdit(driversDir);
    dirEdit->setPlaceholderText("No drivers");
    auto *browse = new QPushButton("Browse...");
    QObject::connect(browse, &QPushButton::clicked, [&] {
      const QString p = QFileDialog::getExistingDirectory(&d, "Select the drivers folder", dirEdit->text());
      if (!p.isEmpty()) dirEdit->setText(p);
    });
    row->addWidget(dirEdit, 1);
    row->addWidget(browse);
    l->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return false;
    QStringList v;
    if (a->isChecked()) v << "bypass";
    if (b->isChecked()) v << "nro";
    if (c->isChecked()) v << "privacy";
    *out = v.isEmpty() ? QStringLiteral("none") : v.join(",");
    driversDir = dirEdit->text().trimmed();
    *drivers = driversDir;
    return true;
  }

  void onStart() {
    if (busy()) { cancelWorker(); return; }

    const QString dst = devCombo->currentData().toString();
    if (dst.isEmpty()) { QMessageBox::warning(this, "No device", "Plug in a USB drive first."); return; }

    QString mode;
    if (bootCombo->currentIndex() == 2) mode = "dos";
    else if (bootCombo->currentIndex() == 1) mode = "format";
    else {
      if (isoPath.isEmpty()) { QMessageBox::warning(this, "No image", "Press SELECT to choose an image."); return; }
      const int img = imageCombo->currentIndex();
      mode = img == 2 ? "windows" : (img == 1 ? "extract" : "dd");
    }

    QString wue = "none", drivers;
    if (mode == "windows" && chkWue->isChecked() && !askWindowsOptions(&wue, &drivers)) { log("Cancelled."); return; }

    QMessageBox::StandardButton r = QMessageBox::warning(
        this, "WARNING: DESTRUCTIVE OPERATION",
        "ALL DATA ON DEVICE\n" + devCombo->currentText() + "\nWILL BE DESTROYED.\n\n"
        "To continue with this operation, click OK. To quit, click Cancel.",
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
    if (r != QMessageBox::Ok) { log("Cancelled."); return; }

    const QString fs = fsKey();
    const QString label = sanitizeLabel(labelEdit->text(), fs);
    static const int clusterSectors[] = {0, 8, 16, 32, 64, 128};
    QStringList a;
    a << "create" << (mode == "dos" || mode == "format" ? QString("none") : isoPath) << dst
      << "--mode" << mode
      << "--scheme" << (schemeCombo->currentIndex() == 0 ? "gpt" : "dos")
      << "--fs" << fs << "--label" << label
      << "--persist-mb" << QString::number(mode == "extract" ? persist->value() * 512 : 0)
      << "--cluster-sectors" << QString::number(clusterSectors[clusterCombo->currentIndex()])
      << "--badblock-passes" << QString::number(chkBad->isChecked() ? passesCombo->currentIndex() + 1 : 0)
      << (chkQuick->isChecked() ? "--quick" : "--full");
    if (!chkExt->isChecked()) a << "--no-autorun";
    if (mode == "windows") a << "--wue" << wue;
    if (mode == "windows" && !drivers.isEmpty()) a << "--drivers" << drivers;
    if (chkUefi->isChecked()) a << "--uefi-validate";
    a << "--verify";
    if (chkFixed->isChecked()) a << "--allow-fixed";
    a << "--real" << "--yes";

    QString self = qEnvironmentVariable("APPIMAGE");
    if (self.isEmpty() || !QFileInfo(self).isExecutable()) self = QCoreApplication::applicationFilePath();

    QString prog = self;
    if (geteuid() != 0) {  // escalate only the worker, not the window
      prog = "pkexec";
      a.prepend(self);
      if (QStandardPaths::findExecutable("pkexec").isEmpty()) {
        QMessageBox::critical(this, "pkexec not found", "Install polkit (pkexec), or run Rufux as root.");
        return;
      }
    }

    lastError.clear();
    bar->setValue(0);
    setStatus("Working...");
    log("Starting: " + mode + " -> " + dst);
    worker = new QProcess(this);
    worker->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(worker, &QProcess::readyRead, [this] { onOutput(worker->readAll()); });
    QObject::connect(worker, &QProcess::finished, [this](int code, QProcess::ExitStatus st) { onFinished(code, st); });
    QObject::connect(worker, &QProcess::errorOccurred, [this](QProcess::ProcessError e) {
      if (e == QProcess::FailedToStart) { lastError = "Could not start the worker process."; }
    });
    worker->start(prog, a);
    syncEnabled();
  }

  void cancelWorker() {
    log("Cancel requested. The drive may be left partly written.");
    setStatus("Cancelling...");
    worker->terminate();
    QTimer::singleShot(4000, worker, [w = worker] { if (w->state() != QProcess::NotRunning) w->kill(); });
  }

  void onOutput(const QByteArray &data) {
    static const QRegularExpression pct(R"(^\s*(\d{1,3})%)");
    // Progress uses \r; messages use \n.
    for (const QByteArray &tok : data.split('\n')) {
      for (const QByteArray &part : tok.split('\r')) {
        QString s = QString::fromUtf8(part).trimmed();
        if (s.isEmpty() || s.startsWith('+')) continue;
        auto m = pct.match(s);
        if (m.hasMatch()) {
          const int p = qBound(0, m.captured(1).toInt(), 100);
          bar->setValue(p);
          s = s.mid(m.capturedLength()).trimmed();
          setStatus(QString("Working... %1%").arg(p));
          if (s.isEmpty()) continue;
        }
        log(s);
        if (s != "Done.") lastError = s;
      }
    }
  }

  void onFinished(int code, QProcess::ExitStatus st) {
    worker->deleteLater();
    worker = nullptr;
    syncEnabled();
    if (st == QProcess::NormalExit && code == 0) {
      bar->setValue(100);
      setStatus("READY");
      log("Done.");
      QMessageBox::information(this, "Done", "The drive is ready.");
      return;
    }
    setStatus("FAILED");
    QString why = lastError.isEmpty() ? QString("The worker exited with code %1.").arg(code) : lastError;
    if (code == 126 || code == 127) why = "Authorization was cancelled or pkexec is unavailable.";
    log("Failed: " + why);
    QMessageBox::critical(this, "Failed", why);
  }
};

struct Colors {
  QColor bg, surface, border, borderHover, hover, text, muted, accent, accentHover;
};

Colors lightColors() {
  return {"#f4f5f7", "#ffffff", "#d7dbe2", "#b8bec9", "#eceff3", "#1b1f24", "#69717e", "#2563eb", "#1d4ed8"};
}
Colors darkColors() {
  return {"#1c1f24", "#262a31", "#383d46", "#4b525d", "#2f343c", "#e7e9ec", "#98a0ab", "#3b82f6", "#2f6fdc"};
}

// The combo-box arrow is drawn once into a PNG (no SVG plugin needed).
QString makeArrow(const QColor &c, QTemporaryDir &dir) {
  QPixmap pm(24, 24);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPen pen(c, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.drawLine(QPointF(5, 9), QPointF(12, 16));
  p.drawLine(QPointF(12, 16), QPointF(19, 9));
  p.end();
  const QString path = dir.filePath("down.png");
  pm.save(path);
  return path;
}

void applyTheme(const QString &theme, QTemporaryDir &tmp) {
  qApp->setStyle(QStyleFactory::create("Fusion"));
  bool dark = theme == "dark";
  if (theme != "dark" && theme != "light")  // follow the system
    dark = qApp->palette().color(QPalette::Window).lightness() < 128;
  const Colors c = dark ? darkColors() : lightColors();

  QPalette p;
  p.setColor(QPalette::Window, c.bg);
  p.setColor(QPalette::WindowText, c.text);
  p.setColor(QPalette::Base, c.surface);
  p.setColor(QPalette::AlternateBase, c.bg);
  p.setColor(QPalette::Text, c.text);
  p.setColor(QPalette::Button, c.surface);
  p.setColor(QPalette::ButtonText, c.text);
  p.setColor(QPalette::Highlight, c.accent);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::ToolTipBase, c.surface);
  p.setColor(QPalette::ToolTipText, c.text);
  p.setColor(QPalette::PlaceholderText, c.muted);
  p.setColor(QPalette::Link, c.accent);
  p.setColor(QPalette::Disabled, QPalette::Text, c.muted);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
  p.setColor(QPalette::Disabled, QPalette::WindowText, c.muted);
  qApp->setPalette(p);

  const QString arrow = makeArrow(c.muted, tmp);
  auto n = [](const QColor &q) { return q.name(); };
  QString qss = QString(R"(
    * { font-size: 10.5pt; }
    QLabel#section { color: %muted; font-size: 8.5pt; font-weight: 600; }
    QFrame#rule { color: %border; background: %border; max-height: 1px; border: none; }
    QLabel#muted { color: %muted; font-size: 9pt; }
    QLabel#status { font-weight: 600; }
    QLabel#status[state="ready"] { color: #16a34a; }
    QLabel#status[state="busy"] { color: %accent; }
    QLabel#status[state="failed"] { color: #dc2626; }
    QComboBox, QLineEdit { background: %surface; border: 1px solid %border; border-radius: 6px;
      padding: 5px 10px; min-height: 22px; selection-background-color: %accent; selection-color: white; }
    QComboBox:hover, QLineEdit:hover { border-color: %borderHover; }
    QComboBox:focus, QLineEdit:focus { border-color: %accent; }
    QComboBox:disabled, QLineEdit:disabled { color: %muted; background: %bg; }
    QComboBox::drop-down { border: none; width: 26px; }
    QComboBox::down-arrow { image: url(%arrow); width: 12px; height: 12px; }
    QComboBox QAbstractItemView { background: %surface; border: 1px solid %border; outline: 0;
      selection-background-color: %accent; selection-color: white; padding: 4px; }
    QPushButton { background: %surface; border: 1px solid %border; border-radius: 6px;
      padding: 6px 16px; min-height: 22px; }
    QPushButton:hover { border-color: %borderHover; background: %hover; }
    QPushButton:pressed { background: %border; }
    QPushButton:disabled { color: %muted; background: %bg; }
    QPushButton#start { background: %accent; border-color: %accent; color: white; font-weight: 600; }
    QPushButton#start:hover { background: %accentHover; border-color: %accentHover; }
    QPushButton#start:disabled { background: %border; border-color: %border; color: %muted; }
    QPushButton#start[busy="true"] { background: #dc2626; border-color: #dc2626; }
    QPushButton#flat { border: none; background: transparent; color: %muted; padding: 6px 10px; }
    QPushButton#flat:hover { color: %text; background: %hover; }
    QPushButton#flat:disabled { color: %border; background: transparent; }
    QProgressBar { background: %border; border: none; border-radius: 4px; min-height: 8px; max-height: 8px; }
    QProgressBar::chunk { background: %accent; border-radius: 4px; }
    QProgressBar[state="ready"]::chunk { background: #16a34a; }
    QProgressBar[state="failed"]::chunk { background: #dc2626; }
    QToolButton { border: none; color: %muted; padding: 4px 2px; }
    QToolButton:hover { color: %text; }
    QPlainTextEdit { background: %surface; border: 1px solid %border; border-radius: 6px; padding: 4px; }
    QSlider::groove:horizontal { height: 4px; background: %border; border-radius: 2px; }
    QSlider::sub-page:horizontal { background: %accent; border-radius: 2px; }
    QSlider::handle:horizontal { background: %accent; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
    QToolTip { background: %surface; color: %text; border: 1px solid %border; padding: 4px; }
  )");
  // Longest names first so %accentHover is not eaten by %accent.
  qss.replace("%accentHover", n(c.accentHover)).replace("%borderHover", n(c.borderHover))
     .replace("%accent", n(c.accent)).replace("%border", n(c.border)).replace("%surface", n(c.surface))
     .replace("%muted", n(c.muted)).replace("%hover", n(c.hover)).replace("%text", n(c.text))
     .replace("%bg", n(c.bg)).replace("%arrow", arrow);
  qApp->setStyleSheet(qss);
}

}  // namespace

int rufux_gui_run(int argc, char **argv) {
  // Our own options are removed before Qt sees argv.
  QString theme = qEnvironmentVariable("RUFUX_THEME", "system");
  std::vector<char *> keep{argv[0]};
  for (int i = 1; i < argc; i++) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--gui" || a == "gui") continue;
    if (a == "--theme" && i + 1 < argc) { theme = QString::fromLocal8Bit(argv[++i]); continue; }
    if (a.startsWith("--theme=")) { theme = a.mid(8); continue; }
    keep.push_back(argv[i]);
  }
  int qargc = int(keep.size());
  QApplication app(qargc, keep.data());
  QApplication::setApplicationName("Rufux");
  QTemporaryDir tmp;  // holds the generated combo-box arrow for the app lifetime
  applyTheme(theme, tmp);

  MainWindow w;
  w.show();

  // RUFUX_GUI_SNAPSHOT=file.png saves a screenshot and exits (tests, docs).
  const QString snap = qEnvironmentVariable("RUFUX_GUI_SNAPSHOT");
  if (!snap.isEmpty()) {
    const QString img = qEnvironmentVariable("RUFUX_GUI_IMAGE");
    if (!img.isEmpty()) w.openImage(img);
    QTimer::singleShot(img.isEmpty() ? 400 : 1500, &app, [&w, snap] {
      w.grab().save(snap);
      QApplication::quit();
    });
  }
  return app.exec();
}
