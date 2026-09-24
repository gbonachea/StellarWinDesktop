// ============================================================================
//  Stellarbox Windesktop 0.2.0
//  ---------------------------------------------------------------------------
//  Escritorio en una ventana: fondo de pantalla + iconos del directorio
//  configurado, con menú contextual, arrastrar-y-soltar y cambio de fondo.
//
//  RECONSTRUCCIÓN SOURCE-LEVEL a partir del binario
//  "stellarbox-desktop" (ELF64 PIE, stripped, Qt 6.4, ~25 KB de código).
//  Los comentarios /* 0x... */ apuntan a las direcciones en el binario.
//
//  Compilación (Qt6 + XCB, requiere AUTOMOC por el Q_OBJECT):
//      cmake -B build && cmake --build build
// ============================================================================

#include <QtWidgets>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QDesktopServices>
#include <QSaveFile>
#include <QFileDialog>
#include <QScreen>

#include <QShortcut>
#include <QClipboard>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QDirIterator>
#include <QMimeData>
#include <QAbstractProxyModel>
#include <QProcess>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QTimer>
#include <functional>

#include <cstdio>
#include <cstring>

#include <xcb/xcb.h>

class DesktopView;      // definiciones más abajo (DesktopIconView depende de ella)
class DesktopIconView;  // definido tras DesktopView (usa la vista padre)

// Widgets del escritorio (definidos más abajo; aquí solo punteros)
class DesktopInfoWidget;
class ClockWidget;
class SystemWidget;
class NetworkWidget;
class DiskWidget;

// ============================================================================
//  Configuración persistente del escritorio
//  ============================================================================
//  Se guarda en ~/.config/stellarbox/settings.ini (QSettings, formato INI)
//  y se edita desde la ventana "Configuración..." (SettingsDialog).
struct AppSettings
{
    QString desktopDir;          // directorio mostrado como escritorio
    QString bgDir;               // directorio con los fondos instalados
    QString iconTheme;           // tema de iconos (vacío = el del sistema)
    int     iconSize = 48;       // tamaño del icono en la rejilla (px)
    QString qssFile;             // hoja de estilo personalizada (opcional)
    bool    iconThemeConfigured = false;   // el usuario la ha fijado antes

    // Iconos del sistema mostrados en el escritorio (página "Apariencia")
    bool showComputer = true;    // "Equipo"
    bool showUser     = true;    // "Usuario"
    bool showNetwork  = true;    // "Red"
    bool showTrash    = true;    // "Papelera"

    // Widgets del escritorio (página "Widget")
    bool widgetClock   = false;  // reloj
    bool widgetSystem  = false;  // monitor de CPU y memoria
    bool widgetNetwork = false;  // velocidad de la red
    bool widgetDisk    = false;  // capacidad de los discos

    // Pantalla (página "Desktop"); se aplican con xrandr al guardar
    QString resolution;          // p. ej. "1920x1080" (vacío = sin tocar)
    double  refreshRate = 0.0;   // Hz (0 = sin tocar)
    int     rotation    = 0;     // 0 normal · 1 90° · 2 180° · 3 270°

    void load();
    void save() const;
    void reset();                // valores de fábrica para un reinicio limpio
};

static QString configFilePath()
{
    return QDir::homePath() + QStringLiteral("/.config/stellarbox/settings.ini");
}

// Instancia global única, consultada por main, las vistas y el diálogo
static AppSettings &globalSettings()
{
    static AppSettings settings;
    return settings;
}

// Registro de todas las vistas (una por pantalla): el diálogo de Configuración
// aplica los cambios a todas usando applySettings()
static QList<DesktopView *> &allDesktopViews()
{
    static QList<DesktopView *> views;
    return views;
}

// ============================================================================
//  Tema de iconos predeterminado del sistema
//  ============================================================================
//  Detecta el tema de iconos que usa el escritorio (GTK 3/4, KDE o XFCE) para
//  que stellarbox adopte por defecto los mismos iconos que el resto del
//  sistema. Devuelve una cadena vacía si el entorno no define ninguno (Qt usa
//  entonces su tema por defecto, normalmente "hicolor").
static QString systemIconThemeName()
{
    // GTK (GNOME, Cinnamon, MATE, XFCE...): gtk-icon-theme-name = ...
    const QStringList gtkInis = {
        QDir::homePath() + QStringLiteral("/.config/gtk-3.0/settings.ini"),
        QDir::homePath() + QStringLiteral("/.config/gtk-4.0/settings.ini"),
    };
    for (const QString &iniPath : gtkInis) {
        QFile ini(iniPath);
        if (!ini.open(QIODevice::ReadOnly))
            continue;
        QTextStream in(&ini);
        bool inSettings = false;
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!inSettings && line == QStringLiteral("[Settings]")) {
                inSettings = true;
                continue;
            }
            if (!inSettings)
                continue;
            if (line.startsWith(QLatin1Char('[')))
                break;                       // fin del grupo [Settings]
            static const QString key = QStringLiteral("gtk-icon-theme-name=");
            if (line.startsWith(key)) {
                const QString value = line.mid(key.size()).trimmed();
                if (!value.isEmpty())
                    return value;
            }
        }
    }

    // KDE/Plasma: ~/.config/kdeglobals, grupo [Icons], clave Theme=
    QFile kde(QDir::homePath() + QStringLiteral("/.config/kdeglobals"));
    if (kde.open(QIODevice::ReadOnly)) {
        QTextStream in(&kde);
        bool inIcons = false;
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!inIcons && line == QStringLiteral("[Icons]")) {
                inIcons = true;
                continue;
            }
            if (!inIcons)
                continue;
            if (line.startsWith(QLatin1Char('[')))
                break;                       // fin del grupo [Icons]
            if (line.startsWith(QStringLiteral("Theme="))) {
                const QString value = line.mid(6).trimmed();
                if (!value.isEmpty())
                    return value;
            }
        }
    }

    // XFCE: xsettings.xml → <property name="Net/IconThemeName" ... value="..."/>
    QFile xfs(QDir::homePath()
              + QStringLiteral("/.config/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml"));
    if (xfs.open(QIODevice::ReadOnly)) {
        const QRegularExpression re(
            QStringLiteral("name=\"Net/IconThemeName\"[^>]*?value=\"([^\"]*)\""));
        const QRegularExpressionMatch match = re.match(QString::fromUtf8(xfs.readAll()));
        const QString value = match.captured(1).trimmed();
        if (!value.isEmpty())
            return value;
    }

    return QString();
}

void AppSettings::load()
{
    QSettings settings(configFilePath(), QSettings::IniFormat);
    desktopDir  = settings.value(QStringLiteral("Desktop/Dir"),
                                 QDir::homePath() + QStringLiteral("/Desktop")).toString();
    bgDir       = settings.value(QStringLiteral("Backgrounds/Dir"),
                                 QStringLiteral("/usr/share/backgrounds/wallpapers")).toString();
    iconTheme   = settings.value(QStringLiteral("Appearance/IconTheme"),
                                 QString()).toString();
    iconSize    = settings.value(QStringLiteral("Appearance/IconSize"), 48).toInt();
    qssFile     = settings.value(QStringLiteral("Appearance/Qss"), QString()).toString();
    iconThemeConfigured = settings.contains(QStringLiteral("Appearance/IconTheme"));

    showComputer = settings.value(QStringLiteral("Appearance/ShowComputer"), true).toBool();
    showUser     = settings.value(QStringLiteral("Appearance/ShowUser"), true).toBool();
    showNetwork  = settings.value(QStringLiteral("Appearance/ShowNetwork"), true).toBool();
    showTrash    = settings.value(QStringLiteral("Appearance/ShowTrash"), true).toBool();

    widgetClock   = settings.value(QStringLiteral("Widgets/Clock"), false).toBool();
    widgetSystem  = settings.value(QStringLiteral("Widgets/System"), false).toBool();
    widgetNetwork = settings.value(QStringLiteral("Widgets/Network"), false).toBool();
    widgetDisk    = settings.value(QStringLiteral("Widgets/Disk"), false).toBool();

    resolution  = settings.value(QStringLiteral("Desktop/Resolution"), QString()).toString();
    refreshRate = settings.value(QStringLiteral("Desktop/Rate"), 0.0).toDouble();
    rotation    = settings.value(QStringLiteral("Desktop/Rotation"), 0).toInt();
}

void AppSettings::save() const
{
    QDir().mkpath(QFileInfo(configFilePath()).absolutePath());
    QSettings settings(configFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("Desktop/Dir"), desktopDir);
    settings.setValue(QStringLiteral("Backgrounds/Dir"), bgDir);
    // El tema de iconos solo se guarda si el usuario lo eligió; en caso
    // contrario se usa el predeterminado del sistema (systemIconThemeName)
    if (iconThemeConfigured)
        settings.setValue(QStringLiteral("Appearance/IconTheme"), iconTheme);
    else
        settings.remove(QStringLiteral("Appearance/IconTheme"));
    settings.setValue(QStringLiteral("Appearance/IconSize"), iconSize);
    settings.setValue(QStringLiteral("Appearance/Qss"), qssFile);
    settings.setValue(QStringLiteral("Appearance/ShowComputer"), showComputer);
    settings.setValue(QStringLiteral("Appearance/ShowUser"), showUser);
    settings.setValue(QStringLiteral("Appearance/ShowNetwork"), showNetwork);
    settings.setValue(QStringLiteral("Appearance/ShowTrash"), showTrash);
    settings.setValue(QStringLiteral("Widgets/Clock"), widgetClock);
    settings.setValue(QStringLiteral("Widgets/System"), widgetSystem);
    settings.setValue(QStringLiteral("Widgets/Network"), widgetNetwork);
    settings.setValue(QStringLiteral("Widgets/Disk"), widgetDisk);
    settings.setValue(QStringLiteral("Desktop/Resolution"), resolution);
    settings.setValue(QStringLiteral("Desktop/Rate"), refreshRate);
    settings.setValue(QStringLiteral("Desktop/Rotation"), rotation);
}

void AppSettings::reset()
{
    *this = AppSettings();   // reinicializa todos los campos
    desktopDir = QDir::homePath() + QStringLiteral("/Desktop");
    bgDir      = QStringLiteral("/usr/share/backgrounds/wallpapers");
    iconTheme  = QString();   // vacío → tema predeterminado del sistema
    iconSize   = 48;
}

// ============================================================================
//  Compose un nombre de destino libre en desktopDir para copiar `source`
//  ============================================================================
//  "nombre", "nombre (1)", "nombre (2)" ... (QString::number en 0xc5d0).
//  Lo reutilizan tanto el arrastrar-y-soltar (DesktopIconView::dropEvent) como el
//  atajo Ctrl+V (DesktopView::pasteFromClipboard).
static QString uniqueDestination(const QString &source, const QString &desktopDir)
{
    const QFileInfo info(source);
    const QString base   = info.completeBaseName();
    const QString suffix = info.suffix();

    QString dest = QDir(desktopDir).filePath(base);
    if (!suffix.isEmpty())
        dest += QLatin1Char('.') + suffix;

    for (int i = 1; QFile::exists(dest); ++i) {
        dest = QDir(desktopDir).filePath(base + QStringLiteral(" (") +
                                         QString::number(i) + QLatin1Char(')'));
        if (!suffix.isEmpty())
            dest += QLatin1Char('.') + suffix;
    }
    return dest;
}

// ============================================================================
//  DesktopProxyModel : QAbstractProxyModel — iconos del sistema sobre el
//  escritorio (Equipo, Usuario, Red, Papelera) + los archivos del directorio
//  ============================================================================
//  QFileSystemModel no permite filas "virtuales", así que interponemos este
//  proxy plano: antepone hasta 4 filas propias a las filas de archivos y las
//  mapea sin transformación al modelo de archivos. Se reenvían también los
//  cambios del modelo de archivos (inserciones, borrados, cambios de datos)
//  para que la lista siga actualizándose como hasta ahora.
class DesktopProxyModel : public QAbstractProxyModel
{
public:
    enum SystemIcon {
        SystemComputer = 0,   // "Equipo"
        SystemUser     = 1,   // "Usuario"
        SystemNetwork  = 2,   // "Red"
        SystemTrash    = 3,   // "Papelera"
        SystemIconCount = 4
    };

    explicit DesktopProxyModel(QObject *parent = nullptr)
        : QAbstractProxyModel(parent)
    { }

    // Raíz de archivos del escritorio (origen de las filas mapeadas)
    void setSourceRoot(const QModelIndex &root)
    {
        m_sourceRoot = root;
        // Asegura que el directorio se haya poblado ya (QFileSystemModel
        // carga de forma perezosa) y reexpone todas las filas.
        if (sourceModel() && m_sourceRoot.isValid())
            sourceModel()->rowCount(m_sourceRoot);
        beginResetModel();
        endResetModel();
    }

    QModelIndex sourceRoot() const { return m_sourceRoot; }

    // Qué iconos del sistema están visibles
    void setSystemIcons(bool computer, bool user, bool network, bool trash)
    {
        const bool flags[SystemIconCount] = { computer, user, network, trash };
        bool changed = false;
        for (int i = 0; i < SystemIconCount; ++i)
            changed = changed || (m_enabled[i] != flags[i]);
        if (!changed)
            return;
        for (int i = 0; i < SystemIconCount; ++i)
            m_enabled[i] = flags[i];
        beginResetModel();
        endResetModel();
    }

    // Número de iconos del sistema activos (primeras filas del proxy)
    int systemIconCount() const
    {
        int n = 0;
        for (int i = 0; i < SystemIconCount; ++i)
            n += m_enabled[i] ? 1 : 0;
        return n;
    }

    bool isSystemIcon(const QModelIndex &index) const
    {
        return index.isValid() && !index.parent().isValid()
            && index.row() >= 0 && index.row() < systemIconCount();
    }

    // Icono del sistema (0..3) al que corresponde una fila virtual
    int systemIconId(const QModelIndex &index) const
    {
        int seen = 0;
        for (int i = 0; i < SystemIconCount; ++i) {
            if (!m_enabled[i])
                continue;
            if (seen == index.row())
                return i;
            ++seen;
        }
        return -1;
    }

    // Fila del proxy para un icono del sistema accesible en este momento
    QModelIndex systemIconIndex(int icon) const
    {
        int row = -1;
        for (int i = 0; i <= icon; ++i)
            if (m_enabled[i])
                ++row;
        return row >= 0 ? index(row, 0) : QModelIndex();
    }

    // Índice del proxy correspondiente a una ruta del escritorio
    QModelIndex filePathIndex(const QString &path) const
    {
        if (!sourceModel() || m_sourceRoot.isValid() == false)
            return QModelIndex();
        auto *fs = static_cast<QFileSystemModel *>(sourceModel());
        return mapFromSource(fs->index(path, 0));
    }

    // --- QAbstractItemModel (modelo completamente plano) -------------------
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
    {
        if (row < 0 || column < 0 || parent.isValid())
            return QModelIndex();
        if (row >= rowCount())
            return QModelIndex();
        return createIndex(row, column);
    }

    QModelIndex parent(const QModelIndex &) const override { return QModelIndex(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return systemIconCount() + fileRowCount();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 1;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || !sourceModel())
            return QVariant();

        if (isSystemIcon(index)) {
            switch (role) {
            case Qt::DisplayRole:
            case Qt::EditRole:
                return systemName(systemIconId(index));
            case Qt::DecorationRole:
                return systemIcon(systemIconId(index));
            default:
                return QVariant();
            }
        }

        const QModelIndex src = mapToSource(index);
        return src.isValid() ? sourceModel()->data(src, role) : QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid())
            return Qt::NoItemFlags;
        if (isSystemIcon(index))
            // no renombrables ni borrables, pero sí arrastrables para poder
            // recolocarlas libremente por el escritorio
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        const QModelIndex src = mapToSource(index);
        return src.isValid() ? sourceModel()->flags(src)
                             : Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    QModelIndex mapToSource(const QModelIndex &proxyIndex) const override
    {
        if (!proxyIndex.isValid() || isSystemIcon(proxyIndex) || !sourceModel())
            return QModelIndex();
        if (proxyIndex.parent().isValid())
            return QModelIndex();
        const int row = proxyIndex.row() - systemIconCount();
        if (row < 0 || row >= fileRowCount())
            return QModelIndex();
        return sourceModel()->index(row, proxyIndex.column(), m_sourceRoot);
    }

    QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override
    {
        if (!sourceIndex.isValid() || !sourceModel())
            return QModelIndex();
        if (sourceIndex.parent() != m_sourceRoot)
            return QModelIndex();
        return index(sourceIndex.row() + systemIconCount(), sourceIndex.column());
    }

    // --- nombres, iconos y destino de los iconos del sistema ----------------
    static QString systemName(int icon)
    {
        switch (icon) {
        case SystemComputer: return QStringLiteral("Equipo");
        case SystemUser:     return QStringLiteral("Usuario");
        case SystemNetwork:  return QStringLiteral("Red");
        case SystemTrash:    return QStringLiteral("Papelera");
        }
        return QString();
    }

    static QIcon systemIcon(int icon)
    {
        switch (icon) {
        case SystemComputer: return QIcon::fromTheme(QStringLiteral("computer"));
        case SystemUser:     return QIcon::fromTheme(QStringLiteral("user-home"),
                                                     QIcon::fromTheme(QStringLiteral("user")));
        case SystemNetwork:  return QIcon::fromTheme(QStringLiteral("network-workgroup"),
                                                     QIcon::fromTheme(QStringLiteral("network")));
        case SystemTrash:    return QIcon::fromTheme(QStringLiteral("user-trash"),
                                                     QIcon::fromTheme(QStringLiteral("trash")));
        }
        return QIcon();
    }

    // Qué abren al activarlas (ruta local o URL con esquema)
    static QUrl systemTarget(int icon)
    {
        switch (icon) {
        case SystemComputer: return QUrl::fromLocalFile(QDir::rootPath());
        case SystemUser:     return QUrl::fromLocalFile(QDir::homePath());
        case SystemNetwork:  return QUrl(QStringLiteral("network:///"));
        case SystemTrash:
        {
            const QString trashFiles =
                QDir::homePath() + QStringLiteral("/.local/share/Trash/files");
            if (QDir(trashFiles).exists())
                return QUrl::fromLocalFile(trashFiles);
            return QUrl(QStringLiteral("trash:///"));
        }
        }
        return QUrl();
    }

    // --- QAbstractItemModel: datos de arrastre ------------------------------
    // Los archivos se arrastran como "text/uri-list" (así se pueden soltar
    // también en el gestor de archivos del sistema). Los iconos del sistema no
    // exponen URLs: solo se recolocan dentro del propio escritorio.
    QStringList mimeTypes() const override
    {
        return { QStringLiteral("text/uri-list"),
                 QStringLiteral("application/x-stellarbox-desktop") };
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        auto *mime = new QMimeData();
        QList<QUrl> urls;
        if (sourceModel()) {
            auto *fs = static_cast<QFileSystemModel *>(sourceModel());
            for (const QModelIndex &index : indexes) {
                if (!index.isValid() || isSystemIcon(index))
                    continue;   // sin URL: no se pueden soltar fuera
                const QModelIndex src = mapToSource(index);
                const QString path = src.isValid() ? fs->filePath(src) : QString();
                if (!path.isEmpty())
                    urls << QUrl::fromLocalFile(path);
            }
        }
        mime->setUrls(urls);
        // Formato inerte para que el arrastre de iconos del sistema arranque
        if (urls.isEmpty())
            mime->setData(QStringLiteral("application/x-stellarbox-desktop"),
                          QByteArray());
        return mime;
    }

    // --- QAbstractProxyModel: reenvío de cambios del modelo de archivos -----
    void setSourceModel(QAbstractItemModel *sourceModel) override
    {
        QAbstractProxyModel::setSourceModel(sourceModel);
        if (!sourceModel)
            return;

        connect(sourceModel, &QAbstractItemModel::modelReset, this, [this] {
            beginResetModel();
            endResetModel();
        });

        connect(sourceModel, &QAbstractItemModel::rowsAboutToBeInserted, this,
                [this](const QModelIndex &parent, int first, int last) {
                    if (parent == m_sourceRoot)
                        beginInsertRows(QModelIndex(), first + systemIconCount(),
                                        last + systemIconCount());
                });
        connect(sourceModel, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex &parent, int, int) {
                    if (parent == m_sourceRoot)
                        endInsertRows();
                });

        connect(sourceModel, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex &parent, int first, int last) {
                    if (parent == m_sourceRoot)
                        beginRemoveRows(QModelIndex(), first + systemIconCount(),
                                        last + systemIconCount());
                });
        connect(sourceModel, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex &parent, int, int) {
                    if (parent == m_sourceRoot)
                        endRemoveRows();
                });

        connect(sourceModel, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                       const QList<int> &roles) {
                    emit dataChanged(mapFromSource(topLeft), mapFromSource(bottomRight), roles);
                });

        // Recargos de directorio (p. ej. F5): se reexpone todo
        connect(sourceModel, &QAbstractItemModel::layoutAboutToBeChanged, this, [this] {
            beginResetModel();
            endResetModel();
        });
    }

private:
    int fileRowCount() const
    {
        return (sourceModel() && m_sourceRoot.isValid())
            ? sourceModel()->rowCount(m_sourceRoot) : 0;
    }

    QModelIndex m_sourceRoot;
    bool m_enabled[SystemIconCount] = { true, true, true, true };
};

// ============================================================================
//  DesktopView : QWidget — el escritorio en sí
//  ============================================================================
//  Vtable en 0x12620 (secundaria en 0x127d0). Slots registrados en el
//  metaobjeto (despacho moc en 0xf4f0): openItem, openInManager, newFolder,
//  changeWallpaper, restoreWallpaper, showContextMenu.
class DesktopView : public QWidget
{
    Q_OBJECT

public:
    DesktopView(QWidget *parent, Qt::WindowFlags flags,
                const QString &desktopDir,
                const QString &wallpaperFile,
                const QString &bgDir);

    QString desktopDir() const { return m_desktopDir; }

    // Acciones invocadas por los atajos de teclado (y reutilizadas por el
    // menú contextual)
    void openSelection();
    void deleteSelected(bool toTrash);
    void renameSelected();
    void copySelected(bool cut);
    void pasteFromClipboard();
    QString copyIntoDesktop(const QString &source);   // usada también por el drop
    void newDocument();                                // crea un documento de texto vacío
    void newLauncher();                                // crea un lanzador .desktop
    void applySettings(const AppSettings &s);          // aplica la configuración a esta vista
    void openSettings();                               // abre la ventana de Configuración
    void showSystemProperties(int iconId);             // "Propiedades" de un icono del sistema
    void emptyTrashNow();                              // "Vaciar la Papelera"
    ~DesktopView() override;                           // deja el registro de vistas

public slots:
    // 0xc0d0 — abre el elemento seleccionado con la app por defecto (los
    // iconos del sistema abren su destino: raíz, casa, red o papelera)
    void openItem(const QModelIndex &index)
    {
#ifdef STELLARBOX_SNAPSHOT_TEST
        ++m_debugOpenCalls;
#endif
        if (!index.isValid())
            return;

        if (m_proxy && m_proxy->isSystemIcon(index)) {
            const QUrl target = DesktopProxyModel::systemTarget(m_proxy->systemIconId(index));
            if (!target.isEmpty())
                QDesktopServices::openUrl(target);
            return;
        }

        QString path;
        if (m_proxy) {
            const QModelIndex src = m_proxy->mapToSource(index);
            if (src.isValid())
                path = m_model->filePath(src);
        } else {
            path = m_model->filePath(index);
        }
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }

#ifdef STELLARBOX_SNAPSHOT_TEST
    // Accesores de diagnóstico para la verificación sin pantalla
    int systemIconsVisible() const;
    int desktopRowCount() const;
    bool widgetClockVisible() const;
    bool widgetSystemVisible() const;
    bool widgetNetworkVisible() const;
    bool widgetDiskVisible() const;
    QString widgetGeometryDump() const;
    DesktopIconView *debugList() const;
    DesktopProxyModel *debugProxy() const;
    int debugOpenCalls() const { return m_debugOpenCalls; }
    QStringList debugSystemMenuLabels(int iconId) const;
#endif

    // 0xf448/0xf530 — abre el propio escritorio en el gestor de archivos
    void openInManager()
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_desktopDir));
    }

    // 0xcece — crea "Nueva carpeta %1" en el escritorio (definido tras
    // DesktopIconView porque selecciona la carpeta recién creada en la lista)
    void newFolder();

    // 0xeba0/0xed30 — elige un fondo nuevo y lo guarda como el actual
    void changeWallpaper()
    {
        const QString file = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Elegir fondo de pantalla"),
            m_bgDir,
            QStringLiteral("Imagenes (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (file.isEmpty())
            return;

        QDir().mkpath(QFileInfo(file).absolutePath());

        QSaveFile out(m_wallpaperFile);              // guardado atómico
        if (out.open(QIODevice::WriteOnly)) {
            out.write(file.toUtf8());
            out.commit();
        }

        if (!m_watcher.files().contains(m_wallpaperFile))
            m_watcher.addPath(m_wallpaperFile);

        loadWallpaper();
    }

    // 0xf3a8/0xf610 — borra el fondo guardado y vuelve al por defecto
    void restoreWallpaper()
    {
        QFile::remove(m_wallpaperFile);
        loadWallpaper();
    }

    // 0xf050 — menú contextual de la lista
    // (definido tras DesktopIconView: usa m_list, que aquí aún es incompleto)
    void showContextMenu(const QPoint &pos);

protected:
    // 0xc520 — pinta el fondo almacenado
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.drawPixmap(QPointF(0, 0), m_wallpaper);
    }

    // Mantiene los widgets del escritorio anclados arriba a la derecha
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        relayoutWidgets();
    }

private:
    QStringList selectedPaths() const;   // rutas de la selección actual
    void selectPath(const QString &path);   // selecciona un elemento del modelo
    void refresh();                      // F5 — re-escanea el directorio
    void ensureWidgets();                // crea los widgets del escritorio (ocultos)
    void relayoutWidgets();              // recoloca los widgets visibles
    QList<QAction *> systemIconMenuActions(QMenu &menu, int iconId) const;

    // 0xdf60 — carga el fondo actual (guardado → instalado → gradiente)
    void loadWallpaper()
    {
        QPixmap pix;

        // 1) fondo seleccionado por el usuario (~/.config/stellarbox/wallpaper)
        QFile f(m_wallpaperFile);
        if (f.exists()) {
            if (f.open(QIODevice::ReadOnly)) {
                const QString path = QString::fromUtf8(f.readAll()).trimmed();
                if (!path.isEmpty() && QFile::exists(path))
                    pix.load(path);
            }
        }

        // 2) fondo por defecto de la instalación ("default_background*")
        if (pix.isNull()) {
            QDir dir(m_bgDir);
            const QStringList names = dir.entryList(
                {QStringLiteral("default_background*")},
                QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                QDir::Name);
            for (const QString &name : names) {
                if (pix.load(dir.absoluteFilePath(name)))
                    break;
            }
        }

        // 3) gradiente oscuro por defecto
        if (pix.isNull())
            pix = makeDefaultBackground();

        m_wallpaper = pix.scaled(
            QGuiApplication::primaryScreen()->geometry().size(),
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);  // 0xe867
        update();
    }

    // 0xe2b2 — gradiente #3B3B3B → #171717 sobre el tamaño de pantalla
    QPixmap makeDefaultBackground() const
    {
        const QSize size = QGuiApplication::primaryScreen()->geometry().size();
        QPixmap pix(size);
        pix.fill(Qt::black);                                   /* QColor(2) */
        QPainter p(&pix);
        QLinearGradient gradient(0, 0, size.width(), size.height());
        gradient.setColorAt(0.0, QColor(0x3b, 0x3b, 0x3b));    /* 0x109f0 */
        gradient.setColorAt(1.0, QColor(0x17, 0x17, 0x17));    /* 0x109f8 */
        p.fillRect(pix.rect(), QBrush(gradient));
        p.end();
        return pix;
    }

    QString m_desktopDir;      /* +0x28 */
    QString m_wallpaperFile;   /* +0x40 */
    QString m_bgDir;           /* +0x58 */

    QPixmap m_wallpaper;       /* +0x70 */

    QFileSystemModel *m_model = nullptr;   /* +0x88 */
    DesktopIconView *m_list   = nullptr;   /* +0x90 */
    DesktopProxyModel *m_proxy = nullptr;  // iconos del sistema + archivos
    QFileSystemWatcher  m_watcher;

    // Widgets del escritorio (reloj, sistema, red, discos)
    ClockWidget   *m_widgetClock   = nullptr;
    SystemWidget  *m_widgetSystem  = nullptr;
    NetworkWidget *m_widgetNetwork = nullptr;
    DiskWidget    *m_widgetDisk    = nullptr;
    bool m_widgetsReady = false;

#ifdef STELLARBOX_SNAPSHOT_TEST
    int m_debugOpenCalls = 0;   // nº de llamadas a openItem (diagnóstico)
#endif
};

// ============================================================================
//  DesktopIconView : QAbstractItemView — lienzo de iconos del escritorio
//  ============================================================================
//  Sustituye al QListView original (vtable 0x12290). Es un lienzo libre:
//    · apila los iconos en una columna VERTICAL por defecto (flujo
//      top-to-bottom en la esquina superior izquierda);
//    · permite ARRASTRAR los iconos a cualquier celda del escritorio;
//    · recuerda dónde se dejaron (grupo [IconPositions/<pantalla>] de
//      ~/.config/stellarbox/settings.ini);
//    · el DOBLE CLIC abre el elemento con la aplicación / gestor de
//      archivos por defecto del sistema (QDesktopServices::openUrl).
//  Se define tras DesktopView porque usa la vista padre (openItem,
//  copyIntoDesktop).
class DesktopIconView : public QAbstractItemView
{
public:
    explicit DesktopIconView(QWidget *parent)
        : QAbstractItemView(parent)
    {
        setObjectName(QStringLiteral("desktopList"));          /* 0x100ac */
        setSelectionMode(QAbstractItemView::ExtendedSelection); /* 0x3 */
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setDragDropMode(QAbstractItemView::InternalMove);
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);
        setMouseTracking(true);          // para el resaltado al pasar el ratón
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setContextMenuPolicy(Qt::CustomContextMenu);           /* 0x3 */
        setStyleSheet(QStringLiteral(R"(
            QAbstractItemView#desktopList {
                background: transparent;
                border: none;
            }
        )"));
        QPalette transparentPal = viewport()->palette();
        transparentPal.setColor(QPalette::Base, Qt::transparent);
        viewport()->setPalette(transparentPal);
        viewport()->setAutoFillBackground(false);

        loadPositions();   // posiciones recordadas en la última sesión
    }

    void setIconSize(const QSize &size)
    {
        m_iconSize = size;
        viewport()->update();
    }

    void setGridSize(const QSize &size)
    {
        m_gridSize = size;
        viewport()->update();
    }

    // Recarga las posiciones guardadas (p. ej. tras un restablecimiento)
    void reloadPositions()
    {
        loadPositions();
        viewport()->update();
    }

    // Guarda en settings.ini la posición actual de TODOS los iconos (los
    // arrastrados y los que quedaron en el flujo vertical por defecto): el
    // escritorio recuerda siempre cómo quedó.
    void savePositions()
    {
        if (!model())
            return;
        const int rows = model()->rowCount(rootIndex());
        QSettings settings(configFilePath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("IconPositions"));
        settings.beginGroup(screenGroup());
        for (int r = 0; r < rows; ++r) {
            const QModelIndex idx = model()->index(r, 0, rootIndex());
            if (!idx.isValid())
                continue;
            const QPoint p = iconRect(r).topLeft();
            settings.setValue(identityKey(r),
                              QStringLiteral("%1,%2").arg(p.x()).arg(p.y()));
        }
        settings.endGroup();
        settings.endGroup();
    }

#ifdef STELLARBOX_SNAPSHOT_TEST
    // Simula el soltado interno de un arrastre (diagnóstico sin pantalla)
    void debugSimulateInternalDrop(const QModelIndexList &indexes,
                                   const QPoint &dropPos)
    {
        applyInternalDrop(indexes, dropPos);
    }

    // Quita las posiciones recordadas para ver el flujo vertical por defecto
    void debugClearPositions()
    {
        m_savedPositions.clear();
        viewport()->update();
    }
#endif

protected:
    // --- geometría ---------------------------------------------------------
    // Celda del icono de la fila `row`: la posición guardada por el usuario
    // o, si no la hay, la siguiente celda libre del flujo vertical (columna
    // arriba a la izquierda, saltando a la siguiente columna al llenarse la
    // pantalla).
    QRect iconRect(int row) const
    {
        const QSize grid = m_gridSize.isValid() ? m_gridSize : QSize(112, 96);
        const int viewW = viewport() ? viewport()->width() : width();
        const int viewH = viewport() ? viewport()->height() : height();
        const int perCol = qMax(1, (viewH - 2 * m_margin) / qMax(1, grid.height()));

        int col    = row / perCol;
        int within = row % perCol;
        QPoint topLeft(m_margin + col * grid.width(),
                       m_margin + within * grid.height());

        const QString key = identityKey(row);
        if (!key.isEmpty() && m_savedPositions.contains(key)) {
            topLeft = m_savedPositions.value(key);
            // mantiene el icono dentro de los márgenes de la vista
            topLeft.setX(qBound(m_margin, topLeft.x(),
                                qMax(m_margin, viewW - grid.width() - m_margin)));
            topLeft.setY(qBound(m_margin, topLeft.y(),
                                qMax(m_margin, viewH - grid.height() - m_margin)));
        }
        return QRect(topLeft, grid);
    }

    // Clave estable de una fila: "sys:N" para los iconos del sistema y la ruta
    // absoluta para los archivos (así siguen en su sitio aunque cambie el
    // orden de la lista).
    QString identityKey(int row) const
    {
        // DesktopProxyModel no lleva Q_OBJECT: no se puede qobject_cast, pero
        // el modelo de esta vista siempre es el proxy
        auto *pm = static_cast<DesktopProxyModel *>(model());
        const QModelIndex idx = model() ? model()->index(row, 0, rootIndex())
                                        : QModelIndex();
        if (!idx.isValid())
            return QString();
        if (pm && pm->isSystemIcon(idx))
            return QStringLiteral("sys:%1").arg(pm->systemIconId(idx));
        if (pm) {
            const QModelIndex src = pm->mapToSource(idx);
            if (auto *fs = qobject_cast<QFileSystemModel *>(pm->sourceModel()))
                return encodeKey(src.isValid() ? fs->filePath(src) : QString());
        }
        return QString::number(row);
    }

    // QSettings interpreta '/' como subgrupo: las rutas de archivo se
    // codifican para que la posición quede en una sola clave plana
    static QString encodeKey(const QString &raw)
    {
        QString key = raw;
        key.replace(QLatin1Char('%'), QStringLiteral("%25"));
        key.replace(QLatin1Char('/'), QStringLiteral("%2F"));
        return key;
    }

    // --- QAbstractItemView: API pública (igual que en QListView) ------------
public:
    QModelIndex indexAt(const QPoint &point) const override
    {
        if (!model())
            return QModelIndex();
        const int rows = model()->rowCount(rootIndex());
        for (int r = 0; r < rows; ++r)
            if (iconRect(r).contains(point))
                return model()->index(r, 0, rootIndex());
        return QModelIndex();
    }

    QRect visualRect(const QModelIndex &index) const override
    {
        if (!index.isValid() || index.column() != 0 || index.parent() != rootIndex())
            return QRect();
        if (!model() || index.row() < 0 || index.row() >= model()->rowCount(rootIndex()))
            return QRect();
        return iconRect(index.row());
    }

    void scrollTo(const QModelIndex &, ScrollHint) override { }   // sin scroll

protected:
    bool isIndexHidden(const QModelIndex &) const override { return false; }

    QRegion visualRegionForSelection(const QItemSelection &selection) const override
    {
        QRegion region;
        const QModelIndexList indexes = selection.indexes();
        for (const QModelIndex &index : indexes)
            region += visualRect(index);
        return region;
    }

    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers) override
    {
        const int rows = model() ? model()->rowCount(rootIndex()) : 0;
        if (rows <= 0)
            return QModelIndex();
        int r = rows - 1;
        const QModelIndex cur = currentIndex();
        if (cur.isValid() && cur.row() >= 0 && cur.row() < rows)
            r = cur.row();
        const int perCol = qMax(1, (qMax(1, viewport()->height()) - 2 * m_margin)
                                       / qMax(1, m_gridSize.height()));
        switch (action) {
        case MoveUp:          r = qMax(0, r - 1);               break;
        case MoveDown:        r = qMin(rows - 1, r + 1);        break;
        case MoveLeft:        r = qMax(0, r - perCol);          break;
        case MoveRight:       r = qMin(rows - 1, r + perCol);   break;
        case MoveHome:        r = 0;                            break;
        case MoveEnd:         r = rows - 1;                     break;
        case MovePageUp:      r = qMax(0, r - perCol);          break;
        case MovePageDown:    r = qMin(rows - 1, r + perCol);   break;
        case MoveNext:        r = qMin(rows - 1, r + 1);        break;
        case MovePrevious:    r = qMax(0, r - 1);               break;
        }
        return model()->index(r, 0, rootIndex());
    }

    void setSelection(const QRect &rect,
                      QItemSelectionModel::SelectionFlags flags) override
    {
        if (!model())
            return;
        const int rows = model()->rowCount(rootIndex());
        for (int r = 0; r < rows; ++r)
            if (rect.intersects(iconRect(r)))
                selectionModel()->select(model()->index(r, 0, rootIndex()), flags);
    }

    int verticalOffset() const override   { return 0; }
    int horizontalOffset() const override { return 0; }

    // --- pintado -----------------------------------------------------------
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing, true);

        if (!model())
            return;
        const int rows = model()->rowCount(rootIndex());
        for (int r = 0; r < rows; ++r) {
            const QModelIndex idx = model()->index(r, 0, rootIndex());
            if (!idx.isValid())
                continue;
            const QRect cell = iconRect(r);
            const bool selected = selectionModel() && selectionModel()->isSelected(idx);
            const bool current  = selectionModel() && idx == currentIndex();
            const bool hover    = idx == m_hoverIndex;

            if (selected || current || hover) {
                QColor bg;
                if (selected)
                    bg = QColor(147, 186, 132, 115);
                else if (current)
                    bg = QColor(255, 255, 255, 40);
                else
                    bg = QColor(255, 255, 255, 31);
                p.setPen(Qt::NoPen);
                p.setBrush(bg);
                p.drawRoundedRect(cell.adjusted(0, 0, -1, -1), 8, 8);
            }

            const QIcon icon = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole));
            if (!icon.isNull()) {
                const QRect iconArea(
                    cell.center().x() - m_iconSize.width() / 2,
                    cell.y() + 6,
                    m_iconSize.width(), m_iconSize.height());
                icon.paint(&p, iconArea, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
            }

            const QString text = idx.data(Qt::DisplayRole).toString();
            if (!text.isEmpty()) {
                const QRect textArea = cell.adjusted(3, m_iconSize.height() + 8, -3, -2);
                QFont f = p.font();
                f.setPixelSize(12);
                f.setWeight(QFont::Normal);
                p.setFont(f);
                p.setPen(QColor(255, 255, 255));
                const QFontMetrics fm(f);
                QString shown = text;
                if (fm.horizontalAdvance(shown) > textArea.width() * 2)
                    shown = fm.elidedText(shown, Qt::ElideMiddle, textArea.width() * 2);
                p.drawText(textArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                           shown);
            }
        }

        // recuadro de selección por rectángulo
        if (!m_rubberBand.isNull()) {
            p.setPen(QPen(QColor(255, 255, 255, 210), 1, Qt::DashLine));
            p.setBrush(QColor(255, 255, 255, 24));
            p.drawRect(m_rubberBand);
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QAbstractItemView::resizeEvent(event);
        viewport()->update();   // el flujo vertical depende de la altura
    }

    // --- ratón -------------------------------------------------------------
    // El manejo es propio (sin pasar por QAbstractItemView): así un icono se
    // arrastra directamente al superar el umbral, en el primer intento,
    // como en un escritorio real, sin depender ni chocar con el arrastre
    // interno de la clase base.
    void mousePressEvent(QMouseEvent *event) override
    {
        const QModelIndex idx = indexAt(event->pos());
        m_pressPos   = event->pos();
        m_pressIndex = idx;
        m_dragging   = false;
        m_rubberBand = QRect();

        if (event->button() != Qt::LeftButton || !selectionModel())
            return;

        if (idx.isValid()) {
            // Semántica ExtendedSelection: Ctrl alterna, Shift extiende
            if (event->modifiers() & Qt::ControlModifier) {
                selectionModel()->select(idx, QItemSelectionModel::Toggle);
                selectionModel()->setCurrentIndex(idx,
                    QItemSelectionModel::NoUpdate);
            } else if (event->modifiers() & Qt::ShiftModifier) {
                selectionModel()->setCurrentIndex(idx,
                    QItemSelectionModel::Select);
            } else {
                selectionModel()->setCurrentIndex(idx,
                    QItemSelectionModel::ClearAndSelect);
            }
        } else {
            // clic sobre el fondo: comienza una selección por rectángulo
            selectionModel()->clearSelection();
            selectionModel()->setCurrentIndex(QModelIndex(),
                                              QItemSelectionModel::Clear);
            m_rubberBand = QRect(m_pressPos, QSize(0, 0));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // resaltado al pasar el ratón (sin botón pulsado)
        if (event->buttons() == Qt::NoButton) {
            const QModelIndex h = indexAt(event->pos());
            if (h != m_hoverIndex) {
                const QRect oldR = m_hoverIndex.isValid()
                    ? iconRect(m_hoverIndex.row()) : QRect();
                m_hoverIndex = h;
                const QRect newR = h.isValid() ? iconRect(h.row()) : QRect();
                if (oldR.isValid()) viewport()->update(oldR);
                if (newR.isValid()) viewport()->update(newR);
            }
            return;
        }

        if (m_dragging)
            return;   // tras completar un arrastre, esperando al release

        if (event->buttons() & Qt::LeftButton) {
            const int moved = (event->pos() - m_pressPos).manhattanLength();

            // 1) recoger el/los iconos pulsados y arrastrarlos
            if (m_pressIndex.isValid()
                && (m_pressIndex.flags() & Qt::ItemIsDragEnabled)
                && moved >= QApplication::startDragDistance()) {
                m_dragging = true;
                startDrag(model()->supportedDragActions());
                m_dragging = false;
                return;
            }

            // 2) selección por rectángulo desde el fondo
            if (!m_pressIndex.isValid() && moved >= 4
                && !m_rubberBand.isNull()) {
                const QRect band = QRect(m_rubberBand.topLeft(),
                                         event->pos()).normalized();
                if (band != m_rubberBand) {
                    m_rubberBand = band;
                    setSelection(band, QItemSelectionModel::ClearAndSelect);
                    viewport()->update();
                }
                return;
            }
        }
        // Nada más: evitamos el arrastre y la selección automáticos de la base
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_dragging = false;
        if (event->button() == Qt::LeftButton && !m_rubberBand.isNull()) {
            // la selección por rectángulo ya quedó aplicada; se limpia el marco
            m_rubberBand = QRect();
            viewport()->update();
        }
        m_pressIndex = QModelIndex();
    }

    void leaveEvent(QEvent *event) override
    {
        if (m_hoverIndex.isValid()) {
            const QRect oldR = iconRect(m_hoverIndex.row());
            m_hoverIndex = QModelIndex();
            viewport()->update(oldR);
        }
        QAbstractItemView::leaveEvent(event);
    }

    // Doble clic → abre con la aplicación / gestor de archivos por defecto
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid()) {
            if (auto *desktop = qobject_cast<DesktopView *>(parent()))
                desktop->openItem(idx);
            return;
        }
        QAbstractItemView::mouseDoubleClickEvent(event);
    }

    // Registra la selección arrastrada para recolocarla al soltar
    void startDrag(Qt::DropActions supportedActions) override
    {
        m_dragIndexes = selectionModel() ? selectionModel()->selectedIndexes()
                                         : QModelIndexList();
        QAbstractItemView::startDrag(supportedActions);
        m_dragIndexes.clear();
    }

    // --- arrastrar-y-soltar ------------------------------------------------
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->source() == this || event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->source() == this || event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        // 1) arrastre interno: recoloca los iconos donde se soltaron
        if (event->source() == this) {
            if (!m_dragIndexes.isEmpty()) {
                applyInternalDrop(m_dragIndexes, event->position().toPoint());
                event->setDropAction(Qt::MoveAction);
                event->accept();
                return;
            }
            event->ignore();
            return;
        }

        // 2) soltar archivos externos: se copian al escritorio con nombre único
        auto *desktop = qobject_cast<DesktopView *>(parent());
        if (desktop && event->mimeData()->hasUrls()) {
            const QList<QUrl> urls = event->mimeData()->urls();
            for (const QUrl &url : urls) {
                if (url.isLocalFile())
                    desktop->copyIntoDesktop(url.toLocalFile());   // QFile::copy en 0xc5d0
            }
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

private:
    void loadPositions()
    {
        m_savedPositions.clear();
        QSettings settings(configFilePath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("IconPositions"));
        settings.beginGroup(screenGroup());
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            const QStringList xy = settings.value(key).toString().split(QLatin1Char(','));
            if (xy.size() != 2)
                continue;
            bool okX = false, okY = false;
            const int x = xy[0].toInt(&okX);
            const int y = xy[1].toInt(&okY);
            if (okX && okY)
                m_savedPositions[key] = QPoint(x, y);
        }
        settings.endGroup();
        settings.endGroup();
    }

    // Recoloca los iconos arrastrados a partir de la celda de destino y
    // guarda inmediatamente la configuración resultante
    void applyInternalDrop(const QModelIndexList &indexes, const QPoint &dropPos)
    {
        if (indexes.isEmpty())
            return;
        const int x = snap(dropPos.x(), m_gridSize.width());
        int y = snap(dropPos.y(), m_gridSize.height());
        for (const QModelIndex &idx : indexes) {
            if (!idx.isValid() || !model() || idx.model() != model())
                continue;
            m_savedPositions[identityKey(idx.row())] = QPoint(x, y);
            y += qMax(1, m_gridSize.height());
        }
        savePositions();
        viewport()->update();
    }

    // Ajusta una coordenada a la rejilla de celdas (anclada al margen)
    int snap(int value, int cell) const
    {
        return qMax(0, cell > 0
            ? qRound(double(value - m_margin) / cell) * cell + m_margin
            : value);
    }

    // Nombre de la pantalla: cada monitor guarda sus posiciones por separado
    QString screenGroup() const
    {
        const QScreen *s = screen();
        if (!s)
            s = QGuiApplication::primaryScreen();
        const QString name = s ? s->name() : QString();
        return name.isEmpty() ? QStringLiteral("screen") : name;
    }

    QSize m_iconSize = QSize(48, 48);                    /* 0x10a10 */
    QSize m_gridSize = QSize(112, 96);                   /* 0x10a18 */
    int   m_margin   = 18;                               /* 0x10a20 */

    QHash<QString, QPoint> m_savedPositions;             // posiciones recordadas
    QModelIndexList m_dragIndexes;                       // selección en arrastre
    QModelIndex m_hoverIndex;                            // fila bajo el cursor
    QModelIndex m_pressIndex;                            // fila pulsada (arrastre)
    QPoint m_pressPos;
    bool m_dragging = false;                             // arrastre en curso
    QRect m_rubberBand;                                  // selección por rectángulo
};

// ============================================================================
//  DesktopView::DesktopView — cuerpo del constructor (definido aquí, donde
//  DesktopIconView ya es un tipo completo)
//  ============================================================================
DesktopView::DesktopView(QWidget *parent, Qt::WindowFlags flags,
                         const QString &desktopDir,
                         const QString &wallpaperFile,
                         const QString &bgDir)
    : QWidget(parent, flags)
    , m_desktopDir(desktopDir)
    , m_wallpaperFile(wallpaperFile)
    , m_bgDir(bgDir)
{
    // atributos 9 y 5 del binario (0xa389): pintar sin fondo de sistema
    // y conservar el contenido estático
    setAttribute(Qt::WA_NoSystemBackground);   /* atributo 9 */
    setAttribute(Qt::WA_StaticContents);       /* atributo 5 */

    // --- modelo de archivos del escritorio -----------------------------
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::Files | QDir::AllDirs | QDir::NoDotAndDotDot); /* 0x6402 */
    m_model->setRootPath(m_desktopDir);

    // --- lienzo de iconos -----------------------------------------------
    // Se muestra a través de DesktopProxyModel: los iconos del sistema
    // (Equipo, Usuario, Red, Papelera) por delante de los archivos. El lienzo
    // (DesktopIconView) apila los iconos en vertical por defecto, permite
    // arrastrarlos libremente y recuerda dónde se dejaron.
    m_proxy = new DesktopProxyModel(this);
    m_proxy->setSourceModel(m_model);

    m_list = new DesktopIconView(this);
    m_list->setIconSize(QSize(48, 48));                     /* 0x10a10 */
    m_list->setGridSize(QSize(112, 96));                    /* 0x10a18 */
    m_list->setModel(m_proxy);
    m_proxy->setSourceRoot(m_model->index(m_desktopDir, 0));

    connect(m_list, &QWidget::customContextMenuRequested,
            this, &DesktopView::showContextMenu);

    // La lista llena todo el view (sin márgenes)
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_list);

    // Recargamos el fondo si alguien reescribe el archivo de configuración
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                if (path == m_wallpaperFile)
                    loadWallpaper();
            });

    // --- atajos de teclado estándar de escritorio -------------------------
    // Contexto WidgetShortcut: están activos mientras la lista tiene el foco.
    const auto addShortcut = [this](const QKeySequence &seq, std::function<void()> fn) {
        auto *shortcut = new QShortcut(seq, m_list, nullptr, nullptr, Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this, std::move(fn));
    };

    addShortcut(QKeySequence(Qt::Key_F2),                       [this] { renameSelected(); });
    addShortcut(QKeySequence(Qt::Key_Delete),                   [this] { deleteSelected(true); });
    addShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete),       [this] { deleteSelected(false); });
    addShortcut(QKeySequence(Qt::Key_Return),                   [this] { openSelection(); });
    addShortcut(QKeySequence(Qt::Key_Enter),                    [this] { openSelection(); });
    addShortcut(QKeySequence(Qt::CTRL | Qt::Key_A),             [this] { m_list->selectAll(); });
    addShortcut(QKeySequence(QKeySequence::Copy),               [this] { copySelected(false); });
    addShortcut(QKeySequence(QKeySequence::Cut),                [this] { copySelected(true); });
    addShortcut(QKeySequence(QKeySequence::Paste),              [this] { pasteFromClipboard(); });
    addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] { newFolder(); });
    addShortcut(QKeySequence(Qt::Key_F5),                       [this] { refresh(); });

    // Se registra para que la ventana de Configuración le aplique los cambios,
    // y se aplica la configuración persistente al crear la vista (tamaño de
    // iconos, rejilla, directorios y fondo de pantalla)
    allDesktopViews().append(this);
    applySettings(globalSettings());
}

// Construye las acciones del menú contextual de un icono del sistema:
// "Abrir" y "Propiedades" para todos; la Papelera añade además
// "Vaciar la Papelera" (separada por un divisor, tras la acción 2).
QList<QAction *> DesktopView::systemIconMenuActions(QMenu &menu, int iconId) const
{
    QList<QAction *> actions;
    actions << menu.addAction(QStringLiteral("Abrir"))
            << menu.addAction(QStringLiteral("Propiedades"));
    if (iconId == DesktopProxyModel::SystemTrash) {
        menu.addSeparator();
        actions << menu.addAction(QStringLiteral("Vaciar la Papelera"));
    }
    return actions;
}

#ifdef STELLARBOX_SNAPSHOT_TEST
QStringList DesktopView::debugSystemMenuLabels(int iconId) const
{
    QMenu menu;
    const QList<QAction *> actions = systemIconMenuActions(menu, iconId);
    QStringList labels;
    for (const QAction *action : actions)
        labels << action->text();
    return labels;
}
#endif

// 0xf050 — menú contextual de la lista (definido aquí, con DesktopIconView completo)
void DesktopView::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    const QModelIndex index = m_list->indexAt(pos);
    if (index.isValid()) {
        // El elemento sobre el que se pincha pasa a ser la selección (así el
        // teclado — Supr, F2, Ctrl+C... — opera sobre él de inmediato)
        if (!m_list->selectionModel()->isSelected(index)) {
            m_list->clearSelection();
            m_list->selectionModel()->select(index,
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            m_list->setCurrentIndex(index);
        }

        // Iconos del sistema: se pueden Abrir, ver sus Propiedades y, en el
        // caso de la Papelera, vaciarla (no se renombran, copian ni borran)
        if (m_proxy->isSystemIcon(index)) {
            const int iconId = m_proxy->systemIconId(index);
            const QList<QAction *> actions = systemIconMenuActions(menu, iconId);

            QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
            if (chosen == actions.value(0))
                openItem(index);
            else if (chosen == actions.value(1))
                showSystemProperties(iconId);
            else if (chosen == actions.value(2))
                emptyTrashNow();
            return;
        }

        QAction *open      = menu.addAction(QStringLiteral("Abrir"));
        QAction *rename    = menu.addAction(QStringLiteral("Renombrar"));
        menu.addSeparator();
        QAction *copy      = menu.addAction(QStringLiteral("Copiar"));
        QAction *cut       = menu.addAction(QStringLiteral("Cortar"));
        menu.addSeparator();
        QAction *trash     = menu.addAction(QStringLiteral("Mover a la papelera"));
        QAction *permanent = menu.addAction(QStringLiteral("Borrar definitivamente"));

        QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (chosen == open)
            openItem(index);
        else if (chosen == rename)
            renameSelected();
        else if (chosen == copy)
            copySelected(false);
        else if (chosen == cut)
            copySelected(true);
        else if (chosen == trash) {
            const QModelIndex src = m_proxy->mapToSource(index);
            if (src.isValid())
                QFile::moveToTrash(m_model->filePath(src));
        } else if (chosen == permanent) {
            deleteSelected(false);
        }
    } else {
        // menú sobre el fondo
        QAction *create    = menu.addAction(QStringLiteral("Nueva carpeta"));
        QAction *document  = menu.addAction(QStringLiteral("Nuevo documento"));
        QAction *launcher  = menu.addAction(QStringLiteral("Nuevo lanzador"));
        QAction *paste     = menu.addAction(QStringLiteral("Pegar"));
        paste->setEnabled(QGuiApplication::clipboard()->mimeData()
                          && QGuiApplication::clipboard()->mimeData()->hasUrls());
        menu.addSeparator();
        QAction *manage = menu.addAction(QStringLiteral("Abrir con gestor de archivos"));
        menu.addSeparator();
        QAction *wallpaper = menu.addAction(QStringLiteral("Cambiar fondo de pantalla..."));
        QAction *restore = menu.addAction(QStringLiteral("Restaurar fondo por defecto"));
        menu.addSeparator();
        QAction *config = menu.addAction(QStringLiteral("Configuración..."));

        QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (chosen == create)
            newFolder();
        else if (chosen == document)
            newDocument();
        else if (chosen == launcher)
            newLauncher();
        else if (chosen == paste)
            pasteFromClipboard();
        else if (chosen == manage)
            openInManager();
        else if (chosen == wallpaper)
            changeWallpaper();
        else if (chosen == restore)
            restoreWallpaper();
        else if (chosen == config)
            openSettings();
    }
}

// ============================================================================
//  Propiedades y Papelera de los iconos del sistema
//  ============================================================================

// 0xf050+ — diálogo de propiedades de un icono del sistema (Equipo, Usuario,
// Red o Papelera): muestra su tipo, destino y algún detalle adicional
// (sistema de archivos, elementos de la papelera...).
void DesktopView::showSystemProperties(int iconId)
{
    const QString name   = DesktopProxyModel::systemName(iconId);
    const QUrl target    = DesktopProxyModel::systemTarget(iconId);

    auto formatBytes = [](quint64 bytes) -> QString {
        if (bytes >= 1024ULL * 1024 * 1024)
            return QStringLiteral("%1 GiB")
                .arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
        if (bytes >= 1024ULL * 1024)
            return QStringLiteral("%1 MiB")
                .arg(double(bytes) / (1024.0 * 1024), 0, 'f', 0);
        return QStringLiteral("%1 KiB")
            .arg(double(bytes) / 1024.0, 0, 'f', 0);
    };

    QString type, extra;
    switch (iconId) {
    case DesktopProxyModel::SystemComputer: {
        type = QStringLiteral("Equipo");
        const QStorageInfo root(QDir::rootPath());
        if (root.isValid())
            extra = QStringLiteral("Sistema de archivos %1 · %2 libres de %3")
                        .arg(root.fileSystemType(),
                             formatBytes(root.bytesAvailable()),
                             formatBytes(root.bytesTotal()));
        break;
    }
    case DesktopProxyModel::SystemUser:
        type  = QStringLiteral("Carpeta personal");
        extra = QDir::homePath();
        break;
    case DesktopProxyModel::SystemNetwork:
        type = QStringLiteral("Red del sistema");
        break;
    case DesktopProxyModel::SystemTrash: {
        type = QStringLiteral("Papelera");
        const QDir d(QDir::homePath() + QStringLiteral("/.local/share/Trash/files"));
        const int n = d.exists()
            ? d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size() : 0;
        extra = (n == 1) ? QStringLiteral("1 elemento")
                         : QStringLiteral("%1 elementos").arg(n);
        break;
    }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Propiedades de %1").arg(name));
    dlg.setModal(true);

    auto *v = new QVBoxLayout(&dlg);

    auto *head = new QHBoxLayout;
    auto *iconLabel = new QLabel;
    iconLabel->setPixmap(DesktopProxyModel::systemIcon(iconId).pixmap(48, 48));
    head->addWidget(iconLabel);
    head->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(name)), 1);
    v->addLayout(head);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Tipo:"), new QLabel(type));
    form->addRow(QStringLiteral("Destino:"),
                 new QLabel(target.isValid() ? target.toDisplayString()
                                             : QString()));
    if (!extra.isEmpty())
        form->addRow(QStringLiteral("Detalles:"), new QLabel(extra));
    v->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    v->addWidget(buttons);

    dlg.exec();
}

// "Vaciar la Papelera": elimina definitivamente el contenido de la papelera
// del usuario (~/.local/share/Trash), con confirmación previa.
void DesktopView::emptyTrashNow()
{
    const QString trashDir = QDir::homePath() + QStringLiteral("/.local/share/Trash");
    const QString filesDir = trashDir + QStringLiteral("/files");
    const QString infoDir  = trashDir + QStringLiteral("/info");

    const QDir files(filesDir);
    const QStringList entries = files.exists()
        ? files.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
        : QStringList();
    if (entries.isEmpty())
        return;   // papelera vacía: nada que hacer

    const auto answer = QMessageBox::warning(
        this,
        QStringLiteral("Vaciar la Papelera"),
        entries.size() == 1
            ? QStringLiteral("¿Seguro que deseas eliminar permanentemente 1 elemento de la papelera?")
            : QStringLiteral("¿Seguro que deseas eliminar permanentemente %1 elementos de la papelera?")
                  .arg(entries.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    bool ok = true;
    for (const QString &entry : entries) {
        const QString path = filesDir + QLatin1Char('/') + entry;
        const QFileInfo fi(path);
        if (!(fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path)))
            ok = false;
    }

    // Limpia también los .trashinfo asociados
    const QDir info(infoDir);
    if (info.exists()) {
        const QStringList infos = info.entryList(
            {QStringLiteral("*.trashinfo")}, QDir::Files);
        for (const QString &entry : infos)
            QFile::remove(infoDir + QLatin1Char('/') + entry);
    }

    if (!ok)
        QMessageBox::warning(this, QStringLiteral("Vaciar la Papelera"),
                             QStringLiteral("No se pudieron eliminar todos los elementos."));
}

// ============================================================================
//  Atajos de teclado, portapapeles y acciones del escritorio
//  ============================================================================

QStringList DesktopView::selectedPaths() const
{
    QStringList paths;
    const QModelIndexList indexes = m_list->selectionModel()->selectedIndexes();
    paths.reserve(indexes.size());
    for (const QModelIndex &index : indexes) {
        if (m_proxy->isSystemIcon(index))
            continue;   // los iconos del sistema no son archivos
        const QModelIndex src = m_proxy->mapToSource(index);
        const QString path = src.isValid() ? m_model->filePath(src) : QString();
        if (!path.isEmpty())
            paths << path;
    }
    return paths;
}

void DesktopView::openSelection()
{
    const QModelIndexList indexes = m_list->selectionModel()->selectedIndexes();
    for (const QModelIndex &index : indexes)
        openItem(index);
}

// Supr → papelera · Shift+Supr → borrado definitivo (con confirmación)
void DesktopView::deleteSelected(bool toTrash)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    if (!toTrash) {
        const auto answer = QMessageBox::warning(
            this,
            QStringLiteral("Eliminar permanentemente"),
            QStringLiteral("¿Seguro que deseas eliminar definitivamente %1 elemento(s)?")
                .arg(paths.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (toTrash) {
            QFile::moveToTrash(path);
        } else if (info.isDir()) {
            QDir(path).removeRecursively();
        } else {
            QFile::remove(path);
        }
    }
}

// F2 — renombra el elemento seleccionado (con diálogo, ya que
// QFileSystemModel no soporta edición inline)
void DesktopView::renameSelected()
{
    const QModelIndexList indexes = m_list->selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;

    const QModelIndex first = indexes.first();
    if (m_proxy->isSystemIcon(first))
        return;

    const QModelIndex src = m_proxy->mapToSource(first);
    const QString path = src.isValid() ? m_model->filePath(src) : QString();
    if (path.isEmpty())
        return;
    const QFileInfo info(path);

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, QStringLiteral("Renombrar"), QStringLiteral("Nuevo nombre:"),
        QLineEdit::Normal, info.fileName(), &ok);
    if (!ok || newName.isEmpty() || newName == info.fileName())
        return;

    const QString newPath = QDir(info.absolutePath()).filePath(newName);
    if (QFile::exists(newPath)) {
        QMessageBox::warning(this, QStringLiteral("Renombrar"),
                             QStringLiteral("Ya existe un elemento con ese nombre."));
        return;
    }

    const bool renamed = info.isDir()
        ? QDir().rename(path, newPath)
        : QFile::rename(path, newPath);
    if (!renamed)
        QMessageBox::warning(this, QStringLiteral("Renombrar"),
                             QStringLiteral("No se pudo renombrar el elemento."));
}

// Ctrl+C / Ctrl+X — copia o corta la selección al portapapeles
void DesktopView::copySelected(bool cut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const QString &path : paths)
        urls << QUrl::fromLocalFile(path);

    auto *mime = new QMimeData();
    mime->setUrls(urls);   // "text/uri-list", el formato estándar

    // Convención de GNOME/Nautilus ("copy\n..." / "cut\n...") para que el
    // portapapeles también funcione con el gestor de archivos del sistema
    QByteArray payload = (cut ? QByteArrayLiteral("cut") : QByteArrayLiteral("copy")) + '\n';
    for (const QUrl &url : urls)
        payload += url.toEncoded() + '\n';
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), payload);

    QGuiApplication::clipboard()->setMimeData(mime);
}

// Ctrl+V — copia (o mueve, si era un "cortar") el portapapeles al escritorio
void DesktopView::pasteFromClipboard()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls())
        return;

    const bool cut = mime->hasFormat(QStringLiteral("x-special/gnome-copied-files"))
        && mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut");

    QStringList moved;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && !copyIntoDesktop(url.toLocalFile()).isEmpty())
            moved << url.toLocalFile();
    }

    // Si era un "cortar", movemos: se eliminan los orígenes y se limpia el
    // portapapeles para que un segundo Ctrl+V no vuelva a mover nada.
    if (cut) {
        for (const QString &source : moved) {
            const QFileInfo info(source);
            if (info.isDir())
                QDir(source).removeRecursively();
            else
                QFile::remove(source);
        }
        QGuiApplication::clipboard()->clear();
    }
}

// Copia un archivo/carpeta al escritorio devolviendo la ruta destino, o una
// cadena vacía si falla. Usada por el drop y por pegar.
QString DesktopView::copyIntoDesktop(const QString &source)
{
    const QFileInfo info(source);
    if (!info.exists())
        return QString();

    const QString dest = uniqueDestination(source, m_desktopDir);

    bool ok = false;
    if (info.isDir()) {
        ok = true;
        if (!QDir().mkpath(dest))
            return QString();
        QDirIterator it(source, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString target =
                QDir(dest).filePath(QDir(source).relativeFilePath(fi.absoluteFilePath()));
            if (fi.isDir())
                QDir().mkpath(target);
            else if (!QFile::copy(fi.absoluteFilePath(), target))
                ok = false;
        }
    } else {
        ok = QFile::copy(source, dest);
    }

    return ok ? dest : QString();
}

// Ctrl+Shift+N / 0xcece — crea "Nueva carpeta %1" y la deja seleccionada
void DesktopView::newFolder()
{
    QDir dir(m_desktopDir);
    for (int i = 0; i < 1000; ++i) {
        const QString name = QStringLiteral("Nueva carpeta %1").arg(i);
        if (!dir.mkdir(name))
            continue;

        // El creador queda seleccionado para renombrarlo o borrarlo con
        // el teclado de inmediato
        selectPath(dir.absoluteFilePath(name));
        return;
    }
}

// Selecciona un elemento del modelo (reutilizada por newFolder, newDocument
// y newLauncher). Si el modelo aún no tiene la fila (p. ej. recién creada en
// disco), reintenta al momento siguiente.
void DesktopView::selectPath(const QString &path)
{
    const QModelIndex index = m_proxy->filePathIndex(path);
    if (!index.isValid()) {
        QTimer::singleShot(60, this, [this, path] { selectPath(path); });
        return;
    }
    m_list->clearSelection();
    m_list->setCurrentIndex(index);
    m_list->selectionModel()->select(index,
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

// "Nuevo documento" — crea un documento de texto vacío en el escritorio
void DesktopView::newDocument()
{
    const QString baseName = QStringLiteral("Documento sin título");
    QString path = QDir(m_desktopDir).filePath(baseName + QStringLiteral(".txt"));
    for (int i = 1; QFile::exists(path); ++i)
        path = QDir(m_desktopDir).filePath(
            baseName + QStringLiteral(" (%1).txt").arg(i));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.close();

    selectPath(path);
}

// "Nuevo lanzador" — crea un archivo .desktop ejecutable para lanzar comandos
void DesktopView::newLauncher()
{
    bool okName = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Crear lanzador"),
        QStringLiteral("Nombre del lanzador:"),
        QLineEdit::Normal, QStringLiteral("Lanzador nuevo"), &okName);
    if (!okName || name.trimmed().isEmpty())
        return;

    bool okCmd = false;
    const QString command = QInputDialog::getText(
        this, QStringLiteral("Crear lanzador"),
        QStringLiteral("Comando a ejecutar:"),
        QLineEdit::Normal, QStringLiteral("x-terminal-emulator"), &okCmd);
    if (!okCmd || command.trimmed().isEmpty())
        return;

    QString path = QDir(m_desktopDir).filePath(name + QStringLiteral(".desktop"));
    for (int i = 1; QFile::exists(path); ++i)
        path = QDir(m_desktopDir).filePath(
            name + QStringLiteral(" (%1).desktop").arg(i));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;

    // Plantilla estándar de freedesktop.org (especificación .desktop)
    {
        QTextStream out(&file);   // codificación por defecto: UTF-8
        out << QStringLiteral("[Desktop Entry]\n")
            << QStringLiteral("Type=Application\n")
            << QStringLiteral("Version=1.0\n")
            << QStringLiteral("Name=") << name << '\n'
            << QStringLiteral("Comment=\n")
            << QStringLiteral("Exec=") << command << '\n'
            << QStringLiteral("Icon=utilities-terminal\n")
            << QStringLiteral("Terminal=false\n")
            << QStringLiteral("Categories=Utility;\n");
    }
    file.close();

    // Permisos: el lanzador debe ser ejecutable para que el sistema lo trate
    // como tal al abrirlo con el gestor de archivos
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                        | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                        | QFileDevice::ExeGroup | QFileDevice::ReadOther
                        | QFileDevice::ExeOther);

    selectPath(path);
}

// F5 — fuerza el re-escaneo del directorio
void DesktopView::refresh()
{
    m_model->setRootPath(m_model->rootPath());
}

// ============================================================================
//  Widgets del escritorio: reloj, monitor de CPU/memoria, velocidad de red y
//  capacidad de los discos. Son hijas semitransparentes del DesktopView,
//  ancladas arriba a la derecha, que se dibujan con QPainter.
//  ============================================================================
class DesktopInfoWidget : public QWidget
{
public:
    explicit DesktopInfoWidget(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(236, 100);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // panel translúcido redondeado
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 24, 28, 190));
        p.drawRoundedRect(rect(), 10, 10);

        // título
        p.setPen(QColor(190, 200, 210));
        QFont f = p.font();
        f.setPixelSize(11);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.drawText(QRect(0, 7, width(), 14), Qt::AlignHCenter, title());

        f.setPixelSize(12);
        f.setWeight(QFont::Normal);
        p.setFont(f);
        paintContent(p, QRect(10, 24, width() - 20, height() - 30));
    }

    virtual QString title() const = 0;
    virtual void paintContent(QPainter &p, const QRect &r) const = 0;

    // Barra de progreso horizontal centrada en `barRect`
    void drawBar(QPainter &p, const QRect &barRect, double ratio,
                 const QColor &fill) const
    {
        QRect bar = barRect.adjusted(0, (barRect.height() - 6) / 2, 0,
                                     -(barRect.height() - 6) / 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 45));
        p.drawRoundedRect(bar, 3, 3);
        const int w = qRound(bar.width() * qBound(0.0, ratio, 1.0));
        if (w <= 0)
            return;
        QRect fillRect = bar.adjusted(0, 0, 0, 0);
        fillRect.setWidth(w);
        p.setBrush(fill);
        p.drawRoundedRect(fillRect, 3, 3);
    }
};

// --- Reloj ---------------------------------------------------------
class ClockWidget : public DesktopInfoWidget
{
public:
    explicit ClockWidget(QWidget *parent)
        : DesktopInfoWidget(parent)
    {
        setFixedSize(236, 84);
        auto *timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this] { update(); });
        timer->start();
    }

private:
    QString title() const override { return QStringLiteral("Reloj"); }

    void paintContent(QPainter &p, const QRect &r) const override
    {
        const QDateTime now = QDateTime::currentDateTime();

        QFont f = p.font();
        f.setPixelSize(34);
        f.setWeight(QFont::Bold);
        f.setFamily(QStringLiteral("monospace"));
        p.setFont(f);
        p.setPen(QColor(255, 255, 255));
        p.drawText(r.adjusted(0, -4, 0, -22), Qt::AlignHCenter,
                   now.toString(QStringLiteral("HH:mm:ss")));

        f.setPixelSize(12);
        f.setWeight(QFont::Normal);
        f.setFamily(QString());
        p.setFont(f);
        p.setPen(QColor(210, 220, 230));
        static const QLocale spanish(QLocale::Spanish);
        p.drawText(r.adjusted(0, 26, 0, 0), Qt::AlignHCenter,
                   spanish.toString(now, QStringLiteral("dddd d 'de' MMMM yyyy")));
    }
};

// --- Monitor de CPU y memoria ----------------------------------------
class SystemWidget : public DesktopInfoWidget
{
public:
    explicit SystemWidget(QWidget *parent)
        : DesktopInfoWidget(parent)
    {
        setFixedSize(236, 120);
        sampleCpu();       // valores base para calcular el primer delta
        sampleMemory();
        auto *timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this] {
            sampleCpu();
            sampleMemory();
            update();
        });
        timer->start();
    }

private:
    QString title() const override { return QStringLiteral("Sistema"); }

    // % de CPU respecto a la muestra anterior (lectura de /proc/stat)
    void sampleCpu()
    {
        m_cpu = 0.0;
        QFile f(QStringLiteral("/proc/stat"));
        if (!f.open(QIODevice::ReadOnly))
            return;
        const QList<QByteArray> parts = f.readLine().simplified().split(' ');
        if (parts.isEmpty() || parts.first() != "cpu")
            return;
        quint64 total = 0;
        quint64 idle  = 0;
        for (int i = 1; i < parts.size() && i < 8; ++i) {
            total += parts[i].toULongLong();
            if (i == 4)
                idle = parts[i].toULongLong();   // hilo "idle"
        }
        const quint64 dTotal = total - m_lastTotal;
        const quint64 dIdle  = idle  - m_lastIdle;
        m_lastTotal = total;
        m_lastIdle  = idle;
        if (dTotal > 0)
            m_cpu = 1.0 - double(dIdle) / double(dTotal);
    }

    // % de memoria usada (lectura de /proc/meminfo)
    void sampleMemory()
    {
        m_mem = 0.0;
        QFile f(QStringLiteral("/proc/meminfo"));
        if (!f.open(QIODevice::ReadOnly))
            return;
        quint64 total = 0;
        quint64 avail = 0;
        while (!f.atEnd()) {
            const QList<QByteArray> parts = f.readLine().simplified().split(' ');
            if (parts.size() < 2)
                continue;
            if (parts[0] == "MemTotal:")          total = parts[1].toULongLong();
            else if (parts[0] == "MemAvailable:") avail = parts[1].toULongLong();
        }
        if (total > 0)
            m_mem = double(total - avail) / double(total);
    }

    void paintContent(QPainter &p, const QRect &r) const override
    {
        p.setPen(QColor(255, 255, 255));
        p.drawText(r.left(), r.top() + 12,
                   QStringLiteral("CPU: %1 %").arg(qRound(m_cpu * 100)));
        drawBar(p, r.adjusted(0, 16, 0, 16), m_cpu, QColor(0x93, 0xba, 0x84));

        p.drawText(r.left(), r.top() + 46,
                   QStringLiteral("Memoria: %1 %").arg(qRound(m_mem * 100)));
        drawBar(p, r.adjusted(0, 50, 0, 50), m_mem, QColor(0x33, 0x9d, 0xd6));
    }

    double m_cpu = 0.0;
    double m_mem = 0.0;
    quint64 m_lastTotal = 0;
    quint64 m_lastIdle  = 0;
};

// --- Velocidad de red --------------------------------------------------
class NetworkWidget : public DesktopInfoWidget
{
public:
    explicit NetworkWidget(QWidget *parent)
        : DesktopInfoWidget(parent)
    {
        setFixedSize(236, 88);
        sampleNet();
        auto *timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this] {
            sampleNet();
            update();
        });
        timer->start();
    }

private:
    QString title() const override { return QStringLiteral("Red"); }

    // Velocidad instantánea con los deltas de /proc/net/dev entre muestras
    void sampleNet()
    {
        QFile f(QStringLiteral("/proc/net/dev"));
        if (!f.open(QIODevice::ReadOnly))
            return;

        quint64 rx = 0;
        quint64 tx = 0;
        bool first = true;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine();
            if (first) {          // cabecera
                first = false;
                continue;
            }
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QString iface = QString::fromUtf8(line.left(colon)).trimmed();
            if (iface == QStringLiteral("lo"))
                continue;
            const QList<QByteArray> parts = line.mid(colon + 1).simplified().split(' ');
            if (parts.size() >= 9) {
                rx += parts[0].toULongLong();   // bytes recibidos
                tx += parts[8].toULongLong();   // bytes transmitidos
            }
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const double dt = m_lastTime > 0
            ? qMax(1.0, double(now - m_lastTime) / 1000.0) : 1.0;
        if (m_lastTime > 0) {
            m_down = rx > m_lastRx ? double(rx - m_lastRx) / dt : 0.0;
            m_up   = tx > m_lastTx ? double(tx - m_lastTx) / dt : 0.0;
        }
        m_lastRx = rx;
        m_lastTx = tx;
        m_lastTime = now;
    }

    static QString humanRate(double bytesPerSecond)
    {
        return bytesPerSecond >= 1024.0 * 1024.0
            ? QStringLiteral("%1 MiB/s").arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 1)
            : QStringLiteral("%1 KiB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 1);
    }

    void paintContent(QPainter &p, const QRect &r) const override
    {
        p.setPen(QColor(0x66, 0xd9, 0xe8));
        p.drawText(QRect(r.left(), r.top(), r.width(), 20),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Bajada: %1").arg(humanRate(m_down)));
        p.setPen(QColor(255, 255, 255));
        p.drawText(QRect(r.left(), r.top() + 24, r.width(), 20),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Subida: %1").arg(humanRate(m_up)));
    }

    double m_down = 0.0;
    double m_up   = 0.0;
    quint64 m_lastRx = 0;
    quint64 m_lastTx = 0;
    qint64 m_lastTime = 0;
};

// --- Capacidad de los discos ---------------------------------------------
class DiskWidget : public DesktopInfoWidget
{
public:
    explicit DiskWidget(QWidget *parent)
        : DesktopInfoWidget(parent)
    {
        setFixedSize(236, 148);
        auto *timer = new QTimer(this);
        timer->setInterval(5000);
        connect(timer, &QTimer::timeout, this, [this] { update(); });
        timer->start();
    }

private:
    QString title() const override { return QStringLiteral("Discos"); }

    void paintContent(QPainter &p, const QRect &r) const override
    {
        const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
        int y = 0;
        int shown = 0;
        for (const QStorageInfo &volume : volumes) {
            if (!volume.isValid() || volume.isReadOnly())
                continue;
            const QString path = volume.rootPath();
            if (path.isEmpty())
                continue;
            // Se saltan los volúmenes virtuales (proc, sys, dev, temporales y
            // de solo-ramida) que no son discos reales
            if (path.startsWith(QStringLiteral("/proc"))
                || path.startsWith(QStringLiteral("/sys"))
                || path.startsWith(QStringLiteral("/dev"))
                || path.startsWith(QStringLiteral("/run"))
                || path.startsWith(QStringLiteral("/var")))
                continue;
            const QString device = volume.device();
            if (device == QStringLiteral("tmpfs")
                || device == QStringLiteral("devtmpfs")
                || device == QStringLiteral("overlay")
                || device == QStringLiteral("squashfs")
                || device == QStringLiteral("udev")
                || device == QStringLiteral("shm"))
                continue;
            if (shown >= 4)
                break;

            const qint64 total = volume.bytesTotal();
            const qint64 free  = volume.bytesAvailable();
            const double ratio = total > 0
                ? double(total - free) / double(total) : 0.0;

            p.setPen(QColor(255, 255, 255));
            p.drawText(QRect(r.left(), r.top() + y, r.width(), 16),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("%1   %2 % ocupado")
                           .arg(path, QString::number(qRound(ratio * 100))));
            drawBar(p, QRect(r.left(), r.top() + y + 16, r.width(), 8),
                    ratio, QColor(0xe6, 0x7e, 0x22));
            y += 30;
            ++shown;
        }
    }
};

#ifdef STELLARBOX_SNAPSHOT_TEST
// Accesores de diagnóstico usados por la verificación sin pantalla
int DesktopView::systemIconsVisible() const { return m_proxy ? m_proxy->systemIconCount() : -1; }
int DesktopView::desktopRowCount() const { return m_proxy ? m_proxy->rowCount() : -1; }
bool DesktopView::widgetClockVisible() const { return m_widgetClock && m_widgetClock->isVisible(); }
bool DesktopView::widgetSystemVisible() const { return m_widgetSystem && m_widgetSystem->isVisible(); }
bool DesktopView::widgetNetworkVisible() const { return m_widgetNetwork && m_widgetNetwork->isVisible(); }
bool DesktopView::widgetDiskVisible() const { return m_widgetDisk && m_widgetDisk->isVisible(); }
DesktopIconView *DesktopView::debugList() const { return m_list; }
DesktopProxyModel *DesktopView::debugProxy() const { return m_proxy; }
QString DesktopView::widgetGeometryDump() const
{
    QStringList parts;
    QWidget *const ws[4] = { m_widgetClock, m_widgetSystem,
                             m_widgetNetwork, m_widgetDisk };
    const char *const names[4] = { "clock", "system", "network", "disk" };
    for (int i = 0; i < 4; ++i) {
        if (!ws[i] || !ws[i]->isVisible())
            continue;
        parts << QStringLiteral("%1=%2,%3").arg(QLatin1String(names[i]))
                .arg(ws[i]->pos().x()).arg(ws[i]->pos().y());
    }
    return parts.join(QStringLiteral(" "));
}
#endif

// Aplica la configuración a esta vista: directorio del escritorio, fondo,
// tamaño de iconos y rejilla (usada al crear la vista y desde Configuración)
void DesktopView::applySettings(const AppSettings &s)
{
    const bool dirChanged = m_desktopDir != s.desktopDir;
    m_desktopDir = s.desktopDir;
    m_bgDir      = s.bgDir;

    if (dirChanged) {
        QDir().mkpath(m_desktopDir);
        m_model->setRootPath(m_desktopDir);
        m_proxy->setSourceRoot(m_model->index(m_desktopDir, 0));
    }

    // Rejilla proporcional al icono: 48 px → 112x96 (valores del binario)
    m_list->setIconSize(QSize(s.iconSize, s.iconSize));
    m_list->setGridSize(QSize(s.iconSize + 64, s.iconSize + 48));

    // Iconos del sistema visibles en el escritorio
    m_proxy->setSystemIcons(s.showComputer, s.showUser, s.showNetwork, s.showTrash);

    // Widgets del escritorio (se crean una vez y se muestran u ocultan)
    ensureWidgets();
    m_widgetClock->setVisible(s.widgetClock);
    m_widgetSystem->setVisible(s.widgetSystem);
    m_widgetNetwork->setVisible(s.widgetNetwork);
    m_widgetDisk->setVisible(s.widgetDisk);
    relayoutWidgets();

    loadWallpaper();   // con el nuevo m_bgDir
    update();
}

// Crea (ocultos) los cuatro widgets la primera vez que se aplica configuración
void DesktopView::ensureWidgets()
{
    if (m_widgetsReady)
        return;
    m_widgetsReady = true;
    m_widgetClock   = new ClockWidget(this);
    m_widgetSystem  = new SystemWidget(this);
    m_widgetNetwork = new NetworkWidget(this);
    m_widgetDisk    = new DiskWidget(this);
    m_widgetClock->hide();
    m_widgetSystem->hide();
    m_widgetNetwork->hide();
    m_widgetDisk->hide();
}

// Ancla los widgets visibles arriba a la derecha del escritorio
void DesktopView::relayoutWidgets()
{
    if (!m_widgetsReady)
        return;
    const int margin = 18;
    int y = margin;
    QWidget *widgets[] = { m_widgetClock, m_widgetSystem,
                           m_widgetNetwork, m_widgetDisk };
    for (QWidget *widget : widgets) {
        if (!widget || !widget->isVisible())
            continue;
        widget->move(width() - margin - widget->width(), y);
        y += widget->height() + 8;
    }
}

DesktopView::~DesktopView()
{
    // Recuerda cómo quedó el escritorio (arrastres y flujo vertical)
    if (m_list)
        m_list->savePositions();
    allDesktopViews().removeOne(this);
}

// ============================================================================
//  Iconos de la barra lateral (carpeta icons/configuracion del proyecto)
//  ============================================================================
static QIcon configIcon(const char *name)
{
    const QString base = QLatin1String(name) + QStringLiteral(".svg");
    const char *const dirs[] = {
        "icons/configuracion",
        "../icons/configuracion",              // al ejecutar desde build/
        "/usr/share/stellarbox/icons/configuracion",
        "/usr/local/share/stellarbox/icons/configuracion",
        "/etc/stellarbox/icons/configuracion",
    };
    for (const char *dir : dirs) {
        QString path = QCoreApplication::applicationDirPath();
        if (dir[0] != '/')
            path += QLatin1Char('/');
        path += QLatin1String(dir);
        path += QLatin1Char('/');
        path += base;
        if (!QFile::exists(path))
            continue;
        QIcon icon(path);
        if (!icon.isNull())
            return icon;
    }
    return QIcon();
}

// ============================================================================
//  Pantalla: consulta y aplicación de resolución / tasa / rotación (xrandr)
//  ============================================================================
struct XrandrInfo
{
    QString output;                      // nombre de la salida (p. ej. "eDP-1")
    QList<QSize> modes;                  // resoluciones disponibles
    QHash<QSize, QList<double>> rates;   // tasas disponibles por resolución
};

static QSize parseModeSize(const QString &mode)
{
    const int x = mode.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return QSize();
    bool okW = false;
    bool okH = false;
    const int w = mode.left(x).toInt(&okW);
    const int h = mode.mid(x + 1).toInt(&okH);
    return (okW && okH && w > 0 && h > 0) ? QSize(w, h) : QSize();
}

static XrandrInfo queryXrandr()
{
    XrandrInfo info;
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        info.output = screen->name();
        const QSize current = screen->geometry().size();
        if (current.width() > 0 && current.height() > 0) {
            info.modes << current;
            info.rates[current] << screen->refreshRate();
        }
    }

    QProcess proc;
    proc.start(QStringLiteral("xrandr"), { QStringLiteral("--query") });
    if (!proc.waitForStarted(1500) || !proc.waitForFinished(3000))
        return info;

    const QStringList lines = QString::fromUtf8(proc.readAll()).split(QLatin1Char('\n'));
    QString currentOutput;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.contains(QStringLiteral(" connected"))) {
            currentOutput = line.left(line.indexOf(QLatin1Char(' ')));
            if (info.output.isEmpty())
                info.output = currentOutput;
            continue;
        }
        if (currentOutput.isEmpty() || line.isEmpty()
            || line.startsWith(QStringLiteral("Screen")))
            continue;

        // "1920x1080     60.03*+  59.94  50.00"
        const int sp = line.indexOf(QLatin1Char(' '));
        const QString modeStr = line.left(sp).trimmed();
        if (modeStr.contains(QLatin1Char(',')))
            continue;   // línea de geometría de la salida
        const QSize size = parseModeSize(modeStr);
        if (!size.isValid())
            continue;
        if (!info.modes.contains(size))
            info.modes << size;

        QList<double> rates;
        const QStringList tokens = line.mid(sp).split(QLatin1Char(' '),
                                                      Qt::SkipEmptyParts);
        for (const QString &token : tokens) {
            QString t = token;
            t.remove(QLatin1Char('*')).remove(QLatin1Char('+'));
            bool ok = false;
            const double r = t.toDouble(&ok);
            if (ok && r > 0.0)
                rates << r;
        }
        if (!rates.isEmpty())
            info.rates[size] = rates;
    }
    return info;
}

static const char *rotationName(int rotation)
{
    switch (rotation) {
    case 1: return "left";
    case 2: return "inverted";
    case 3: return "right";
    }
    return "normal";
}

static bool applyXrandr(const XrandrInfo &info, const QSize &mode,
                        double rate, int rotation)
{
    if (info.output.isEmpty() || !mode.isValid())
        return false;

    QStringList args = {
        QStringLiteral("--output"), info.output,
        QStringLiteral("--mode"),
        QString::number(mode.width()) + QLatin1Char('x') + QString::number(mode.height())
    };
    if (rate > 0.0)
        args << QStringLiteral("--rate") << QString::number(rate, 'f', 2);
    args << QStringLiteral("--rotate") << QString::fromLatin1(rotationName(rotation));

    QProcess proc;
    proc.start(QStringLiteral("xrandr"), args);
    if (!proc.waitForStarted(1500) || !proc.waitForFinished(4000))
        return false;
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// ============================================================================
//  SettingsDialog : QDialog — configuración del escritorio con barra lateral
//  (Apariencia · Desktop · Widget)
//  ============================================================================
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const QString &wallpaperFilePath, QWidget *parent = nullptr);

private:
    QWidget *makeAppearancePage();
    QWidget *makeDesktopPage();
    QWidget *makeWidgetPage();

    void browseDir(QLineEdit *edit);
    void chooseWallpaper();
    void restoreWallpaper();
    void updateWallpaperLabel();
    void loadScreenInfo();
    void reloadRates();
    void applyDisplayChanges();
    void restoreAllDefaults();
    void accept() override;

    AppSettings m_settings;
    QString m_wallpaperFilePath;

    QListWidget *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;

    // Apariencia
    QLabel    *m_wallpaperLabel = nullptr;
    QSpinBox  *m_iconSizeSpin   = nullptr;
    QLineEdit *m_iconThemeEdit  = nullptr;
    QCheckBox *m_iconComputer = nullptr;
    QCheckBox *m_iconUser     = nullptr;
    QCheckBox *m_iconNetwork  = nullptr;
    QCheckBox *m_iconTrash    = nullptr;

    // Desktop
    QLineEdit *m_desktopDirEdit = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_rateCombo = nullptr;
    QComboBox *m_rotationCombo = nullptr;
    XrandrInfo m_xrandr;
    QSize  m_initialMode;
    double m_initialRate = 0.0;
    int    m_initialRotation = 0;

    // Widget
    QCheckBox *m_widgetClock   = nullptr;
    QCheckBox *m_widgetSystem  = nullptr;
    QCheckBox *m_widgetNetwork = nullptr;
    QCheckBox *m_widgetDisk    = nullptr;
};

SettingsDialog::SettingsDialog(const QString &wallpaperFilePath, QWidget *parent)
    : QDialog(parent)
    , m_wallpaperFilePath(wallpaperFilePath)
{
    setWindowTitle(QStringLiteral("Configuración de Stellarbox Windesktop"));
    setModal(true);
    resize(700, 450);

    m_settings = globalSettings();

    // --- páginas ---------------------------------------------------------
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(makeAppearancePage());
    m_pages->addWidget(makeDesktopPage());
    m_pages->addWidget(makeWidgetPage());

    // --- barra lateral ---------------------------------------------------
    m_sidebar = new QListWidget(this);
    m_sidebar->setObjectName(QStringLiteral("configSidebar"));
    m_sidebar->setFixedWidth(176);
    m_sidebar->setIconSize(QSize(32, 32));
    m_sidebar->setStyleSheet(QStringLiteral(R"(
        QListWidget#configSidebar {
            background: #22272e;
            border: none;
            outline: none;
            padding: 6px;
        }
        QListWidget#configSidebar::item {
            color: #eceff4;
            padding: 8px;
            border-radius: 6px;
        }
        QListWidget#configSidebar::item:selected {
            background: #3d4750;
        }
        QListWidget#configSidebar::item:hover {
            background: #2f3840;
        }
    )"));

    const auto addSection = [this](const char *icon, const QString &text) {
        auto *item = new QListWidgetItem(configIcon(icon), text);
        item->setSizeHint(QSize(0, 46));
        m_sidebar->addItem(item);
    };
    addSection("apariencia", QStringLiteral("Apariencia"));
    addSection("desktop",    QStringLiteral("Desktop"));
    addSection("widgen",     QStringLiteral("Widget"));
    m_sidebar->setCurrentRow(0);

    connect(m_sidebar, &QListWidget::currentRowChanged,
            m_pages, &QStackedWidget::setCurrentIndex);

    auto *center = new QHBoxLayout;
    center->addWidget(m_sidebar);
    center->addWidget(m_pages, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Aplicar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(center, 1);
    layout->addWidget(buttons);

    updateWallpaperLabel();
}

// --- página "Apariencia" ------------------------------------------------
QWidget *SettingsDialog::makeAppearancePage()
{
    auto *page = new QWidget(this);

    m_wallpaperLabel = new QLabel(page);
    m_wallpaperLabel->setWordWrap(true);
    auto *wallpaperChange = new QPushButton(QStringLiteral("Cambiar…"), page);
    auto *wallpaperRestore = new QPushButton(QStringLiteral("Restaurar"), page);
    auto *wallpaperRow = new QHBoxLayout;
    wallpaperRow->addWidget(m_wallpaperLabel, 1);
    wallpaperRow->addWidget(wallpaperChange);
    wallpaperRow->addWidget(wallpaperRestore);

    m_iconSizeSpin = new QSpinBox(page);
    m_iconSizeSpin->setRange(16, 256);
    m_iconSizeSpin->setValue(m_settings.iconSize);
    m_iconSizeSpin->setSuffix(QStringLiteral(" px"));

    m_iconThemeEdit = new QLineEdit(m_settings.iconTheme, page);
    m_iconThemeEdit->setPlaceholderText(QStringLiteral("Predeterminado del sistema"));

    m_iconComputer = new QCheckBox(QStringLiteral("Equipo"), page);
    m_iconUser     = new QCheckBox(QStringLiteral("Usuario"), page);
    m_iconNetwork  = new QCheckBox(QStringLiteral("Red"), page);
    m_iconTrash    = new QCheckBox(QStringLiteral("Papelera"), page);
    m_iconComputer->setChecked(m_settings.showComputer);
    m_iconUser->setChecked(m_settings.showUser);
    m_iconNetwork->setChecked(m_settings.showNetwork);
    m_iconTrash->setChecked(m_settings.showTrash);
    m_iconComputer->setIcon(QIcon::fromTheme(QStringLiteral("computer")));
    m_iconUser->setIcon(QIcon::fromTheme(QStringLiteral("user-home")));
    m_iconNetwork->setIcon(QIcon::fromTheme(QStringLiteral("network-workgroup")));
    m_iconTrash->setIcon(QIcon::fromTheme(QStringLiteral("user-trash")));

    auto *iconsGroup = new QGroupBox(QStringLiteral("Iconos del escritorio"), page);
    auto *iconsLayout = new QVBoxLayout(iconsGroup);
    iconsLayout->addWidget(m_iconComputer);
    iconsLayout->addWidget(m_iconUser);
    iconsLayout->addWidget(m_iconNetwork);
    iconsLayout->addWidget(m_iconTrash);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Fondo de pantalla:"), wallpaperRow);
    form->addRow(QStringLiteral("Tamaño del icono:"), m_iconSizeSpin);
    form->addRow(QStringLiteral("Tema de iconos:"), m_iconThemeEdit);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(iconsGroup);
    layout->addStretch();

    connect(wallpaperChange, &QPushButton::clicked, this, &SettingsDialog::chooseWallpaper);
    connect(wallpaperRestore, &QPushButton::clicked, this, &SettingsDialog::restoreWallpaper);

    return page;
}

// --- página "Desktop" ---------------------------------------------------
QWidget *SettingsDialog::makeDesktopPage()
{
    auto *page = new QWidget(this);

    // Directorio mostrado como escritorio
    m_desktopDirEdit = new QLineEdit(m_settings.desktopDir, page);
    auto *dirBrowse = new QPushButton(QStringLiteral("Examinar…"), page);
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(m_desktopDirEdit, 1);
    dirRow->addWidget(dirBrowse);

    // Pantalla
    m_resolutionCombo = new QComboBox(page);
    m_rateCombo = new QComboBox(page);
    m_rotationCombo = new QComboBox(page);
    m_rotationCombo->addItem(QStringLiteral("Normal"), 0);
    m_rotationCombo->addItem(QStringLiteral("Izquierda (90°)"), 1);
    m_rotationCombo->addItem(QStringLiteral("Invertida (180°)"), 2);
    m_rotationCombo->addItem(QStringLiteral("Derecha (270°)"), 3);

    auto *screenGroup = new QGroupBox(QStringLiteral("Pantalla"), page);
    auto *screenForm = new QFormLayout(screenGroup);
    screenForm->addRow(QStringLiteral("Resolución:"), m_resolutionCombo);
    screenForm->addRow(QStringLiteral("Tasa de actualización:"), m_rateCombo);
    screenForm->addRow(QStringLiteral("Rotación:"), m_rotationCombo);

    auto *restoreBtn = new QPushButton(
        QStringLiteral("Restaurar todo a los valores por defecto"), page);

    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Directorio del escritorio:"), dirRow);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(screenGroup);
    layout->addSpacing(8);
    layout->addWidget(restoreBtn);
    layout->addStretch();

    connect(dirBrowse, &QPushButton::clicked, this, [this] { browseDir(m_desktopDirEdit); });
    connect(restoreBtn, &QPushButton::clicked, this, &SettingsDialog::restoreAllDefaults);
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { reloadRates(); });

    loadScreenInfo();

    return page;
}

// --- página "Widget" ----------------------------------------------------
QWidget *SettingsDialog::makeWidgetPage()
{
    auto *page = new QWidget(this);

    m_widgetClock    = new QCheckBox(QStringLiteral("Reloj"), page);
    m_widgetSystem   = new QCheckBox(QStringLiteral("Monitor de CPU y memoria"), page);
    m_widgetNetwork  = new QCheckBox(QStringLiteral("Velocidad de red"), page);
    m_widgetDisk     = new QCheckBox(QStringLiteral("Capacidad de los discos"), page);

    m_widgetClock->setChecked(m_settings.widgetClock);
    m_widgetSystem->setChecked(m_settings.widgetSystem);
    m_widgetNetwork->setChecked(m_settings.widgetNetwork);
    m_widgetDisk->setChecked(m_settings.widgetDisk);

    auto *hint = new QLabel(QStringLiteral(
        "Los widgets activos se muestran en la esquina superior derecha del "
        "escritorio."), page);
    hint->setWordWrap(true);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_widgetClock);
    layout->addWidget(m_widgetSystem);
    layout->addWidget(m_widgetNetwork);
    layout->addWidget(m_widgetDisk);
    layout->addSpacing(10);
    layout->addWidget(hint);
    layout->addStretch();

    return page;
}

void SettingsDialog::browseDir(QLineEdit *edit)
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Elegir directorio"), edit->text());
    if (!dir.isEmpty())
        edit->setText(dir);
}

void SettingsDialog::chooseWallpaper()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Elegir fondo de pantalla"),
        m_settings.bgDir,
        QStringLiteral("Imagenes (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (file.isEmpty())
        return;

    QSaveFile out(m_wallpaperFilePath);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(file.toUtf8());
        out.commit();
    }
    updateWallpaperLabel();
}

void SettingsDialog::restoreWallpaper()
{
    QFile::remove(m_wallpaperFilePath);
    updateWallpaperLabel();
}

void SettingsDialog::updateWallpaperLabel()
{
    QString current = QStringLiteral("(por defecto)");
    QFile f(m_wallpaperFilePath);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QString path = QString::fromUtf8(f.readAll()).trimmed();
        if (!path.isEmpty())
            current = path;
    }
    m_wallpaperLabel->setText(QStringLiteral("Actual: %1").arg(current));
}

// Resolución, tasa y rotación: valores guardados o los actuales del monitor
void SettingsDialog::loadScreenInfo()
{
    m_xrandr = queryXrandr();

    QSize current;
    if (!m_xrandr.modes.isEmpty())
        current = m_xrandr.modes.first();
    if (!m_settings.resolution.isEmpty()) {
        const QSize saved = parseModeSize(m_settings.resolution);
        if (saved.isValid())
            current = saved;
    }

    m_resolutionCombo->clear();
    for (const QSize &mode : m_xrandr.modes)
        m_resolutionCombo->addItem(
            QStringLiteral("%1×%2").arg(mode.width()).arg(mode.height()),
            QVariant::fromValue(mode));

    if (m_resolutionCombo->count() == 0) {
        m_initialMode = QSize();
        return;
    }
    const int idx = m_resolutionCombo->findData(QVariant::fromValue(current));
    m_resolutionCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_initialMode = m_resolutionCombo->currentData().toSize();

    reloadRates();
    m_initialRate = m_rateCombo->currentData().toDouble();

    // Rotación: preferencia guardada > orientación actual del monitor
    int rotation = 0;
    {
        QSettings qs(configFilePath(), QSettings::IniFormat);
        if (qs.contains(QStringLiteral("Desktop/Rotation"))) {
            rotation = qs.value(QStringLiteral("Desktop/Rotation"), 0).toInt();
        } else if (m_settings.rotation > 0) {
            rotation = m_settings.rotation;
        } else {
            const QScreen *screen = QGuiApplication::primaryScreen();
            if (screen) {
                switch (screen->primaryOrientation()) {
                case Qt::PortraitOrientation:           rotation = 1; break;
                case Qt::InvertedLandscapeOrientation:  rotation = 2; break;
                case Qt::InvertedPortraitOrientation:   rotation = 3; break;
                default:                                rotation = 0;
                }
            }
        }
    }
    m_rotationCombo->setCurrentIndex(qBound(0, rotation, m_rotationCombo->count() - 1));
    m_initialRotation = rotation;
}

// Rellena la tasa de refresco según la resolución elegida
void SettingsDialog::reloadRates()
{
    const QSize mode = m_resolutionCombo->currentData().toSize();
    QList<double> rates = m_xrandr.rates.value(mode);
    if (rates.isEmpty()) {
        const QScreen *screen = QGuiApplication::primaryScreen();
        if (screen && screen->refreshRate() > 0.0)
            rates << screen->refreshRate();
        else
            rates << 60.0;
    }

    double wanted = m_settings.refreshRate;
    if (wanted <= 0.0) {
        const QScreen *screen = QGuiApplication::primaryScreen();
        wanted = screen && screen->refreshRate() > 0.0 ? screen->refreshRate() : 60.0;
    }

    m_rateCombo->clear();
    int bestIndex = 0;
    double bestDiff = 1e9;
    for (int i = 0; i < rates.size(); ++i) {
        m_rateCombo->addItem(QStringLiteral("%1 Hz").arg(rates[i], 0, 'f', 2), rates[i]);
        const double diff = qAbs(rates[i] - wanted);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIndex = i;
        }
    }
    if (m_rateCombo->count() == 0)
        m_rateCombo->addItem(QStringLiteral("—"), 0.0);
    else
        m_rateCombo->setCurrentIndex(bestIndex);
}

// Aplica con xrandr la configuración de pantalla si ha cambiado
void SettingsDialog::applyDisplayChanges()
{
    const QSize mode = m_resolutionCombo->currentData().toSize();
    const double rate = m_rateCombo->currentData().toDouble();
    const int rotation = m_rotationCombo->currentData().toInt();

    if (!mode.isValid()
        || (mode == m_initialMode && rate == m_initialRate
            && rotation == m_initialRotation))
        return;

    const XrandrInfo info = queryXrandr();   // salida real actualizada
    if (!applyXrandr(info, mode, rate, rotation)) {
        QMessageBox::warning(
            this, QStringLiteral("Configuración"),
            QStringLiteral("No se pudo aplicar la configuración de pantalla "
                           "con xrandr."));
    }
}

// Botón de fábrica: restaura la pantalla y toda la configuración de la app
void SettingsDialog::restoreAllDefaults()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Restaurar configuración"),
        QStringLiteral("¿Restaurar todos los ajustes a sus valores por defecto?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    // Pantalla: vuelve al modo preferido del monitor
    {
        QProcess proc;
        proc.start(QStringLiteral("xrandr"), { QStringLiteral("--auto") });
        proc.waitForStarted(1500);
        proc.waitForFinished(3000);
    }

    // Configuración de la aplicación y fondo de pantalla
    QFile::remove(configFilePath());
    QFile::remove(m_wallpaperFilePath);

    m_settings.reset();
    m_settings.save();
    globalSettings() = m_settings;

    QIcon::setThemeName(m_settings.iconTheme.isEmpty()
                            ? systemIconThemeName()
                            : m_settings.iconTheme);
    qApp->setStyleSheet(QString());
    for (DesktopView *view : allDesktopViews())
        view->applySettings(m_settings);

    // Refleja los valores de fábrica en el diálogo
    m_desktopDirEdit->setText(m_settings.desktopDir);
    m_iconSizeSpin->setValue(m_settings.iconSize);
    m_iconThemeEdit->setText(m_settings.iconTheme);
    m_iconComputer->setChecked(m_settings.showComputer);
    m_iconUser->setChecked(m_settings.showUser);
    m_iconNetwork->setChecked(m_settings.showNetwork);
    m_iconTrash->setChecked(m_settings.showTrash);
    m_widgetClock->setChecked(m_settings.widgetClock);
    m_widgetSystem->setChecked(m_settings.widgetSystem);
    m_widgetNetwork->setChecked(m_settings.widgetNetwork);
    m_widgetDisk->setChecked(m_settings.widgetDisk);
    updateWallpaperLabel();
    loadScreenInfo();
}

void SettingsDialog::accept()
{
    // Apariencia
    m_settings.iconSize  = m_iconSizeSpin->value();
    m_settings.iconTheme = m_iconThemeEdit->text().trimmed();
    // Campo vacío en Configuración → tema predeterminado del sistema
    m_settings.iconThemeConfigured = !m_settings.iconTheme.isEmpty();
    m_settings.showComputer = m_iconComputer->isChecked();
    m_settings.showUser     = m_iconUser->isChecked();
    m_settings.showNetwork  = m_iconNetwork->isChecked();
    m_settings.showTrash    = m_iconTrash->isChecked();

    // Desktop
    m_settings.desktopDir = m_desktopDirEdit->text().trimmed();
    if (m_resolutionCombo->currentData().isValid()) {
        const QSize mode = m_resolutionCombo->currentData().toSize();
        m_settings.resolution = QStringLiteral("%1x%2")
            .arg(mode.width()).arg(mode.height());
    }
    m_settings.refreshRate = m_rateCombo->currentData().toDouble();
    m_settings.rotation    = m_rotationCombo->currentData().toInt();

    // Widget
    m_settings.widgetClock   = m_widgetClock->isChecked();
    m_settings.widgetSystem  = m_widgetSystem->isChecked();
    m_settings.widgetNetwork = m_widgetNetwork->isChecked();
    m_settings.widgetDisk    = m_widgetDisk->isChecked();

    QDir().mkpath(m_settings.desktopDir);
    m_settings.save();

    // Aplica los cambios en vivo (tema, hoja de estilo y todas las vistas)
    QIcon::setThemeName(m_settings.iconTheme.isEmpty()
                            ? systemIconThemeName()
                            : m_settings.iconTheme);
    if (!m_settings.qssFile.isEmpty()) {
        QFile f(m_settings.qssFile);
        if (f.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
    } else {
        qApp->setStyleSheet(QString());
    }

    globalSettings() = m_settings;
    for (DesktopView *view : allDesktopViews())
        view->applySettings(m_settings);

    applyDisplayChanges();

    QDialog::accept();
}

// "Configuración..." — abre la herramienta de configuración del escritorio
void DesktopView::openSettings()
{
    SettingsDialog dialog(m_wallpaperFile, this);
    dialog.exec();
}

// ============================================================================
//  X11: convierte la ventana en el "escritorio" real del WM
//  ============================================================================
//  0xa491 — xcb_connect / intern_atom(_NET_WM_WINDOW_TYPE,
//  _NET_WM_WINDOW_TYPE_DESKTOP) / change_property / flush / disconnect.
static void setDesktopWindowType(WId windowId)
{
    xcb_connection_t *c = xcb_connect(nullptr, 0);
    if (!c || xcb_connection_has_error(c)) {
        if (c)
            xcb_disconnect(c);
        return;
    }

    xcb_intern_atom_cookie_t wtype =
        xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen("_NET_WM_WINDOW_TYPE")),
                        "_NET_WM_WINDOW_TYPE");
    xcb_intern_atom_cookie_t desk =
        xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen("_NET_WM_WINDOW_TYPE_DESKTOP")),
                        "_NET_WM_WINDOW_TYPE_DESKTOP");

    xcb_intern_atom_reply_t *type  = xcb_intern_atom_reply(c, wtype, nullptr);
    xcb_intern_atom_reply_t *desktopType = xcb_intern_atom_reply(c, desk, nullptr);

    if (type && desktopType) {
        const xcb_atom_t atoms[] = { desktopType->atom };
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, windowId,
                            type->atom, XCB_ATOM_ATOM, 32, 1, atoms);
        xcb_flush(c);
    }

    std::free(type);
    std::free(desktopType);
    xcb_disconnect(c);
}

// ============================================================================
//  main
//  ============================================================================
//  0x9c60 — arranque completo: QSS global, tema de iconos (GTK), directorios
//  y un DesktopView por pantalla.
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("stellarbox-windesktop"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));
    app.setOrganizationName(QStringLiteral("Stellarbox"));
    app.setQuitOnLastWindowClosed(false);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // --- configuración persistente (settings.ini) ---------------------------
    // Las variables de entorno STELLARBOX_* tienen prioridad sobre la config
    AppSettings settings;
    settings.load();
    const QByteArray desktopDirEnv = qgetenv("STELLARBOX_DESKTOP_DIR");
    if (!desktopDirEnv.isEmpty())
        settings.desktopDir = QString::fromLocal8Bit(desktopDirEnv);
    const QByteArray bgDirEnv = qgetenv("STELLARBOX_BG_DIR");
    if (!bgDirEnv.isEmpty())
        settings.bgDir = QString::fromLocal8Bit(bgDirEnv);
    else if (settings.bgDir == QStringLiteral("/usr/share/backgrounds/wallpapers")
             && !QDir(settings.bgDir).exists())
        settings.bgDir = QStringLiteral("/usr/local/share/backgrounds/wallpapers");
    QDir().mkpath(settings.desktopDir);
    globalSettings() = settings;

    // --- hoja de estilo global ---------------------------------------------
    const QByteArray qssEnv = qgetenv("STELLARBOX_QSS");
    bool qssLoaded = false;
    if (!qssEnv.isEmpty()) {
        const QString path = QString::fromLocal8Bit(qssEnv);
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            app.setStyleSheet(QString::fromUtf8(f.readAll()));
            qssLoaded = true;
        }
    } else {
        // preferencia guardada en Configuración > rutas por defecto
        QStringList candidates;
        if (!settings.qssFile.isEmpty())
            candidates << settings.qssFile;
        candidates << QStringLiteral("/usr/share/stellarbox/stellarbox.qss")
                   << QStringLiteral("/usr/local/share/stellarbox/stellarbox.qss");
        for (const QString &candidate : candidates) {
            QFile f(candidate);
            if (f.open(QIODevice::ReadOnly)) {
                app.setStyleSheet(QString::fromUtf8(f.readAll()));
                qssLoaded = true;
                break;
            }
        }
    }
    if (!qssLoaded)
        std::fputs("stellarbox: no se pudo cargar QSS\n", stderr);

    // --- tema de iconos (preferencia guardada > tema del sistema) -----------
    // Si el usuario no eligió uno en Configuración, adoptamos el tema de
    // iconos por defecto del sistema (GTK 3/4, KDE o XFCE); si el entorno no
    // define ninguno, Qt usa su tema por defecto ("hicolor").
    QIcon::setThemeName(!settings.iconTheme.isEmpty()
                            ? settings.iconTheme
                            : systemIconThemeName());
    // Directorios donde se buscan los temas (incluidos los del usuario)
    QIcon::setThemeSearchPaths({ QDir::homePath() + QStringLiteral("/.icons"),
                                 QDir::homePath() + QStringLiteral("/.local/share/icons"),
                                 QStringLiteral("/usr/local/share/icons"),
                                 QStringLiteral("/usr/share/icons") });

    // --- directorios de trabajo ---------------------------------------------
    const QString configDir = QDir::homePath() + QStringLiteral("/.config/stellarbox");
    QDir().mkpath(configDir);
    const QString wallpaperFile = configDir + QStringLiteral("/wallpaper");

    // --- modo escritorio (real) o ventana normal ----------------------------
    const bool windowed = qEnvironmentVariableIsSet("STELLARBOX_DESKTOP_WINDOWED");

    Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint; /* 0x801 */
    if (!windowed)
        flags |= Qt::WindowStaysOnBottomHint;                     /* 0x4000000 */

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        auto *view = new DesktopView(nullptr, flags,
                                     globalSettings().desktopDir, wallpaperFile,
                                     globalSettings().bgDir);
        const QRect geometry = screen->geometry();
        view->move(geometry.topLeft());
        view->resize(geometry.size() - QSize(1, 1));   /* +QSize(1,1) en 0xa278 */

        if (!windowed)
            setDesktopWindowType(view->winId());

        view->show();
    }

#ifdef STELLARBOX_SNAPSHOT_TEST
    // Herramienta de verificación sin pantalla: vuelca capturas del diálogo
    // de Configuración (barra lateral + páginas) y del escritorio con los
    // iconos del sistema y widgets activos.
    if (qEnvironmentVariableIsSet("STELLARBOX_CONFIG_SNAP")) {
        QFile diag(QStringLiteral("/tmp/opencode/snap-info.txt"));
        diag.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream out(&diag);

        SettingsDialog dialog(configDir + QStringLiteral("/wallpaper"));
        dialog.resize(720, 480);
        auto *sidebar = dialog.findChild<QListWidget *>(QStringLiteral("configSidebar"));
        if (sidebar) {
            out << "sidebar items=" << sidebar->count() << "\n";
            for (int i = 0; i < sidebar->count(); ++i) {
                out << "  item " << i << " text='" << sidebar->item(i)->text()
                    << "' iconNull=" << sidebar->item(i)->icon().isNull()
                    << " iconSize=" << sidebar->item(i)->icon().availableSizes().value(0, QSize(-1,-1)).width()
                    << "\n";
            }
            for (int i = 0; i < sidebar->count(); ++i) {
                sidebar->setCurrentRow(i);
                dialog.show();
                QCoreApplication::processEvents();
                dialog.grab().save(QStringLiteral("/tmp/opencode/config-%1.png")
                                       .arg(i + 1));
                auto *stack = dialog.findChild<QStackedWidget *>();
                out << "page " << i << " stackedIndex="
                    << (stack ? stack->currentIndex() : -1)
                    << " grabed=" << QFile::exists(QStringLiteral("/tmp/opencode/config-%1.png").arg(i + 1))
                    << "\n";
                if (stack) {
                    std::function<void(QObject *, const QString &)> dump;
                    dump = [&out, &dump](QObject *obj, const QString &indent) {
                        auto *w = qobject_cast<QWidget *>(obj);
                        if (!w)
                            return;
                        QString text;
                        if (auto *lbl = qobject_cast<QLabel *>(w))
                            text = QStringLiteral("lbl='%1'").arg(lbl->text());
                        else if (auto *chk = qobject_cast<QCheckBox *>(w))
                            text = QStringLiteral("chk='%1'").arg(chk->text());
                        else if (auto *btn = qobject_cast<QPushButton *>(w))
                            text = QStringLiteral("btn='%1'").arg(btn->text());
                        else if (auto *le = qobject_cast<QLineEdit *>(w))
                            text = QStringLiteral("le='%1'").arg(le->text());
                        else if (auto *cb = qobject_cast<QComboBox *>(w))
                            text = QStringLiteral("combo=%1 items").arg(cb->count());
                        else if (auto *gb = qobject_cast<QGroupBox *>(w))
                            text = QStringLiteral("group='%1'").arg(gb->title());
                        out << indent << w->metaObject()->className()
                            << " geo=" << w->geometry().x() << "," << w->geometry().y()
                            << " " << w->geometry().width() << "x" << w->geometry().height()
                            << (text.isEmpty() ? QString() : QStringLiteral(" ") + text)
                            << "\n";
                        for (QObject *child : obj->children())
                            dump(child, indent + QStringLiteral("  "));
                    };
                    out << "--- widgets página " << i << " ---\n";
                    dump(stack->currentWidget(), QString());
                }
            }
        }

        // Estado del proxy: iconos del sistema + filas de archivos
        if (!allDesktopViews().isEmpty()) {
            DesktopView *view = allDesktopViews().first();
            const AppSettings snap0 = globalSettings();
            out << "proxy system count="
                << (view->systemIconsVisible() ? view->systemIconsVisible() : -1)
                << "\n";
            out << "proxy rowCount="
                << (view->desktopRowCount() ? view->desktopRowCount() : -1)
                << "\n";
        }

        AppSettings snap = globalSettings();
        snap.showComputer = true;
        snap.showUser = true;
        snap.showNetwork = true;
        snap.showTrash = true;
        snap.widgetClock = true;
        snap.widgetSystem = true;
        snap.widgetNetwork = true;
        snap.widgetDisk = true;
        for (DesktopView *view : allDesktopViews())
            view->applySettings(snap);
        QCoreApplication::processEvents();

        if (!allDesktopViews().isEmpty()) {
            DesktopView *view = allDesktopViews().first();
            out << "after apply: proxy rowCount=" << view->desktopRowCount()
                << "\n";
            DesktopIconView *list = view->debugList();
            DesktopProxyModel *pm = view->debugProxy();
            out << "list model==proxy=" << (list && list->model() == pm)
                << " rootValid=" << list->rootIndex().isValid()
                << "\n";

            // Sin posiciones recordadas: los iconos deben apilarse en VERTICAL
            list->debugClearPositions();
            QCoreApplication::processEvents();
            for (int r = 0; pm && r < pm->rowCount(); ++r) {
                const QModelIndex idx = pm->index(r, 0);
                const QRect vr = list->visualRect(idx);
                out << "  row " << r << " '" << idx.data().toString()
                    << "' vr=" << vr.x() << "," << vr.y() << " " << vr.width() << "x" << vr.height()
                    << " sys=" << pm->isSystemIcon(idx)
                    << "\n";
            }
            view->grab().save(QStringLiteral("/tmp/opencode/desktop-widgets.png"));
            out << "widgets visible: clock=" << view->widgetClockVisible()
                << " system=" << view->widgetSystemVisible()
                << " network=" << view->widgetNetworkVisible()
                << " disk=" << view->widgetDiskVisible()
                << "\n";
            out << "widget geometry: " << view->widgetGeometryDump()
                << "\n";
            out << "view size=" << view->width() << "x" << view->height()
                << " list size=" << list->width() << "x" << list->height()
                << " viewport=" << list->viewport()->width() << "x" << list->viewport()->height()
                << "\n";

            // Renderiza cada fila de forma aislada con el delegate real
            for (int r = 0; pm && r < pm->rowCount(); ++r) {
                const QModelIndex idx = pm->index(r, 0);
                QPixmap cell(112, 96);
                cell.fill(QColor(30, 30, 30));   // oscuro: el texto del item es blanco
                QPainter cellPainter(&cell);
                QStyleOptionViewItem opt;
                opt.initFrom(list);
                opt.rect = QRect(0, 0, 112, 96);
                opt.state = QStyle::State_Enabled;
                list->itemDelegate()->paint(&cellPainter, opt, idx);
                cellPainter.end();
                cell.save(QStringLiteral("/tmp/opencode/row-%1.png").arg(r));
                out << "  cell " << r << " '" << idx.data().toString() << "' saved\n";
            }

            // --- lienzo de iconos: memoria y doble clic ------------------
            // 1) savePositions() debe persistir la posición de TODAS las filas
            list->savePositions();
            {
                QSettings s(configFilePath(), QSettings::IniFormat);
                s.beginGroup(QStringLiteral("IconPositions"));
                int total = 0;
                const QStringList groups = s.childGroups();
                for (const QString &g : groups) {
                    s.beginGroup(g);
                    total += s.childKeys().size();
                    s.endGroup();
                }
                const int expected = pm ? pm->rowCount() : 0;
                out << "saved icon positions groups=["
                    << groups.join(QLatin1Char(',')) << "] keys=" << total
                    << " expected=" << expected << "\n";
                s.endGroup();
            }

            // 2) Doble clic real sobre la primera fila de archivos → openItem
            //    (abre la carpeta/archivo con la app por defecto del sistema)
            int fileRow = -1;
            for (int r = 0; pm && r < pm->rowCount(); ++r) {
                const QModelIndex idx = pm->index(r, 0);
                if (!pm->isSystemIcon(idx)) {
                    fileRow = r;
                    break;
                }
            }
            if (fileRow >= 0) {
                const QPoint center = list->visualRect(pm->index(fileRow, 0)).center();
                QMouseEvent dbl(QEvent::MouseButtonDblClick,
                                QPointF(center),
                                QPointF(list->viewport()->mapToGlobal(center)),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &dbl);
                out << "dblclick row=" << fileRow
                    << " openItem calls=" << view->debugOpenCalls() << "\n";
            }

            // 3) Soltado interno (arrastrar un icono): recoloca la fila fuera
            //    del flujo vertical original y la recuerda en settings.ini
            if (fileRow >= 0) {
                QModelIndexList dragged;
                dragged << pm->index(fileRow, 0);
                const QPoint dropAt(300, 400);
                list->debugSimulateInternalDrop(dragged, dropAt);
                const QRect after = list->visualRect(pm->index(fileRow, 0));
                out << "dropped row=" << fileRow
                    << " newPos=" << after.x() << "," << after.y() << "\n";
                // ¿Quedó guardada?
                QSettings s(configFilePath(), QSettings::IniFormat);
                s.beginGroup(QStringLiteral("IconPositions"));
                QString sample;
                for (const QString &g : s.childGroups()) {
                    s.beginGroup(g);
                    sample = s.value(s.childKeys().value(0)).toString();
                    s.endGroup();
                    if (!sample.isEmpty())
                        break;
                }
                s.endGroup();
                out << "saved sample='" << sample << "'\n";
            }

            // 4) Ronda completa de memoria: al recargar desde disco, el icono
            //    soltado debe volver exactamente a donde se dejó
            if (fileRow >= 0) {
                list->debugClearPositions();
                list->reloadPositions();
                const QRect restored = list->visualRect(pm->index(fileRow, 0));
                out << "restored row=" << fileRow << " pos=" << restored.x()
                    << "," << restored.y() << "\n";
            }

            // 5) Clic sintético sobre un icono → selección (ratón propio)
            if (fileRow >= 0) {
                const QPoint c = list->visualRect(pm->index(fileRow, 0)).center();
                const QPointF g = QPointF(list->viewport()->mapToGlobal(c));
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(c), g,
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &press);
                QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(c), g,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &rel);
                out << "synthetic click row=" << fileRow
                    << " selected="
                    << list->selectionModel()->isSelected(pm->index(fileRow, 0))
                    << "\n";
            }

            // 6) Menú contextual de iconos del sistema: Equipo → Abrir +
            //    Propiedades; Papelera → + Vaciar la Papelera
            out << "menu Equipo=[" << view->debugSystemMenuLabels(0).join(QLatin1Char('|'))
                << "] trash=[" << view->debugSystemMenuLabels(3).join(QLatin1Char('|'))
                << "]\n";
            out << "openItem total calls=" << view->debugOpenCalls() << "\n";

            // 7) Tema de iconos: detección del sistema y tema activo
            const QPixmap computerPm =
                QIcon::fromTheme(QStringLiteral("computer")).pixmap(48, 48);
            computerPm.save(QStringLiteral("/tmp/opencode/computer-icon.png"));
            out << "theme system=\"" << systemIconThemeName()
                << "\" active=\"" << QIcon::themeName()
                << "\" computerNull=" << QIcon::fromTheme(QStringLiteral("computer")).isNull()
                << " trashNull=" << QIcon::fromTheme(QStringLiteral("user-trash")).isNull()
                << " computerPixmap=" << computerPm.width() << "x"
                << computerPm.height() << " null=" << computerPm.isNull()
                << "\n";
        }

        out.flush();
        diag.close();

        for (DesktopView *view : allDesktopViews())
            view->deleteLater();
        return 0;
    }
#endif

    return app.exec();
}

#include "stellarbox-windesktop.moc"