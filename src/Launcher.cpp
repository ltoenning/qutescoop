#include "Airac.h"
#include "GuiMessage.h"
#include "JobList.h"
#include "Launcher.h"
#include "NavData.h"
#include "Net.h"
#include "Settings.h"
#include "Whazzup.h"
#include "dialogs/Window.h"

// singleton instance
Launcher* launcherInstance = 0;

Launcher* Launcher::instance(bool createIfNoInstance) {
    if (launcherInstance == 0 && createIfNoInstance) {
        launcherInstance = new Launcher();
    }
    return launcherInstance;
}

Launcher::Launcher(QWidget* parent)
    : QWidget(parent,
          Qt::FramelessWindowHint | Qt::WindowSystemMenuHint) {
    _map = QPixmap(":/startup/logo").scaled(600, 600);
    resize(_map.width(), _map.height());
    move(
        qApp->primaryScreen()->availableGeometry().center()
        - rect().center()
    );
    setMask(_map.mask());

    _image = new QLabel(this);
    _text = new QLabel(this);
    _progress = new QProgressBar(this);

    _image->setPixmap(_map);
    _image->resize(_map.width(), _map.height());

    _text->setText("Launcher started");
    _text->setStyleSheet(
        "QLabel {"
        "color: white;"
        "font-weight: bold;"
        "}"
    );
    _text->setAlignment(Qt::AlignCenter);
    _text->setWordWrap(true);
    _text->resize(440, _text->height());
    //qDebug() << "text frames w:" << text->frameSize() << " text h:" << text->height();
    _text->move((_map.width() / 2) - 220, (_map.height() / 3) * 2 + 30);

    _progress->hide();
    _progress->setTextVisible(false);
    _progress->resize(300, _progress->height());
    _progress->move(
        _map.width() / 2 - 150,
        _map.height() / 3 * 2 + 30 + _text->height()
    );

    GuiMessages::instance()->addStatusLabel(_text);
    GuiMessages::instance()->addProgressBar(_progress);

    _image->lower();
    _text->raise();
    _progress->raise();
}

Launcher::~Launcher() {
    delete _image;
    delete _text;
}

///////////////////////////
// Events
///////////////////////////

void Launcher::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _dragPosition = event->globalPos() - frameGeometry().topLeft();
        event->accept();
    }
}

void Launcher::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        move(event->globalPos() - _dragPosition);
        event->accept();
    }
}

///////////////////////////
// General
///////////////////////////

void Launcher::fireUp() {
    qDebug() << "Launcher::fireUp()";
    show();

    JobList* jobs = new JobList(this);

    // check for datafile updates
    if (Settings::checkForUpdates()) {
        jobs->append(
            JobList::Job(
                Launcher::instance(),
                SLOT(checkData()), SIGNAL(dataChecked())
            )
        );
    }

    // load NavData
    jobs->append(
        JobList::Job(
            NavData::instance(),
            SLOT(load()), SIGNAL(loaded())
        )
    );

    // load Airac
    if (Settings::useNavdata()) {
        jobs->append(
            JobList::Job(
                Airac::instance(),
                SLOT(load()), SIGNAL(loaded())
            )
        );
    }

    // set up main window
    jobs->append(
        JobList::Job(
            Window::instance(),
            SLOT(restore()),
            SIGNAL(restored())
        )
    );

    if (Settings::downloadOnStartup()) {
        jobs->append(
            JobList::Job(
                Whazzup::instance(),
                SLOT(downloadJson3()),
                SIGNAL(whazzupDownloaded())
            )
        );
    }

    connect(
        jobs,
        &JobList::finished,
        this,
        [this] {
            GuiMessages::remove("joblist");
            deleteLater();
        }
    );

    jobs->start();
    qDebug() << "Launcher::fireUp() finished";
}

///////////////////////////
//Navdata update & loading
///////////////////////////
void Launcher::checkData() {
    qDebug() << "Launcher::checkData()";
    GuiMessages::status("Checking for QuteScoop data file updates...", "checknavdata");
    QUrl url(Settings::remoteDataRepository().arg("dataversions.txt"));
    qDebug() << "checkForDataUpdates()" << url.toString();
    _replyDataVersionsAndFiles = Net::g(url);

    connect(_replyDataVersionsAndFiles, &QNetworkReply::finished, this, &Launcher::dataVersionsDownloaded);
}

void Launcher::dataVersionsDownloaded() {
    qDebug() << "dataVersionsDownloaded()";
    disconnect(_replyDataVersionsAndFiles, &QNetworkReply::finished, this, &Launcher::dataVersionsDownloaded);
    _replyDataVersionsAndFiles->deleteLater();

    if (_replyDataVersionsAndFiles->error() != QNetworkReply::NoError) {
        GuiMessages::warning(_replyDataVersionsAndFiles->errorString());

        GuiMessages::remove("checknavdata");
        emit dataChecked();
    }

    _serverDataVersionsList.clear();
    while (_replyDataVersionsAndFiles->canReadLine()) {
        QStringList splitLine = QString(_replyDataVersionsAndFiles->readLine()).split("%%");
        if (splitLine.count() != 2) {
            continue;
        }
        _serverDataVersionsList[splitLine.first()] = splitLine.last().toInt();
    }
    qDebug() << "dataVersionsDownloaded() server:" << _serverDataVersionsList;

    QFile localVersionsFile(Settings::dataDirectory("data/dataversions.txt"));
    if (!localVersionsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        GuiMessages::criticalUserInteraction(
            QString("Could not read %1.\nThus we are updating all datafiles.")
            .arg(localVersionsFile.fileName()),
            "Complete datafiles update necessary"
        );
    }
    _localDataVersionsList.clear();
    while (!localVersionsFile.atEnd()) {
        QStringList splitLine = QString(localVersionsFile.readLine()).split("%%");
        if (splitLine.count() != 2) {
            continue;
        }
        _localDataVersionsList[splitLine.first()] = splitLine.last().toInt();
    }
    qDebug() << "dataVersionsDownloaded() local: " << _localDataVersionsList;
    localVersionsFile.close();

    //collecting files to update
    foreach (const QString fn, _serverDataVersionsList.keys()) {
        // also download files that are locally not available
        if (
            !_localDataVersionsList.contains(fn)
            || _serverDataVersionsList[fn] > _localDataVersionsList[fn]
        ) {
            _dataFilesToDownload.append(fn);
        }
    }

    if (!_dataFilesToDownload.isEmpty()) {
        GuiMessages::infoUserAttention(
            "New sector data is available.\n"
            "It will be downloaded now.",
            "New datafiles"
        );
        QUrl url(Settings::remoteDataRepository()
            .arg(_dataFilesToDownload.first()));
        qDebug() << "dataVersionsDownloaded() Downloading datafile" << url.toString();
        _replyDataVersionsAndFiles = Net::g(url);
        connect(_replyDataVersionsAndFiles, &QNetworkReply::finished, this, &Launcher::dataFileDownloaded);
    } else {
        qDebug() << "dataVersionsDownloaded() all files up to date";
        GuiMessages::remove("checknavdata");
        emit dataChecked();
    }
}

void Launcher::dataFileDownloaded() {
    // processing received file
    if (!_replyDataVersionsAndFiles->url().isEmpty()) {
        qDebug() << "dataFileDownloaded() received"
                 << _replyDataVersionsAndFiles->url();
        disconnect(_replyDataVersionsAndFiles, &QNetworkReply::finished, this, &Launcher::dataFileDownloaded);
        // error?
        if (_replyDataVersionsAndFiles->error() != QNetworkReply::NoError) {
            GuiMessages::criticalUserInteraction(
                QString("Error downloading %1:\n%2")
                .arg(_replyDataVersionsAndFiles->url().toString(), _replyDataVersionsAndFiles->errorString()),
                "New datafiles"
            );
        } else {
            // saving file
            QFile f(Settings::dataDirectory("data/%1")
                .arg(_dataFilesToDownload.first()));
            qDebug() << "dataFileDownloaded() writing" << f.fileName();
            if (f.open(QIODevice::WriteOnly)) {
                f.write(_replyDataVersionsAndFiles->readAll());
                f.flush();
                f.close();

                _localDataVersionsList[_dataFilesToDownload.first()] =
                    _serverDataVersionsList[_dataFilesToDownload.first()];
                qDebug() << "dataFileDownloaded() updated" <<
                    _dataFilesToDownload.first() << "to version"
                         << _localDataVersionsList[_dataFilesToDownload.first()];

                GuiMessages::infoUserAttention(
                    QString("Downloaded %1")
                    .arg(_replyDataVersionsAndFiles->url().toString()),
                    "New datafiles"
                );
            } else {
                GuiMessages::criticalUserInteraction(
                    QString("Error: Could not write to %1").arg(f.fileName()),
                    "New datafiles"
                );
            }
            _dataFilesToDownload.removeFirst();
            _replyDataVersionsAndFiles->deleteLater();
        }
    }
    // starting new downloads
    if (!_dataFilesToDownload.isEmpty()) {
        QUrl url(Settings::remoteDataRepository()
            .arg(_dataFilesToDownload.first()));
        qDebug() << "dataVersionsDownloaded() Downloading datafile" << url.toString();
        _replyDataVersionsAndFiles = Net::g(url);
        connect(_replyDataVersionsAndFiles, &QNetworkReply::finished, this, &Launcher::dataFileDownloaded);
    } else {
        // we are finished
        GuiMessages::infoUserAttention(
            "New data has been downloaded.",
            "New datafiles"
        );
        // updating local dataversions.txt
        QFile localDataVersionsFile(Settings::dataDirectory("data/dataversions.txt"));
        if (localDataVersionsFile.open(QIODevice::WriteOnly)) {
            foreach (const QString fn, _localDataVersionsList.keys()) {
                localDataVersionsFile.write(
                    QString("%1%%%2\n")
                    .arg(fn).arg(_localDataVersionsList[fn]).toLatin1()
                );
            }
            localDataVersionsFile.close();
        } else {
            GuiMessages::criticalUserInteraction(
                QString("Error writing %1").arg(localDataVersionsFile.fileName()),
                "New datafiles"
            );
        }
        GuiMessages::remove("checknavdata");
        emit dataChecked();
    }
}

void Launcher::loadNavdata() {
    qDebug() << "Launcher:loadNavdata()";
    GuiMessages::status("Loading Navdata", "loadnavdata");

    NavData::instance()->load();
    Airac::instance()->load();
    GuiMessages::remove("loadnavdata");
    qDebug() << "Launcher:loadNavdata() finished";
}
