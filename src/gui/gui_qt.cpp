// Qt6 front end for Rufux, laid out like Rufus.
//
// The window stays in the user's session. Anything that touches a disk runs
// in a separate root process: `pkexec rufux create ... --real --yes`, the same
// worker the CLI uses. This file only builds the command line, shows progress
// and reports the result.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QListView>
#include <QLocale>
#include <QMimeData>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTimeZone>
#include <QUrl>
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
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QScrollBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <unistd.h>
#include <memory>
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

// Rufus separates its three sections with a small caption followed by a rule
// that runs to the right edge, rather than with framed group boxes.
QHBoxLayout *sectionHeader(const QString &text) {
  auto *l = new QHBoxLayout;
  l->setContentsMargins(0, 8, 0, 2);
  l->setSpacing(8);
  auto *t = new QLabel(text);
  t->setObjectName("section");
  auto *rule = new QFrame;
  rule->setFrameShape(QFrame::HLine);
  rule->setFrameShadow(QFrame::Sunken);
  l->addWidget(t);
  l->addWidget(rule, 1);
  return l;
}

// Caption above the control, the way Rufus stacks them.
QVBoxLayout *field(const QString &caption, QWidget *w) {
  auto *l = new QVBoxLayout;
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(2);
  l->addWidget(new QLabel(caption));
  l->addWidget(w);
  return l;
}

QVBoxLayout *field(const QString &caption, QLayout *row) {
  auto *l = new QVBoxLayout;
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(2);
  l->addWidget(new QLabel(caption));
  l->addLayout(row);
  return l;
}

// Two fields sharing a row, each taking half the width.
QHBoxLayout *pair(QLayout *left, QLayout *right) {
  auto *l = new QHBoxLayout;
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(12);
  l->addLayout(left, 1);
  l->addLayout(right, 1);
  return l;
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

void applyTheme(const QString &theme);

// Item views let the delegate size rows consistently; everything else about
// the popup is left to the platform style.
void polishCombo(QComboBox *c) {
  c->setMaxVisibleItems(12);
  if (auto *v = qobject_cast<QListView *>(c->view())) v->setUniformItemSizes(true);
}

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
    setAcceptDrops(true);
    build();
    log(QStringLiteral("Rufux ") + RUFUX_VERSION + " ready. Select a device and an image, then press START.");
    refreshDevices(true);
    auto *poll = new QTimer(this);
    QObject::connect(poll, &QTimer::timeout, [this] { refreshDevices(false); });
    poll->start(2000);
    syncEnabled();
  }

  void openImage(const QString &path) { loadImage(path); }

  // For screenshots: show the Windows options dialog, save it, close it.
  void demoWindowsDialog(const QString &path) {
    QTimer::singleShot(500, this, [path] {
      if (QWidget *m = QApplication::activeModalWidget()) {
        m->grab().save(path);
        if (auto *d = qobject_cast<QDialog *>(m)) d->reject();
      }
    });
    QString a, b;
    QStringList c;
    askWindowsOptions(&a, &b, &c);
  }

  // For screenshots: open the file-system list and save the popup.
  void demoPopup(const QString &path) {
    fsCombo->showPopup();
    QTimer::singleShot(400, this, [this, path] { fsCombo->view()->window()->grab().save(path); });
  }

 protected:
  void dragEnterEvent(QDragEnterEvent *e) override {
    if (!busy() && e->mimeData()->hasUrls()) e->acceptProposedAction();
  }
  void dropEvent(QDropEvent *e) override {
    for (const QUrl &u : e->mimeData()->urls())
      if (u.isLocalFile()) { loadImage(u.toLocalFile()); break; }
  }

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
  QComboBox *devCombo, *bootCombo, *imageCombo, *schemeCombo, *targetCombo, *fsCombo, *clusterCombo, *passesCombo;
  QPushButton *selectBtn, *hashBtn, *startBtn, *closeBtn, *logBtn, *aboutBtn, *settingsBtn;
  QLabel *imageInfo, *fsNote;
  QWidget *imageRow = nullptr, *persistBox = nullptr;
  QLabel *persistValue, *devCount;
  QSlider *persist;
  QLineEdit *labelEdit;
  QCheckBox *chkFixed, *chkUefi, *chkQuick, *chkExt, *chkBad, *chkWue;
  QProgressBar *bar;
  QPlainTextEdit *logView = nullptr;
  QWidget *logPanel = nullptr;

  // --- state ---
  QString isoPath;
  bool isoWindows = false;
  QVector<RufuxDevice> devs;
  QString devSignature;
  QProcess *worker = nullptr;
  QString lastError;
  QStringList logLines;
  QString driversDir;
  QString lastMode, lastDev;
  QSettings settings{"Rufux", "Rufux"};

  void build() {
    auto *root = new QVBoxLayout(this);

    // Drive Properties ------------------------------------------------------
    root->addLayout(sectionHeader("Drive Properties"));

    devCombo = new QComboBox;
    root->addLayout(field("Device", devCombo));

    bootCombo = new QComboBox;
    bootCombo->addItems({kNoIso, "Non bootable", "FreeDOS"});
    selectBtn = new QPushButton("SELECT");
    selectBtn->setMinimumWidth(90);
    auto *bootRow = new QHBoxLayout;
    bootRow->setSpacing(6);
    bootRow->addWidget(bootCombo, 1);
    bootRow->addWidget(selectBtn);
    root->addLayout(field("Boot selection", bootRow));

    // These two rows appear only for an ISO, so they live in containers that
    // can be hidden as a whole, caption included.
    imageCombo = new QComboBox;
    imageCombo->addItems({"Write in DD Image mode", "Write in ISO Image mode", "Windows installation"});
    imageRow = new QWidget;
    imageRow->setLayout(field("Image option", imageCombo));
    root->addWidget(imageRow);

    persist = new QSlider(Qt::Horizontal);
    persist->setRange(0, 128);  // 512 MiB steps, up to 64 GiB
    persistValue = new QLabel("Disabled");
    persistValue->setMinimumWidth(70);
    persistValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *persistRow = new QHBoxLayout;
    persistRow->addWidget(persist, 1);
    persistRow->addWidget(persistValue);
    persistBox = new QWidget;
    persistBox->setLayout(field("Persistent partition size", persistRow));
    root->addWidget(persistBox);

    // Rufus puts these two side by side, and they are linked to each other.
    schemeCombo = new QComboBox;
    schemeCombo->addItems({"GPT", "MBR"});
    targetCombo = new QComboBox;
    targetCombo->addItems({"BIOS or UEFI", "BIOS (or UEFI-CSM)", "UEFI (non CSM)"});
    root->addLayout(pair(field("Partition scheme", schemeCombo),
                         field("Target system", targetCombo)));

    auto *advDrive = new QWidget;
    auto *adl = new QVBoxLayout(advDrive);
    adl->setContentsMargins(4, 2, 0, 0);
    chkFixed = new QCheckBox("List all drives, including internal disks (dangerous)");
    chkUefi = new QCheckBox("Validate the UEFI bootloader after copying");
    adl->addWidget(chkFixed);
    adl->addWidget(chkUefi);
    auto *adv1 = new Advanced("Show advanced drive properties", advDrive);
    root->addWidget(adv1->toggle);
    root->addWidget(advDrive);

    // Format Options --------------------------------------------------------
    root->addLayout(sectionHeader("Format Options"));

    labelEdit = new QLineEdit("RUFUX");
    root->addLayout(field("Volume label", labelEdit));

    fsCombo = new QComboBox;
    fsCombo->addItems({"FAT32", "NTFS", "exFAT", "UDF", "ext4"});
    clusterCombo = new QComboBox;
    clusterCombo->addItems({"Default", "4096 bytes", "8192 bytes", "16384 bytes",
                            "32768 bytes", "65536 bytes"});
    root->addLayout(pair(field("File system", fsCombo),
                         field("Cluster size", clusterCombo)));

    fsNote = new QLabel;
    fsNote->setObjectName("muted");
    fsNote->setWordWrap(true);
    root->addWidget(fsNote);

    auto *advFmt = new QWidget;
    auto *afl = new QVBoxLayout(advFmt);
    afl->setContentsMargins(4, 2, 0, 0);
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

    // Status ----------------------------------------------------------------
    root->addLayout(sectionHeader("Status"));
    bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(true);
    bar->setFormat("READY");
    bar->setProperty("state", "ready");
    root->addWidget(bar);

    devCount = new QLabel;
    devCount->setObjectName("muted");
    imageInfo = new QLabel;
    imageInfo->setObjectName("muted");
    imageInfo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *info = new QHBoxLayout;
    info->addWidget(devCount);
    info->addWidget(imageInfo, 1);
    root->addLayout(info);

    // Buttons ---------------------------------------------------------------
    auto *foot = new QHBoxLayout;
    aboutBtn = new QPushButton("About");
    settingsBtn = new QPushButton("Settings");
    logBtn = new QPushButton("Log");
    hashBtn = new QPushButton("Checksums");
    aboutBtn->setToolTip("About Rufux");
    logBtn->setToolTip("Show the log in this window");
    hashBtn->setToolTip("Compute MD5, SHA-1, SHA-256 and SHA-512 of the image");
    settingsBtn->setToolTip("Appearance and options");
    startBtn = new QPushButton("START");
    startBtn->setObjectName("start");
    startBtn->setDefault(true);
    closeBtn = new QPushButton("CLOSE");
    startBtn->setMinimumWidth(96);
    closeBtn->setMinimumWidth(96);
    foot->addWidget(aboutBtn);
    foot->addWidget(settingsBtn);
    foot->addWidget(logBtn);
    foot->addWidget(hashBtn);
    foot->addStretch(1);
    foot->addWidget(startBtn);
    foot->addWidget(closeBtn);
    root->addLayout(foot);

    // The log lives in the window rather than in a separate one: it is the
    // first thing a bug report needs, so it should not be behind a dialog
    // that has to be kept out of the way of the main window.
    logPanel = new QWidget;
    auto *lp = new QVBoxLayout(logPanel);
    lp->setContentsMargins(0, 4, 0, 0);
    lp->setSpacing(4);
    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView->setMinimumHeight(150);
    logView->setPlainText(logLines.join("\n"));
    lp->addWidget(logView);
    auto *logFoot = new QHBoxLayout;
    auto *saveLog = new QPushButton("Save log...");
    auto *clearLog = new QPushButton("Clear");
    logFoot->addStretch(1);
    logFoot->addWidget(clearLog);
    logFoot->addWidget(saveLog);
    lp->addLayout(logFoot);
    logPanel->setVisible(false);
    root->addWidget(logPanel, 1);
    QObject::connect(saveLog, &QPushButton::clicked, [this] { saveLogToFile(); });
    QObject::connect(clearLog, &QPushButton::clicked, [this] {
      logLines.clear();
      logView->clear();
    });

    setMinimumWidth(500);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(6);

    // --- wiring ---
    QObject::connect(selectBtn, &QPushButton::clicked, [this] { pickImage(); });
    QObject::connect(hashBtn, &QPushButton::clicked, [this] { showChecksums(); });
    QObject::connect(startBtn, &QPushButton::clicked, [this] { onStart(); });
    QObject::connect(closeBtn, &QPushButton::clicked, [this] { close(); });
    QObject::connect(logBtn, &QPushButton::clicked, [this] { toggleLog(); });
    QObject::connect(aboutBtn, &QPushButton::clicked, [this] { showAbout(); });
    QObject::connect(settingsBtn, &QPushButton::clicked, [this] { showSettings(); });
    for (QComboBox *c : {devCombo, bootCombo, imageCombo, schemeCombo, targetCombo, fsCombo, clusterCombo, passesCombo})
      polishCombo(c);
    QObject::connect(chkBad, &QCheckBox::toggled, passesCombo, &QComboBox::setEnabled);
    QObject::connect(chkFixed, &QCheckBox::toggled, [this](bool on) {
      if (on)
        log("Internal disks are now listed. Double-check the device before writing.");
      refreshDevices(true);
    });
    QObject::connect(bootCombo, &QComboBox::currentIndexChanged, [this](int) { syncEnabled(); });
    QObject::connect(imageCombo, &QComboBox::currentIndexChanged, [this](int) { syncEnabled(); });
    QObject::connect(fsCombo, &QComboBox::currentIndexChanged, [this](int) { relabel(); syncEnabled(); });
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
    bar->setFormat(text);
    const char *state = text == "READY" ? "ready" : (text == "FAILED" ? "failed" : "busy");
    bar->setProperty("state", state);
    bar->style()->unpolish(bar);
    bar->style()->polish(bar);
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
    imageRow->setVisible(iso && haveIso);
    persistBox->setVisible(iso && haveIso && img == 1);
    chkWue->setVisible(iso && haveIso && img == 2);
    const bool win = iso && haveIso && img == 2;
    fsNote->setVisible(win);
    if (win)
      fsNote->setText(fsCombo->currentIndex() == 0
          ? "FAT32: the most compatible layout and it works with Secure Boot. An install.wim over 4 GiB "
            "is split automatically (needs wimlib)."
          : "NTFS + a small UEFI:NTFS partition: handles any image size. If Windows Setup cannot see the "
            "drive, try FAT32.");
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
    imageInfo->setText("Using image: " + QFileInfo(r.path).fileName());
    log("Using image: " + r.path + " (" + humanSize(r.ok ? r.info.size_bytes : QFileInfo(r.path).size()) + ")");
    if (isoWindows) {
      imageCombo->setCurrentIndex(2);
      fsCombo->setCurrentIndex(1);      // NTFS
      schemeCombo->setCurrentIndex(1);  // MBR: Windows Setup reads it on more USB sticks than GPT
      log("Windows installation media detected. MBR selected: it works on more USB sticks than GPT and still boots UEFI machines.");
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
    text->setMinimumSize(620, 130);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    l->addWidget(new QLabel(QFileInfo(path).fileName()));
    l->addWidget(text);
    auto *expected = new QLineEdit;
    expected->setPlaceholderText("Paste the checksum from the download page to compare");
    auto *verdict = new QLabel;
    l->addWidget(expected);
    l->addWidget(verdict);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    l->addWidget(bb);
    dlg->show();

    auto hashes = std::make_shared<QStringList>();  // MD5, SHA-1, SHA-256, SHA-512
    auto compare = [expected, verdict, hashes] {
      const QString want = expected->text().trimmed().toLower();
      if (want.isEmpty() || hashes->isEmpty()) { verdict->clear(); return; }
      static const char *names[] = {"MD5", "SHA-1", "SHA-256", "SHA-512"};
      for (int i = 0; i < hashes->size(); i++)
        if (hashes->at(i) == want) {
          verdict->setText(QString("<span style='color:#16a34a'><b>Match</b></span> (%1)").arg(names[i]));
          return;
        }
      verdict->setText("<span style='color:#dc2626'><b>Does not match</b></span> any of the four checksums. "
                       "The download may be corrupt.");
    };
    QObject::connect(expected, &QLineEdit::textChanged, dlg, compare);
    auto *w = new QFutureWatcher<QStringList>(dlg);
    QObject::connect(w, &QFutureWatcher<QStringList>::finished, [text, w, hashes, compare] {
      *hashes = w->result();
      if (hashes->size() != 4) { text->setPlainText(hashes->value(0, "Cannot read the image.")); hashes->clear(); return; }
      text->setPlainText(QString("MD5      %1\nSHA-1    %2\nSHA-256  %3\nSHA-512  %4\n")
                             .arg(hashes->at(0), hashes->at(1), hashes->at(2), hashes->at(3)));
      compare();
    });
    w->setFuture(QtConcurrent::run([path] {
      QFile f(path);
      if (!f.open(QIODevice::ReadOnly)) return QStringList{"Cannot open the image."};
      QCryptographicHash md5(QCryptographicHash::Md5), s1(QCryptographicHash::Sha1),
          s256(QCryptographicHash::Sha256), s512(QCryptographicHash::Sha512);
      while (!f.atEnd()) {
        const QByteArray chunk = f.read(4 << 20);
        md5.addData(chunk); s1.addData(chunk); s256.addData(chunk); s512.addData(chunk);
      }
      return QStringList{QString(md5.result().toHex()), QString(s1.result().toHex()),
                         QString(s256.result().toHex()), QString(s512.result().toHex())};
    }));
  }

  void showSettings() {
    QDialog d(this);
    d.setWindowTitle("Settings");
    auto *l = new QVBoxLayout(&d);
    auto *form = new QFormLayout;
    auto *theme = new QComboBox;
    theme->addItems({"Follow the system", "Light", "Dark"});
    const QString cur = settings.value("theme", "system").toString();
    theme->setCurrentIndex(cur == "light" ? 1 : (cur == "dark" ? 2 : 0));
    polishCombo(theme);
    form->addRow("Appearance", theme);
    l->addLayout(form);
    auto *eject = new QCheckBox("Offer to eject the drive when the write is done");
    eject->setChecked(settings.value("eject", true).toBool());
    l->addWidget(eject);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);
    d.setMinimumWidth(380);
    if (d.exec() != QDialog::Accepted) return;
    static const char *keys[] = {"system", "light", "dark"};
    settings.setValue("theme", keys[qBound(0, theme->currentIndex(), 2)]);
    settings.setValue("eject", eject->isChecked());
    applyTheme(keys[qBound(0, theme->currentIndex(), 2)]);
  }

  // --- log / about ---
  void toggleLog() {
    const bool show = !logPanel->isVisible();
    logPanel->setVisible(show);
    logBtn->setText(show ? "Hide log" : "Log");
    if (show) logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
    QTimer::singleShot(0, this, [this] { adjustSize(); });
  }

  void saveLogToFile() {
    const QString p = QFileDialog::getSaveFileName(this, "Save log", "rufux.log", "Text (*.log *.txt)");
    if (p.isEmpty()) return;
    QFile f(p);
    if (f.open(QIODevice::WriteOnly)) f.write(logLines.join("\n").toUtf8() + "\n");
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
  // Rufus's "Windows User Experience" dialog. `wue` gets the --wue list, `drivers` the
  // folder, `extra` any further worker arguments (regional values).
  bool askWindowsOptions(QString *wue, QString *drivers, QStringList *extra) {
    QDialog d(this);
    d.setWindowTitle("Windows User Experience");
    auto *l = new QVBoxLayout(&d);
    l->setSpacing(8);
    l->addWidget(new QLabel("Customize the Windows installation?"));
    auto check = [&](const QString &text, const char *key, bool def) {
      auto *c = new QCheckBox(text);
      c->setChecked(settings.value(QString("wue/") + key, def).toBool());
      l->addWidget(c);
      return c;
    };
    auto *bypass = check("Remove requirement for 4 GB+ RAM, Secure Boot and TPM 2.0", "bypass", true);
    auto *nro = check("Remove requirement for an online Microsoft account", "nro", false);

    // Local account, prefilled with this user's name as Rufus does.
    auto *userRow = new QHBoxLayout;
    auto *userOn = new QCheckBox("Create a local account with username:");
    userOn->setChecked(settings.value("wue/user_on", false).toBool());
    QString defName = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "User"));
    if (!defName.isEmpty()) defName[0] = defName[0].toUpper();
    auto *userEdit = new QLineEdit(settings.value("wue/user", defName).toString());
    userEdit->setEnabled(userOn->isChecked());
    QObject::connect(userOn, &QCheckBox::toggled, userEdit, &QLineEdit::setEnabled);
    userRow->addWidget(userOn);
    userRow->addWidget(userEdit, 1);
    l->addLayout(userRow);

    QString langTag = QLocale::system().name().replace('_', '-');
    if (langTag == "C" || langTag.isEmpty()) langTag = "en-US";
    const QString zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    auto *locale = check("Set regional options to the same values as this user's", "locale", false);
    QString langName = QLocale::system().nativeLanguageName();
    if (langName.isEmpty()) langName = langTag;
    auto *localeNote = new QLabel(QString("%1  \u00b7  %2").arg(langName, zone));
    localeNote->setObjectName("muted");
    localeNote->setContentsMargins(24, 0, 0, 0);
    l->addWidget(localeNote);

    auto *privacy = check("Disable data collection (skip privacy questions)", "privacy", false);
    auto *bitlocker = check("Disable BitLocker automatic device encryption", "bitlocker", false);
    auto *qol = check("Disable Windows 11 annoyances (Copilot, ads, news...)", "qol", false);

    auto *dl = new QLabel("Storage or USB drivers (optional): a folder with .inf files, for example Intel RST/VMD."
                          "<br>Setup loads them automatically. Use it if Setup shows no drives or asks for a driver.");
    dl->setWordWrap(true);
    dl->setObjectName("muted");
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
    d.setMinimumWidth(560);
    if (d.exec() != QDialog::Accepted) return false;

    const QString uname = userEdit->text().trimmed();
    if (userOn->isChecked() && uname.isEmpty()) {
      QMessageBox::warning(this, "Local account", "Type a user name, or untick the option.");
      return askWindowsOptions(wue, drivers, extra);
    }
    settings.setValue("wue/bypass", bypass->isChecked());
    settings.setValue("wue/nro", nro->isChecked());
    settings.setValue("wue/user_on", userOn->isChecked());
    settings.setValue("wue/user", uname);
    settings.setValue("wue/locale", locale->isChecked());
    settings.setValue("wue/privacy", privacy->isChecked());
    settings.setValue("wue/bitlocker", bitlocker->isChecked());
    settings.setValue("wue/qol", qol->isChecked());
    QStringList v;
    if (bypass->isChecked()) v << "bypass";
    if (nro->isChecked()) v << "nro";
    if (privacy->isChecked()) v << "privacy";
    if (bitlocker->isChecked()) v << "bitlocker";
    if (qol->isChecked()) v << "qol";
    if (locale->isChecked()) { v << "locale"; *extra << "--locale" << langTag << "--timezone" << zone; }
    if (userOn->isChecked()) v << ("user=" + uname);
    *wue = v.isEmpty() ? QStringLiteral("none") : v.join(",");
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
    QStringList wueExtra;
    if (mode == "windows" && chkWue->isChecked() && !askWindowsOptions(&wue, &drivers, &wueExtra)) { log("Cancelled."); return; }
    lastMode = mode;
    lastDev = dst;

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
    if (mode == "windows") a << wueExtra;
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
      QApplication::alert(this);
      QMessageBox box(QMessageBox::Information, "Done", "The drive is ready.", QMessageBox::NoButton, this);
      if (lastMode == "windows")
        box.setInformativeText(
            "If Windows Setup says a media driver is missing, plug the drive into a USB 2.0 or plain USB-A "
            "port (not USB-C/Thunderbolt or a hub). Press Shift+F10 in Setup and run diskpart, then "
            "\"list disk\": the drive should be listed.");
      QPushButton *eject = settings.value("eject", true).toBool()
                               ? box.addButton("Eject drive", QMessageBox::ActionRole) : nullptr;
      box.addButton("Close", QMessageBox::AcceptRole);
      box.exec();
      if (eject && box.clickedButton() == eject) {
        log("Ejecting " + lastDev);
        QProcess::startDetached("udisksctl", {"power-off", "-b", lastDev});
      }
      return;
    }
    setStatus("FAILED");
    QString why = lastError.isEmpty() ? QString("The worker exited with code %1.").arg(code) : lastError;
    if (code == 126 || code == 127) why = "Authorization was cancelled or pkexec is unavailable.";
    log("Failed: " + why);
    QMessageBox box(QMessageBox::Critical, "Failed", why, QMessageBox::Ok, this);
    box.setDetailedText(logLines.join("\n"));  // "Show Details" holds the whole log
    box.exec();
  }
};

// Fusion's stock palette is light only, so dark mode needs its own.
QPalette lightPalette() {
  QPalette p;
  p.setColor(QPalette::Window, QColor("#f0f0f0"));
  p.setColor(QPalette::WindowText, QColor("#1b1b1b"));
  p.setColor(QPalette::Base, Qt::white);
  p.setColor(QPalette::AlternateBase, QColor("#f7f7f7"));
  p.setColor(QPalette::Text, QColor("#1b1b1b"));
  p.setColor(QPalette::Button, QColor("#e9e9e9"));
  p.setColor(QPalette::ButtonText, QColor("#1b1b1b"));
  p.setColor(QPalette::Mid, QColor("#6b6b6b"));
  p.setColor(QPalette::Highlight, QColor("#2a6fd6"));
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, QColor("#2a6fd6"));
  return p;
}

QPalette darkPalette() {
  QPalette p;
  p.setColor(QPalette::Window, QColor("#2b2b2b"));
  p.setColor(QPalette::WindowText, QColor("#e6e6e6"));
  p.setColor(QPalette::Base, QColor("#1f1f1f"));
  p.setColor(QPalette::AlternateBase, QColor("#2b2b2b"));
  p.setColor(QPalette::Text, QColor("#e6e6e6"));
  p.setColor(QPalette::Button, QColor("#3a3a3a"));
  p.setColor(QPalette::ButtonText, QColor("#e6e6e6"));
  p.setColor(QPalette::Mid, QColor("#9a9a9a"));
  p.setColor(QPalette::Highlight, QColor("#3d7fe0"));
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, QColor("#6fa8f5"));
  p.setColor(QPalette::Disabled, QPalette::Text, QColor("#777777"));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#777777"));
  p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#777777"));
  return p;
}

// Which colour scheme the desktop is using.
//
// This has to be answered before the style is replaced, because installing a
// style resets the palette to that style's own default (a light one), and the
// old code looked at the palette afterwards: the answer was always "light".
// Qt 6.5 exposes the desktop setting directly, but an AppImage often ships
// without the platform theme plugin that reads it, so fall back to asking the
// desktop portal, and then gsettings, before giving up.
bool systemPrefersDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark: return true;
    case Qt::ColorScheme::Light: return false;
    default: break;
  }
#endif
  QProcess portal;
  portal.start("gdbus", {"call", "--session", "--dest", "org.freedesktop.portal.Desktop",
                         "--object-path", "/org/freedesktop/portal/desktop",
                         "--method", "org.freedesktop.portal.Settings.Read",
                         "org.freedesktop.appearance", "color-scheme"});
  if (portal.waitForFinished(1500) && portal.exitCode() == 0) {
    const QString out = QString::fromUtf8(portal.readAllStandardOutput());
    if (out.contains("uint32 1")) return true;   // prefer dark
    if (out.contains("uint32 2")) return false;  // prefer light
  }
  QProcess gs;
  gs.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
  if (gs.waitForFinished(1500) && gs.exitCode() == 0)
    return QString::fromUtf8(gs.readAllStandardOutput()).contains("dark");
  return false;
}

void applyTheme(const QString &theme) {
  const bool dark = theme == "dark" || (theme != "light" && systemPrefersDark());
  qApp->setStyle(QStyleFactory::create("Fusion"));
  qApp->setPalette(dark ? darkPalette() : lightPalette());
  // Only the three things the platform style cannot express on its own: the
  // green/red progress bar states and the emphasised START button.
  qApp->setStyleSheet(QString(R"(
    QLabel#muted { color: palette(mid); }
    QProgressBar { min-height: 22px; text-align: center; }
    QProgressBar[state="ready"]::chunk { background: #2e7d32; }
    QProgressBar[state="failed"]::chunk { background: #c62828; }
    QPushButton#start { font-weight: 600; }
    QPushButton#start[busy="true"] { color: #c62828; }
  )"));
}

}  // namespace

int rufux_gui_run(int argc, char **argv) {
  // Our own options are removed before Qt sees argv.
  QString theme = qEnvironmentVariable("RUFUX_THEME", QSettings("Rufux", "Rufux").value("theme", "system").toString());
  QString openFile;
  std::vector<char *> keep{argv[0]};
  for (int i = 1; i < argc; i++) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--gui" || a == "gui") continue;
    if (a == "--theme" && i + 1 < argc) { theme = QString::fromLocal8Bit(argv[++i]); continue; }
    if (a.startsWith("--theme=")) { theme = a.mid(8); continue; }
    if (!a.startsWith("-") && QFileInfo(a).isFile()) { openFile = a; continue; }
    keep.push_back(argv[i]);
  }
  int qargc = int(keep.size());
  QApplication app(qargc, keep.data());
  QApplication::setApplicationName("Rufux");
  applyTheme(theme);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  // Follow the desktop when it switches between light and dark while we run.
  if (theme != "light" && theme != "dark")
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                     &app, [] { applyTheme(QStringLiteral("system")); });
#endif

  MainWindow w;
  w.show();
  if (!openFile.isEmpty()) w.openImage(QFileInfo(openFile).absoluteFilePath());

  // RUFUX_GUI_SNAPSHOT=file.png saves a screenshot and exits (tests, docs).
  const QString snap = qEnvironmentVariable("RUFUX_GUI_SNAPSHOT");
  if (!snap.isEmpty()) {
    const QString wdlg = qEnvironmentVariable("RUFUX_GUI_WINDLG");
    if (!wdlg.isEmpty()) QTimer::singleShot(200, &app, [&w, wdlg] { w.demoWindowsDialog(wdlg); });
    const QString popup = qEnvironmentVariable("RUFUX_GUI_POPUP");
    if (!popup.isEmpty()) QTimer::singleShot(300, &app, [&w, popup] { w.demoPopup(popup); });
    const QString img = qEnvironmentVariable("RUFUX_GUI_IMAGE");
    if (!img.isEmpty()) w.openImage(img);
    QTimer::singleShot((img.isEmpty() && popup.isEmpty() && wdlg.isEmpty()) ? 400 : 1800, &app, [&w, snap] {
      w.grab().save(snap);
      QApplication::quit();
    });
  }
  return app.exec();
}
