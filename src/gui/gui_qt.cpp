// Qt6 front end for Rufux. It follows Rufus's main window: same sections, same
// wording, same order. It uses the desktop's own widget style, palette, icons
// and file dialogs.
//
// Anything that touches a disk runs in a separate root process:
// `pkexec rufux create ... --real --yes`, the same worker the command line
// uses. This file builds the command line, shows progress and reports errors.
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <memory>
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

// Largest label each file system accepts.
int labelLimit(const QString &fs) {
  if (fs == "vfat") return 11;
  if (fs == "exfat") return 15;
  if (fs == "ext4") return 16;
  if (fs == "udf") return 30;
  return 32;
}

QString sanitizeLabel(const QString &in, const QString &fs) {
  QString out;
  for (QChar c : in) {
    if (c == ' ') c = '_';
    if (c.isLetterOrNumber() && c.unicode() < 128) out += (fs == "vfat" ? c.toUpper() : c);
    else if (c == '_' || c == '-') out += c;
    if (out.size() >= labelLimit(fs)) break;
  }
  return out.isEmpty() ? QStringLiteral("NO_LABEL") : out;
}

QString humanSize(quint64 bytes) {
  static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(bytes);
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  return QString::number(v, 'f', (i < 2 || v >= 100) ? 0 : 1) + " " + u[i];
}

QToolButton *iconButton(const char *themeIcon, const QString &fallback, const QString &tip) {
  auto *b = new QToolButton;
  const QIcon ic = QIcon::fromTheme(themeIcon);
  if (ic.isNull()) b->setText(fallback);
  else b->setIcon(ic);
  b->setToolTip(tip);
  b->setAutoRaise(false);
  return b;
}

// "Show advanced ..." row: an arrow toggle that reveals a block of options.
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
    setAcceptDrops(true);
    build();
    log(QStringLiteral("Rufux ") + RUFUX_VERSION);
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
    QString a;
    QStringList b;
    askWindowsOptions(&a, &b);
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
    if (busy()) {
      QMessageBox::warning(this, "Rufux", "An operation is in progress. Press CANCEL first.");
      e->ignore();
      return;
    }
    e->accept();
  }

 private:
  // ---- widgets ----
  QComboBox *devCombo, *bootCombo, *imageCombo, *schemeCombo, *targetCombo, *fsCombo, *clusterCombo, *passesCombo;
  QPushButton *selectBtn, *hashBtn, *startBtn, *closeBtn;
  QToolButton *aboutBtn, *logBtn;
  QSlider *persist;
  QLabel *persistValue, *devCount, *imageInfo;
  QLineEdit *labelEdit;
  QCheckBox *chkFixed, *chkQuick, *chkExt, *chkBad;
  QProgressBar *bar;
  QFormLayout *drive = nullptr, *format = nullptr;
  QWidget *persistRow = nullptr;
  QPlainTextEdit *logView = nullptr;
  QDialog *logDialog = nullptr;

  // ---- state ----
  QString isoPath;
  bool isoWindows = false, isoHybrid = false, isoValid = false;
  QString isoLabel;
  QVector<RufuxDevice> devs;
  QString devSignature;
  QProcess *worker = nullptr;
  QString lastError;
  QStringList logLines;
  QString savedUser;

  void build() {
    auto *root = new QVBoxLayout(this);

    // ---- Drive Properties ----
    auto *g1 = new QGroupBox("Drive Properties");
    auto *v1 = new QVBoxLayout(g1);
    drive = new QFormLayout;
    devCombo = new QComboBox;
    drive->addRow("Device", devCombo);

    bootCombo = new QComboBox;
    bootCombo->addItems({"Non bootable", "FreeDOS", "Disk or ISO image (Please select)"});
    bootCombo->setCurrentIndex(2);
    selectBtn = new QPushButton("SELECT");
    hashBtn = new QPushButton(QStringLiteral("\u2713"));
    hashBtn->setToolTip("Compute the checksums of the image");
    hashBtn->setFixedWidth(hashBtn->sizeHint().height() + 6);
    auto *bootRow = new QHBoxLayout;
    bootRow->addWidget(bootCombo, 1);
    bootRow->addWidget(hashBtn);
    bootRow->addWidget(selectBtn);
    drive->addRow("Boot selection", bootRow);

    imageCombo = new QComboBox;
    imageCombo->addItem("Standard Windows installation");
    drive->addRow("Image Option", imageCombo);

    persist = new QSlider(Qt::Horizontal);
    persist->setRange(0, 128);  // 512 MiB steps, up to 64 GiB
    persistValue = new QLabel("No persistence");
    persistValue->setMinimumWidth(100);
    persistRow = new QWidget;
    auto *pl = new QHBoxLayout(persistRow);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(persist, 1);
    pl->addWidget(persistValue);
    drive->addRow("Persistent partition size", persistRow);

    schemeCombo = new QComboBox;
    schemeCombo->addItems({"MBR", "GPT"});
    drive->addRow("Partition scheme", schemeCombo);
    targetCombo = new QComboBox;
    targetCombo->addItems({"BIOS (or UEFI-CSM)", "UEFI (non CSM)"});
    drive->addRow("Target system", targetCombo);
    v1->addLayout(drive);

    auto *advDrive = new QWidget;
    auto *adl = new QVBoxLayout(advDrive);
    adl->setContentsMargins(20, 0, 0, 0);
    chkFixed = new QCheckBox("List USB Hard Drives");
    adl->addWidget(chkFixed);
    auto *adv1 = new Advanced("Show advanced drive properties", advDrive);
    v1->addWidget(adv1->toggle);
    v1->addWidget(advDrive);
    root->addWidget(g1);

    // ---- Format Options ----
    auto *g2 = new QGroupBox("Format Options");
    auto *v2 = new QVBoxLayout(g2);
    format = new QFormLayout;
    labelEdit = new QLineEdit("NO_LABEL");
    format->addRow("Volume label", labelEdit);
    fsCombo = new QComboBox;
    fsCombo->addItems({"FAT32", "NTFS", "exFAT", "UDF", "ext4"});
    format->addRow("File system", fsCombo);
    clusterCombo = new QComboBox;
    clusterCombo->addItems({"512 bytes", "1024 bytes", "2048 bytes", "4096 bytes (Default)", "8192 bytes",
                            "16 kilobytes", "32 kilobytes", "64 kilobytes"});
    clusterCombo->setCurrentIndex(3);
    format->addRow("Cluster size", clusterCombo);
    v2->addLayout(format);

    auto *advFmt = new QWidget;
    auto *afl = new QVBoxLayout(advFmt);
    afl->setContentsMargins(20, 0, 0, 0);
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
    afl->addWidget(chkQuick);
    afl->addWidget(chkExt);
    afl->addLayout(badRow);
    auto *adv2 = new Advanced("Show advanced format options", advFmt);
    v2->addWidget(adv2->toggle);
    v2->addWidget(advFmt);
    root->addWidget(g2);

    // ---- Status ----
    auto *g3 = new QGroupBox("Status");
    auto *v3 = new QVBoxLayout(g3);
    bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(100);
    bar->setTextVisible(true);
    bar->setFormat("READY");
    bar->setAlignment(Qt::AlignCenter);
    v3->addWidget(bar);
    auto *foot = new QHBoxLayout;
    aboutBtn = iconButton("help-about", "i", "About Rufux");
    logBtn = iconButton("text-x-generic", "Log", "Show the log");
    startBtn = new QPushButton("START");
    startBtn->setDefault(true);
    closeBtn = new QPushButton("CLOSE");
    foot->addWidget(aboutBtn);
    foot->addWidget(logBtn);
    foot->addStretch(1);
    foot->addWidget(startBtn);
    foot->addWidget(closeBtn);
    v3->addLayout(foot);
    root->addWidget(g3);

    devCount = new QLabel;
    imageInfo = new QLabel;
    imageInfo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *info = new QHBoxLayout;
    info->addWidget(devCount);
    info->addWidget(imageInfo, 1);
    root->addLayout(info);
    root->addStretch(1);
    setMinimumWidth(540);

    for (QFormLayout *f : {drive, format})
      for (int i = 0; i < f->rowCount(); i++)
        if (QLayoutItem *it = f->itemAt(i, QFormLayout::LabelRole))
          if (QWidget *lw = it->widget()) lw->setMinimumWidth(170);

    // ---- wiring ----
    QObject::connect(selectBtn, &QPushButton::clicked, [this] { pickImage(); });
    QObject::connect(hashBtn, &QPushButton::clicked, [this] { showChecksums(); });
    QObject::connect(startBtn, &QPushButton::clicked, [this] { onStart(); });
    QObject::connect(closeBtn, &QPushButton::clicked, [this] { close(); });
    QObject::connect(logBtn, &QToolButton::clicked, [this] { showLog(); });
    QObject::connect(aboutBtn, &QToolButton::clicked, [this] { showAbout(); });
    QObject::connect(chkBad, &QCheckBox::toggled, passesCombo, &QComboBox::setEnabled);
    QObject::connect(chkFixed, &QCheckBox::toggled, [this](bool) { refreshDevices(true); });
    QObject::connect(bootCombo, &QComboBox::currentIndexChanged, [this](int) { syncEnabled(); });
    QObject::connect(fsCombo, &QComboBox::currentIndexChanged, [this](int) { relabel(); });
    QObject::connect(persist, &QSlider::valueChanged, [this](int v) {
      if (v == 0) persistValue->setText("No persistence");
      else if (v % 2) persistValue->setText(QString::number(v * 512) + " MB");
      else persistValue->setText(QString::number(v / 2) + " GB");
    });
    QObject::connect(targetCombo, &QComboBox::currentIndexChanged, [this](int t) {
      QSignalBlocker b(schemeCombo);
      schemeCombo->setCurrentIndex(t == 1 ? 1 : 0);
    });
    QObject::connect(schemeCombo, &QComboBox::currentIndexChanged, [this](int s) {
      QSignalBlocker b(targetCombo);
      targetCombo->setCurrentIndex(s == 1 ? 1 : 0);
    });
    schemeCombo->setCurrentIndex(1);  // GPT, UEFI (non CSM)
    targetCombo->setCurrentIndex(1);
  }

  // ---- helpers ----
  QString fsKey() const {
    static const char *keys[] = {"vfat", "ntfs", "exfat", "udf", "ext4"};
    const int i = fsCombo->currentIndex();
    return QString::fromLatin1(keys[i >= 0 && i < 5 ? i : 0]);
  }
  void log(const QString &line) {
    const QString s = QDateTime::currentDateTime().toString("HH:mm:ss  ") + line;
    logLines << s;
    if (logView) logView->appendPlainText(s);
  }
  void relabel() { labelEdit->setText(sanitizeLabel(labelEdit->text(), fsKey())); }
  bool busy() const { return worker && worker->state() != QProcess::NotRunning; }
  bool isoMode() const { return bootCombo->currentIndex() == 2; }

  void syncEnabled() {
    const bool run = busy(), iso = isoMode(), have = !isoPath.isEmpty();
    drive->setRowVisible(imageCombo, iso && have && isoWindows);
    drive->setRowVisible(persistRow, iso && have && !isoWindows && isoValid);
    for (QWidget *w : {static_cast<QWidget *>(devCombo), static_cast<QWidget *>(bootCombo),
                       static_cast<QWidget *>(selectBtn), static_cast<QWidget *>(schemeCombo),
                       static_cast<QWidget *>(targetCombo), static_cast<QWidget *>(fsCombo),
                       static_cast<QWidget *>(clusterCombo), static_cast<QWidget *>(labelEdit)})
      w->setEnabled(!run);
    hashBtn->setEnabled(!run && have);
    closeBtn->setEnabled(!run);
    startBtn->setText(run ? "CANCEL" : "START");
  }

  // ---- devices ----
  void refreshDevices(bool force) {
    if (busy()) return;
    RufuxDevice raw[64];
    const int n = rufux_list_devices(raw, 64, chkFixed->isChecked() ? 1 : 0);
    QString sig;
    for (int i = 0; i < n; i++)
      sig += QString::fromUtf8(raw[i].devnode) + ":" + QString::number(raw[i].size_bytes) + ";";
    if (!force && sig == devSignature) return;
    devSignature = sig;
    const QString keep = devCombo->currentData().toString();
    devs.clear();
    devCombo->clear();
    for (int i = 0; i < n; i++) {
      devs << raw[i];
      QString name = QString::fromUtf8(raw[i].model).trimmed();
      if (name.isEmpty()) name = QString::fromUtf8(raw[i].vendor).trimmed();
      if (name.isEmpty()) name = "NO_LABEL";
      devCombo->addItem(QString("%1 (%2) [%3]").arg(name, QString::fromUtf8(raw[i].sysname),
                                                     humanSize(raw[i].size_bytes)),
                        QString::fromUtf8(raw[i].devnode));
    }
    const int idx = devCombo->findData(keep);
    if (idx >= 0) devCombo->setCurrentIndex(idx);
    int shown = n;
    if (n == 0 && qEnvironmentVariableIsSet("RUFUX_GUI_DEMO")) {  // screenshots only
      devCombo->addItem("SanDisk Ultra (sdb) [14.9 GB]", "/dev/sdb");
      shown = 1;
    } else if (n == 0) {
      devCombo->addItem("No USB drive found");
    }
    devCount->setText(shown == 1 ? "1 device found" : QString::number(shown) + " devices found");
  }

  // ---- image selection ----
  void pickImage() {
    const QString p = QFileDialog::getOpenFileName(
        this, "Select a disk image", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        "All files (*);;ISO Image (*.iso);;Disk Image (*.img *.raw *.bin *.vhd *.dd)");
    if (!p.isEmpty()) loadImage(p);
  }

  void loadImage(const QString &path) {
    bar->setFormat("Reading image...");
    auto *w = new QFutureWatcher<ProbeResult>(this);
    QObject::connect(w, &QFutureWatcher<ProbeResult>::finished, [this, w] {
      const ProbeResult r = w->result();
      w->deleteLater();
      bar->setFormat("READY");
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
    isoValid = r.ok;
    isoHybrid = r.ok && r.info.bootable;
    isoLabel = r.ok && r.info.label[0] ? QString::fromUtf8(r.info.label) : QString();
    bootCombo->setItemText(2, QFileInfo(r.path).fileName());
    bootCombo->setCurrentIndex(2);
    imageInfo->setText("Using image: " + QFileInfo(r.path).fileName());
    log("Using image: " + r.path + " (" + humanSize(r.ok ? r.info.size_bytes : QFileInfo(r.path).size()) + ")");
    if (isoWindows) {
      fsCombo->setCurrentIndex(1);      // NTFS
      schemeCombo->setCurrentIndex(1);  // GPT, UEFI (non CSM)
    } else {
      fsCombo->setCurrentIndex(0);      // FAT32
      schemeCombo->setCurrentIndex(0);  // MBR
    }
    labelEdit->setText(sanitizeLabel(isoLabel.isEmpty() ? QString("NO_LABEL") : isoLabel, fsKey()));
    syncEnabled();
  }

  // ---- checksums ----
  void showChecksums() {
    const QString path = isoPath;
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle("Checksums");
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto *l = new QVBoxLayout(dlg);
    auto *text = new QPlainTextEdit("Computing...\n");
    text->setReadOnly(true);
    text->setMinimumSize(620, 120);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
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

  // ---- log / about ----
  void showLog() {
    if (!logDialog) {
      logDialog = new QDialog(this);
      logDialog->setWindowTitle("Log");
      auto *l = new QVBoxLayout(logDialog);
      logView = new QPlainTextEdit;
      logView->setReadOnly(true);
      logView->setPlainText(logLines.join("\n"));
      logView->setMinimumSize(640, 340);
      l->addWidget(logView);
      auto *bb = new QDialogButtonBox;
      auto *clear = bb->addButton("Clear", QDialogButtonBox::ActionRole);
      auto *save = bb->addButton("Save", QDialogButtonBox::ActionRole);
      bb->addButton(QDialogButtonBox::Close);
      QObject::connect(bb, &QDialogButtonBox::rejected, logDialog, &QDialog::hide);
      QObject::connect(clear, &QPushButton::clicked, [this] { logLines.clear(); logView->clear(); });
      QObject::connect(save, &QPushButton::clicked, [this] {
        const QString p = QFileDialog::getSaveFileName(this, "Save log", "rufux.log", "Log (*.log *.txt)");
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
        QString("<h3>Rufux %1</h3><p>Create bootable USB drives on Linux.</p>"
                "<p>A Linux port of <a href=\"https://github.com/pbatard/rufus\">Rufus</a> by Pete Batard.<br>"
                "License: GNU General Public License, version 3.</p>"
                "<p><a href=\"https://github.com/Hultwl/Rufux\">github.com/Hultwl/Rufux</a></p>")
            .arg(RUFUX_VERSION));
  }

  // ---- Windows User Experience (Rufus's dialog, same wording) ----
  bool askWindowsOptions(QString *wue, QStringList *extra) {
    QDialog d(this);
    d.setWindowTitle("Windows User Experience");
    auto *l = new QVBoxLayout(&d);
    l->addWidget(new QLabel("Customize Windows installation?"));
    QSettings s("Rufux", "Rufux");
    auto check = [&](const QString &text, const char *key, bool def) {
      auto *c = new QCheckBox(text);
      c->setChecked(s.value(QString("wue/") + key, def).toBool());
      l->addWidget(c);
      return c;
    };
    auto *bypass = check("Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0", "bypass", true);
    auto *nro = check("Remove requirement for an online Microsoft account", "nro", true);
    auto *userRow = new QHBoxLayout;
    auto *userOn = new QCheckBox("Create a local account with username:");
    userOn->setChecked(s.value("wue/user_on", true).toBool());
    QString def = qEnvironmentVariable("USER", "User");
    auto *userEdit = new QLineEdit(s.value("wue/user", def).toString());
    userEdit->setEnabled(userOn->isChecked());
    QObject::connect(userOn, &QCheckBox::toggled, userEdit, &QLineEdit::setEnabled);
    userRow->addWidget(userOn);
    userRow->addWidget(userEdit, 1);
    l->addLayout(userRow);
    auto *locale = check("Set regional options to the same values as this user's", "locale", true);
    auto *privacy = check("Disable data collection (Skip privacy questions)", "privacy", true);
    auto *bitlocker = check("Disable BitLocker automatic device encryption", "bitlocker", true);
    auto *qol = check("QoL improvements (Don't force Copilot, OneDrive, Outlook, Fast Startup, etc.)", "qol", false);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return false;
    const QString uname = userEdit->text().trimmed();
    if (userOn->isChecked() && uname.isEmpty()) {
      QMessageBox::warning(this, "Rufux", "Please enter a user name.");
      return askWindowsOptions(wue, extra);
    }
    s.setValue("wue/bypass", bypass->isChecked());
    s.setValue("wue/nro", nro->isChecked());
    s.setValue("wue/user_on", userOn->isChecked());
    s.setValue("wue/user", uname);
    s.setValue("wue/locale", locale->isChecked());
    s.setValue("wue/privacy", privacy->isChecked());
    s.setValue("wue/bitlocker", bitlocker->isChecked());
    s.setValue("wue/qol", qol->isChecked());
    QStringList v;
    if (bypass->isChecked()) v << "bypass";
    if (nro->isChecked()) v << "nro";
    if (privacy->isChecked()) v << "privacy";
    if (bitlocker->isChecked()) v << "bitlocker";
    if (qol->isChecked()) v << "qol";
    if (locale->isChecked()) {
      QString tag = QLocale::system().name().replace('_', '-');
      if (tag == "C" || tag.isEmpty()) tag = "en-US";
      v << "locale";
      *extra << "--locale" << tag << "--timezone" << QString::fromUtf8(QTimeZone::systemTimeZoneId());
    }
    if (userOn->isChecked()) v << ("user=" + uname);
    *wue = v.isEmpty() ? QStringLiteral("none") : v.join(",");
    return true;
  }

  // Rufus's question for an ISOHybrid image. Returns "extract", "dd" or "" (cancelled).
  QString askIsoMode() {
    QMessageBox box(QMessageBox::Question, "ISOHybrid image detected",
        "The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
        "ISO Image (file copy) mode or DD Image (disk image) mode.\n"
        "Rufux recommends using ISO Image mode, so that you always have full access to the drive after writing it.\n"
        "However, if you encounter issues during boot, you can try writing this image again in DD Image mode.\n\n"
        "Please select the mode that you want to use to write this image:",
        QMessageBox::NoButton, this);
    auto *iso = box.addButton("Write in ISO Image mode (Recommended)", QMessageBox::AcceptRole);
    auto *dd = box.addButton("Write in DD Image mode", QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == iso) return "extract";
    if (box.clickedButton() == dd) return "dd";
    return QString();
  }

  // ---- start / cancel ----
  void onStart() {
    if (busy()) { cancelWorker(); return; }
    const QString dst = devCombo->currentData().toString();
    if (dst.isEmpty()) return;

    QString mode;
    if (bootCombo->currentIndex() == 1) mode = "dos";
    else if (bootCombo->currentIndex() == 0) mode = "format";
    else {
      if (isoPath.isEmpty()) { QMessageBox::warning(this, "Rufux", "Please select a disk or ISO image."); return; }
      if (isoWindows) mode = "windows";
      else if (!isoValid) mode = "dd";
      else if (isoHybrid) { mode = askIsoMode(); if (mode.isEmpty()) return; }
      else mode = "extract";
    }

    QString wue = "none";
    QStringList extra;
    if (mode == "windows" && !askWindowsOptions(&wue, &extra)) return;

    if (QMessageBox::warning(this, "Rufux",
            QString("WARNING: ALL DATA ON DEVICE '%1' WILL BE DESTROYED.\n"
                    "To continue with this operation, click OK. To quit click CANCEL.")
                .arg(devCombo->currentText()),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
      return;

    const QString fs = fsKey();
    static const int clusterSectors[] = {1, 2, 4, 0, 16, 32, 64, 128};  // 0 = default (4096)
    QStringList a;
    a << "create" << (mode == "dos" || mode == "format" ? QString("none") : isoPath) << dst
      << "--mode" << mode
      << "--scheme" << (schemeCombo->currentIndex() == 1 ? "gpt" : "dos")
      << "--fs" << fs << "--label" << sanitizeLabel(labelEdit->text(), fs)
      << "--persist-mb" << QString::number(mode == "extract" ? persist->value() * 512 : 0)
      << "--cluster-sectors" << QString::number(clusterSectors[clusterCombo->currentIndex()])
      << "--badblock-passes" << QString::number(chkBad->isChecked() ? passesCombo->currentIndex() + 1 : 0)
      << (chkQuick->isChecked() ? "--quick" : "--full");
    if (!chkExt->isChecked()) a << "--no-autorun";
    if (mode == "windows") a << "--wue" << wue << extra;
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
        QMessageBox::critical(this, "Rufux", "pkexec was not found. Install polkit, or run Rufux as root.");
        return;
      }
    }

    lastError.clear();
    bar->setValue(0);
    bar->setFormat("%p%");
    log("Starting: " + mode + " -> " + dst);
    worker = new QProcess(this);
    worker->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(worker, &QProcess::readyRead, [this] { onOutput(worker->readAll()); });
    QObject::connect(worker, &QProcess::finished, [this](int code, QProcess::ExitStatus st) { onFinished(code, st); });
    QObject::connect(worker, &QProcess::errorOccurred, [this](QProcess::ProcessError e) {
      if (e == QProcess::FailedToStart) lastError = "Could not start the worker process.";
    });
    worker->start(prog, a);
    syncEnabled();
  }

  void cancelWorker() {
    log("Cancel requested.");
    bar->setFormat("Cancelling...");
    worker->terminate();
    QTimer::singleShot(4000, worker, [w = worker] { if (w->state() != QProcess::NotRunning) w->kill(); });
  }

  void onOutput(const QByteArray &data) {
    static const QRegularExpression pct(R"(^\s*(\d{1,3})%)");
    for (const QByteArray &tok : data.split('\n'))
      for (const QByteArray &part : tok.split('\r')) {
        QString s = QString::fromUtf8(part).trimmed();
        if (s.isEmpty() || s.startsWith('+')) continue;
        const auto m = pct.match(s);
        if (m.hasMatch()) {
          bar->setValue(qBound(0, m.captured(1).toInt(), 100));
          s = s.mid(m.capturedLength()).trimmed();
          if (s.isEmpty()) continue;
        }
        log(s);
        if (s != "Done.") lastError = s;
      }
  }

  void onFinished(int code, QProcess::ExitStatus st) {
    worker->deleteLater();
    worker = nullptr;
    syncEnabled();
    if (st == QProcess::NormalExit && code == 0) {
      bar->setValue(100);
      bar->setFormat("READY");
      log("Done.");
      QApplication::alert(this);
      return;
    }
    bar->setFormat("READY");
    QString why = lastError.isEmpty() ? QString("The worker exited with code %1.").arg(code) : lastError;
    if (code == 126 || code == 127) why = "Authorization was cancelled or pkexec is unavailable.";
    log("Failed: " + why);
    QMessageBox box(QMessageBox::Critical, "Rufux", why, QMessageBox::Ok, this);
    box.setDetailedText(logLines.join("\n"));
    box.exec();
  }
};

}  // namespace

int rufux_gui_run(int argc, char **argv) {
  // Our own options are removed before Qt sees argv.
  QString openFile;
  std::vector<char *> keep{argv[0]};
  for (int i = 1; i < argc; i++) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == "--gui" || a == "gui") continue;
    if (!a.startsWith("-") && QFileInfo(a).isFile()) { openFile = a; continue; }
    keep.push_back(argv[i]);
  }
  // The AppImage carries its own Qt, which cannot see the desktop's platform theme.
  // The portal theme gives it the desktop's file dialogs.
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME") && qEnvironmentVariableIsSet("APPIMAGE"))
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
  int qargc = int(keep.size());
  QApplication app(qargc, keep.data());
  QApplication::setApplicationName("Rufux");
  QApplication::setDesktopFileName("io.github.hultwl.rufux");
  QApplication::setWindowIcon(QIcon::fromTheme("io.github.hultwl.rufux"));

  MainWindow w;
  w.show();
  if (!openFile.isEmpty()) w.openImage(QFileInfo(openFile).absoluteFilePath());

  // RUFUX_GUI_SNAPSHOT=file.png saves a screenshot and exits (tests, docs).
  const QString snap = qEnvironmentVariable("RUFUX_GUI_SNAPSHOT");
  if (!snap.isEmpty()) {
    const QString wdlg = qEnvironmentVariable("RUFUX_GUI_WINDLG");
    if (!wdlg.isEmpty()) QTimer::singleShot(200, &app, [&w, wdlg] { w.demoWindowsDialog(wdlg); });
    const QString img = qEnvironmentVariable("RUFUX_GUI_IMAGE");
    if (!img.isEmpty()) w.openImage(img);
    QTimer::singleShot((img.isEmpty() && wdlg.isEmpty()) ? 400 : 1800, &app, [&w, snap] {
      w.grab().save(snap);
      QApplication::quit();
    });
  }
  return app.exec();
}
