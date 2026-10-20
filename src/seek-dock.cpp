#include "seek-dock.hpp"
#include "i18n.hpp"
#include "window-activation.hpp"
#include "presentation-escape.hpp"

#include <QAction>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHoverEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPersistentModelIndex>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTextEdit>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOption>
#include <QTableView>
#include <QTimer>
#ifdef Q_OS_MACOS
#include <QTemporaryFile>
#endif
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <utility>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace obs_seeker {
namespace {
constexpr int SearchNameRole = Qt::UserRole + 1;
constexpr int FolderRole = Qt::UserRole + 2;
constexpr int NumberRole = Qt::UserRole + 3;
constexpr int TimeColumnMinTableWidth = 560;
constexpr int MaxSearchHistory = 50;
constexpr int RepeatOpenIntervalMs = 1000;
constexpr int FavoriteHitWidth = 24;
constexpr int ToolbarControlSize = 26;

class CompactStatusLabel final : public QLabel {
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return QSize(0, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

class ResultsTable final : public QTableView {
public:
    ResultsTable(NameFilter *filter, QWidget *parent) : QTableView(parent), filter_(filter) {}
    int hoveredRow() const { return hoveredRow_; }
protected:
    // 文件名搜索使用输入框。
    void keyboardSearch(const QString &) override {}
    bool viewportEvent(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove)
            hoverPosition_ = static_cast<QHoverEvent *>(event)->position().toPoint();
        else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave)
            hoverPosition_ = QPoint(-1, -1);
        return QTableView::viewportEvent(event);
    }
    void paintEvent(QPaintEvent *event) override
    {
        hoveredRow_ = indexAt(hoverPosition_).row();
        QTableView::paintEvent(event);
        if (!filter_ || !filter_->showsFavoriteSeparator())
            return;
        const auto rect = viewport()->rect();
        const auto first = rowAt(rect.top());
        if (first < 0)
            return;
        const auto bottom = rowAt(rect.bottom());
        const auto last = bottom < 0 ? model()->rowCount() - 1 : bottom;
        // 只检查可见行，不插入列表项。
        for (int row = first; row <= last; ++row) {
            const auto index = model()->index(row, 0);
            if (!index.data(FileModel::FavoriteRole).toBool())
                break;
            const auto next = model()->index(row + 1, 0);
            if (!next.isValid() || next.data(FileModel::FavoriteRole).toBool())
                continue;
            QPainter painter(viewport());
            QPen pen(QColor(128, 128, 128));
            painter.setPen(pen);
            const auto y = rowViewportPosition(row) + rowHeight(row) - 1;
            painter.drawLine(rect.left(), y, rect.right(), y);
            break;
        }
    }
private:
    QPointer<NameFilter> filter_;
    QPoint hoverPosition_{-1, -1};
    int hoveredRow_ = -1;
};

QString favoriteKey(const QString &path)
{
    const auto key = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    return key.toCaseFolded();
#else
    return key;
#endif
}

QPainterPath starPath()
{
    QPainterPath path;
    path.moveTo(8, 1);
    for (const auto &point : {QPointF(10.2, 5.7), QPointF(15.4, 6.1), QPointF(11.5, 9.6),
                              QPointF(12.7, 14.7), QPointF(8, 12.1), QPointF(3.3, 14.7),
                              QPointF(4.5, 9.6), QPointF(.6, 6.1), QPointF(5.8, 5.7)})
        path.lineTo(point);
    path.closeSubpath();
    return path;
}

QRect favoriteHitRect(const QRect &cell)
{
    return QRect(cell.left(), cell.top(), qMin(FavoriteHitWidth, cell.width()), cell.height());
}

class CompactScopeCombo final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const auto field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        // 长路径居中省略；展开列表和悬停仍可查看完整位置。
        option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideMiddle, qMax(0, field.width() - 2));
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

enum class PanelIcon { History, Favorites, Refresh, Settings };

QIcon panelIcon(const QWidget *widget, PanelIcon kind)
{
    constexpr int size = 16;
    const auto ratio = widget->devicePixelRatioF();
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        QPixmap pixmap(qRound(size * ratio), qRound(size * ratio));
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        const auto role = kind == PanelIcon::History ? QPalette::Text : QPalette::ButtonText;
        QPen pen(widget->palette().color(group, role), 1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        if (kind == PanelIcon::Favorites) {
            if (widget->property("checked").toBool())
                painter.setBrush(pen.color());
            painter.drawPath(starPath());
        } else if (kind == PanelIcon::Settings) {
            painter.drawEllipse(QRectF(4.5, 4.5, 7, 7));
            painter.drawEllipse(QRectF(7, 7, 2, 2));
            painter.translate(8, 8);
            for (int spoke = 0; spoke < 8; ++spoke) {
                painter.drawLine(QPointF(0, -3.5), QPointF(0, -6));
                painter.rotate(45);
            }
        } else {
            QPainterPath path;
            if (kind == PanelIcon::Refresh) {
                path.arcMoveTo(QRectF(3, 3, 10, 10), 0);
                path.arcTo(QRectF(3, 3, 10, 10), 0, -270);
                path.moveTo(5.5, 0.5);
                path.lineTo(8, 3);
                path.lineTo(5.5, 5.5);
            } else {
                path.moveTo(4.5, 6.5);
                path.lineTo(8, 10);
                path.lineTo(11.5, 6.5);
            }
            painter.drawPath(path);
        }
        painter.end();
        icon.addPixmap(pixmap, mode);
    }
    return icon;
}

class HighlightDelegate final : public QStyledItemDelegate {
public:
    HighlightDelegate(NameFilter *filter, QObject *parent, QListWidget *nativeList = nullptr)
        : QStyledItemDelegate(parent), filter_(filter), nativeList_(nativeList) {}
    void setTokens(QStringList tokens) { tokens_ = std::move(tokens); }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const bool file = index.data(FileModel::PathRole).isValid();
        const bool nameCell = !file || index.column() == 0;
        if (file) {
            const auto *table = static_cast<const ResultsTable *>(parent());
            styled.widget = nativeList_;
            styled.styleObject = nativeList_;
            styled.palette = nativeList_->palette();
            styled.viewItemPosition = QStyleOptionViewItem::OnlyOne;
            styled.showDecorationSelected = true;
            styled.state.setFlag(QStyle::State_MouseOver, table->hoveredRow() == index.row());
            styled.state.setFlag(QStyle::State_HasFocus, table->hasFocus() && table->currentIndex().row() == index.row());
        }
        const auto original = styled.text;
        auto *style = styled.widget ? styled.widget->style() : QApplication::style();
        auto textStyle = styled;
        if (file && nameCell)
            textStyle.rect.adjust(FavoriteHitWidth, 0, 0, 0);
        const auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &textStyle, styled.widget);
        auto background = styled;
        background.text.clear();
        if (file) {
            // 两列共用原生列表的整行背景，圆角和边框只出现在行的外侧。
            background.rect.setLeft(0);
            background.rect.setRight(static_cast<const ResultsTable *>(parent())->viewport()->width() - 1);
        }
        painter->save();
        painter->setClipRect(option.rect, Qt::IntersectClip);
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, styled.widget);
        painter->restore();
        if (file && nameCell) {
            painter->save();
            painter->setClipRect(option.rect);
            painter->setRenderHint(QPainter::Antialiasing);
            const auto group = styled.state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled;
            const auto color = styled.palette.color(group, styled.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
            painter->setPen(QPen(color, 1.25));
            painter->setBrush(index.data(FileModel::FavoriteRole).toBool() ? QBrush(color) : QBrush(Qt::NoBrush));
            const auto size = qMin(16, qMax(0, option.rect.height() - 4));
            painter->translate(option.rect.left() + (FavoriteHitWidth - size) / 2.0, option.rect.center().y() - size / 2.0);
            painter->scale(size / 16.0, size / 16.0);
            painter->drawPath(starPath());
            painter->restore();
        }
        const auto visible = styled.fontMetrics.elidedText(original, styled.textElideMode, rect.width());
        QTextLayout layout(visible, styled.font, painter->device());
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::NoWrap);
        textOption.setAlignment(styled.displayAlignment);
        layout.setTextOption(textOption);
        QList<QTextLayout::FormatRange> formats;
        for (const auto &range : highlightRanges(visible, nameCell ? (filter_ ? filter_->tokens() : tokens_) : QStringList{})) {
            QTextLayout::FormatRange format;
            format.start = range.start;
            format.length = range.length;
            format.format.setBackground(QColor(247, 215, 95));
            format.format.setForeground(QColor(31, 35, 40));
            format.format.setFontWeight(QFont::Bold);
            formats.append(format);
        }
        layout.setFormats(formats);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid())
            line.setLineWidth(rect.width());
        layout.endLayout();
        painter->save();
        painter->setClipRect(rect);
        painter->setPen(styled.palette.color((styled.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
        layout.draw(painter, QPointF(rect.left(), rect.top() + (rect.height() - layout.boundingRect().height()) / 2.0));
        painter->restore();
    }
private:
    QPointer<NameFilter> filter_;
    QPointer<QListWidget> nativeList_;
    QStringList tokens_;
};

bool blocksHomeEscape(const QWidget *widget)
{
    for (; widget; widget = widget->parentWidget()) {
        if (qobject_cast<const QDialog *>(widget) || qobject_cast<const QMenu *>(widget))
            return true;
        if (const auto *edit = qobject_cast<const QLineEdit *>(widget); edit && !edit->isReadOnly())
            return true;
        if (const auto *edit = qobject_cast<const QTextEdit *>(widget); edit && !edit->isReadOnly())
            return true;
        if (const auto *edit = qobject_cast<const QPlainTextEdit *>(widget); edit && !edit->isReadOnly())
            return true;
        if (const auto *spin = qobject_cast<const QAbstractSpinBox *>(widget); spin && !spin->isReadOnly())
            return true;
        if (const auto *combo = qobject_cast<const QComboBox *>(widget); combo && combo->isEditable())
            return true;
    }
    return false;
}

void showMessage(QWidget *parent, const QString &title, const QString &text,
                 QMessageBox::Icon icon = QMessageBox::Warning)
{
    auto *box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

class FolderList final : public QListWidget {
public:
    using QListWidget::QListWidget;
    void addFolders(const QStringList &folders)
    {
        QStringList paths;
        for (int row = 0; row < count(); ++row)
            paths.append(item(row)->text());
        paths.append(folders);
        clear();
        for (const auto &path : normalizedFolders(paths))
            addItem(QDir::toNativeSeparators(path));
        setUnavailableFolders(unavailableFolders_);
    }
    void setUnavailableFolders(const QStringList &folders)
    {
        unavailableFolders_ = folders;
        const auto warning = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        for (int row = 0; row < count(); ++row) {
            auto *folder = item(row);
            const bool unavailable = unavailableFolders_.contains(folder->text());
            folder->setIcon(unavailable ? warning : QIcon());
            folder->setToolTip(unavailable ? uiText("Settings.FolderUnavailable") : QString());
        }
    }
protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->source() == this) {
            QListWidget::dragEnterEvent(event);
            return;
        }
        if (folderUrls(event->mimeData()).isEmpty()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->source() == this) {
            QListWidget::dragMoveEvent(event);
            return;
        }
        if (!event->mimeData()->hasUrls()) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
    void dropEvent(QDropEvent *event) override
    {
        // 列表内移动交给 Qt，顺序在点击保存后写入。
        if (event->source() == this) {
            QListWidget::dropEvent(event);
            return;
        }
        const auto folders = folderUrls(event->mimeData());
        if (folders.isEmpty()) {
            event->ignore();
            return;
        }
        addFolders(folders);
        // 拖入只添加搜索位置，始终以复制动作回应。
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
private:
    QStringList unavailableFolders_;
    static QStringList folderUrls(const QMimeData *mime)
    {
        QStringList folders;
        for (const auto &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const auto path = url.toLocalFile();
            const QFileInfo folder(path);
            if (QDir::isAbsolutePath(path) && folder.isDir() && !isKeynotePackage(folder))
                folders.append(path);
        }
        return folders;
    }
};

class SettingsDialog final : public QDialog {
public:
    SettingsDialog(const Settings &initial, const QString &settingsPath, const ShortcutSummary &shortcuts,
                   const QStringList &unavailableFolders,
                   const QList<SceneChoice> &scenes,
                   QWidget *parent)
        : QDialog(parent), value(initial)
    {
        setObjectName(QStringLiteral("obsSeekerSettings"));
        setWindowTitle(uiText("Settings.Title"));
        resize(500, 560);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(6);
        auto *folders = new FolderList(this);
        folders->setObjectName(QStringLiteral("folderList"));
        folders->setSelectionMode(QAbstractItemView::ExtendedSelection);
        folders->setDragDropMode(QAbstractItemView::InternalMove);
        folders->setDefaultDropAction(Qt::MoveAction);
        folders->setIconSize(QSize(16, 16));
        folders->setToolTip(uiText("Settings.DropFolders"));
        for (const auto &folder : initial.folders)
            folders->addItem(QDir::toNativeSeparators(folder));
        folders->setUnavailableFolders(unavailableFolders);
        // 固定预留七行，增删位置时不调整窗口高度。
        const auto folderRowHeight = qMax(folders->sizeHintForRow(0),
            qMax(folders->fontMetrics().height(), folders->iconSize().height()) + 4);
        folders->setMinimumHeight(7 * folderRowHeight + 2 * folders->frameWidth());
        auto *folderTitle = new QLabel(this);
        folderTitle->setObjectName(QStringLiteral("folderCountLabel"));
        const auto updateFolderCount = [folders, folderTitle]() {
            folderTitle->setText(uiText("Settings.Folders").arg(folders->count()));
        };
        updateFolderCount();
        connect(folders->model(), &QAbstractItemModel::rowsInserted, folderTitle, updateFolderCount);
        connect(folders->model(), &QAbstractItemModel::rowsRemoved, folderTitle, updateFolderCount);
        connect(folders->model(), &QAbstractItemModel::modelReset, folderTitle, updateFolderCount);
        auto *buttons = new QHBoxLayout;
        buttons->addWidget(folderTitle);
        buttons->addStretch();
        auto *add = new QPushButton(uiText("Settings.AddFolder"), this);
        add->setObjectName(QStringLiteral("addFolderButton"));
        auto *remove = new QPushButton(uiText("Settings.RemoveFolder"), this);
        remove->setObjectName(QStringLiteral("removeFolderButton"));
        buttons->addWidget(add);
        buttons->addWidget(remove);
        layout->addLayout(buttons);
        layout->addWidget(folders, 1);
        connect(add, &QPushButton::clicked, this, [this, folders]() {
            const auto start = folders->count() ? folders->item(0)->text() : QString();
            auto *picker = new QFileDialog(this, uiText("Settings.ChooseFolder"), start);
            picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->setFileMode(QFileDialog::Directory);
            picker->setOption(QFileDialog::ShowDirsOnly);
            connect(picker, &QFileDialog::fileSelected, this, [folders](const QString &folder) {
                folders->addFolders({folder});
            });
            picker->open();
        });
        connect(remove, &QPushButton::clicked, this, [folders]() {
            const auto selected = folders->selectedItems();
            for (auto *item : selected)
                delete folders->takeItem(folders->row(item));
        });
        connect(folders, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            const auto path = QDir::fromNativeSeparators(item->text());
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                showMessage(this, uiText("Error.OpenFolder.Title"), item->text());
        });
        auto *recursive = new QCheckBox(uiText("Settings.IncludeSubfolders"), this);
        recursive->setObjectName(QStringLiteral("recursiveCheck"));
        recursive->setChecked(initial.recursive);
        auto *autoRefresh = new QCheckBox(uiText("Settings.AutoRefresh"), this);
        autoRefresh->setObjectName(QStringLiteral("autoRefreshCheck"));
        autoRefresh->setToolTip(uiText("Settings.AutoRefreshTooltip"));
        autoRefresh->setChecked(initial.autoRefresh);
        auto *favoritesFirst = new QCheckBox(uiText("Settings.FavoritesFirst"), this);
        favoritesFirst->setObjectName(QStringLiteral("favoritesFirstCheck"));
        favoritesFirst->setToolTip(uiText("Settings.FavoritesFirstHint"));
        favoritesFirst->setChecked(initial.favoritesFirst);
        auto *options = new QHBoxLayout;
        options->addWidget(recursive, 1);
        options->addWidget(autoRefresh, 1);
        options->addWidget(favoritesFirst, 1);
        layout->addLayout(options);
        auto *form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        const auto setHintColor = [](QLabel *label) {
            label->setForegroundRole(QPalette::PlaceholderText);
            auto colors = label->palette();
            colors.setColor(QPalette::PlaceholderText, QColor(128, 128, 128));
            label->setPalette(colors);
        };
        auto *pollLabel = new QLabel(uiText("Settings.PollInterval"), this);
        pollLabel->setObjectName(QStringLiteral("pollIntervalLabel"));
        auto *pollField = new QWidget(this);
        auto *pollRow = new QHBoxLayout(pollField);
        pollRow->setContentsMargins(0, 0, 0, 0);
        auto *pollInterval = new QSpinBox(pollField);
        pollInterval->setObjectName(QStringLiteral("pollIntervalSpin"));
        pollInterval->setRange(MinPollIntervalSeconds, MaxPollIntervalSeconds);
        pollInterval->setValue(initial.pollIntervalSeconds);
        pollInterval->setFixedWidth(92);
        pollInterval->setToolTip(uiText("Settings.PollHint"));
        pollLabel->setBuddy(pollInterval);
        pollLabel->setEnabled(initial.autoRefresh);
        pollInterval->setEnabled(initial.autoRefresh);
        connect(autoRefresh, &QCheckBox::toggled, pollLabel, &QWidget::setEnabled);
        connect(autoRefresh, &QCheckBox::toggled, pollInterval, &QWidget::setEnabled);
        pollRow->addWidget(pollInterval);
        auto *pollHint = new QLabel(uiText("Settings.PollHint"), pollField);
        pollHint->setObjectName(QStringLiteral("pollHintLabel"));
        setHintColor(pollHint);
        pollHint->setWordWrap(true);
        pollHint->setEnabled(initial.autoRefresh);
        connect(autoRefresh, &QCheckBox::toggled, pollHint, &QWidget::setEnabled);
        pollRow->addWidget(pollHint, 1);
        form->addRow(pollLabel, pollField);
        layout->addLayout(form);
        auto *typesLabel = new QLabel(uiText("Settings.FileTypes"), this);
        typesLabel->setObjectName(QStringLiteral("fileTypesLabel"));
        auto *extensions = new QLineEdit(initial.extensions.join(QStringLiteral(";")), this);
        extensions->setObjectName(QStringLiteral("extensionsEdit"));
        extensions->setPlaceholderText(Settings{}.extensions.join(QLatin1Char(';')));
        typesLabel->setBuddy(extensions);
        auto *typesHint = new QLabel(uiText("Settings.FileTypeOrder"), this);
        typesHint->setObjectName(QStringLiteral("fileTypeOrderHintLabel"));
        setHintColor(typesHint);
        extensions->setToolTip(typesHint->text());
        auto *typesHeading = new QHBoxLayout;
        typesHeading->addWidget(typesLabel);
        typesHeading->addWidget(typesHint);
        typesHeading->addStretch();
        layout->addLayout(typesHeading);
        layout->addWidget(extensions);
        const auto addSeparator = [this, layout](const char *name) {
            auto *line = new QFrame(this);
            line->setObjectName(QString::fromLatin1(name));
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Plain);
            line->setFixedHeight(1);
            line->setStyleSheet(QStringLiteral("QFrame { border: none; background-color: #404040; }"));
            layout->addSpacing(4);
            layout->addWidget(line);
            layout->addSpacing(4);
        };
        addSeparator("sceneOptionsSeparator");
        auto *switchScene = new QCheckBox(uiText("Settings.SwitchScene"), this);
        switchScene->setObjectName(QStringLiteral("switchSceneCheck"));
        switchScene->setToolTip(uiText("Settings.PresentationHint"));
        switchScene->setChecked(initial.switchSceneOnOpen);
        auto *escapeHome = new QCheckBox(uiText("Settings.EscapeHome"), this);
        escapeHome->setObjectName(QStringLiteral("escapeHomeCheck"));
        escapeHome->setToolTip(uiText("Settings.EscapeHomeHint"));
        escapeHome->setChecked(initial.escapeReturnsHome);

        auto *sceneActions = new QHBoxLayout;
        sceneActions->addWidget(switchScene);
        sceneActions->addStretch();
        sceneActions->addWidget(escapeHome);
        layout->addLayout(sceneActions);
        auto *sceneOptions = new QWidget(this);
        auto *sceneRow = new QHBoxLayout(sceneOptions);
        sceneRow->setContentsMargins(0, 0, 0, 0);
        auto *sceneLabel = new QLabel(uiText("Settings.TargetScene"), sceneOptions);
        auto *targetScene = new QComboBox(sceneOptions);
        targetScene->setObjectName(QStringLiteral("targetSceneCombo"));
        targetScene->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        targetScene->setMinimumContentsLength(8);
        targetScene->addItem(uiText("Settings.ScenePlaceholder"), QString());
        for (const auto &scene : scenes)
            targetScene->addItem(scene.name, scene.id);
        if (!initial.targetSceneId.isEmpty() && targetScene->findData(initial.targetSceneId) < 0)
            targetScene->addItem(uiText("Settings.SceneUnavailable"), initial.targetSceneId);
        targetScene->setCurrentIndex(qMax(0, targetScene->findData(initial.targetSceneId)));
        sceneLabel->setBuddy(targetScene);
        auto *delayLabel = new QLabel(uiText("Settings.SceneDelay"), sceneOptions);
        auto *sceneDelay = new QDoubleSpinBox(sceneOptions);
        sceneDelay->setObjectName(QStringLiteral("sceneDelaySpin"));
        sceneDelay->setDecimals(1);
        sceneDelay->setRange(MinSceneDelayMs / 1000.0, MaxSceneDelayMs / 1000.0);
        sceneDelay->setSingleStep(0.5);
        sceneDelay->setValue(initial.sceneDelayMs / 1000.0);
        sceneDelay->setFixedWidth(92);
        delayLabel->setBuddy(sceneDelay);
        sceneRow->addWidget(sceneLabel);
        sceneRow->addWidget(targetScene, 1);
        sceneRow->addWidget(delayLabel);
        sceneRow->addWidget(sceneDelay);
        const auto updateSceneOptions = [=] {
            sceneLabel->setEnabled(switchScene->isChecked());
            targetScene->setEnabled(switchScene->isChecked());
            const bool delayEnabled = switchScene->isChecked() || escapeHome->isChecked();
            delayLabel->setEnabled(delayEnabled);
            sceneDelay->setEnabled(delayEnabled);
        };
        connect(switchScene, &QCheckBox::toggled, sceneOptions, updateSceneOptions);
        connect(escapeHome, &QCheckBox::toggled, sceneOptions, updateSceneOptions);
        updateSceneOptions();
        layout->addWidget(sceneOptions);
        addSeparator("shortcutTopSeparator");
        auto *shortcutGrid = new QGridLayout;
        shortcutGrid->setHorizontalSpacing(16);
        shortcutGrid->setColumnStretch(1, 1);
        shortcutGrid->setColumnStretch(3, 1);
        const auto addShortcut = [this, shortcutGrid](int row, int column, const char *title, const char *name, const QString &binding) {
            auto *label = new QLabel(uiText(title), this);
            auto *keys = new QLabel(binding.isEmpty() ? uiText("Settings.ShortcutUnset") : binding, this);
            keys->setObjectName(QString::fromLatin1(name));
            keys->setTextFormat(Qt::PlainText);
            keys->setTextInteractionFlags(Qt::TextSelectableByMouse);
            keys->setWordWrap(true);
            keys->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            keys->setToolTip(QStringLiteral("<qt>") + keys->text().toHtmlEscaped() + QStringLiteral("</qt>"));
            shortcutGrid->addWidget(label, row, column, Qt::AlignTop);
            shortcutGrid->addWidget(keys, row, column + 1);
        };
        addShortcut(0, 0, "Settings.ShortcutSearch", "searchShortcutValue", shortcuts.search);
        addShortcut(0, 2, "Settings.ShortcutRefresh", "refreshShortcutValue", shortcuts.refresh);
        addShortcut(1, 0, "Settings.ShortcutFavorite", "favoriteShortcutValue", shortcuts.favorite);
        addShortcut(1, 2, "Settings.ShortcutFavoritesView", "favoritesViewShortcutValue", shortcuts.favoritesView);
        layout->addLayout(shortcutGrid);
        addSeparator("shortcutBottomSeparator");
        auto *box = new QDialogButtonBox(this);
        box->addButton(uiText("Settings.Save"), QDialogButtonBox::AcceptRole);
        box->addButton(uiText("Settings.Cancel"), QDialogButtonBox::RejectRole);
        auto *footer = new QHBoxLayout;
        const auto versionText = uiText("Settings.Version").arg(QStringLiteral(OBS_SEEKER_VERSION)).toHtmlEscaped()
            .replace(QStringLiteral("haliluya.net"), QStringLiteral("<a href=\"https://haliluya.net\">haliluya.net</a>"));
        auto *version = new QLabel(versionText, this);
        version->setObjectName(QStringLiteral("pluginVersionLabel"));
        version->setTextFormat(Qt::RichText);
        version->setTextInteractionFlags(Qt::TextBrowserInteraction);
        version->setOpenExternalLinks(true);
        footer->addWidget(version);
        footer->addStretch();
        footer->addWidget(box);
        layout->addLayout(footer);
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(box, &QDialogButtonBox::accepted, this, [=]() {
            auto candidate = value;
            candidate.folders.clear();
            for (int row = 0; row < folders->count(); ++row)
                candidate.folders.append(folders->item(row)->text());
            candidate.folders = normalizedFolders(candidate.folders);
            candidate.recursive = recursive->isChecked();
            candidate.autoRefresh = autoRefresh->isChecked();
            candidate.favoritesFirst = favoritesFirst->isChecked();
            candidate.pollIntervalSeconds = pollInterval->value();
            candidate.switchSceneOnOpen = switchScene->isChecked();
            candidate.escapeReturnsHome = escapeHome->isChecked();
            candidate.targetSceneId = targetScene->currentData().toString();
            candidate.sceneDelayMs = qRound(sceneDelay->value() * 1000);
            candidate.extensions = parseExtensions(extensions->text());
            if (candidate.extensions.isEmpty()) {
                showMessage(this, uiText("Error.InvalidFileTypes.Title"), uiText("Error.InvalidFileTypes.Message"));
                return;
            }
            QString error;
            if (!saveSettings(settingsPath, candidate, &error)) {
                showMessage(this, uiText("Error.SaveSettings.Title"), error);
                return;
            }
            value = std::move(candidate);
            accept();
        });
    }
    Settings value;
};
} // namespace

FileModel::FileModel(QObject *parent) : QAbstractTableModel(parent) {}
int FileModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(files_.size());
}
int FileModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 2; }

QVariant FileModel::data(const QModelIndex &index, int role) const
{
    const auto *file = fileAt(index.row());
    if (!index.isValid() || !file)
        return {};
    if (role == SearchNameRole)
        return file->searchableName;
    if (role == FolderRole)
        return file->canonicalFolder.isEmpty() ? file->folder : file->canonicalFolder;
    if (role == NumberRole)
        return numberKeys_.value(index.row());
    if (role == BaseNameRole)
        return baseNameKeys_.at(index.row());
    if (role == ExtensionRole)
        return extensionKeys_.at(index.row());
    if (role == PathRole)
        return file->path;
    if (role == FavoriteRole)
        return favoriteOrders_.at(index.row()) != 0;
    if (role == FavoriteOrderRole)
        return favoriteOrders_.at(index.row());
    if (role == Qt::AccessibleTextRole && index.column() == 0)
        return file->name + (favoriteOrders_.at(index.row()) ? QStringLiteral(" · ") + uiText("Favorite.Marked") : QString());
    if (role == Qt::ToolTipRole && index.column() == 1) {
        const auto folder = QDir::toNativeSeparators(file->folder)
            .replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
        return QStringLiteral("<p style=\"white-space:pre\">%1</p>").arg(folder.toHtmlEscaped());
    }
    if (role == Qt::TextAlignmentRole && index.column() == 1)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case 0: return file->name;
    case 1: return QDateTime::fromSecsSinceEpoch(file->modified).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    default: return {};
    }
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0: return uiText("Column.FileName");
    case 1: return uiText("Column.Modified");
    default: return {};
    }
}

void FileModel::setFiles(QList<FileEntry> files)
{
    beginResetModel();
    files_ = std::move(files);
    numberKeys_.clear();
    numberKeys_.reserve(files_.size());
    baseNameKeys_.clear();
    baseNameKeys_.reserve(files_.size());
    extensionKeys_.clear();
    extensionKeys_.reserve(files_.size());
    favoriteOrders_.clear();
    favoriteOrders_.reserve(files_.size());
    for (const auto &file : files_) {
        numberKeys_.append(leadingNumberKey(file.searchableName));
        const auto dot = file.searchableName.lastIndexOf(QLatin1Char('.'));
        baseNameKeys_.append(dot < 0 ? file.searchableName : file.searchableName.left(dot));
        extensionKeys_.append(dot < 0 ? QString() : file.searchableName.mid(dot + 1));
        // 零表示未收藏，正数记录顺序。
        favoriteOrders_.append(favorites_.value(favoriteKey(file.path)));
    }
    endResetModel();
}

const FileEntry *FileModel::fileAt(int row) const
{
    return row >= 0 && row < files_.size() ? &files_.at(row) : nullptr;
}

bool FileModel::toggleFavorite(const QString &path)
{
    const auto key = favoriteKey(path);
    for (int row = 0; row < files_.size(); ++row) {
        if (favoriteKey(files_.at(row).path) != key)
            continue;
        if (!favorites_.remove(key))
            favorites_.insert(key, nextFavoriteOrder_++);
        favoriteOrders_[row] = favorites_.value(key);
        // 排序角色也需通知，确保收藏变化后立即重排。
        emit dataChanged(index(row, 0), index(row, columnCount() - 1), {FavoriteRole, FavoriteOrderRole, Qt::DisplayRole, Qt::AccessibleTextRole});
        return true;
    }
    return false;
}

bool FileModel::isFavorite(const QString &path) const
{
    return favorites_.contains(favoriteKey(path));
}

int FileModel::removeFavorites(const QStringList &paths)
{
    int removed = 0;
    for (const auto &path : paths)
        removed += favorites_.remove(favoriteKey(path));
    if (!removed)
        return 0;
    int first = -1, last = -1;
    for (int row = 0; row < files_.size(); ++row) {
        if (favoriteOrders_.at(row) && !favorites_.contains(favoriteKey(files_.at(row).path))) {
            favoriteOrders_[row] = 0;
            if (first < 0)
                first = row;
            last = row;
        }
    }
    // 批量取消只通知一次，避免逐项筛选和重排。
    if (first >= 0)
        emit dataChanged(index(first, 0), index(last, columnCount() - 1),
                         {FavoriteRole, FavoriteOrderRole, Qt::DisplayRole, Qt::AccessibleTextRole});
    return removed;
}

bool FileModel::moveFavoriteOrder(const QList<int> &rows, bool toEnd)
{
    if (rows.size() < 2)
        return false;
    QList<quint64> orders;
    orders.reserve(rows.size());
    int first = rows.first();
    int last = first;
    for (const auto row : rows) {
        if (!fileAt(row) || !favoriteOrders_.at(row)
            || (!orders.isEmpty() && favoriteOrders_.at(row) <= orders.last()))
            return false;
        orders.append(favoriteOrders_.at(row));
        first = qMin(first, row);
        last = qMax(last, row);
    }
    // 轮换已有序号，保留其他收藏的相对顺序。
    std::rotate(orders.begin(), toEnd ? orders.end() - 1 : orders.begin() + 1, orders.end());
    for (qsizetype position = 0; position < rows.size(); ++position) {
        const auto row = rows.at(position);
        favoriteOrders_[row] = orders.at(position);
        favorites_.insert(favoriteKey(files_.at(row).path), orders.at(position));
    }
    emit dataChanged(index(first, 0), index(last, columnCount() - 1),
                     {FavoriteOrderRole, Qt::DisplayRole});
    return true;
}

NameFilter::NameFilter(QObject *parent) : QSortFilterProxyModel(parent)
{
    setFilterRole(FileModel::FavoriteRole);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setSortLocaleAware(true);
    nameCollator_.setCaseSensitivity(Qt::CaseInsensitive);
}
void NameFilter::setSourceModel(QAbstractItemModel *model)
{
    if (sourceModel() == model)
        return;
    for (const auto &connection : countConnections_)
        disconnect(connection);
    countConnections_.clear();
    scopeCount_ = -1;
    if (model) {
        const auto clearCount = [this]() { scopeCount_ = -1; };
        countConnections_.append(connect(model, &QAbstractItemModel::modelReset, this, clearCount));
        countConnections_.append(connect(model, &QAbstractItemModel::rowsInserted, this, clearCount));
        countConnections_.append(connect(model, &QAbstractItemModel::rowsRemoved, this, clearCount));
        countConnections_.append(connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
                if (roles.isEmpty() || roles.contains(FileModel::FavoriteRole) || roles.contains(FolderRole))
                    scopeCount_ = -1;
            }));
        countConnections_.append(connect(model, &QObject::destroyed, this, clearCount));
    }
    QSortFilterProxyModel::setSourceModel(model);
}
void NameFilter::setQuery(const QString &query)
{
    const auto tokens = queryTokens(query);
    if (tokens_ == tokens)
        return;
    tokens_ = tokens;
    const auto numeric = tokens_.size() == 1 ? numberKey(tokens_.first()) : QString();
    const bool rankingChanged = numeric != numericQuery_;
    numericQuery_ = numeric;
    if (rankingChanged)
        invalidate();
    else
        invalidateFilter();
}
void NameFilter::setFolderScope(const QString &folder)
{
    folderScope_ = folder;
    scopeCount_ = -1;
    invalidateFilter();
}
void NameFilter::setFavoritesOnly(bool enabled)
{
    if (favoritesOnly_ == enabled)
        return;
    favoritesOnly_ = enabled;
    scopeCount_ = -1;
    invalidate();
}
void NameFilter::setFavoritesFirst(bool enabled)
{
    if (favoritesFirst_ == enabled)
        return;
    favoritesFirst_ = enabled;
    invalidate();
}
bool NameFilter::acceptsFolder(const QModelIndex &index) const
{
    return folderScope_.isEmpty() || folderContains(folderAliases_.value(folderScope_, folderScope_), index.data(FolderRole).toString());
}

void NameFilter::setExtensionOrder(const QStringList &extensions)
{
    extensionOrder_.clear();
    for (const auto &extension : parseExtensions(extensions.join(QLatin1Char(';'))))
        extensionOrder_.insert(extension, extensionOrder_.size());
    invalidate();
}
bool NameFilter::acceptsScope(const QModelIndex &index) const
{
    return acceptsFolder(index) && (!favoritesOnly_ || index.data(FileModel::FavoriteRole).toBool());
}
void NameFilter::setFolderAliases(QHash<QString, QString> aliases)
{
    folderAliases_ = std::move(aliases);
    scopeCount_ = -1;
    invalidateFilter();
}
int NameFilter::scopedFileCount() const
{
    if (!sourceModel())
        return 0;
    if (folderScope_.isEmpty() && !favoritesOnly_)
        return sourceModel()->rowCount();
    // 计数跟随数据和筛选范围变化，搜索输入只读取缓存。
    if (scopeCount_ < 0) {
        scopeCount_ = 0;
        for (int row = 0; row < sourceModel()->rowCount(); ++row)
            scopeCount_ += acceptsScope(sourceModel()->index(row, 0));
    }
    return scopeCount_;
}
bool NameFilter::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const auto index = sourceModel()->index(row, 0, parent);
    // 数字和文字都按包含关系匹配，保留所有相关文件。
    return acceptsScope(index) && matchesName(index.data(SearchNameRole).toString(), tokens_);
}
bool NameFilter::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (!numericQuery_.isEmpty()) {
        const bool leftPreferred = left.data(NumberRole).toString() == numericQuery_;
        const bool rightPreferred = right.data(NumberRole).toString() == numericQuery_;
        // 同编号优先，收藏只在同一匹配组内前置。
        if (leftPreferred != rightPreferred)
            return sortOrder() == Qt::AscendingOrder ? leftPreferred : rightPreferred;
    }
    if (favoritesFirst_ || favoritesOnly_) {
        const auto leftOrder = left.data(FileModel::FavoriteOrderRole).toULongLong();
        const auto rightOrder = right.data(FileModel::FavoriteOrderRole).toULongLong();
        const bool leftFavorite = leftOrder != 0;
        const bool rightFavorite = rightOrder != 0;
        // 同组收藏优先，不受升降序影响。
        if (leftFavorite != rightFavorite)
            return sortOrder() == Qt::AscendingOrder ? leftFavorite : rightFavorite;
        if (leftFavorite && rightFavorite && leftOrder != rightOrder)
            return sortOrder() == Qt::AscendingOrder ? leftOrder < rightOrder : leftOrder > rightOrder;
    }
    if (sortColumn() == 0 && !extensionOrder_.isEmpty()) {
        const auto leftBase = left.data(FileModel::BaseNameRole).toString();
        const auto rightBase = right.data(FileModel::BaseNameRole).toString();
        // 同名分组，扩展名按填写顺序排列。
        if (leftBase != rightBase) {
            const auto order = nameCollator_.compare(leftBase, rightBase);
            return order ? order < 0 : leftBase < rightBase;
        }
        const auto leftRank = extensionOrder_.value(left.data(FileModel::ExtensionRole).toString(), extensionOrder_.size());
        const auto rightRank = extensionOrder_.value(right.data(FileModel::ExtensionRole).toString(), extensionOrder_.size());
        if (leftRank != rightRank)
            return sortOrder() == Qt::AscendingOrder ? leftRank < rightRank : leftRank > rightRank;
    }
    return QSortFilterProxyModel::lessThan(left, right);
}

ScanThread::ScanThread(Settings settings, QObject *parent) : QThread(parent), settings_(std::move(settings)) {}
void ScanThread::cancel() { cancelled_.store(true, std::memory_order_relaxed); }
void ScanThread::run() { result = scanFolders(settings_, cancelled_); }

SearchDock::SearchDock(QString settingsPath, QWidget *parent, std::function<QStringList(const QString &)> command)
    : QWidget(parent), settingsPath_(std::move(settingsPath)), mainWindow_(parent ? parent->window() : this),
      presentationCommand_(std::move(command))
{
    setObjectName(QStringLiteral("obsSeekerPanel"));
    setMinimumSize(300, 160);
    QString settingsError;
    settings_ = loadSettings(settingsPath_, &settingsError);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("searchInput"));
    search_->setPlaceholderText(uiText("Search.Placeholder"));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(uiText("Search.Tooltip") + QLatin1Char('\n') + uiText("Panel.Help"));
    search_->setMinimumWidth(90);
    search_->setFixedHeight(ToolbarControlSize);
    search_->setStyleSheet(QStringLiteral("QLineEdit { margin-top: 0; margin-bottom: 0; }"));
    historyAction_ = search_->addAction(QIcon(), QLineEdit::TrailingPosition);
    historyAction_->setObjectName(QStringLiteral("searchHistoryAction"));
    historyAction_->setText(uiText("Search.History"));
    historyAction_->setToolTip(uiText("Search.HistoryTooltip"));
    connect(historyAction_, &QAction::triggered, this, &SearchDock::showSearchHistory);
    bar->addWidget(search_, 3);
    scopeRow_ = new QWidget(this);
    scopeRow_->setObjectName(QStringLiteral("scopeRow"));
    auto *scopeLayout = new QHBoxLayout(scopeRow_);
    scopeLayout->setContentsMargins(0, 0, 0, 0);
    scope_ = new CompactScopeCombo(scopeRow_);
    scope_->setObjectName(QStringLiteral("folderScopeCombo"));
    scope_->setAccessibleName(uiText("Column.Location"));
    scope_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    scope_->setMinimumContentsLength(6);
    scope_->setFixedHeight(ToolbarControlSize);
    scope_->setStyleSheet(QStringLiteral("QComboBox { margin-top: 0; margin-bottom: 0; }"));
    scopeLayout->addWidget(scope_, 1);
    bar->addWidget(scopeRow_, 2);
    favoritesButton_ = new QToolButton(this);
    favoritesButton_->setObjectName(QStringLiteral("favoritesButton"));
    favoritesButton_->setText(uiText("Favorite.ShowOnly"));
    favoritesButton_->setAccessibleName(uiText("Favorite.ShowOnly"));
    favoritesButton_->setCheckable(true);
    favoritesButton_->setAutoRaise(true);
    favoritesButton_->setIconSize(QSize(16, 16));
    bar->addWidget(favoritesButton_);
    refreshButton_ = new QToolButton(this);
    refreshButton_->setObjectName(QStringLiteral("refreshButton"));
    refreshButton_->setText(uiText("Action.Refresh"));
    refreshButton_->setToolTip(uiText("Action.Refresh"));
    refreshButton_->setAccessibleName(uiText("Action.Refresh"));
    refreshButton_->setAutoRaise(true);
    refreshButton_->setIconSize(QSize(16, 16));
    bar->addWidget(refreshButton_);
    settingsButton_ = new QToolButton(this);
    settingsButton_->setObjectName(QStringLiteral("settingsButton"));
    settingsButton_->setText(uiText("Action.Settings"));
    settingsButton_->setToolTip(uiText("Action.Settings"));
    settingsButton_->setAccessibleName(uiText("Action.Settings"));
    settingsButton_->setAutoRaise(true);
    settingsButton_->setIconSize(QSize(16, 16));
    bar->addWidget(settingsButton_);
    for (auto *button : {favoritesButton_, refreshButton_, settingsButton_}) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(ToolbarControlSize, ToolbarControlSize);
    }
    layout->addLayout(bar);
    updateToolbarIcons();
    model_ = new FileModel(this);
    filter_ = new NameFilter(this);
    filter_->setFavoritesFirst(settings_.favoritesFirst);
    filter_->setExtensionOrder(settings_.extensions);
    filter_->setSourceModel(model_);
    sceneResults_ = new QListWidget(this);
    table_ = new ResultsTable(filter_, this);
    table_->setObjectName(QStringLiteral("resultsTable"));
    table_->setModel(filter_);
    table_->setItemDelegate(new HighlightDelegate(filter_, table_, sceneResults_));
    table_->setMouseTracking(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->setMinimumSectionSize(1);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->setColumnWidth(1, 140);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    emptyResults_ = new QLabel(table_->viewport());
    emptyResults_->setObjectName(QStringLiteral("emptyResultsLabel"));
    emptyResults_->setTextFormat(Qt::PlainText);
    emptyResults_->setAlignment(Qt::AlignCenter);
    emptyResults_->setWordWrap(true);
    emptyResults_->setForegroundRole(QPalette::PlaceholderText);
    emptyResults_->setAttribute(Qt::WA_TransparentForMouseEvents);
    emptyResults_->hide();
    layout->addWidget(table_, 1);
    sceneResults_->setObjectName(QStringLiteral("sceneResults"));
    sceneResults_->setAccessibleName(uiText("Search.Scenes"));
    sceneResults_->setAlternatingRowColors(true);
    sceneResults_->setItemDelegate(new HighlightDelegate(nullptr, sceneResults_));
    sceneResults_->setMouseTracking(true);
    sceneResults_->setUniformItemSizes(true);
    sceneResults_->setSpacing(0);
    sceneResults_->hide();
    layout->addWidget(sceneResults_, 1);
    connect(sceneResults_, &QListWidget::itemDoubleClicked, this, [this]() { openSelected(); });
    status_ = new CompactStatusLabel(this);
    status_->setObjectName(QStringLiteral("statusLabel"));
    status_->setTextFormat(Qt::PlainText);
    help_ = new QLabel(QStringLiteral(" | ") + uiText("Panel.Help"), this);
    help_->setObjectName(QStringLiteral("helpLabel"));
    auto *footer = new QHBoxLayout;
    footer->setSpacing(0);
    footer->addWidget(status_);
    footer->addWidget(help_);
    footer->addStretch();
    layout->addLayout(footer);
    updateCompactLayout();
    updateListMetrics();
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(120);
    historyTimer_ = new QTimer(this);
    historyTimer_->setObjectName(QStringLiteral("historyTimer"));
    historyTimer_->setSingleShot(true);
    historyTimer_->setInterval(1000);
    noticeTimer_ = new QTimer(this);
    noticeTimer_->setObjectName(QStringLiteral("statusNoticeTimer"));
    noticeTimer_->setSingleShot(true);
    noticeTimer_->setInterval(2000);
    connect(noticeTimer_, &QTimer::timeout, this, [this]() {
        statusNotice_.clear();
        updateStatus();
    });
    connect(search_, &QLineEdit::textChanged, this, [this]() {
        historyCleared_ = false;
        searchTimer_->start();
        historyTimer_->start();
    });
    connect(search_, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (isSceneSearch() || !text.endsWith(QLatin1Char('*')))
            return;
        // 等输入法提交完成，再处理文件名后的星号。
        QTimer::singleShot(0, this, [this]() {
            auto query = search_->text();
            if (shuttingDown_ || isSceneSearch() || !query.endsWith(QLatin1Char('*')))
                return;
            while (query.endsWith(QLatin1Char('*')))
                query.chop(1);
            search_->setText(query);
            if (queryTokens(query) == filter_->tokens())
                searchTimer_->stop();
            toggleSelectedFavorite(true);
        });
    });
    connect(historyTimer_, &QTimer::timeout, this, &SearchDock::rememberSearch);
    connect(searchTimer_, &QTimer::timeout, this, [this]() { applyQuery(); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto *file = index.isValid() ? model_->fileAt(filter_->mapToSource(index).row()) : nullptr;
        if (!file)
            return;
        const auto path = file->path;
        rememberSearch();
        if (searchTimer_->isActive())
            applyQuery();
        openFile(path);
    });
    updateColumnVisibility();
    connect(table_, &QTableView::customContextMenuRequested, this, &SearchDock::showContextMenu);
    connect(model_, &QAbstractItemModel::modelReset, this, &SearchDock::updateFavoriteTooltip);
    connect(model_, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
            if (roles.isEmpty() || roles.contains(FileModel::FavoriteRole)) {
                updateFavoriteTooltip();
                table_->viewport()->update();
            }
        });
    updateFavoriteTooltip();
    connect(favoritesButton_, &QToolButton::toggled, this, [this](bool checked) {
        table_->setSelectionMode(checked ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
        filter_->setFavoritesOnly(checked);
        updateToolbarIcons();
        table_->clearSelection();
        table_->setCurrentIndex({});
        applyQuery();
    });
    connect(refreshButton_, &QToolButton::clicked, this, [this]() {
        if (isSceneSearch())
            applyQuery();
        else
            refresh();
    });
    connect(settingsButton_, &QToolButton::clicked, this, &SearchDock::openSettings);
    watcher_ = new QFileSystemWatcher(this);
    watchTimer_ = new QTimer(this);
    watchTimer_->setSingleShot(true);
    watchTimer_->setInterval(600);
    pollTimer_ = new QTimer(this);
    pollTimer_->setObjectName(QStringLiteral("pollTimer"));
    sceneSwitchTimer_ = new QTimer(this);
    sceneSwitchTimer_->setObjectName(QStringLiteral("sceneSwitchTimer"));
    sceneSwitchTimer_->setSingleShot(true);
    sceneSwitchTimer_->setTimerType(Qt::PreciseTimer);
    connect(sceneSwitchTimer_, &QTimer::timeout, this, [this]() {
        if (switchScene_)
            switchScene_(settings_.targetSceneId);
    });
    homeExitTimer_ = new QTimer(this);
    homeExitTimer_->setObjectName(QStringLiteral("homeExitTimer"));
    homeExitTimer_->setSingleShot(true);
    homeExitTimer_->setTimerType(Qt::PreciseTimer);
    connect(homeExitTimer_, &QTimer::timeout, this, [this] { finishPresentationEscape(true); });

    // 合并连续文件变化；监控失败时才使用定时扫描。
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this]() {
        if (settings_.autoRefresh && !shuttingDown_)
            watchTimer_->start();
    });
    connect(watchTimer_, &QTimer::timeout, this, &SearchDock::refresh);
    connect(pollTimer_, &QTimer::timeout, this, [this]() {
        if (!worker_)
            refresh();
    });
    syncWatchers({});
    updateScopeChoices();
    connect(scope_, &QComboBox::currentIndexChanged, this, [this](int) {
        filter_->setFolderScope(scope_->currentData().toString());
        scope_->setToolTip(scope_->currentText());
        applyQuery();
    });
    if (!settingsError.isEmpty()) {
        status_->setText(uiText("Status.ReadSettingsError").arg(settingsError));
    } else {
        updateStatus();
        QTimer::singleShot(0, this, &SearchDock::refresh);
    }
    // 面板按键在应用层处理。
    qApp->installEventFilter(this);
}

SearchDock::~SearchDock() { shutdown(); }

void SearchDock::shutdown()
{
    shuttingDown_ = true;
    if (qApp)
        qApp->removeEventFilter(this);
    searchTimer_->stop();
    historyTimer_->stop();
    noticeTimer_->stop();
    watchTimer_->stop();
    pollTimer_->stop();
    cancelPresentation();
    delete std::exchange(escapeInterceptor_, nullptr);
    watcher_->blockSignals(true);
    delete settingsDialog_.data();
    if (worker_) {
        // 等后台扫描结束，再释放控件和插件代码。
        worker_->cancel();
        disconnect(worker_, nullptr, this, nullptr);
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
}

void SearchDock::focusSearch()
{
    if (shuttingDown_)
        return;
    bringWindowForward(mainWindow_.data());
    if (settingsDialog_ && settingsDialog_->isVisible()) {
        bringWindowForward(settingsDialog_.data());
        return;
    }
    if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        if (popup->parent() == this)
            popup->close();
    }
    if (auto *container = qobject_cast<QDockWidget *>(parentWidget())) {
        container->show();
        container->raise();
    }
    auto *top = window();
    if (top != mainWindow_.data())
        bringWindowForward(top);
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
}

void SearchDock::toggleSelectedFavorite(bool clearInput)
{
    const auto *focused = QApplication::focusWidget();
    // 收藏快捷键在面板内生效，设置窗口和弹出菜单除外。
    if (shuttingDown_ || isSceneSearch() || !isVisible() || !window()->isActiveWindow() || !focused
        || (focused != this && !isAncestorOf(focused)) || QApplication::activePopupWidget()
        || (settingsDialog_ && settingsDialog_->isVisible()))
        return;
    if (searchTimer_->isActive())
        applyQuery();
    if (table_->selectionModel()->selectedRows().size() != 1)
        return;
    if (const auto *file = selectedFile()) {
        const auto path = file->path;
        const QPersistentModelIndex selected = filter_->mapToSource(table_->currentIndex());
        toggleFavorite(path);
        if (clearInput)
            clearSearch(selected);
    }
}

void SearchDock::setFavoriteShortcutMatcher(std::function<bool(const QInputEvent &)> matcher)
{
    favoriteShortcutMatches_ = std::move(matcher);
}

void SearchDock::setShortcutSummaryProvider(std::function<ShortcutSummary()> provider)
{
    shortcutSummaryProvider_ = std::move(provider);
}

void SearchDock::setSceneCallbacks(std::function<QList<SceneChoice>()> scenes, std::function<void(const QString &)> switchScene)
{
    sceneChoices_ = std::move(scenes);
    switchScene_ = std::move(switchScene);
    if (!escapeInterceptor_) {
        escapeInterceptor_ = new PresentationEscape(this, [this](auto complete) { presentationEscape(std::move(complete)); });
        escapeInterceptor_->setEnabled(settings_.escapeReturnsHome);
        escapeInterceptor_->start();
    }
}

void SearchDock::presentationEscape(std::function<void(bool)> complete)
{
    if (shuttingDown_ || !settings_.escapeReturnsHome) {
        complete(true);
        return;
    }
    if (pendingEscape_) {
        complete(false);
        return;
    }
    cancelSceneSwitch();
    const auto home = homeSceneId();
    if (home.isEmpty() || !switchScene_) {
        complete(true);
        return;
    }
    pendingEscape_ = std::move(complete);
    switchScene_(home);
    homeExitTimer_->start(settings_.sceneDelayMs);
}

void SearchDock::finishPresentationEscape(bool send)
{
    homeExitTimer_->stop();
    if (auto complete = std::exchange(pendingEscape_, {}))
        complete(send);
}

void SearchDock::cancelSceneSwitch()
{
    sceneSwitchTimer_->stop();
    ++sceneSwitchGeneration_;
    pendingPresentationPath_.clear();
}

void SearchDock::toggleFavoritesView()
{
    if (shuttingDown_ || isSceneSearch() || (settingsDialog_ && settingsDialog_->isVisible()))
        return;
    favoritesButton_->toggle();
    focusSearch();
}

void SearchDock::homeHotkey(bool pressed)
{
    const bool repeated = std::exchange(homeKeyPressed_, pressed);
    if (!pressed) {
        homeEscapeBlocked_ = false;
        return;
    }
    if (repeated || shuttingDown_ || pendingEscape_ || !settings_.escapeReturnsHome || homeEscapeBlocked_
        || QApplication::queryKeyboardModifiers() != Qt::NoModifier
        || QApplication::activePopupWidget() || QApplication::activeModalWidget()
        || (settingsDialog_ && settingsDialog_->isVisible()))
        return;
    // 后台时忽略 OBS 留存的输入焦点。
    if (QApplication::applicationState() == Qt::ApplicationActive && blocksHomeEscape(QApplication::focusWidget()))
        return;
    cancelSceneSwitch();
    const auto home = homeSceneId();
    if (!home.isEmpty() && switchScene_)
        switchScene_(home);
}

QString SearchDock::homeSceneId() const
{
    if (sceneChoices_)
        for (const auto &scene : sceneChoices_())
            if (scene.name == QStringLiteral("HOME"))
                return scene.id;
    return {};
}


bool SearchDock::eventFilter(QObject *watched, QEvent *event)
{
    if (shuttingDown_)
        return false;
    if (event->type() == QEvent::ApplicationDeactivate)
        homeEscapeBlocked_ = false;
    if (watched == this && (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
        QTimer::singleShot(0, this, &SearchDock::updateListMetrics);
    if (((watched == search_ || watched == favoritesButton_ || watched == refreshButton_ || watched == settingsButton_)
         && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange))
        || (watched == window() && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::DevicePixelRatioChange))) {
        // 等主题应用完毕后取色，切换主题或屏幕缩放时同步更新。
        QTimer::singleShot(0, this, &SearchDock::updateToolbarIcons);
    }
    if (watched == this && event->type() == QEvent::Resize)
        updateCompactLayout();
    if (watched == table_ && event->type() == QEvent::Resize)
        updateColumnVisibility();
    if (watched == table_->viewport() && event->type() == QEvent::Resize)
        emptyResults_->setGeometry(table_->viewport()->rect().adjusted(8, 4, -8, -4));
    if (watched == table_->viewport() && (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            const auto index = table_->indexAt(mouse->position().toPoint());
            const bool onStar = mouse->modifiers() == Qt::NoModifier && index.isValid() && index.column() == 0
                && favoriteHitRect(table_->visualRect(index)).contains(mouse->position().toPoint());
            if (event->type() == QEvent::MouseButtonPress) {
                favoritePressPath_.clear();
                if (onStar) {
                    table_->setFocus(Qt::MouseFocusReason);
                    table_->setCurrentIndex(index);
                    favoritePressPath_ = index.data(FileModel::PathRole).toString();
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonDblClick) {
                favoritePressPath_.clear();
                if (onStar)
                    return true;
            } else {
                const auto path = std::exchange(favoritePressPath_, QString());
                if (!path.isEmpty()) {
                    if (onStar && index.data(FileModel::PathRole).toString() == path)
                        toggleFavorite(path);
                    return true;
                }
                if (onStar)
                    return true;
            }
        }
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride
        && event->type() != QEvent::MouseButtonPress)
        return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget)
        return false;
    const auto *key = event->type() == QEvent::MouseButtonPress ? nullptr : static_cast<QKeyEvent *>(event);
    if (settings_.escapeReturnsHome && key && key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier
        && event->type() == QEvent::ShortcutOverride && !key->isAutoRepeat()) {
        // 保留按键时的上下文，避免菜单关闭后的回调误切场景。
        homeEscapeBlocked_ = QApplication::activePopupWidget() || QApplication::activeModalWidget()
            || blocksHomeEscape(widget) || blocksHomeEscape(QApplication::focusWidget());
    }

    if (key && key->key() == Qt::Key_Escape
        && key->modifiers() == Qt::NoModifier && !QApplication::activePopupWidget()) {
        const auto *top = widget->window();
        // 使用 OBS 的投影标记，仅保护全屏窗口。
        if (top->isFullScreen() && top->windowHandle()
            && top->windowHandle()->property("isOBSProjectorWindow").toBool()) {
            event->accept();
            return true;
        }
    }
    auto *owner = widget;
    while (owner && owner != this && !qobject_cast<QDialog *>(owner))
        owner = owner->parentWidget();
    if (owner != this)
        return false;
    auto *popup = qobject_cast<QMenu *>(widget->window());
    // 设置、文件选择和提示对话框继续使用各自的 Esc 行为。
    if (widget->window() != window() && !popup)
        return false;
    const bool inputStar = watched == search_ && key && (key->key() == Qt::Key_Asterisk || key->text() == QStringLiteral("*"))
        && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    const bool allowPanelAction = !popup && !QApplication::activePopupWidget()
        && !(settingsDialog_ && settingsDialog_->isVisible());
    const bool favoriteShortcut = favoriteShortcutMatches_
        ? favoriteShortcutMatches_(*static_cast<QInputEvent *>(event))
        : key && key->key() == Qt::Key_Asterisk && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if (allowPanelAction && !isSceneSearch() && (inputStar || favoriteShortcut)) {
        event->accept();
        // 输入星号切换收藏后清空查询，长按只执行一次。
        if (event->type() != QEvent::ShortcutOverride && (!key || !key->isAutoRepeat()))
            toggleSelectedFavorite(watched == search_);
        return true;
    }
    if (!key)
        return false;
    if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
        event->accept();
        if (event->type() == QEvent::KeyPress) {
            if (popup)
                popup->close();
            clearSearch();
        }
        return true;
    }
    const bool inResults = watched == table_ || watched == table_->viewport();
    const bool inScenes = watched == sceneResults_ || watched == sceneResults_->viewport();
    if (inResults && key->matches(QKeySequence::Copy) && allowPanelAction) {
        event->accept();
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
            copySelectedName();
        return true;
    }
    const bool plainEnter = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if ((watched == search_ || inResults || inScenes) && plainEnter && allowPanelAction) {
        event->accept();
        // 两种回车共用打开操作，忽略小键盘标记。
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
            openSelected();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        if (watched == search_ && key->key() == Qt::Key_Down && key->modifiers() == Qt::NoModifier) {
            if (searchTimer_->isActive())
                applyQuery();
            QAbstractItemView *results = isSceneSearch() ? static_cast<QAbstractItemView *>(sceneResults_) : table_;
            if (results->model()->rowCount()) {
                results->setFocus(Qt::TabFocusReason);
                if (!results->currentIndex().isValid())
                    results->setCurrentIndex(results->model()->index(0, 0));
            }
            return true;
        }
        const auto current = inScenes ? sceneResults_->currentIndex() : table_->currentIndex();
        if ((inResults || inScenes) && key->key() == Qt::Key_Up && key->modifiers() == Qt::NoModifier
            && (!current.isValid() || current.row() == 0)) {
            search_->setFocus(Qt::TabFocusReason);
            search_->selectAll();
            return true;
        }
    }
    return false;
}

void SearchDock::rememberSearch()
{
    historyTimer_->stop();
    const auto query = search_->text().simplified();
    if (query.isEmpty() || shuttingDown_ || historyCleared_)
        return;
    // 历史仅驻留内存，最近查询优先。
    const auto key = normalizedText(query);
    for (int row = 0; row < searchHistory_.size(); ++row) {
        if (normalizedText(searchHistory_.at(row)) == key) {
            searchHistory_.removeAt(row);
            break;
        }
    }
    searchHistory_.prepend(query);
    if (searchHistory_.size() > MaxSearchHistory)
        searchHistory_.removeLast();
}

void SearchDock::showSearchHistory()
{
    if (shuttingDown_)
        return;
    rememberSearch();
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("searchHistoryMenu"));
    if (searchHistory_.isEmpty())
        menu->addAction(uiText("History.Empty"))->setEnabled(false);
    for (const auto &query : searchHistory_) {
        auto *action = menu->addAction(QString(query).replace(QLatin1Char('&'), QStringLiteral("&&")));
        connect(action, &QAction::triggered, this, [this, query]() {
            if (shuttingDown_)
                return;
            search_->setText(query);
            rememberSearch();
            applyQuery();
            search_->setFocus();
        });
    }
    if (!searchHistory_.isEmpty()) {
        menu->addSeparator();
        auto *clear = menu->addAction(uiText("History.Clear"));
        clear->setObjectName(QStringLiteral("clearHistoryAction"));
        connect(clear, &QAction::triggered, this, [this, menu]() {
            historyTimer_->stop();
            searchHistory_.clear();
            // 当前输入保持不变，输入变化后才重新记录。
            historyCleared_ = true;
            menu->close();
        });
    }
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(search_->mapToGlobal(QPoint(0, search_->height())));
}

void SearchDock::updateFavoriteTooltip()
{
    favoritesButton_->setToolTip(uiText("Favorite.Count").arg(model_->favoriteCount())
        + QLatin1Char('\n') + uiText("Favorite.Session"));
}

void SearchDock::updateToolbarIcons()
{
    if (shuttingDown_ || !historyAction_)
        return;
    // 图标使用各控件的主题文字色，同步亮暗主题和屏幕缩放。
    historyAction_->setIcon(panelIcon(search_, PanelIcon::History));
    favoritesButton_->setIcon(panelIcon(favoritesButton_, PanelIcon::Favorites));
    refreshButton_->setIcon(panelIcon(refreshButton_, PanelIcon::Refresh));
    settingsButton_->setIcon(panelIcon(settingsButton_, PanelIcon::Settings));
}

void SearchDock::updateCompactLayout()
{
    const auto minimum = qMax(90, scope_->fontMetrics().horizontalAdvance(uiText("Scope.All")) + 28);
    scopeRow_->setMinimumWidth(minimum);
    // 多位置时按比例伸展；宽窗口把额外空间留给位置，单位置则铺满搜索框。
    search_->setMaximumWidth(settings_.folders.size() > 1 ? 480 : QWIDGETSIZE_MAX);
}

void SearchDock::updateListMetrics()
{
    if (shuttingDown_)
        return;
    // 只在主题或字体变化时测量一行，沿用原生列表的行高与内容留白。
    QListWidget reference(this);
    reference.setFont(sceneResults_->font());
    reference.addItem(QStringLiteral("Ag"));
    reference.ensurePolished();
    table_->setStyleSheet(QStringLiteral("QTableView { border: none; padding: %1px; selection-background-color: transparent; }").arg(reference.frameWidth()));
    table_->setFont(reference.font());
    table_->verticalHeader()->setDefaultSectionSize(qMax(1, reference.sizeHintForRow(0)));
}

void SearchDock::clearSearch(const QPersistentModelIndex &selected)
{
    QPersistentModelIndex keep = selected;
    if (!keep.isValid() && table_->selectionModel()->hasSelection())
        keep = filter_->mapToSource(table_->currentIndex());
    // 清空前记住完整输入；空查询不进入历史。
    rememberSearch();
    search_->clear();
    historyTimer_->stop();
    applyQuery();
    const auto current = keep.isValid() ? filter_->mapFromSource(keep) : QModelIndex();
    if (current.isValid()) {
        table_->setCurrentIndex(current);
        table_->scrollTo(current);
    } else if (!filter_->rowCount()) {
        table_->clearSelection();
        table_->setCurrentIndex({});
    }
    search_->setFocus();
}

void SearchDock::updateColumnVisibility()
{
    // 窄面板只显示文件名，始终铺满剩余宽度。
    const bool hideTime = table_->width() < TimeColumnMinTableWidth;
    if (table_->isColumnHidden(1) != hideTime)
        table_->setColumnHidden(1, hideTime);
}

void SearchDock::openSettings()
{
    if (shuttingDown_)
        return;
    if (settingsDialog_) {
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }
    // 对话框使用非阻塞方式。
    const auto shortcuts = shortcutSummaryProvider_ ? shortcutSummaryProvider_() : ShortcutSummary{};
    const auto scenes = sceneChoices_ ? sceneChoices_() : QList<SceneChoice>{};
    auto *dialog = new SettingsDialog(settings_, settingsPath_, shortcuts, warnings_,
                                      scenes,
                                      this);
    settingsDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog]() {
        cancelPresentation();
        const bool rescan = settings_.folders != dialog->value.folders || settings_.extensions != dialog->value.extensions
            || settings_.recursive != dialog->value.recursive || settings_.autoRefresh != dialog->value.autoRefresh;
        settings_ = std::move(dialog->value);
        if (escapeInterceptor_) {
            escapeInterceptor_->setEnabled(settings_.escapeReturnsHome);
            escapeInterceptor_->start();
        }

        filter_->setFavoritesFirst(settings_.favoritesFirst);
        filter_->setExtensionOrder(settings_.extensions);
        if (!rescan) {
            pollTimer_->setInterval(settings_.pollIntervalSeconds * 1000);
            table_->viewport()->update();
            updateStatus();
            return;
        }
        model_->setFiles({});
        folderCounts_.clear();
        warnings_.clear();
        updateScopeChoices();
        syncWatchers({});
        refresh();
    });
    dialog->open();
}

bool SearchDock::isSceneSearch() const
{
    return search_->text().startsWith(QLatin1Char('/'));
}

void SearchDock::refreshScenes()
{
    if (!shuttingDown_ && isSceneSearch())
        searchTimer_->start();
}

void SearchDock::applyQuery()
{
    searchTimer_->stop();
    const bool scenes = isSceneSearch();
    table_->setVisible(!scenes);
    sceneResults_->setVisible(scenes);
    scope_->setEnabled(!scenes);
    favoritesButton_->setEnabled(!scenes);
    help_->setText(QStringLiteral(" | ") + uiText(scenes ? "Panel.SceneHelp" : "Panel.Help"));
    if (scenes) {
        const auto tokens = queryTokens(search_->text().mid(1));
        const auto number = tokens.size() == 1 ? numberKey(tokens.first()) : QString();
        static_cast<HighlightDelegate *>(sceneResults_->itemDelegate())->setTokens(tokens);
        const auto *selected = sceneResults_->currentItem();
        const auto previous = selected ? selected->data(Qt::UserRole).toString() : QString();
        sceneResults_->clear();
        const auto choices = sceneChoices_ ? sceneChoices_() : QList<SceneChoice>{};
        QListWidgetItem *numberedScene = nullptr;
        for (qsizetype row = 0; row < choices.size(); ++row) {
            const auto &scene = choices.at(row);
            const auto ordinal = QString::number(row + 1);
            const bool preferred = ordinal == number;
            if (!preferred && !matchesName(normalizedText(scene.name), tokens))
                continue;
            auto *item = new QListWidgetItem(ordinal + QStringLiteral(". ") + scene.name);
            sceneResults_->insertItem(preferred ? 0 : sceneResults_->count(), item);
            item->setData(Qt::UserRole, scene.id);
            if (preferred)
                numberedScene = item;
            else if (number.isEmpty() && scene.id == previous)
                sceneResults_->setCurrentItem(item);
        }
        // 无效序号保留名称匹配，但须手动选择，避免回车误切。
        if (!number.isEmpty())
            sceneResults_->setCurrentItem(numberedScene);
        else if (!sceneResults_->currentItem() && sceneResults_->count())
            sceneResults_->setCurrentRow(0);
        updateStatus();
        return;
    }
    filter_->setQuery(search_->text());
    table_->viewport()->update();
    if (filter_->rowCount())
        table_->setCurrentIndex(filter_->index(0, 0));
    updateStatus();
}

void SearchDock::updateScopeChoices()
{
    const auto previous = scope_->currentData().toString();
    const QSignalBlocker blocker(scope_);
    scope_->clear();
    scope_->addItem(uiText("Scope.All"), QString());
    for (const auto &folder : settings_.folders) {
        const auto display = QDir::toNativeSeparators(folder);
        scope_->addItem(display, folder);
        scope_->setItemData(scope_->count() - 1, display, Qt::ToolTipRole);
    }
    const auto index = scope_->findData(previous);
    scope_->setCurrentIndex(settings_.folders.size() > 1 && index >= 0 ? index : 0);
    scope_->setToolTip(scope_->currentText());
    updateScopeCounts();
    // 单位置收起筛选，位置删除后回到“全部”。
    scopeRow_->setVisible(settings_.folders.size() > 1);
    filter_->setFolderScope(scope_->currentData().toString());
    updateCompactLayout();
}

void SearchDock::updateScopeCounts()
{
    const QSignalBlocker blocker(scope_);
    for (int row = 1; row < scope_->count(); ++row) {
        const auto folder = scope_->itemData(row).toString();
        const auto display = QDir::toNativeSeparators(folder);
        const auto count = folderCounts_.constFind(folder);
        const auto text = count == folderCounts_.cend() ? display : uiText("Scope.Count").arg(display, QString::number(count.value()));
        scope_->setItemText(row, text);
        scope_->setItemData(row, text, Qt::ToolTipRole);
    }
    scope_->setToolTip(scope_->currentText());
}

void SearchDock::syncWatchers(QStringList directories)
{
    if (settings_.autoRefresh) {
        // 缺失的根目录监控其现存父目录，重建后可重新发现。
        for (auto path : settings_.folders) {
            while (!QFileInfo(path).isDir()) {
                const auto parent = QFileInfo(path).absolutePath();
                if (parent == path)
                    break;
                path = parent;
            }
            if (QFileInfo(path).isDir())
                directories.append(path);
        }
        directories.removeDuplicates();
    } else {
        directories.clear();
        watchTimer_->stop();
    }
    const auto current = watcher_->directories();
    const QSet<QString> desiredSet(directories.cbegin(), directories.cend());
    const QSet<QString> currentSet(current.cbegin(), current.cend());
    QStringList remove;
    for (const auto &path : current) {
        if (!desiredSet.contains(path))
            remove.append(path);
    }
    if (!remove.isEmpty())
        watcher_->removePaths(remove);
    QStringList add;
    for (const auto &path : directories) {
        if (!currentSet.contains(path))
            add.append(path);
    }
    watchFailures_ = add.isEmpty() ? QStringList() : watcher_->addPaths(add);
    // 定时扫描仅用于监控失败的情况，间隔由用户设置。
    pollTimer_->setInterval(settings_.pollIntervalSeconds * 1000);
    if (watchFailures_.isEmpty())
        pollTimer_->stop();
    else if (!pollTimer_->isActive())
        pollTimer_->start();
}

void SearchDock::refresh()
{
    if (shuttingDown_)
        return;
    if (worker_) {
        // 丢弃旧扫描；设置改变或连续刷新只保留最后一次请求。
        rescanRequested_ = true;
        worker_->cancel();
        updateStatus();
        return;
    }
    if (settings_.folders.isEmpty()) {
        model_->setFiles({});
        warnings_.clear();
        updateStatus();
        return;
    }
    // 磁盘扫描在后台执行，完成后由界面线程更新结果。
    worker_ = new ScanThread(settings_, this);
    connect(worker_, &QThread::finished, this, &SearchDock::scanFinished);
    worker_->start();
    updateStatus();
}

void SearchDock::scanFinished()
{
    auto *finished = worker_;
    worker_ = nullptr;
    if (!finished)
        return;
    auto result = std::move(finished->result);
    finished->deleteLater();
    if (rescanRequested_) {
        rescanRequested_ = false;
        refresh();
        return;
    }
    if (!result.cancelled) {
        const auto *previous = selectedFile();
        const bool preserveView = filter_->rowCount() > 0 && !searchTimer_->isActive();
        const auto previousPath = previous && preserveView && table_->selectionModel()->hasSelection()
            ? previous->path : QString();
        const auto oldTop = table_->indexAt(QPoint(0, 0));
        const auto topPath = preserveView ? oldTop.data(FileModel::PathRole).toString() : QString();
        const auto topOffset = oldTop.isValid() ? table_->visualRect(oldTop).top() : 0;
        const auto vertical = table_->verticalScrollBar()->value();
        const auto horizontal = table_->horizontalScrollBar()->value();
        warnings_ = std::move(result.warnings);
        if (settingsDialog_) {
            auto *folders = settingsDialog_->findChild<QListWidget *>(QStringLiteral("folderList"));
            static_cast<FolderList *>(folders)->setUnavailableFolders(warnings_);
        }
        folderCounts_ = std::move(result.folderCounts);
        filter_->setFolderAliases(std::move(result.rootAliases));
        syncWatchers(std::move(result.directories));
        model_->setFiles(std::move(result.files));
        updateScopeCounts();
        const auto scanTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        refreshButton_->setToolTip(uiText("Action.Refresh") + QLatin1Char('\n') + uiText("Action.LastScan").arg(scanTime));
        applyQuery();
        // 分别恢复选中项和顶部文件，避免刷新打断浏览。
        if (preserveView) {
            QModelIndex selected;
            QModelIndex top;
            for (int row = 0; row < filter_->rowCount(); ++row) {
                const auto index = filter_->index(row, 0);
                const auto path = index.data(FileModel::PathRole).toString();
                if (!previousPath.isEmpty() && path == previousPath)
                    selected = index;
                if (!topPath.isEmpty() && path == topPath)
                    top = index;
                if ((previousPath.isEmpty() || selected.isValid()) && (topPath.isEmpty() || top.isValid()))
                    break;
            }
            table_->clearSelection();
            table_->setCurrentIndex(selected);
            table_->doItemsLayout();
            if (top.isValid()) {
                table_->scrollTo(top, QAbstractItemView::PositionAtTop);
                if (table_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
                    table_->verticalScrollBar()->setValue(table_->verticalScrollBar()->value() - topOffset);
            } else {
                table_->verticalScrollBar()->setValue(vertical);
            }
            table_->horizontalScrollBar()->setValue(horizontal);
        }
    }
    updateStatus();
}

void SearchDock::updateStatus()
{
    if (isSceneSearch()) {
        emptyResults_->hide();
        status_->setText(uiText("Status.Scenes").arg(sceneResults_->count()));
        status_->setToolTip(sceneResults_->count() ? uiText("Panel.SceneHelp") : uiText("Empty.Scenes"));
        return;
    }
    // 空列表提示使用现有视口，不增加面板高度。
    const bool empty = filter_->rowCount() == 0 && !worker_ && !rescanRequested_;
    emptyResults_->setVisible(empty);
    if (empty) {
        const char *key = settings_.folders.isEmpty() ? "Status.NoFolders"
            : model_->rowCount() == 0 ? "Empty.Files"
            : favoritesButton_->isChecked() ? "Empty.Favorites"
            : !scope_->currentData().toString().isEmpty() ? "Empty.Location"
            : "Empty.Query";
        emptyResults_->setText(uiText(key));
        emptyResults_->setGeometry(table_->viewport()->rect().adjusted(8, 4, -8, -4));
    }
    if (settings_.folders.isEmpty()) {
        status_->setText(uiText("Status.NoFolders"));
        status_->setToolTip({});
        return;
    }
    auto text = rescanRequested_ ? uiText("Status.RescanPending")
        : worker_ && model_->rowCount() == 0 ? uiText("Status.ScanningInitial")
        : filter_->tokens().isEmpty()
            ? uiText(favoritesButton_->isChecked() ? "Status.Favorites" : "Status.Total").arg(filter_->rowCount())
        : uiText("Status.Count").arg(filter_->rowCount()).arg(filter_->scopedFileCount());
    if (worker_ && !rescanRequested_ && model_->rowCount() > 0)
        text += uiText("Status.ScanningSuffix");
    if (!warnings_.isEmpty())
        text += uiText("Status.Inaccessible").arg(warnings_.size());
    if (!watchFailures_.isEmpty())
        text += uiText("Status.WatchFallback");
    const bool notice = noticeTimer_->isActive() && !statusNotice_.isEmpty();
    status_->setText(notice ? statusNotice_ : text);
    const auto details = (warnings_ + watchFailures_).join(QLatin1Char('\n'));
    auto tooltip = details.isEmpty() ? text : text + QLatin1Char('\n') + details;
    if (notice)
        tooltip.prepend(statusNotice_ + QLatin1Char('\n'));
    status_->setToolTip(QStringLiteral("<qt>") + tooltip.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</qt>"));
}

void SearchDock::showStatusNotice(const QString &text)
{
    statusNotice_ = text;
    noticeTimer_->start();
    updateStatus();
}

void SearchDock::copySelectedName()
{
    if (table_->selectionModel()->selectedRows().size() == 1) {
        if (const auto *file = selectedFile())
            QApplication::clipboard()->setText(file->name);
    }
}

const FileEntry *SearchDock::selectedFile() const
{
    const auto current = table_->currentIndex();
    return current.isValid() ? model_->fileAt(filter_->mapToSource(current).row()) : nullptr;
}

void SearchDock::openSelected()
{
    rememberSearch();
    // 回车操作使用当前查询。
    if (searchTimer_->isActive())
        applyQuery();
    if (isSceneSearch()) {
        const auto *item = sceneResults_->currentItem();
        if (!item || !sceneChoices_ || !switchScene_)
            return;
        const auto id = item->data(Qt::UserRole).toString();
        // 执行前重读当前集合，避免转到已删除或旧集合中的场景。
        for (const auto &scene : sceneChoices_()) {
            if (scene.id == id) {
                cancelSceneSwitch();
                switchScene_(id);
                return;
            }
        }
        applyQuery();
        return;
    }
    if (table_->selectionModel()->selectedRows().size() != 1)
        return;
    const auto *file = selectedFile();
    if (file)
        openFile(file->path);
}

void SearchDock::openFile(const QString &path)
{
    if (shuttingDown_)
        return;
    const QFileInfo file(path);
    if (!file.isFile() && !isKeynotePackage(file)) {
        cancelSceneSwitch();
        showMessage(this, uiText("Error.FileMissing.Title"), uiText("Error.FileMissing.Message"), QMessageBox::Information);
        return;
    }
    auto key = file.canonicalFilePath();
    if (key.isEmpty())
        key = file.absoluteFilePath();
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    // 合并短时间内同一文件的打开请求。
    if (key == lastOpenedPath_ && lastOpenTimer_.isValid() && lastOpenTimer_.elapsed() < RepeatOpenIntervalMs)
        return;
    lastOpenedPath_ = key;
    lastOpenTimer_.start();
    finishPresentationEscape(false);
    cancelSceneSwitch();
    if (presentationProcess_) {
        pendingPresentationPath_ = path;
        return;
    }
    auto command = settings_.switchSceneOnOpen && presentationCommand_ ? presentationCommand_(file.absoluteFilePath()) : QStringList{};
    if (!command.isEmpty()) {
        auto *process = new QProcess(this);
        process->setObjectName(QStringLiteral("presentationProcess"));
        presentationProcess_ = process;
        const auto generation = sceneSwitchGeneration_;
        const auto finish = [this, process, path, generation](bool success, bool canFallback) {
            if (presentationProcess_ != process)
                return;
            presentationProcess_.clear();
            process->deleteLater();
            if (shuttingDown_)
                return;
            if (!success)
                qWarning().noquote() << "[OBS-Seeker] Presentation failed:" << process->exitCode()
                    << process->errorString()
                    << process->property("presentationError").toString()
                    << QString::fromUtf8(process->isOpen() ? process->readAllStandardError() : QByteArray{}).trimmed();
            if (!pendingPresentationPath_.isEmpty()) {
                const auto next = std::exchange(pendingPresentationPath_, QString{});
                lastOpenTimer_.invalidate();
                openFile(next);
                return;
            }
            if (!success) {
                const bool denied = process->property("presentationStatus").toInt() == PresentationPermissionDenied;
                if (canFallback && generation == sceneSwitchGeneration_ && openNormally(path)) {
                    lastOpenTimer_.start();
                    showStatusNotice(uiText(denied ? "Status.PresentationPermissionDenied" : "Status.PresentationFallback"));
                    return;
                }
                lastOpenedPath_.clear();
                lastOpenTimer_.invalidate();
                showStatusNotice(uiText(denied ? "Status.PresentationPermissionDenied" : "Status.PresentationFailed"));
            } else if (generation == sceneSwitchGeneration_) {
                scheduleSceneSwitch(path);
            }
        };
        connect(process, &QProcess::finished, this, [finish, process](int code, QProcess::ExitStatus status) {
#ifdef Q_OS_MACOS
            const auto resultPath = process->property("presentationResultPath").toString();
            if (!resultPath.isEmpty()) {
                QFile result(resultPath);
                bool valid = false;
                if (result.open(QIODevice::ReadOnly)) {
                    code = result.readLine().trimmed().toInt(&valid);
                    process->setProperty("presentationError", QString::fromUtf8(result.readAll()).trimmed());
                }
                if (!valid)
                    code = 1;
            }
            process->setProperty("presentationStatus", code);
#endif
            const bool canFallback = (code == PresentationNotOpened || code == PresentationPermissionDenied)
                && status == QProcess::NormalExit;
            finish(code == 0 && status == QProcess::NormalExit, canFallback);
        });
        connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false, true);
        });
        auto *timeout = new QTimer(process);
        timeout->setObjectName(QStringLiteral("presentationTimeout"));
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, process, &QProcess::kill);
        timeout->start(30000);
#ifdef Q_OS_MACOS
        if (command.first() == QStringLiteral("/usr/bin/open")) {
            auto *result = new QTemporaryFile(process);
            if (!result->open()) {
                finish(false, true);
                return;
            }
            result->write("2\n");
            result->close();
            process->setProperty("presentationResultPath", result->fileName());
            command.append(result->fileName());
        }
#endif
        const auto program = command.takeFirst();
        process->start(program, command);
        return;
    }
    if (openNormally(path))
        scheduleSceneSwitch(path);
}

bool SearchDock::openNormally(const QString &path)
{
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        lastOpenedPath_.clear();
        lastOpenTimer_.invalidate();
        showMessage(this, uiText("Error.OpenFile.Title"), uiText("Error.OpenFile.Message"));
        return false;
    }
    return true;
}

void SearchDock::cancelPresentation()
{
    cancelSceneSwitch();
    finishPresentationEscape(false);
    if (!presentationProcess_)
        return;
    auto *process = presentationProcess_.data();
    presentationProcess_.clear();
    disconnect(process, nullptr, this, nullptr);
    process->kill();
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (process->state() == QProcess::NotRunning)
        process->deleteLater();
}

void SearchDock::scheduleSceneSwitch(const QString &path)
{
    if (settings_.switchSceneOnOpen && !settings_.targetSceneId.isEmpty() && switchScene_
        && settings_.extensions.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive))
        sceneSwitchTimer_->start(settings_.sceneDelayMs);
}

void SearchDock::revealFile(const QString &filePath)
{
    const auto path = QDir::toNativeSeparators(filePath);
#ifdef Q_OS_WIN
    // PIDL 避免中文路径和特殊字符的命令行转义。
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto *pidl = ILCreateFromPathW(reinterpret_cast<LPCWSTR>(path.utf16()));
    const bool revealed = pidl && SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
    if (pidl)
        ILFree(pidl);
    if (SUCCEEDED(initialized))
        CoUninitialize();
    if (revealed)
        return;
#elif defined(Q_OS_MACOS)
    if (QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path}))
        return;
#endif
    const auto folder = QFileInfo(path).absolutePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))
        showMessage(this, uiText("Error.OpenFolder.Title"), QDir::toNativeSeparators(folder));
}

void SearchDock::showContextMenu(const QPoint &point)
{
    const auto index = table_->indexAt(point);
    if (!index.isValid())
        return;
    if (table_->selectionModel()->isSelected(index))
        table_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    else
        table_->setCurrentIndex(index);
    const auto *file = selectedFile();
    if (!file)
        return;
    // 菜单操作使用打开时的路径。
    const auto path = file->path;
    QStringList paths;
    for (const auto &selected : table_->selectionModel()->selectedRows())
        paths.append(selected.data(FileModel::PathRole).toString());
    const bool multiple = paths.size() > 1;
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("fileContextMenu"));
    auto *open = menu->addAction(uiText("Menu.Open"));
    auto *reveal = menu->addAction(uiText("Menu.Reveal"));
    menu->addSeparator();
    auto *favorite = menu->addAction(multiple ? uiText("Favorite.RemoveMany").arg(paths.size())
        : uiText(index.data(FileModel::FavoriteRole).toBool() ? "Favorite.Remove" : "Favorite.Add"));
    favorite->setObjectName(QStringLiteral("toggleFavoriteAction"));
    menu->addSeparator();
    if (favoritesButton_->isChecked()) {
        const bool reorder = canReorderFavorites();
        const auto previous = index.row() > 0 ? filter_->index(index.row() - 1, 0).data(FileModel::PathRole).toString() : QString();
        const auto next = filter_->index(index.row() + 1, 0).data(FileModel::PathRole).toString();
        const auto addMove = [&](const char *text, const char *name, const QString &neighbor, int direction, bool edge) {
            auto *action = menu->addAction(uiText(text));
            action->setObjectName(QString::fromLatin1(name));
            action->setEnabled(reorder && (direction < 0 ? index.row() > 0 : index.row() + 1 < filter_->rowCount()));
            connect(action, &QAction::triggered, this, [this, path, neighbor, direction, edge]() {
                moveFavorite(path, neighbor, direction, edge);
            });
        };
        addMove("Menu.MoveTop", "moveFavoriteTopAction", {}, -1, true);
        addMove("Menu.MoveUp", "moveFavoriteUpAction", previous, -1, false);
        addMove("Menu.MoveDown", "moveFavoriteDownAction", next, 1, false);
        addMove("Menu.MoveBottom", "moveFavoriteBottomAction", {}, 1, true);
        menu->addSeparator();
    }
    auto *copyName = menu->addAction(uiText("Menu.CopyName"));
    copyName->setObjectName(QStringLiteral("copyNameAction"));
    // 快捷键仅在列表生效，菜单只显示提示。
    copyName->setText(copyName->text() + QLatin1Char('\t') + QKeySequence(QKeySequence::Copy).toString(QKeySequence::NativeText));
    auto *copy = menu->addAction(uiText("Menu.CopyPath"));
    copy->setObjectName(QStringLiteral("copyPathAction"));
    for (auto *action : {open, reveal, copyName, copy})
        action->setEnabled(!multiple);
    connect(open, &QAction::triggered, this, [this, path]() { openFile(path); });
    connect(favorite, &QAction::triggered, this, [this, path, paths, multiple]() {
        if (!multiple) {
            toggleFavorite(path);
            return;
        }
        const auto removed = model_->removeFavorites(paths);
        if (removed) {
            table_->clearSelection();
            table_->setCurrentIndex({});
            updateFavoriteTooltip();
            showStatusNotice(uiText("Status.FavoritesRemoved").arg(removed));
        }
    });
    connect(reveal, &QAction::triggered, this, [this, path]() { revealFile(path); });
    connect(copyName, &QAction::triggered, this, [path]() { QApplication::clipboard()->setText(QFileInfo(path).fileName()); });
    connect(copy, &QAction::triggered, this, [path]() { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(table_->viewport()->mapToGlobal(point));
}

void SearchDock::toggleFavorite(const QString &path)
{
    const auto previous = table_->currentIndex().data(FileModel::PathRole).toString();
    if (!model_->toggleFavorite(path))
        return;
    // 当前文件离开结果列表时清空选择。
    if (!previous.isEmpty() && table_->currentIndex().data(FileModel::PathRole).toString() != previous) {
        table_->clearSelection();
        table_->setCurrentIndex({});
    }
    showStatusNotice(uiText(model_->isFavorite(path) ? "Status.FavoriteAdded" : "Status.FavoriteRemoved")
        .arg(QFileInfo(path).fileName()));
}

bool SearchDock::canReorderFavorites() const
{
    return !shuttingDown_ && !worker_ && !rescanRequested_ && favoritesButton_->isChecked()
        && table_->selectionModel()->selectedRows().size() == 1
        && search_->text().trimmed().isEmpty() && !searchTimer_->isActive()
        && scope_->currentData().toString().isEmpty()
        && !(settingsDialog_ && settingsDialog_->isVisible());
}

void SearchDock::moveFavorite(const QString &path, const QString &neighbor, int direction, bool toEdge)
{
    if (!canReorderFavorites() || (!toEdge && neighbor.isEmpty()))
        return;
    int row = -1;
    int neighborRow = -1;
    for (int current = 0; current < filter_->rowCount(); ++current) {
        const auto value = filter_->index(current, 0).data(FileModel::PathRole).toString();
        if (value == path)
            row = current;
        if (value == neighbor)
            neighborRow = current;
    }
    if (row < 0)
        return;
    if (toEdge)
        neighborRow = direction < 0 ? 0 : filter_->rowCount() - 1;
    else if (neighborRow < 0 || neighborRow != row + direction)
        return;
    if (row == neighborRow)
        return;
    const QPersistentModelIndex source = filter_->mapToSource(filter_->index(row, 0));
    QList<int> rows;
    rows.reserve(qAbs(row - neighborRow) + 1);
    for (int position = qMin(row, neighborRow); position <= qMax(row, neighborRow); ++position)
        rows.append(filter_->mapToSource(filter_->index(position, 0)).row());
    if (!model_->moveFavoriteOrder(rows, direction > 0))
        return;
    const auto moved = filter_->mapFromSource(source);
    table_->setCurrentIndex(moved);
    table_->scrollTo(moved);
    updateStatus();
}
} // namespace obs_seeker
