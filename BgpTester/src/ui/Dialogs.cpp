#include "ui/Dialogs.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace bgptester
{

RouterDialog::RouterDialog(const RouterConfig& router, const QStringList& otherRouterIds, QWidget* parent)
    : QDialog(parent), original_(router), otherRouterIds_(otherRouterIds)
{
    setWindowTitle(QStringLiteral("路由器属性"));
    setMinimumWidth(430);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    idEdit_ = new QLineEdit(router.id, this);
    routerIdEdit_ = new QLineEdit(router.routerId, this);
    asnEdit_ = new QLineEdit(QString::number(router.asn), this);
    asnEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,10}")), asnEdit_));
    clusterIdEdit_ = new QLineEdit(router.clusterId, this);
    clusterIdEdit_->setPlaceholderText(QStringLiteral("默认与 Router ID 相同"));
    prefixesEdit_ = new QTextEdit(this);
    prefixesEdit_->setAcceptRichText(false);
    prefixesEdit_->setPlainText(router.originatedPrefixes.join(u'\n'));
    prefixesEdit_->setMinimumHeight(110);
    pluginCombo_ = new QComboBox(this);
    for (const auto& plugin : RouterPluginRegistry::instance().plugins())
    {
        pluginCombo_->addItem(QStringLiteral("%1  (%2)").arg(plugin.metadata.displayName, plugin.metadata.id), plugin.metadata.id);
    }
    auto pluginIndex = pluginCombo_->findData(router.pluginId);
    if (pluginIndex < 0)
    {
        pluginCombo_->addItem(QStringLiteral("未注册：%1").arg(router.pluginId), router.pluginId);
        pluginIndex = pluginCombo_->count() - 1;
    }
    pluginCombo_->setCurrentIndex(pluginIndex);
    pluginDescriptionLabel_ = new QLabel(this);
    pluginDescriptionLabel_->setWordWrap(true);
    pluginDescriptionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pluginSettingsEdit_ = new QTextEdit(this);
    pluginSettingsEdit_->setAcceptRichText(false);
    pluginSettingsEdit_->setMinimumHeight(90);
    pluginSettingsEdit_->setPlainText(QString::fromUtf8(QJsonDocument(router.pluginSettings).toJson(QJsonDocument::Indented)));

    const auto updateDescription = [this]
    {
        const auto id = pluginCombo_->currentData().toString();
        const auto metadata = RouterPluginRegistry::instance().metadata(id);
        pluginDescriptionLabel_->setText(metadata ? metadata->description
                                                  : QStringLiteral("该插件当前未编译或未注册；拓扑可以保存，但无法启动仿真。"));
    };
    updateDescription();
    connect(pluginCombo_, &QComboBox::activated, this,
            [this, updateDescription](int)
            {
                updateDescription();
                const auto metadata = RouterPluginRegistry::instance().metadata(pluginCombo_->currentData().toString());
                if (metadata)
                {
                    pluginSettingsEdit_->setPlainText(
                        QString::fromUtf8(QJsonDocument(metadata->defaultSettings).toJson(QJsonDocument::Indented)));
                }
            });
    form->addRow(QStringLiteral("节点 ID"), idEdit_);
    form->addRow(QStringLiteral("BGP Router ID"), routerIdEdit_);
    form->addRow(QStringLiteral("ASN"), asnEdit_);
    form->addRow(QStringLiteral("Cluster ID"), clusterIdEdit_);
    form->addRow(QStringLiteral("本地起源前缀（每行一个）"), prefixesEdit_);
    form->addRow(QStringLiteral("路由器插件"), pluginCombo_);
    form->addRow(QStringLiteral("插件说明"), pluginDescriptionLabel_);
    form->addRow(QStringLiteral("插件配置（JSON 对象）"), pluginSettingsEdit_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &RouterDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

RouterConfig RouterDialog::router() const
{
    auto result = original_;
    result.id = idEdit_->text().trimmed();
    result.routerId = routerIdEdit_->text().trimmed();
    result.asn = asnEdit_->text().toUInt();
    result.clusterId = clusterIdEdit_->text().trimmed();
    if (result.clusterId.isEmpty())
    {
        result.clusterId = result.routerId;
    }
    result.originatedPrefixes.clear();
    for (const auto& line : prefixesEdit_->toPlainText().split(u'\n'))
    {
        const auto prefix = line.trimmed();
        if (!prefix.isEmpty() && !result.originatedPrefixes.contains(prefix))
        {
            result.originatedPrefixes.append(prefix);
        }
    }
    result.pluginId = pluginCombo_->currentData().toString().trimmed();
    const auto settingsDocument = QJsonDocument::fromJson(pluginSettingsEdit_->toPlainText().trimmed().toUtf8());
    if (settingsDocument.isObject())
    {
        result.pluginSettings = settingsDocument.object();
    }
    return result;
}

void RouterDialog::accept()
{
    const auto result = router();
    bool asnOk = false;
    const auto asn = asnEdit_->text().toULongLong(&asnOk);
    if (result.id.isEmpty() || otherRouterIds_.contains(result.id))
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"),
                             result.id.isEmpty() ? QStringLiteral("节点 ID 不能为空。")
                                                 : QStringLiteral("节点 ID 已存在：%1").arg(result.id));
        return;
    }
    if (!asnOk || asn == 0 || asn > std::numeric_limits<quint32>::max())
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"), QStringLiteral("ASN 必须在 1 到 4294967295 之间。"));
        return;
    }
    QJsonParseError settingsError;
    const auto settingsDocument = QJsonDocument::fromJson(pluginSettingsEdit_->toPlainText().trimmed().toUtf8(), &settingsError);
    if (settingsError.error != QJsonParseError::NoError || !settingsDocument.isObject())
    {
        QMessageBox::warning(
            this, QStringLiteral("输入无效"),
            settingsError.error == QJsonParseError::NoError
                ? QStringLiteral("插件配置必须是 JSON 对象。")
                : QStringLiteral("插件配置 JSON 无效（偏移 %1）：%2").arg(settingsError.offset).arg(settingsError.errorString()));
        return;
    }
    Topology topology;
    topology.routers.insert(result.id, result);
    const auto problems = topology.validate();
    if (!problems.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"), problems.join(u'\n'));
        return;
    }
    QDialog::accept();
}

LinkDialog::LinkDialog(const LinkConfig& link, bool externalSession, QWidget* parent) : QDialog(parent), original_(link)
{
    setWindowTitle(QStringLiteral("链路属性：%1 ↔ %2").arg(link.a, link.b));
    setMinimumWidth(450);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    enabledCheck_ = new QCheckBox(QStringLiteral("初始启用"), this);
    enabledCheck_->setChecked(link.enabled);
    delaySpin_ = new QSpinBox(this);
    delaySpin_->setRange(0, 24 * 60 * 60 * 1000);
    delaySpin_->setSuffix(QStringLiteral(" ms"));
    delaySpin_->setValue(link.delayMs);
    relationshipCombo_ = new QComboBox(this);
    relationshipCombo_->addItem(QStringLiteral("未指定"), static_cast<int>(LinkBusinessRelationship::Unspecified));
    relationshipCombo_->addItem(QStringLiteral("Peer"), static_cast<int>(LinkBusinessRelationship::PeerToPeer));
    relationshipCombo_->addItem(QStringLiteral("%1 Provider / %2 Customer").arg(link.a, link.b),
                                static_cast<int>(LinkBusinessRelationship::AProviderOfB));
    relationshipCombo_->addItem(QStringLiteral("%1 Provider / %2 Customer").arg(link.b, link.a),
                                static_cast<int>(LinkBusinessRelationship::BProviderOfA));
    auto relationshipIndex = relationshipCombo_->findData(static_cast<int>(link.businessRelationship));
    if (relationshipIndex < 0)
    {
        relationshipIndex = 0;
    }
    relationshipCombo_->setCurrentIndex(externalSession ? relationshipIndex : 0);
    relationshipCombo_->setEnabled(externalSession);
    if (!externalSession)
    {
        relationshipCombo_->setToolTip(QStringLiteral("同一 ASN 的 iBGP 链路不使用商业关系。"));
    }
    rrFromACheck_ = new QCheckBox(QStringLiteral("%1 将 %2 视为 RR Client").arg(link.a, link.b), this);
    rrFromACheck_->setChecked(link.rrClientFromA);
    rrFromBCheck_ = new QCheckBox(QStringLiteral("%1 将 %2 视为 RR Client").arg(link.b, link.a), this);
    rrFromBCheck_->setChecked(link.rrClientFromB);
    mraiFromASpin_ = new QSpinBox(this);
    mraiFromASpin_->setRange(0, 24 * 60 * 60 * 1000);
    mraiFromASpin_->setSuffix(QStringLiteral(" ms"));
    mraiFromASpin_->setValue(link.mraiMsFromA);
    mraiFromBSpin_ = new QSpinBox(this);
    mraiFromBSpin_->setRange(0, 24 * 60 * 60 * 1000);
    mraiFromBSpin_->setSuffix(QStringLiteral(" ms"));
    mraiFromBSpin_->setValue(link.mraiMsFromB);
    form->addRow(QStringLiteral("状态"), enabledCheck_);
    form->addRow(QStringLiteral("链路延迟"), delaySpin_);
    form->addRow(QStringLiteral("商业关系"), relationshipCombo_);
    form->addRow(QStringLiteral("%1 → %2 MRAI").arg(link.a, link.b), mraiFromASpin_);
    form->addRow(QStringLiteral("%1 → %2 MRAI").arg(link.b, link.a), mraiFromBSpin_);
    form->addRow(QStringLiteral("路由反射"), rrFromACheck_);
    form->addRow(QString{}, rrFromBCheck_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

LinkConfig LinkDialog::link() const
{
    auto result = original_;
    result.enabled = enabledCheck_->isChecked();
    result.delayMs = delaySpin_->value();
    result.businessRelationship = relationshipCombo_->isEnabled()
                                      ? static_cast<LinkBusinessRelationship>(relationshipCombo_->currentData().toInt())
                                      : LinkBusinessRelationship::Unspecified;
    result.rrClientFromA = rrFromACheck_->isChecked();
    result.rrClientFromB = rrFromBCheck_->isChecked();
    result.mraiMsFromA = mraiFromASpin_->value();
    result.mraiMsFromB = mraiFromBSpin_->value();
    return result;
}

TopologyBatchEditDialog::TopologyBatchEditDialog(const QVector<LinkConfig>& links, const QVector<RouterConfig>& routers,
                                                 RouterScope routerScope, QWidget* parent)
    : QDialog(parent)
{
    constexpr auto maximumIntervalMs = 24 * 60 * 60 * 1000;

    setWindowTitle(QStringLiteral("批量配置拓扑"));
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    const auto routerTarget = routerScope == RouterScope::Selection ? QStringLiteral("选中的 %1 台路由器").arg(routers.size())
                                                                    : QStringLiteral("当前拓扑的全部 %1 台路由器").arg(routers.size());
    auto* summaryLabel = new QLabel(
        QStringLiteral("路由器种类和出站 MRAI 可应用到%1；链路延迟可应用到当前拓扑的全部 %2 条链路。").arg(routerTarget).arg(links.size()),
        this);
    summaryLabel->setObjectName(QStringLiteral("batchTopologySummaryLabel"));
    summaryLabel->setWordWrap(true);
    layout->addWidget(summaryLabel);

    auto* routerGroup = new QGroupBox(QStringLiteral("BGP 路由器种类"), this);
    auto* routerLayout = new QVBoxLayout(routerGroup);
    routerPluginCombo_ = new QComboBox(routerGroup);
    routerPluginCombo_->setObjectName(QStringLiteral("batchRouterPluginCombo"));

    QString currentPluginId;
    auto commonPlugin = !routers.isEmpty();
    for (const auto& router : routers)
    {
        if (currentPluginId.isEmpty())
        {
            currentPluginId = router.pluginId;
        }
        else if (currentPluginId != router.pluginId)
        {
            commonPlugin = false;
            break;
        }
    }

    QString unchangedText = QStringLiteral("保持各路由器现有种类");
    if (commonPlugin)
    {
        const auto metadata = RouterPluginRegistry::instance().metadata(currentPluginId);
        const auto currentName = metadata ? metadata->displayName : currentPluginId;
        unchangedText = QStringLiteral("保持现有种类（%1）").arg(currentName);
    }
    routerPluginCombo_->addItem(unchangedText, QString{});
    for (const auto& plugin : RouterPluginRegistry::instance().plugins())
    {
        routerPluginCombo_->addItem(QStringLiteral("%1  (%2)").arg(plugin.metadata.displayName, plugin.metadata.id), plugin.metadata.id);
    }
    routerPluginCombo_->setEnabled(!routers.isEmpty());

    routerPluginDescriptionLabel_ = new QLabel(routerGroup);
    routerPluginDescriptionLabel_->setWordWrap(true);
    routerPluginDescriptionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const auto updateRouterPluginDescription = [this]
    {
        const auto pluginId = routerPluginId();
        if (pluginId.isEmpty())
        {
            routerPluginDescriptionLabel_->setText(QStringLiteral("不修改目标路由器现有的种类和插件配置。"));
            return;
        }
        const auto metadata = RouterPluginRegistry::instance().metadata(pluginId);
        routerPluginDescriptionLabel_->setText(metadata ? metadata->description : QStringLiteral("该路由器种类当前不可用。"));
    };
    updateRouterPluginDescription();
    connect(routerPluginCombo_, &QComboBox::currentIndexChanged, this,
            [updateRouterPluginDescription](int) { updateRouterPluginDescription(); });

    routerLayout->addWidget(routerPluginCombo_);
    routerLayout->addWidget(routerPluginDescriptionLabel_);
    auto* routerNoteLabel =
        new QLabel(QStringLiteral("更换种类时使用新种类的默认插件配置；种类未变化的路由器保留现有插件配置。"), routerGroup);
    routerNoteLabel->setWordWrap(true);
    routerLayout->addWidget(routerNoteLabel);
    layout->addWidget(routerGroup);

    auto* mraiGroup = new QGroupBox(QStringLiteral("路由器出站 MRAI"), this);
    auto* mraiLayout = new QVBoxLayout(mraiGroup);

    unchangedMraiRadio_ = new QRadioButton(QStringLiteral("保持现有值"), mraiGroup);
    unchangedMraiRadio_->setChecked(true);
    mraiLayout->addWidget(unchangedMraiRadio_);

    fixedMraiRadio_ = new QRadioButton(QStringLiteral("固定值"), mraiGroup);
    fixedMraiSpin_ = new QSpinBox(mraiGroup);
    fixedMraiSpin_->setRange(0, maximumIntervalMs);
    fixedMraiSpin_->setSuffix(QStringLiteral(" ms"));
    auto* fixedMraiRow = new QHBoxLayout;
    fixedMraiRow->addWidget(fixedMraiRadio_);
    fixedMraiRow->addWidget(fixedMraiSpin_, 1);
    mraiLayout->addLayout(fixedMraiRow);

    randomMraiRadio_ = new QRadioButton(QStringLiteral("随机范围（包含上下限）"), mraiGroup);
    minimumMraiSpin_ = new QSpinBox(mraiGroup);
    minimumMraiSpin_->setRange(0, maximumIntervalMs);
    minimumMraiSpin_->setSuffix(QStringLiteral(" ms"));
    maximumMraiSpin_ = new QSpinBox(mraiGroup);
    maximumMraiSpin_->setRange(0, maximumIntervalMs);
    maximumMraiSpin_->setSuffix(QStringLiteral(" ms"));
    auto* randomMraiRow = new QHBoxLayout;
    randomMraiRow->addWidget(randomMraiRadio_);
    randomMraiRow->addWidget(new QLabel(QStringLiteral("最小"), mraiGroup));
    randomMraiRow->addWidget(minimumMraiSpin_, 1);
    randomMraiRow->addWidget(new QLabel(QStringLiteral("最大"), mraiGroup));
    randomMraiRow->addWidget(maximumMraiSpin_, 1);
    mraiLayout->addLayout(randomMraiRow);

    QSet<QString> targetRouterIds;
    targetRouterIds.reserve(routers.size());
    for (const auto& router : routers)
    {
        targetRouterIds.insert(router.id);
    }
    QVector<int> currentMraiValues;
    for (const auto& link : links)
    {
        if (targetRouterIds.contains(link.a))
        {
            currentMraiValues.append(link.mraiMsFromA);
        }
        if (targetRouterIds.contains(link.b))
        {
            currentMraiValues.append(link.mraiMsFromB);
        }
    }
    if (!currentMraiValues.isEmpty())
    {
        auto minimum = currentMraiValues.front();
        auto maximum = minimum;
        for (const auto value : currentMraiValues)
        {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        fixedMraiSpin_->setValue(minimum == maximum ? minimum : 0);
        minimumMraiSpin_->setValue(minimum);
        maximumMraiSpin_->setValue(maximum);
    }

    const auto updateMraiInputState = [this]
    {
        fixedMraiSpin_->setEnabled(fixedMraiRadio_->isChecked());
        minimumMraiSpin_->setEnabled(randomMraiRadio_->isChecked());
        maximumMraiSpin_->setEnabled(randomMraiRadio_->isChecked());
    };
    connect(fixedMraiRadio_, &QRadioButton::toggled, this, [updateMraiInputState](bool) { updateMraiInputState(); });
    connect(randomMraiRadio_, &QRadioButton::toggled, this, [updateMraiInputState](bool) { updateMraiInputState(); });
    updateMraiInputState();

    mraiGroup->setEnabled(!currentMraiValues.isEmpty());
    auto* mraiNoteLabel =
        new QLabel(QStringLiteral("固定值应用到目标路由器的全部出站邻居；随机模式为每台目标路由器独立生成一个值。"), mraiGroup);
    mraiNoteLabel->setWordWrap(true);
    mraiLayout->addWidget(mraiNoteLabel);
    layout->addWidget(mraiGroup);

    auto* delayGroup = new QGroupBox(QStringLiteral("链路延迟"), this);
    auto* delayLayout = new QVBoxLayout(delayGroup);

    unchangedDelayRadio_ = new QRadioButton(QStringLiteral("保持现有值"), delayGroup);
    unchangedDelayRadio_->setChecked(true);
    delayLayout->addWidget(unchangedDelayRadio_);

    fixedDelayRadio_ = new QRadioButton(QStringLiteral("固定值"), delayGroup);
    fixedDelaySpin_ = new QSpinBox(delayGroup);
    fixedDelaySpin_->setRange(0, maximumIntervalMs);
    fixedDelaySpin_->setSuffix(QStringLiteral(" ms"));
    auto* fixedRow = new QHBoxLayout;
    fixedRow->addWidget(fixedDelayRadio_);
    fixedRow->addWidget(fixedDelaySpin_, 1);
    delayLayout->addLayout(fixedRow);

    randomDelayRadio_ = new QRadioButton(QStringLiteral("随机范围（包含上下限）"), delayGroup);
    minimumDelaySpin_ = new QSpinBox(delayGroup);
    minimumDelaySpin_->setRange(0, maximumIntervalMs);
    minimumDelaySpin_->setSuffix(QStringLiteral(" ms"));
    maximumDelaySpin_ = new QSpinBox(delayGroup);
    maximumDelaySpin_->setRange(0, maximumIntervalMs);
    maximumDelaySpin_->setSuffix(QStringLiteral(" ms"));
    auto* randomRow = new QHBoxLayout;
    randomRow->addWidget(randomDelayRadio_);
    randomRow->addWidget(new QLabel(QStringLiteral("最小"), delayGroup));
    randomRow->addWidget(minimumDelaySpin_, 1);
    randomRow->addWidget(new QLabel(QStringLiteral("最大"), delayGroup));
    randomRow->addWidget(maximumDelaySpin_, 1);
    delayLayout->addLayout(randomRow);

    if (!links.isEmpty())
    {
        auto minimum = links.front().delayMs;
        auto maximum = minimum;
        for (const auto& link : links)
        {
            minimum = std::min(minimum, link.delayMs);
            maximum = std::max(maximum, link.delayMs);
        }
        fixedDelaySpin_->setValue(minimum == maximum ? minimum : 0);
        minimumDelaySpin_->setValue(minimum);
        maximumDelaySpin_->setValue(maximum);
    }

    const auto updateInputState = [this]
    {
        fixedDelaySpin_->setEnabled(fixedDelayRadio_->isChecked());
        minimumDelaySpin_->setEnabled(randomDelayRadio_->isChecked());
        maximumDelaySpin_->setEnabled(randomDelayRadio_->isChecked());
    };
    connect(fixedDelayRadio_, &QRadioButton::toggled, this, [updateInputState](bool) { updateInputState(); });
    connect(randomDelayRadio_, &QRadioButton::toggled, this, [updateInputState](bool) { updateInputState(); });
    updateInputState();

    delayGroup->setEnabled(!links.isEmpty());
    layout->addWidget(delayGroup);
    auto* noteLabel = new QLabel(QStringLiteral("随机模式会为每条链路独立生成一个整数毫秒值。"), this);
    noteLabel->setWordWrap(true);
    layout->addWidget(noteLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用"));
    connect(buttons, &QDialogButtonBox::accepted, this, &TopologyBatchEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

TopologyBatchEditDialog::DelayMode TopologyBatchEditDialog::delayMode() const
{
    if (fixedDelayRadio_->isChecked())
    {
        return DelayMode::Fixed;
    }
    return randomDelayRadio_->isChecked() ? DelayMode::RandomRange : DelayMode::Unchanged;
}

int TopologyBatchEditDialog::fixedDelayMs() const
{
    return fixedDelaySpin_->value();
}

int TopologyBatchEditDialog::minimumDelayMs() const
{
    return minimumDelaySpin_->value();
}

int TopologyBatchEditDialog::maximumDelayMs() const
{
    return maximumDelaySpin_->value();
}

TopologyBatchEditDialog::MraiMode TopologyBatchEditDialog::mraiMode() const
{
    if (fixedMraiRadio_->isChecked())
    {
        return MraiMode::Fixed;
    }
    return randomMraiRadio_->isChecked() ? MraiMode::RandomRange : MraiMode::Unchanged;
}

int TopologyBatchEditDialog::fixedMraiMs() const
{
    return fixedMraiSpin_->value();
}

int TopologyBatchEditDialog::minimumMraiMs() const
{
    return minimumMraiSpin_->value();
}

int TopologyBatchEditDialog::maximumMraiMs() const
{
    return maximumMraiSpin_->value();
}

QString TopologyBatchEditDialog::routerPluginId() const
{
    return routerPluginCombo_->currentData().toString();
}

QJsonObject TopologyBatchEditDialog::routerPluginDefaultSettings() const
{
    const auto metadata = RouterPluginRegistry::instance().metadata(routerPluginId());
    return metadata ? metadata->defaultSettings : QJsonObject{};
}

void TopologyBatchEditDialog::accept()
{
    if (mraiMode() == MraiMode::RandomRange && minimumMraiMs() > maximumMraiMs())
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"), QStringLiteral("随机 MRAI 的最小值不能大于最大值。"));
        return;
    }
    if (delayMode() == DelayMode::RandomRange && minimumDelayMs() > maximumDelayMs())
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"), QStringLiteral("随机延迟的最小值不能大于最大值。"));
        return;
    }
    QDialog::accept();
}

SimulationSettingsDialog::SimulationSettingsDialog(const SimulationSettings& settings, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("仿真设置"));
    setMinimumWidth(430);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    nameEdit_ = new QLineEdit(settings.name, this);
    logDirectoryEdit_ = new QLineEdit(settings.logDirectory, this);
    quietSpin_ = new QSpinBox(this);
    quietSpin_->setRange(0, 600000);
    quietSpin_->setSuffix(QStringLiteral(" ms"));
    quietSpin_->setValue(settings.convergenceQuietMs);
    form->addRow(QStringLiteral("实验名称"), nameEdit_);
    form->addRow(QStringLiteral("日志目录"), logDirectoryEdit_);
    form->addRow(QStringLiteral("收敛静默窗口"), quietSpin_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SimulationSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

SimulationSettings SimulationSettingsDialog::settings() const
{
    return SimulationSettings{.name = nameEdit_->text().trimmed(),
                              .logDirectory = logDirectoryEdit_->text().trimmed(),
                              .convergenceQuietMs = quietSpin_->value()};
}

void SimulationSettingsDialog::accept()
{
    if (nameEdit_->text().trimmed().isEmpty() || logDirectoryEdit_->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("输入无效"), QStringLiteral("实验名称和日志目录不能为空。"));
        return;
    }
    QDialog::accept();
}

} // namespace bgptester
